#pragma once

#include "codestudio/store.hpp"
#include "../provider/protocol.hpp"
#include <algorithm>
#include <string_view>

namespace codestudio::durable_request_storage {
inline constexpr std::size_t history_limit = 10000;
inline constexpr std::size_t completion_limit = 300 * 1024;
inline constexpr const char* columns =
    "d.database_id,r.request_key,r.operation_id,r.session_id,r.method,r.state,"
    "r.task_id,r.completion,r.request_digest,r.workspace";

inline void bounded_text(const std::string& value, std::size_t maximum, const char* message) {
    if (value.empty() || value.size()>maximum || value.find('\0')!=std::string::npos)
        throw std::invalid_argument(message);
}
inline void hex(const std::string& value, std::size_t length, const char* message) {
    if (value.size()!=length || !std::ranges::all_of(value,[](char c) {
        return (c>='0' && c<='9') || (c>='a' && c<='f'); }))
        throw std::invalid_argument(message);
}
inline void key(const std::string& value) {
    bounded_text(value,128,"Request key must contain 1..128 bytes without NUL");
}
inline void method(const std::string& value) {
    if (value!="tasks/start" && value!="tasks/resume")
        throw std::invalid_argument("Unsupported durable request method");
}
inline void admission(const DurableRequest& value) {
    hex(value.database_id,32,"Invalid durable request database identity");
    key(value.request_key);
    hex(value.request_digest,64,"Invalid durable request digest");
    method(value.method);
    hex(value.operation_id,32,"Invalid durable operation identity");
    hex(value.session_id,32,"Invalid durable session identity");
    bounded_text(value.workspace,32768,"Invalid durable request workspace");
}

template<class Statement>
void match(const Statement& query, const std::string& digest, const std::string& method,
           const std::string& workspace) {
    if ((!digest.empty() && query.text(8)!=digest) ||
        (!method.empty() && query.text(4)!=method) ||
        (!workspace.empty() && query.text(9)!=workspace))
        throw RequestConflict("Request key is already bound to different parameters or authorization");
}

template<class Statement>
Json receipt(const Statement& query) {
    constexpr std::size_t maximum[] = {32,128,32,32,16,16,128,completion_limit,64,32768};
    for (int i=0;i<10;++i) if (query.bytes(i)>maximum[i])
        throw std::runtime_error("Durable request exceeds read limits");
    hex(query.text(0),32,"Invalid stored database identity");
    key(query.text(1));
    hex(query.text(2),32,"Invalid stored operation identity");
    hex(query.text(3),32,"Invalid stored session identity");
    method(query.text(4));
    hex(query.text(8),64,"Invalid stored request digest");
    bounded_text(query.text(9),32768,"Invalid stored request workspace");
    const auto state=query.text(5);
    if (state!="incomplete" && state!="finished")
        throw std::runtime_error("Invalid stored durable request state");
    Json completion=nullptr;
    if (!query.null(7)) {
        completion=protocol::strict_json(query.text(7));
        if (!completion.is_object()) throw std::runtime_error("Invalid stored durable completion");
    }
    if ((state=="finished")!=!completion.is_null())
        throw std::runtime_error("Durable request completion disagrees with its state");
    return {{"database_id",query.text(0)},{"request_key",query.text(1)},
        {"operation_id",query.text(2)},{"session_id",query.text(3)},{"method",query.text(4)},
        {"state",state},{"task_id",query.null(6) ? Json(nullptr) : Json(query.text(6))},
        {"completion",std::move(completion)}};
}
}
