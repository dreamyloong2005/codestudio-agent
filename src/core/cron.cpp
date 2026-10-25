#include "codestudio/cron.hpp"

#include <array>
#include <charconv>
#include <chrono>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace codestudio {
namespace {
using namespace std::chrono;

[[noreturn]] void invalid(const char* reason) {
    throw std::invalid_argument(std::string("Invalid cron expression: ")+reason);
}

int integer(std::string_view value,int low,int high) {
    int number=0;
    const auto [end,error]=std::from_chars(value.data(),value.data()+value.size(),number);
    if (value.empty() || error!=std::errc{} || end!=value.data()+value.size() || number<low || number>high)
        invalid("field value or step is out of range");
    return number;
}

struct Field {
    std::array<bool,60> selected{};
    std::vector<int> values;
    bool unrestricted=false;
};

Field field(std::string_view text,int low,int high,bool weekday_field=false) {
    Field result;
    for (std::size_t start=0;;) {
        const auto comma=text.find(',',start);
        auto part=text.substr(start,comma==std::string_view::npos ? text.size()-start : comma-start);
        if (part.empty()) invalid("empty list item");
        const auto slash=part.find('/');
        int step=1;
        const bool stepped=slash!=std::string_view::npos;
        if (stepped) {
            if (part.find('/',slash+1)!=std::string_view::npos) invalid("multiple step separators");
            step=integer(part.substr(slash+1),1,high-low+1);
            part=part.substr(0,slash);
        }
        int first=low,last=high;
        if (part=="*") {
            // */1 is the same unrestricted field as *. A larger step selects
            // specific days, so it participates in the restricted-day OR rule.
            if (step==1) result.unrestricted=true;
        } else {
            const auto dash=part.find('-');
            if (dash==std::string_view::npos) {
                first=integer(part,low,high);
                last=stepped ? high : first;
            } else {
                if (part.find('-',dash+1)!=std::string_view::npos) invalid("multiple range separators");
                first=integer(part.substr(0,dash),low,high);
                last=integer(part.substr(dash+1),low,high);
                if (first>last) invalid("descending ranges are not supported");
            }
        }
        for (int value=first;value<=last;value+=step)
            result.selected[weekday_field && value==7 ? 0 : value]=true;
        if (comma==std::string_view::npos) break;
        start=comma+1;
    }
    for (int value=low;value<=(weekday_field ? 6 : high);++value)
        if (result.selected[value]) result.values.push_back(value);
    if (result.values.empty()) invalid("field selects no values");
    return result;
}

struct Expression {
    Field minute,hour,month_day,month,week_day;
    explicit Expression(const std::string& text) {
        if (text.empty() || text.size()>256) invalid("length must be 1..256 bytes");
        for (unsigned char value : text)
            if (value>=127 || (value<32 && value!='\t')) invalid("only ASCII fields, spaces and tabs are supported");
        std::istringstream input(text);
        std::array<std::string,5> parts;
        for (auto& part : parts) if (!(input>>part)) invalid("exactly five fields are required");
        std::string extra;
        if (input>>extra) invalid("exactly five fields are required");
        minute=field(parts[0],0,59);
        hour=field(parts[1],0,23);
        month_day=field(parts[2],1,31);
        month=field(parts[3],1,12);
        week_day=field(parts[4],0,7,true);
        if (week_day.unrestricted) {
            // A leap year has every realizable month/day combination. With a
            // restricted weekday, that field alone can satisfy the OR rule.
            bool possible=false;
            for (int selected_month : month.values) {
                const auto last=year_month_day_last{year{2000},month_day_last{std::chrono::month{static_cast<unsigned>(selected_month)}}};
                const auto maximum=static_cast<unsigned>(last.day());
                for (int selected_day : month_day.values)
                    if (static_cast<unsigned>(selected_day)<=maximum) possible=true;
            }
            if (!possible) invalid("selected month/day combination never occurs");
        }
    }

    bool matches(local_days day) const {
        const year_month_day date{day};
        if (!month.selected[static_cast<unsigned>(date.month())]) return false;
        const bool dom=month_day.selected[static_cast<unsigned>(date.day())];
        const bool dow=week_day.selected[weekday{day}.c_encoding()];
        return month_day.unrestricted || week_day.unrestricted ? dom && dow : dom || dow;
    }
};

const time_zone* timezone(const std::string& name) {
    if (name=="UTC") return nullptr;
    return locate_zone(name);
}

constexpr auto minimum_ms=duration_cast<milliseconds>(sys_days{year{1601}/January/1}.time_since_epoch()).count();
constexpr auto maximum_ms=duration_cast<milliseconds>(sys_days{year{10000}/January/1}.time_since_epoch()).count()-1;

std::int64_t search(const std::string& expression,const std::string& name,std::int64_t value,bool forward) {
    if (value<minimum_ms || value>maximum_ms)
        throw std::out_of_range("Cron timestamp must have a UTC year in 1601..9999");
    const Expression selected(expression);
    const auto normalized=cron_timezone(name);
    const auto* zone=timezone(normalized);
    const sys_time<milliseconds> reference{milliseconds{value}};
    const auto local=zone ? zone->to_local(reference) : local_time<milliseconds>{reference.time_since_epoch()};
    // A historical offset change may repeat a date, not just an hour. Search
    // neighboring local dates too, and compare candidates by UTC instant.
    const auto start=floor<days>(local)+(forward ? days{-2} : days{2});
    std::optional<std::int64_t> best;
    constexpr int horizon_days=8*366+4;
    const auto consider=[&](sys_seconds candidate) {
        const auto stamp=duration_cast<milliseconds>(candidate.time_since_epoch()).count();
        if (stamp<minimum_ms || stamp>maximum_ms || (forward ? stamp<=value : stamp>value)) return;
        if (!best || (forward ? stamp<*best : stamp>*best)) best=stamp;
    };
    for (int index=0;index<=horizon_days;++index) {
        const auto date=start+(forward ? days{index} : days{-index});
        // IANA offsets are strictly inside +/-24h. This bound ensures that a
        // later scanned local date cannot hide an earlier UTC occurrence.
        const auto floor_ms=duration_cast<milliseconds>(date.time_since_epoch()-hours{24}).count();
        const auto ceiling_ms=duration_cast<milliseconds>(date.time_since_epoch()+days{1}+hours{24}).count();
        if (best && (forward ? floor_ms>*best : ceiling_ms<*best)) break;
        if (!selected.matches(date)) continue;
        for (int hour : selected.hour.values) for (int minute : selected.minute.values) {
            const local_seconds candidate{date.time_since_epoch()+hours{hour}+minutes{minute}};
            if (!zone) { consider(sys_seconds{candidate.time_since_epoch()}); continue; }
            const auto info=zone->get_info(candidate);
            if (info.result==local_info::nonexistent) continue;
            const auto convert=[&](const sys_info& part) {
                if (part.offset<=-hours{24} || part.offset>=hours{24})
                    throw std::runtime_error("Cron time-zone offset is outside the supported range");
                consider(sys_seconds{candidate.time_since_epoch()-part.offset});
            };
            convert(info.first);
            if (info.result==local_info::ambiguous) convert(info.second);
        }
    }
    if (!best) throw std::out_of_range("No cron occurrence within eight years or the supported date range");
    return *best;
}
}

std::string cron_timezone(const std::string& zone) {
    if (zone.empty() || zone.size()>128) throw std::invalid_argument("Cron time zone must contain 1..128 bytes");
    for (unsigned char value : zone)
        if (value<=32 || value>=127) throw std::invalid_argument("Invalid cron time-zone name");
    if (zone=="UTC") return zone;
    try {
        const auto* resolved=zone=="local" ? std::chrono::current_zone() : std::chrono::locate_zone(zone);
        return std::string(resolved->name());
    } catch (const std::exception&) {
        throw std::invalid_argument("Unknown or unavailable cron time zone: "+zone);
    }
}

void validate_cron(const std::string& expression,const std::string& zone) {
    (void)Expression(expression);
    (void)cron_timezone(zone);
}

std::int64_t cron_next(const std::string& expression,const std::string& zone,std::int64_t after_ms) {
    return search(expression,zone,after_ms,true);
}
std::int64_t cron_previous(const std::string& expression,const std::string& zone,std::int64_t at_ms) {
    return search(expression,zone,at_ms,false);
}
}
