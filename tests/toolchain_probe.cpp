// Compile and execute against the same C++ runtime as the application. Merely
// accepting -std=c++2c does not establish std::format or timezone support.
#include <chrono>
#include <format>
#include <iostream>
#include <stdexcept>
#include <nlohmann/json.hpp>
#include <sqlite3.h>
#include <curl/curl.h>
#include <openssl/crypto.h>

int main() {
    try {
        using namespace std::chrono;
        static_assert(__cplusplus > 202302L, "C++26 language mode is required");
        const auto formatted = std::format("{}:{:02}", "probe", 7);
        if (formatted != "probe:07") throw std::runtime_error("std::format failed");
        const auto* utc = locate_zone("UTC");
        const auto* shanghai = locate_zone("Asia/Shanghai");
        const auto* new_york = locate_zone("America/New_York");
        const auto winter = sys_days{year{2026}/January/15};
        const auto summer = sys_days{year{2026}/July/15};
        const bool offsets = utc->get_info(winter).offset == 0s &&
            shanghai->get_info(winter).offset == 8h &&
            new_york->get_info(winter).offset == -5h &&
            new_york->get_info(summer).offset == -4h;
        const auto gap = local_days{year{2026}/March/8} + 2h + 30min;
        const auto fold = local_days{year{2026}/November/1} + 1h + 30min;
        const bool dst = new_york->get_info(gap).result == local_info::nonexistent &&
            new_york->get_info(fold).result == local_info::ambiguous;
        if (!offsets || !dst) throw std::runtime_error("timezone or DST probe failed");
        nlohmann::json result{{"cxx26", true}, {"cplusplus", __cplusplus},
            {"format", formatted}, {"timezone_offsets", offsets}, {"timezone_dst", dst},
            {"tzdb_version", get_tzdb().version}, {"current_zone", current_zone()->name()},
            {"dependencies", {{"sqlite", sqlite3_libversion()}, {"curl", curl_version()},
                {"openssl", OpenSSL_version(OPENSSL_VERSION)},
                {"json", std::format("{}.{}.{}", NLOHMANN_JSON_VERSION_MAJOR,
                    NLOHMANN_JSON_VERSION_MINOR, NLOHMANN_JSON_VERSION_PATCH)}}}};
#ifdef _LIBCPP_VERSION
        result["libcpp_version"] = _LIBCPP_VERSION;
#endif
#ifdef __GLIBCXX__
        result["glibcxx_version"] = __GLIBCXX__;
#endif
        std::cout << result.dump() << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
