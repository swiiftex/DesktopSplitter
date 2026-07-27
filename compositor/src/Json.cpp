#include "Json.h"

#include <cstdlib>
#include <cstdio>
#include <cmath>

namespace ds {
namespace json {

namespace {

const Value g_nullValue;

class Parser {
public:
    Parser(const std::string& text) : m_s(text), m_p(0) {}

    bool parse(Value& out, std::string& err) {
        skipBom();
        skipWs();
        if (!parseValue(out, 0)) { err = m_err; return false; }
        skipWs();
        if (m_p != m_s.size()) {
            // Trailing content is tolerated only if it is whitespace.
            fail("trailing characters after top-level value");
            err = m_err;
            return false;
        }
        return true;
    }

private:
    static const int kMaxDepth = 64;

    void skipBom() {
        if (m_s.size() >= 3 &&
            static_cast<unsigned char>(m_s[0]) == 0xEF &&
            static_cast<unsigned char>(m_s[1]) == 0xBB &&
            static_cast<unsigned char>(m_s[2]) == 0xBF) {
            m_p = 3;
        }
    }

    bool eof() const { return m_p >= m_s.size(); }
    char cur() const { return m_p < m_s.size() ? m_s[m_p] : '\0'; }

    void skipWs() {
        while (m_p < m_s.size()) {
            const char c = m_s[m_p];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
                ++m_p;
            } else if (c == '/' && m_p + 1 < m_s.size() && m_s[m_p + 1] == '/') {
                // Tolerate // line comments (common in hand-edited config files).
                while (m_p < m_s.size() && m_s[m_p] != '\n') ++m_p;
            } else if (c == '/' && m_p + 1 < m_s.size() && m_s[m_p + 1] == '*') {
                m_p += 2;
                while (m_p + 1 < m_s.size() && !(m_s[m_p] == '*' && m_s[m_p + 1] == '/')) ++m_p;
                m_p = (m_p + 2 <= m_s.size()) ? m_p + 2 : m_s.size();
            } else {
                break;
            }
        }
    }

    bool fail(const char* what) {
        char buf[256];
        _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%s at offset %zu", what, m_p);
        m_err = buf;
        return false;
    }

    bool parseValue(Value& out, int depth) {
        if (depth > kMaxDepth) return fail("nesting too deep");
        skipWs();
        if (eof()) return fail("unexpected end of input");
        switch (cur()) {
        case '{': return parseObject(out, depth);
        case '[': return parseArray(out, depth);
        case '"': {
            std::string s;
            if (!parseString(s)) return false;
            out.setString(std::move(s));
            return true;
        }
        case 't':
            if (m_s.compare(m_p, 4, "true") == 0) { m_p += 4; out.setBool(true); return true; }
            return fail("invalid literal");
        case 'f':
            if (m_s.compare(m_p, 5, "false") == 0) { m_p += 5; out.setBool(false); return true; }
            return fail("invalid literal");
        case 'n':
            if (m_s.compare(m_p, 4, "null") == 0) { m_p += 4; out.setNull(); return true; }
            return fail("invalid literal");
        default:
            return parseNumber(out);
        }
    }

    bool parseObject(Value& out, int depth) {
        ++m_p; // '{'
        out.setObject();
        skipWs();
        if (cur() == '}') { ++m_p; return true; }
        for (;;) {
            skipWs();
            if (cur() != '"') return fail("expected object key");
            std::string key;
            if (!parseString(key)) return false;
            skipWs();
            if (cur() != ':') return fail("expected ':'");
            ++m_p;
            Value v;
            if (!parseValue(v, depth + 1)) return false;
            out.addMember(std::move(key), std::move(v));
            skipWs();
            if (cur() == ',') { ++m_p; continue; }
            if (cur() == '}') { ++m_p; return true; }
            return fail("expected ',' or '}'");
        }
    }

    bool parseArray(Value& out, int depth) {
        ++m_p; // '['
        out.setArray();
        skipWs();
        if (cur() == ']') { ++m_p; return true; }
        for (;;) {
            Value v;
            if (!parseValue(v, depth + 1)) return false;
            out.pushBack(std::move(v));
            skipWs();
            if (cur() == ',') { ++m_p; continue; }
            if (cur() == ']') { ++m_p; return true; }
            return fail("expected ',' or ']'");
        }
    }

    static void appendUtf8(std::string& s, uint32_t cp) {
        if (cp < 0x80) {
            s.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            s.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            s.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            s.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            s.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            s.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            s.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }

    bool parseHex4(uint32_t& value) {
        if (m_p + 4 > m_s.size()) return fail("truncated \\u escape");
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = m_s[m_p + static_cast<size_t>(i)];
            v <<= 4;
            if (c >= '0' && c <= '9')      v |= static_cast<uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f') v |= static_cast<uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= static_cast<uint32_t>(c - 'A' + 10);
            else return fail("invalid hex digit in \\u escape");
        }
        m_p += 4;
        value = v;
        return true;
    }

    bool parseString(std::string& out) {
        ++m_p; // opening quote
        out.clear();
        for (;;) {
            if (eof()) return fail("unterminated string");
            const char c = m_s[m_p];
            if (c == '"') { ++m_p; return true; }
            if (c == '\\') {
                ++m_p;
                if (eof()) return fail("unterminated escape");
                const char e = m_s[m_p++];
                switch (e) {
                case '"':  out.push_back('"');  break;
                case '\\': out.push_back('\\'); break;
                case '/':  out.push_back('/');  break;
                case 'b':  out.push_back('\b'); break;
                case 'f':  out.push_back('\f'); break;
                case 'n':  out.push_back('\n'); break;
                case 'r':  out.push_back('\r'); break;
                case 't':  out.push_back('\t'); break;
                case 'u': {
                    uint32_t cp = 0;
                    if (!parseHex4(cp)) return false;
                    if (cp >= 0xD800 && cp <= 0xDBFF) {
                        // High surrogate: expect a low surrogate to follow.
                        if (m_p + 1 < m_s.size() && m_s[m_p] == '\\' && m_s[m_p + 1] == 'u') {
                            const size_t save = m_p;
                            m_p += 2;
                            uint32_t lo = 0;
                            if (!parseHex4(lo)) return false;
                            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                            } else {
                                m_p = save; // not a pair; emit replacement char
                                cp = 0xFFFD;
                            }
                        } else {
                            cp = 0xFFFD;
                        }
                    } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                        cp = 0xFFFD;
                    }
                    appendUtf8(out, cp);
                    break;
                }
                default:
                    return fail("invalid escape sequence");
                }
                continue;
            }
            out.push_back(c);
            ++m_p;
        }
    }

    bool parseNumber(Value& out) {
        const char* begin = m_s.c_str() + m_p;
        char* end = nullptr;
        const double v = std::strtod(begin, &end);
        if (end == begin) return fail("invalid number");
        m_p += static_cast<size_t>(end - begin);
        out.setNumber(v);
        return true;
    }

    const std::string& m_s;
    size_t             m_p;
    std::string        m_err;
};

} // namespace

int Value::asInt(int def) const {
    if (m_type != Type::Number) return def;
    return static_cast<int>(m_num < 0.0 ? m_num - 0.5 : m_num + 0.5);
}

unsigned int Value::asUInt(unsigned int def) const {
    if (m_type != Type::Number || m_num < 0.0) return def;
    return static_cast<unsigned int>(m_num + 0.5);
}

const Value& Value::at(size_t i) const {
    if (m_type != Type::Array || i >= m_arr.size()) return g_nullValue;
    return m_arr[i];
}

const Value* Value::find(const char* key) const {
    if (m_type != Type::Object || key == nullptr) return nullptr;
    for (const auto& kv : m_obj) {
        if (kv.first == key) return &kv.second;
    }
    return nullptr;
}

int Value::intMember(const char* key, int def) const {
    const Value* v = find(key);
    return v ? v->asInt(def) : def;
}

unsigned int Value::uintMember(const char* key, unsigned int def) const {
    const Value* v = find(key);
    return v ? v->asUInt(def) : def;
}

std::string Value::stringMember(const char* key, const char* def) const {
    const Value* v = find(key);
    if (v && v->isString()) return v->asString();
    return std::string(def ? def : "");
}

bool Parse(const std::string& text, Value& out, std::string& error) {
    Parser p(text);
    return p.parse(out, error);
}

} // namespace json
} // namespace ds
