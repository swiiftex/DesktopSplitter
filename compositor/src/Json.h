// Json.h - minimal dependency-free JSON parser for dscomp.
// Hand-written recursive-descent parser; no external libraries.
#pragma once

#include <string>
#include <vector>
#include <utility>
#include <cstdint>

namespace ds {
namespace json {

class Value {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    Value() = default;

    Type type() const { return m_type; }
    bool isNull()   const { return m_type == Type::Null; }
    bool isBool()   const { return m_type == Type::Bool; }
    bool isNumber() const { return m_type == Type::Number; }
    bool isString() const { return m_type == Type::String; }
    bool isArray()  const { return m_type == Type::Array; }
    bool isObject() const { return m_type == Type::Object; }

    bool               asBool(bool def = false) const { return m_type == Type::Bool ? m_bool : def; }
    double             asDouble(double def = 0.0) const { return m_type == Type::Number ? m_num : def; }
    int                asInt(int def = 0) const;
    unsigned int       asUInt(unsigned int def = 0) const;
    const std::string& asString() const { return m_str; }

    // Array access (empty / null-object when the type does not match).
    size_t       size() const { return m_type == Type::Array ? m_arr.size() : 0; }
    const Value& at(size_t i) const;

    // Object access. Returns nullptr when absent or not an object.
    const Value* find(const char* key) const;

    // Convenience typed lookups with defaults.
    int          intMember(const char* key, int def = 0) const;
    unsigned int uintMember(const char* key, unsigned int def = 0) const;
    std::string  stringMember(const char* key, const char* def = "") const;

    // Building (used by the parser).
    void setNull()   { reset(); m_type = Type::Null; }
    void setBool(bool v) { reset(); m_type = Type::Bool; m_bool = v; }
    void setNumber(double v) { reset(); m_type = Type::Number; m_num = v; }
    void setString(std::string v) { reset(); m_type = Type::String; m_str = std::move(v); }
    void setArray()  { reset(); m_type = Type::Array; }
    void setObject() { reset(); m_type = Type::Object; }
    void pushBack(Value v) { m_arr.push_back(std::move(v)); }
    void addMember(std::string k, Value v) { m_obj.emplace_back(std::move(k), std::move(v)); }

private:
    void reset() {
        m_bool = false;
        m_num = 0.0;
        m_str.clear();
        m_arr.clear();
        m_obj.clear();
    }

    Type        m_type = Type::Null;
    bool        m_bool = false;
    double      m_num = 0.0;
    std::string m_str;
    // std::vector supports incomplete element types (C++17 [vector.overview]).
    std::vector<Value>                            m_arr;
    std::vector<std::pair<std::string, Value>>    m_obj;
};

// Parses UTF-8 JSON text. Returns false and fills 'error' on failure.
bool Parse(const std::string& text, Value& out, std::string& error);

} // namespace json
} // namespace ds
