/**
 * 最小 JSON 解析器（仅支持烧录配置所需子集：object/array/string/number/bool/null）。
 * 避免引入第三方依赖；schema 由 BurnService.ets 组装，字段可控。
 */
#ifndef OSC_MINI_JSON_H
#define OSC_MINI_JSON_H

#include <string>
#include <memory>
#include <map>
#include <vector>

namespace osc {

class JsonValue {
public:
    enum class Type { NUL, BOOL, NUMBER, STRING, ARRAY, OBJECT };

    Type type = Type::NUL;
    bool boolVal = false;
    double numVal = 0;
    std::string strVal;
    std::vector<JsonValue> arr;
    std::map<std::string, JsonValue> obj;

    bool IsObject() const { return type == Type::OBJECT; }
    bool IsArray() const { return type == Type::ARRAY; }
    bool IsString() const { return type == Type::STRING; }
    bool IsNumber() const { return type == Type::NUMBER; }
    bool IsBool() const { return type == Type::BOOL; }

    const JsonValue *Get(const std::string &key) const
    {
        auto it = obj.find(key);
        if (it == obj.end()) {
            return nullptr;
        }
        return &it->second;
    }
    double Num(const std::string &key, double def) const
    {
        const JsonValue *v = Get(key);
        return (v != nullptr && v->IsNumber()) ? v->numVal : def;
    }
    int Int(const std::string &key, int def) const
    {
        return static_cast<int>(Num(key, def));
    }
    bool Bool(const std::string &key, bool def) const
    {
        const JsonValue *v = Get(key);
        return (v != nullptr && v->IsBool()) ? v->boolVal : def;
    }
    std::string Str(const std::string &key, const std::string &def) const
    {
        const JsonValue *v = Get(key);
        return (v != nullptr && v->IsString()) ? v->strVal : def;
    }
};

class MiniJson {
public:
    static bool Parse(const std::string &text, JsonValue &out);

private:
    static bool ParseValue(const std::string &s, size_t &pos, JsonValue &out);
    static void SkipSpace(const std::string &s, size_t &pos);
    static bool ParseString(const std::string &s, size_t &pos, std::string &out);
};

} // namespace osc

#endif // OSC_MINI_JSON_H
