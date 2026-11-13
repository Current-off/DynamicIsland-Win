// DynamicIsland-Win / 极简 JSON 读写（C++17，无第三方依赖）
//
// 为什么自己写：规则配置文件是用户手改的，必须容错；而引入 nlohmann/json 会让
// 「clone 下来就能 cl / cmake 直接编」这件事变成需要包管理器。解析器只支持
// RFC 8259 的子集（对象/数组/字符串/数字/布尔/null），够用且可单测。
//
// 设计约束：
//   1. 解析失败不抛异常，返回 false + 错误位置，调用方回落到默认规则。
//   2. 数字一律存 double，取整数时由调用方转；配置文件里的都是小整数。
//   3. 序列化输出 2 空格缩进，保证人手改过的文件 diff 干净。
#ifndef DIW_SRC_CORE_MINI_JSON_HPP
#define DIW_SRC_CORE_MINI_JSON_HPP

#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

namespace diw {
namespace json {

struct Value {
    enum class Type { Null, Bool, Number, String, Array, Object };

    Type type = Type::Null;
    bool boolean = false;
    double number = 0.0;
    std::string text;
    std::vector<Value> items;                          // Array
    std::vector<std::pair<std::string, Value>> members;  // Object（保留写入顺序）

    bool IsNull() const { return type == Type::Null; }
    bool IsBool() const { return type == Type::Bool; }
    bool IsNumber() const { return type == Type::Number; }
    bool IsString() const { return type == Type::String; }
    bool IsArray() const { return type == Type::Array; }
    bool IsObject() const { return type == Type::Object; }

    const Value* Find(const std::string& key) const {
        if (type != Type::Object) return nullptr;
        for (const auto& kv : members) {
            if (kv.first == key) return &kv.second;
        }
        return nullptr;
    }

    // 取不到时返回静态 Null，避免调用点到处判空指针。
    const Value& At(const std::string& key) const {
        static const Value kNull;
        const Value* found = Find(key);
        return found == nullptr ? kNull : *found;
    }

    const Value& AtIndex(std::size_t index) const {
        static const Value kNull;
        if (type != Type::Array || index >= items.size()) return kNull;
        return items[index];
    }

    std::string AsString(const std::string& fallback = std::string()) const {
        return type == Type::String ? text : fallback;
    }

    bool AsBool(bool fallback) const { return type == Type::Bool ? boolean : fallback; }

    int AsInt(int fallback) const {
        if (type != Type::Number) return fallback;
        return static_cast<int>(number);
    }

    double AsDouble(double fallback) const { return type == Type::Number ? number : fallback; }
};

namespace detail {

inline void AppendUtf8(std::string& out, unsigned codepoint) {
    if (codepoint < 0x80u) {
        out.push_back(static_cast<char>(codepoint));
    } else if (codepoint < 0x800u) {
        out.push_back(static_cast<char>(0xC0u | (codepoint >> 6)));
        out.push_back(static_cast<char>(0x80u | (codepoint & 0x3Fu)));
    } else if (codepoint < 0x10000u) {
        out.push_back(static_cast<char>(0xE0u | (codepoint >> 12)));
        out.push_back(static_cast<char>(0x80u | ((codepoint >> 6) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (codepoint & 0x3Fu)));
    } else {
        out.push_back(static_cast<char>(0xF0u | (codepoint >> 18)));
        out.push_back(static_cast<char>(0x80u | ((codepoint >> 12) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | ((codepoint >> 6) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (codepoint & 0x3Fu)));
    }
}

class Parser {
public:
    explicit Parser(const std::string& text) : text_(text) {}

    bool Run(Value& out, std::string* error) {
        SkipWhitespace();
        if (!ParseValue(out)) {
            if (error != nullptr) {
                *error = message_ + " (offset " + std::to_string(offset_) + ")";
            }
            return false;
        }
        SkipWhitespace();
        if (offset_ != text_.size()) {
            if (error != nullptr) *error = "trailing characters after value";
            return false;
        }
        return true;
    }

private:
    bool Fail(const char* why) {
        if (message_.empty()) message_ = why;
        return false;
    }

    void SkipWhitespace() {
        while (offset_ < text_.size()) {
            const char c = text_[offset_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                ++offset_;
            } else {
                break;
            }
        }
    }

    bool Literal(const char* word) {
        const std::string target(word);
        if (text_.compare(offset_, target.size(), target) != 0) return false;
        offset_ += target.size();
        return true;
    }

    bool ParseValue(Value& out) {
        if (offset_ >= text_.size()) return Fail("unexpected end of input");
        switch (text_[offset_]) {
            case '{': return ParseObject(out);
            case '[': return ParseArray(out);
            case '"': {
                out.type = Value::Type::String;
                return ParseString(out.text);
            }
            case 't':
                if (Literal("true")) {
                    out.type = Value::Type::Bool;
                    out.boolean = true;
                    return true;
                }
                return Fail("bad literal");
            case 'f':
                if (Literal("false")) {
                    out.type = Value::Type::Bool;
                    out.boolean = false;
                    return true;
                }
                return Fail("bad literal");
            case 'n':
                if (Literal("null")) {
                    out.type = Value::Type::Null;
                    return true;
                }
                return Fail("bad literal");
            default: return ParseNumber(out);
        }
    }

    bool ParseNumber(Value& out) {
        const char* begin = text_.c_str() + offset_;
        char* end = nullptr;
        const double value = std::strtod(begin, &end);
        if (end == begin) return Fail("bad number");
        out.type = Value::Type::Number;
        out.number = value;
        offset_ += static_cast<std::size_t>(end - begin);
        return true;
    }

    bool ParseString(std::string& out) {
        if (offset_ >= text_.size() || text_[offset_] != '"') return Fail("expected string");
        ++offset_;
        out.clear();
        while (offset_ < text_.size()) {
            const char c = text_[offset_++];
            if (c == '"') return true;
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            if (offset_ >= text_.size()) return Fail("truncated escape");
            const char esc = text_[offset_++];
            switch (esc) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u': {
                    unsigned cp = 0;
                    if (!ParseHex4(cp)) return Fail("bad \\u escape");
                    // 代理对：高位 D800-DBFF 需要紧跟低位 DC00-DFFF。
                    if (cp >= 0xD800u && cp <= 0xDBFFu && offset_ + 1 < text_.size() &&
                        text_[offset_] == '\\' && text_[offset_ + 1] == 'u') {
                        offset_ += 2;
                        unsigned low = 0;
                        if (!ParseHex4(low)) return Fail("bad surrogate");
                        if (low >= 0xDC00u && low <= 0xDFFFu) {
                            cp = 0x10000u + ((cp - 0xD800u) << 10) + (low - 0xDC00u);
                        }
                    }
                    AppendUtf8(out, cp);
                    break;
                }
                default: return Fail("unknown escape");
            }
        }
        return Fail("unterminated string");
    }

    bool ParseHex4(unsigned& value) {
        if (offset_ + 4 > text_.size()) return false;
        value = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = text_[offset_++];
            unsigned digit = 0;
            if (c >= '0' && c <= '9') {
                digit = static_cast<unsigned>(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                digit = static_cast<unsigned>(c - 'a') + 10u;
            } else if (c >= 'A' && c <= 'F') {
                digit = static_cast<unsigned>(c - 'A') + 10u;
            } else {
                return false;
            }
            value = (value << 4) | digit;
        }
        return true;
    }

    bool ParseArray(Value& out) {
        out.type = Value::Type::Array;
        ++offset_;  // '['
        SkipWhitespace();
        if (offset_ < text_.size() && text_[offset_] == ']') {
            ++offset_;
            return true;
        }
        for (;;) {
            Value item;
            SkipWhitespace();
            if (!ParseValue(item)) return false;
            out.items.push_back(std::move(item));
            SkipWhitespace();
            if (offset_ >= text_.size()) return Fail("unterminated array");
            if (text_[offset_] == ',') {
                ++offset_;
                continue;
            }
            if (text_[offset_] == ']') {
                ++offset_;
                return true;
            }
            return Fail("expected ',' or ']'");
        }
    }

    bool ParseObject(Value& out) {
        out.type = Value::Type::Object;
        ++offset_;  // '{'
        SkipWhitespace();
        if (offset_ < text_.size() && text_[offset_] == '}') {
            ++offset_;
            return true;
        }
        for (;;) {
            SkipWhitespace();
            std::string key;
            if (!ParseString(key)) return false;
            SkipWhitespace();
            if (offset_ >= text_.size() || text_[offset_] != ':') return Fail("expected ':'");
            ++offset_;
            SkipWhitespace();
            Value value;
            if (!ParseValue(value)) return false;
            out.members.emplace_back(std::move(key), std::move(value));
            SkipWhitespace();
            if (offset_ >= text_.size()) return Fail("unterminated object");
            if (text_[offset_] == ',') {
                ++offset_;
                continue;
            }
            if (text_[offset_] == '}') {
                ++offset_;
                return true;
            }
            return Fail("expected ',' or '}'");
        }
    }

    const std::string& text_;
    std::size_t offset_ = 0;
    std::string message_;
};

}  // namespace detail

inline bool Parse(const std::string& text, Value& out, std::string* error = nullptr) {
    detail::Parser parser(text);
    return parser.Run(out, error);
}

inline std::string Escape(const std::string& in) {
    std::string out;
    out.reserve(in.size() + 8);
    for (const char c : in) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20u) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(c));
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

inline std::string FormatNumber(double value) {
    // 配置里的数字都是小整数，去掉小数点后无意义的零，避免写回时出现 3.000000。
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%.10g", value);
    return buf;
}

inline std::string Quote(const std::string& in) { return "\"" + Escape(in) + "\""; }

}  // namespace json
}  // namespace diw

#endif  // DIW_SRC_CORE_MINI_JSON_HPP
