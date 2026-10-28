// ai_json.h - AI 功能用的极简 JSON：解析 + 序列化，不引入第三方库
//
// 只覆盖 OpenAI 请求/响应需要的子集：对象、数组、字符串、数字、布尔、null。
// 不做原地修改、不做流式解析；体积与可读性优先，错误信息带字节偏移。
#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace rshell::ai {

class Json {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    Json() = default;

    static Json null() { return Json(); }
    static Json boolean(bool value);
    static Json number(double value);
    static Json string(std::string value);
    static Json array();
    static Json object();

    Type type() const { return type_; }
    bool isNull() const { return type_ == Type::Null; }
    bool isBool() const { return type_ == Type::Bool; }
    bool isNumber() const { return type_ == Type::Number; }
    bool isString() const { return type_ == Type::String; }
    bool isArray() const { return type_ == Type::Array; }
    bool isObject() const { return type_ == Type::Object; }

    bool asBool(bool fallback = false) const;
    double asNumber(double fallback = 0) const;
    int asInt(int fallback = 0) const;
    // 类型不符时返回空串（引用指向静态对象，可安全返回）
    const std::string& asString() const;

    const std::vector<Json>& items() const { return arr_; }
    const std::vector<std::pair<std::string, Json>>& members() const { return obj_; }

    // 对象取值：不存在返回 nullptr
    const Json* find(std::string_view key) const;
    // 对象写入：不存在则插入；已存在则覆盖
    Json& operator[](std::string key);
    // 数组追加
    Json& push(Json value);

    std::size_t size() const;
    bool empty() const;

    // indent < 0 时输出紧凑格式；>= 0 时按该缩进宽度换行（调试用）
    std::string dump(int indent = -1) const;

    // 解析失败时 out 保持不变；error 非空时写入原因（含偏移）
    static bool parse(std::string_view text, Json& out, std::string* error = nullptr);

private:
    void dumpTo(std::string& out, int indent, int depth) const;

    Type type_ = Type::Null;
    bool bool_ = false;
    double num_ = 0.0;
    std::string str_;
    std::vector<Json> arr_;
    std::vector<std::pair<std::string, Json>> obj_;
};

// 按 JSON 规则转义并写出带引号的字符串（不含首尾控制字符）
void dumpString(std::string& out, std::string_view text);

}  // namespace rshell::ai
