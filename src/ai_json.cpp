// ai_json.cpp
#include "ai_json.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace rshell::ai {

namespace {

const std::string& emptyString() {
    static const std::string value;
    return value;
}


// 递归下降解析器。任何失败都记录首次错误的偏移，后续不再覆盖。
class Parser {
public:
    Parser(std::string_view text) : text_(text) {}

    bool run(Json& out) {
        skipSpace();
        if (!parseValue(out, 0)) return false;
        skipSpace();
        if (pos_ != text_.size()) {
            fail("解析结束后还有多余内容");
            return false;
        }
        return true;
    }

    const std::string& error() const { return error_; }

private:
    // 嵌套过深时直接失败，避免恶意响应把栈打爆
    static constexpr int kMaxDepth = 128;

    bool fail(const char* what) {
        if (error_.empty()) {
            error_ = std::string(what) + "（偏移 " + std::to_string(pos_) + "）";
        }
        return false;
    }

    bool eof() const { return pos_ >= text_.size(); }
    char peek() const { return pos_ < text_.size() ? text_[pos_] : '\0'; }

    void skipSpace() {
        while (pos_ < text_.size()) {
            const char c = text_[pos_];
            if (c != ' ' && c != '\t' && c != '\n' && c != '\r') break;
            ++pos_;
        }
    }

    bool literal(std::string_view word) {
        if (text_.substr(pos_, word.size()) != word) return false;
        pos_ += word.size();
        return true;
    }

    bool parseValue(Json& out, int depth) {
        if (depth > kMaxDepth) return fail("JSON 嵌套过深");
        if (eof()) return fail("意外的结尾");

        switch (peek()) {
            case '{': return parseObject(out, depth);
            case '[': return parseArray(out, depth);
            case '"': {
                std::string value;
                if (!parseString(value)) return false;
                out = Json::string(std::move(value));
                return true;
            }
            case 't':
                if (!literal("true")) return fail("无法识别的字面量");
                out = Json::boolean(true);
                return true;
            case 'f':
                if (!literal("false")) return fail("无法识别的字面量");
                out = Json::boolean(false);
                return true;
            case 'n':
                if (!literal("null")) return fail("无法识别的字面量");
                out = Json::null();
                return true;
            default: return parseNumber(out);
        }
    }

    bool parseObject(Json& out, int depth) {
        ++pos_;  // '{'
        out = Json::object();
        skipSpace();
        if (peek() == '}') {
            ++pos_;
            return true;
        }
        for (;;) {
            skipSpace();
            std::string key;
            if (!parseString(key)) return false;
            skipSpace();
            if (peek() != ':') return fail("对象成员缺少冒号");
            ++pos_;
            skipSpace();
            Json value;
            if (!parseValue(value, depth + 1)) return false;
            out[key] = std::move(value);
            skipSpace();
            if (peek() == ',') {
                ++pos_;
                continue;
            }
            if (peek() == '}') {
                ++pos_;
                return true;
            }
            return fail("对象成员之间缺少逗号或右花括号");
        }
    }

    bool parseArray(Json& out, int depth) {
        ++pos_;  // '['
        out = Json::array();
        skipSpace();
        if (peek() == ']') {
            ++pos_;
            return true;
        }
        for (;;) {
            skipSpace();
            Json value;
            if (!parseValue(value, depth + 1)) return false;
            out.push(std::move(value));
            skipSpace();
            if (peek() == ',') {
                ++pos_;
                continue;
            }
            if (peek() == ']') {
                ++pos_;
                return true;
            }
            return fail("数组元素之间缺少逗号或右方括号");
        }
    }

    // \u 转义：把 UTF-16 代理对合成一个码点再写 UTF-8
    static void appendUtf8(std::string& out, unsigned codepoint) {
        if (codepoint <= 0x7F) {
            out += static_cast<char>(codepoint);
        } else if (codepoint <= 0x7FF) {
            out += static_cast<char>(0xC0 | (codepoint >> 6));
            out += static_cast<char>(0x80 | (codepoint & 0x3F));
        } else if (codepoint <= 0xFFFF) {
            out += static_cast<char>(0xE0 | (codepoint >> 12));
            out += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (codepoint & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (codepoint >> 18));
            out += static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (codepoint & 0x3F));
        }
    }

    bool parseHex4(unsigned& value) {
        if (pos_ + 4 > text_.size()) return fail("\\u 转义不完整");
        value = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = text_[pos_++];
            value <<= 4;
            if (c >= '0' && c <= '9') {
                value |= static_cast<unsigned>(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                value |= static_cast<unsigned>(c - 'a' + 10);
            } else if (c >= 'A' && c <= 'F') {
                value |= static_cast<unsigned>(c - 'A' + 10);
            } else {
                return fail("\\u 转义含非法字符");
            }
        }
        return true;
    }

    bool parseString(std::string& out) {
        if (peek() != '"') return fail("期望字符串");
        ++pos_;
        out.clear();
        while (pos_ < text_.size()) {
            const char c = text_[pos_++];
            if (c == '"') return true;
            if (c != '\\') {
                out += c;
                continue;
            }
            if (eof()) return fail("字符串转义不完整");
            switch (text_[pos_++]) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    unsigned high = 0;
                    if (!parseHex4(high)) return false;
                    if (high >= 0xD800 && high <= 0xDBFF && pos_ + 6 <= text_.size() &&
                        text_[pos_] == '\\' && text_[pos_ + 1] == 'u') {
                        pos_ += 2;
                        unsigned low = 0;
                        if (!parseHex4(low)) return false;
                        if (low < 0xDC00 || low > 0xDFFF) return fail("UTF-16 代理对不合法");
                        high = 0x10000u + ((high - 0xD800u) << 10) + (low - 0xDC00u);
                    }
                    appendUtf8(out, high);
                    break;
                }
                default: return fail("无法识别的字符串转义");
            }
        }
        return fail("字符串未闭合");
    }

    bool parseNumber(Json& out) {
        const std::size_t start = pos_;
        if (peek() == '-' || peek() == '+') ++pos_;
        bool digits = false;
        while (pos_ < text_.size()) {
            const char c = text_[pos_];
            if (c >= '0' && c <= '9') {
                digits = true;
                ++pos_;
            } else if (c == '.' || c == 'e' || c == 'E' || c == '-' || c == '+') {
                ++pos_;
            } else {
                break;
            }
        }
        if (!digits) return fail("期望数字");
        const std::string literalText(text_.substr(start, pos_ - start));
        char* end = nullptr;
        const double value = std::strtod(literalText.c_str(), &end);
        if (end == literalText.c_str() || *end != '\0') return fail("数字格式不合法");
        out = Json::number(value);
        return true;
    }

    std::string_view text_;
    std::size_t pos_ = 0;
    std::string error_;
};

}  // namespace

void dumpString(std::string& out, std::string_view text) {
    out += '"';
    for (const unsigned char c : text) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(c));
                    out += buf;
                } else {
                    out += static_cast<char>(c);
                }
                break;
        }
    }
    out += '"';
}

Json Json::boolean(bool value) {
    Json j;
    j.type_ = Type::Bool;
    j.bool_ = value;
    return j;
}

Json Json::number(double value) {
    Json j;
    j.type_ = Type::Number;
    j.num_ = value;
    return j;
}

Json Json::string(std::string value) {
    Json j;
    j.type_ = Type::String;
    j.str_ = std::move(value);
    return j;
}

Json Json::array() {
    Json j;
    j.type_ = Type::Array;
    return j;
}

Json Json::object() {
    Json j;
    j.type_ = Type::Object;
    return j;
}

bool Json::asBool(bool fallback) const {
    return type_ == Type::Bool ? bool_ : fallback;
}

double Json::asNumber(double fallback) const {
    return type_ == Type::Number ? num_ : fallback;
}

int Json::asInt(int fallback) const {
    if (type_ != Type::Number) return fallback;
    return static_cast<int>(num_);
}

const std::string& Json::asString() const {
    return type_ == Type::String ? str_ : emptyString();
}

const Json* Json::find(std::string_view key) const {
    if (type_ != Type::Object) return nullptr;
    for (const auto& member : obj_) {
        if (member.first == key) return &member.second;
    }
    return nullptr;
}

Json& Json::operator[](std::string key) {
    if (type_ != Type::Object) {
        // 对非对象调用时退化为空对象，避免调用方崩溃
        *this = object();
    }
    for (auto& member : obj_) {
        if (member.first == key) return member.second;
    }
    obj_.emplace_back(std::move(key), Json());
    return obj_.back().second;
}

Json& Json::push(Json value) {
    if (type_ != Type::Array) *this = array();
    arr_.push_back(std::move(value));
    return arr_.back();
}

std::size_t Json::size() const {
    switch (type_) {
        case Type::Array: return arr_.size();
        case Type::Object: return obj_.size();
        case Type::String: return str_.size();
        default: return 0;
    }
}

bool Json::empty() const { return size() == 0; }

std::string Json::dump(int indent) const {
    std::string out;
    dumpTo(out, indent, 0);
    return out;
}

void Json::dumpTo(std::string& out, int indent, int depth) const {
    const auto newline = [&](int level) {
        if (indent < 0) return;
        out += '\n';
        out.append(static_cast<std::size_t>(indent * level), ' ');
    };

    switch (type_) {
        case Type::Null: out += "null"; break;
        case Type::Bool: out += bool_ ? "true" : "false"; break;
        case Type::Number: {
            char buf[40];
            // 15 位有效数字足够 API 参数使用，也不会把 0.7 输出成 0.69999999999999996
            std::snprintf(buf, sizeof(buf), "%.15g", num_);
            out += buf;
            break;
        }
        case Type::String: dumpString(out, str_); break;
        case Type::Array:
            out += '[';
            for (std::size_t i = 0; i < arr_.size(); ++i) {
                if (i) out += ',';
                newline(depth + 1);
                arr_[i].dumpTo(out, indent, depth + 1);
            }
            if (!arr_.empty()) newline(depth);
            out += ']';
            break;
        case Type::Object:
            out += '{';
            for (std::size_t i = 0; i < obj_.size(); ++i) {
                if (i) out += ',';
                newline(depth + 1);
                dumpString(out, obj_[i].first);
                out += ':';
                if (indent >= 0) out += ' ';
                obj_[i].second.dumpTo(out, indent, depth + 1);
            }
            if (!obj_.empty()) newline(depth);
            out += '}';
            break;
    }
}

bool Json::parse(std::string_view text, Json& out, std::string* error) {
    Parser parser(text);
    Json parsed;
    if (!parser.run(parsed)) {
        if (error) *error = parser.error();
        return false;
    }
    out = std::move(parsed);
    if (error) error->clear();
    return true;
}

}  // namespace rshell::ai
