// balance_island 核心域：IPC 协议解码（C++17，无平台依赖）
//
// 与 docs/protocol.md 的 v1 协议一一对应，也与 Python 侧的可执行规约
// bridge/protocol.py::decode 保持同一套判定顺序：
//   缺字段 / 类型错误 / 布尔冒充整数 / 负 seq 与 ts / 非对象 activity /
//   多行输入 / 非法 JSON —— 一律拒绝，并给出可读原因。
// 未知字段被忽略，这是前向兼容的约定（老解码器遇到新字段不能崩）。
//
// 放在 core 而不是 win32，是为了让"协议对不对"能被纯逻辑测试覆盖，
// 而"管道怎么读写"才属于采集层。
#ifndef BI_SRC_CORE_IPC_PROTOCOL_HPP
#define BI_SRC_CORE_IPC_PROTOCOL_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "Types.hpp"
#include "mini_json.hpp"

namespace bi {
namespace ipc {

constexpr int kProtocolVersion = 1;
// 单行上限：一条活动本来就只装标题与一小段 content，8KB 已经非常宽松。
// 设上限的意义是不让一个写错的外部程序把服务端内存吃光。
constexpr std::size_t kMaxLineBytes = 8 * 1024;
constexpr std::size_t kMaxIdBytes = 128;
constexpr std::size_t kMaxKindBytes = 64;
constexpr std::size_t kMaxTitleBytes = 512;
constexpr std::size_t kMaxContentBytes = 4096;

struct Request {
    enum class Type { Invalid, Upsert, End };
    Type type = Type::Invalid;
    Activity activity;  // End 时只用到 id
};

namespace detail {

inline bool IsIntegral(double v) { return v == static_cast<double>(static_cast<std::int64_t>(v)); }

// 把解析出来的 content 对象写回 JSON 文本 —— Activity.content_json 的类型是
// 字符串，渲染层再自己解析。写回而不是原样切片，是因为原文里可能有空白、
// 键序也可能变，统一成紧凑格式便于日志与断言比对。
inline void Write(const json::Value& v, std::string& out) {
    switch (v.type) {
        case json::Value::Type::Null:
            out += "null";
            return;
        case json::Value::Type::Bool:
            out += v.boolean ? "true" : "false";
            return;
        case json::Value::Type::Number:
            out += json::FormatNumber(v.number);
            return;
        case json::Value::Type::String:
            out += json::Quote(v.text);
            return;
        case json::Value::Type::Array:
            out += '[';
            for (std::size_t i = 0; i < v.items.size(); ++i) {
                if (i != 0) out += ',';
                Write(v.items[i], out);
            }
            out += ']';
            return;
        case json::Value::Type::Object:
            out += '{';
            for (std::size_t i = 0; i < v.members.size(); ++i) {
                if (i != 0) out += ',';
                out += json::Quote(v.members[i].first);
                out += ':';
                Write(v.members[i].second, out);
            }
            out += '}';
            return;
    }
}

inline bool Fail(std::string* error, const char* why) {
    if (error != nullptr) *error = why;
    return false;
}

}  // namespace detail

// 严格解码一行。成功时 out->type 为 Upsert 或 End；失败返回 false 并写 error。
inline bool DecodeLine(const std::string& raw_line, Request* out, std::string* error) {
    if (out == nullptr) return false;
    out->type = Request::Type::Invalid;
    if (raw_line.empty()) return detail::Fail(error, "empty line");
    // 容忍行尾的 CRLF：WriteLine 在 Windows 上默认就是 \r\n，而绝大多数语言
    // 的"写一行"都带它。拒绝 CRLF 会让这套协议对调用方莫名苛刻。
    // 注意只剥**行尾**那一个，内嵌换行仍然要拒绝。
    std::string line = raw_line;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.size() > kMaxLineBytes) return detail::Fail(error, "line too long");
    // 多行输入必须拒绝：一条消息一行是这套协议的基本假设，
    // 允许内嵌换行会让"按行切分"这个前提失效。
    if (line.find('\n') != std::string::npos || line.find('\r') != std::string::npos) {
        return detail::Fail(error, "multi-line input");
    }

    json::Value root;
    std::string parse_error;
    if (!json::Parse(line, root, &parse_error)) {
        if (error != nullptr) *error = "invalid json: " + parse_error;
        return false;
    }
    if (!root.IsObject()) return detail::Fail(error, "root is not an object");

    const json::Value& version = root.At("protocolVersion");
    if (!version.IsNumber() || !detail::IsIntegral(version.number) || version.number < 1) {
        return detail::Fail(error, "protocolVersion must be a positive integer");
    }
    if (static_cast<int>(version.number) > kProtocolVersion) {
        return detail::Fail(error, "protocolVersion newer than supported");
    }
    const json::Value& seq = root.At("seq");
    if (!seq.IsNumber() || !detail::IsIntegral(seq.number) || seq.number < 0) {
        return detail::Fail(error, "seq must be a non-negative integer");
    }
    const json::Value& ts = root.At("ts");
    if (!ts.IsNumber() || !detail::IsIntegral(ts.number) || ts.number < 0) {
        return detail::Fail(error, "ts must be a non-negative integer");
    }

    const std::string type = root.At("type").AsString();
    if (type.empty()) return detail::Fail(error, "type must be a non-empty string");

    const json::Value& activity = root.At("activity");
    if (!activity.IsObject()) return detail::Fail(error, "activity must be an object");

    const std::string id = activity.At("id").AsString();
    if (id.empty()) return detail::Fail(error, "activity.id must be a non-empty string");
    if (id.size() > kMaxIdBytes) return detail::Fail(error, "activity.id too long");
    out->activity.id = id;

    if (type == "activity.end") {
        out->type = Request::Type::End;
        return true;
    }
    if (type != "activity.upsert") return detail::Fail(error, "unsupported type");

    const std::string kind = activity.At("kind").AsString();
    if (kind.empty()) return detail::Fail(error, "activity.kind must be a non-empty string");
    if (kind.size() > kMaxKindBytes) return detail::Fail(error, "activity.kind too long");

    const json::Value& title = activity.At("title");
    if (!title.IsString()) return detail::Fail(error, "activity.title must be a string");
    if (title.text.size() > kMaxTitleBytes) return detail::Fail(error, "activity.title too long");

    Activity a;
    a.id = id;
    a.kind = kind;
    a.title = title.text;

    const json::Value& score = activity.At("relevanceScore");
    if (!score.IsNull()) {
        if (!score.IsNumber()) return detail::Fail(error, "relevanceScore must be a number");
        a.relevance_score = score.number;
    }
    const json::Value& created = activity.At("createdAtMs");
    if (!created.IsNull()) {
        if (!created.IsNumber() || !detail::IsIntegral(created.number) || created.number < 0) {
            return detail::Fail(error, "createdAtMs must be a non-negative integer");
        }
        a.created_at_ms = static_cast<std::int64_t>(created.number);
    }
    const json::Value& stale = activity.At("staleAtMs");
    if (!stale.IsNull()) {
        if (!stale.IsNumber() || !detail::IsIntegral(stale.number) || stale.number < 0) {
            return detail::Fail(error, "staleAtMs must be a non-negative integer");
        }
        a.stale_at_ms = static_cast<std::int64_t>(stale.number);
    }
    const json::Value& content = activity.At("content");
    if (!content.IsNull()) {
        if (!content.IsObject()) return detail::Fail(error, "activity.content must be an object");
        std::string text;
        detail::Write(content, text);
        if (text.size() > kMaxContentBytes) return detail::Fail(error, "activity.content too large");
        a.content_json = text;
    } else {
        a.content_json = "{}";
    }

    out->activity = a;
    out->type = Request::Type::Upsert;
    return true;
}

}  // namespace ipc
}  // namespace bi

#endif  // BI_SRC_CORE_IPC_PROTOCOL_HPP
