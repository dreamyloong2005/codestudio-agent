#include "codestudio/cron.hpp"

#include <chrono>
#include <limits>
#include <stdexcept>

namespace {
using namespace codestudio;
using namespace std::chrono;
void check(bool value,const char* message) {
    if (!value) throw std::runtime_error(std::string("cron: ")+message);
}
template<class F> void rejects(F action,const char* message) {
    bool caught=false;
    try { action(); } catch (const std::exception&) { caught=true; }
    check(caught,message);
}
std::int64_t stamp(int y,unsigned m,unsigned d,int h=0,int minute=0) {
    return duration_cast<milliseconds>((sys_days{year{y}/month{m}/day{d}}+hours{h}+minutes{minute}).time_since_epoch()).count();
}

void syntax_and_steps() {
    for (const auto* valid : {"* * * * *","  0\t12 1,15 1-12/2 0,7  ","5/10 0-23/2 */2 * 1-5","0 0 31 2 1"})
        validate_cron(valid,"UTC");
    for (const auto* invalid : {""," ","* * * *","* * * * * *","@daily","60 * * * *",
        "-1 * * * *","* 24 * * *","* * 0 * *","* * * 13 *","* * * * 8",
        "*/0 * * * *","*/-1 * * * *","*/61 * * * *","5-3 * * * *","1,,2 * * * *",
        ",1 * * * *","1, * * * *","1-2-3 * * * *","*/2/3 * * * *","+1 * * * *",
        "0 0 ? * *","0 0 L * *","0 0 * JAN MON","0 0 30 2 *","0 0 31 4 *","0 0 * * *\n"})
        rejects([&] { validate_cron(invalid,"UTC"); },"invalid expression was accepted");
    rejects([] { validate_cron(std::string(257,'*'),"UTC"); },"oversized expression accepted");
    rejects([] { validate_cron(std::string("* * * * *\0hidden",16),"UTC"); },"NUL expression accepted");
    const auto base=stamp(2026,10,21,12,5);
    check(cron_next("5/10 * * * *","UTC",base)==stamp(2026,10,21,12,15),"numeric-start step did not advance");
    check(cron_previous("5/10 * * * *","UTC",base+59'999)==base,"previous stepped occurrence was not inclusive");
    check(cron_next("1,1,2-4/2 * * * *","UTC",stamp(2026,10,21,12,1))==stamp(2026,10,21,12,2),"list/range duplicate handling failed");
}

void calendar_and_ordering() {
    const auto base=stamp(2026,10,21,12,0);
    check(cron_next("* * * * *","UTC",base)==base+60'000,"next must be strictly after an exact minute");
    check(cron_next("* * * * *","UTC",base-1)==base,"sub-minute next rounded incorrectly");
    check(cron_previous("* * * * *","UTC",base)==base,"previous must include an exact minute");
    check(cron_previous("* * * * *","UTC",base-1)==base-60'000,"sub-minute previous rounded incorrectly");
    check(cron_next("0 0 * * *","UTC",stamp(2026,12,31,23,59))==stamp(2027,1,1),"year rollover failed");
    check(cron_previous("0 0 1 * *","UTC",stamp(2026,1,1)-1)==stamp(2025,12,1),"reverse month rollover failed");
    check(cron_next("0 0 29 2 *","UTC",stamp(2096,2,29))==stamp(2104,2,29),"century exception lost next leap day");
    check(cron_previous("0 0 29 2 *","UTC",stamp(2104,2,29)-1)==stamp(2096,2,29),"century exception lost previous leap day");
    // April 1, 2026 is Wednesday. With both fields restricted, Monday OR
    // day-of-month 1 is eligible, rather than their intersection.
    check(cron_next("0 0 1 * 1","UTC",stamp(2026,3,31))==stamp(2026,4,1),"restricted day fields were not ORed");
    check(cron_next("0 0 1 * 1","UTC",stamp(2026,4,1))==stamp(2026,4,6),"weekday half of OR was ignored");
    check(cron_next("0 0 * * 1","UTC",stamp(2026,3,31))==stamp(2026,4,6),"wildcard day-of-month ignored weekday restriction");
    check(cron_next("0 0 1 * *","UTC",stamp(2026,4,1))==stamp(2026,5,1),"wildcard weekday ignored day-of-month restriction");
    check(cron_next("0 0 * * 0","UTC",stamp(2026,10,21))==cron_next("0 0 * * 7","UTC",stamp(2026,10,21)),"Sunday alias differs");
    check(cron_next("0 0 31 2 1","UTC",stamp(2026,2,1))==stamp(2026,2,2),"weekday OR did not make February expression satisfiable");
    rejects([] { cron_next("* * * * *","UTC",std::numeric_limits<std::int64_t>::max()); },"overflow timestamp accepted");
    rejects([] { cron_previous("* * * * *","UTC",std::numeric_limits<std::int64_t>::min()); },"underflow timestamp accepted");
}

void zones_and_daylight_saving() {
    check(cron_timezone("UTC")=="UTC","UTC normalization changed");
    check(!cron_timezone("local").empty(),"local zone was not resolved");
    for (const auto* zone : {"","not/a-real-time-zone","UTC+08:00"," UTC","UTC\n"})
        rejects([&] { validate_cron("* * * * *",zone); },"unknown or invalid zone accepted");
    check(cron_next("0 9 * * *","Asia/Shanghai",stamp(2026,10,21))==stamp(2026,10,21,1),"IANA UTC offset ignored");
    // New York skips 02:30 on March 8, 2026.
    check(cron_next("30 2 * * *","America/New_York",stamp(2026,3,8,6,59))==stamp(2026,3,9,6,30),"DST gap was moved instead of skipped");
    check(cron_previous("30 2 * * *","America/New_York",stamp(2026,3,8,8))==stamp(2026,3,7,7,30),"reverse DST gap search invented an occurrence");
    // The repeated 01:30 has two distinct UTC instants; neither is dropped.
    const auto first=stamp(2026,11,1,5,30),second=stamp(2026,11,1,6,30);
    check(cron_next("30 1 * * *","America/New_York",first-1)==first,"first repeated minute missing");
    check(cron_next("30 1 * * *","America/New_York",first)==second,"second repeated minute missing");
    check(cron_previous("30 1 * * *","America/New_York",second)==second,"inclusive second repeated minute missing");
    check(cron_previous("30 1 * * *","America/New_York",second-1)==first,"reverse fold selected wrong instant");
    check(cron_next("0,30 1 * * *","America/New_York",first)==stamp(2026,11,1,6),"fold ordering followed local fields instead of UTC");
    // Lord Howe's half-hour transition must use the actual offsets.
    check(cron_next("45 1 * * *","Australia/Lord_Howe",stamp(2026,4,4,14,45))==stamp(2026,4,4,15,15),"non-hour DST fold was assumed to be one hour");
}
}

void run_cron_tests() {
    syntax_and_steps(); calendar_and_ordering(); zones_and_daylight_saving();
}
