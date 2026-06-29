#pragma once

// ============================================================================
// Genesis::JJson — lightweight read/write JSON
//
// std::variant-based node tree. No external dependencies.
//
// Parse:
//   auto root = JJson::parse(R"({"port":"/dev/ttyUSB0","baud":115200})");
//   auto root = JJson::parseFile("config.json");          // throws on error
//   auto opt  = JJson::tryParse(src);                     // returns nullopt on error
//   std::string port = root["port"].str();
//   int baud = root["baud"].number<int>();
//
// Build + serialise:
//   JJson obj = JJson::object();
//   obj["port"] = JJson("/dev/ttyUSB0");
//   obj["baud"] = JJson(115200);
//   obj.dumpToFile("config.json");
//   std::string s = obj.dump(2);    // pretty-print with 2-space indent
// ============================================================================

#include <string>
#include <vector>
#include <optional>
#include <unordered_map>
#include <variant>
#include <stdexcept>
#include <sstream>
#include <fstream>
#include <iomanip>
#include <algorithm>
#include <cstdint>
#include <cstring>

namespace Genesis {

class JJson {
public:
    using Null   = std::monostate;
    using Array  = std::vector<JJson>;
    using Object = std::vector<std::pair<std::string, JJson>>;  // insertion-ordered

    JJson()               : m_val(Null{})           {}
    JJson(std::nullptr_t) : m_val(Null{})           {}
    JJson(bool v)         : m_val(v)                {}
    JJson(double v)       : m_val(v)                {}
    JJson(int v)          : m_val(static_cast<double>(v)) {}
    JJson(int64_t v)      : m_val(static_cast<double>(v)) {}
    JJson(const char* v)  : m_val(std::string(v))   {}
    JJson(std::string v)  : m_val(std::move(v))     {}
    JJson(Array v)        : m_val(std::move(v))     {}
    JJson(Object v)       : m_val(std::move(v))     {}

    static JJson object() { return JJson(Object{}); }
    static JJson array()  { return JJson(Array{}); }

    // ---- JType checks --------------------------------------------------------
    bool isNull()   const { return std::holds_alternative<Null>(m_val);        }
    bool isBool()   const { return std::holds_alternative<bool>(m_val);        }
    bool isNumber() const { return std::holds_alternative<double>(m_val);      }
    bool isString() const { return std::holds_alternative<std::string>(m_val); }
    bool isArray()  const { return std::holds_alternative<Array>(m_val);       }
    bool isObject() const { return std::holds_alternative<Object>(m_val);      }

    // ---- Value accessors ----------------------------------------------------
    bool boolean(bool def = false) const {
        if (isBool())   return std::get<bool>(m_val);
        if (isNumber()) return std::get<double>(m_val) != 0.0;
        return def;
    }

    double number(double def = 0.0) const {
        return isNumber() ? std::get<double>(m_val) : def;
    }

    template<typename T>
    T number(T def = T{}) const { return static_cast<T>(number(static_cast<double>(def))); }

    const std::string& str() const {
        static const std::string empty;
        return isString() ? std::get<std::string>(m_val) : empty;
    }

    // Safe array/object accessors — return empty ref on wrong type (like str() does).
    const Array& arr() const {
        static const Array empty;
        return isArray() ? std::get<Array>(m_val) : empty;
    }
    const Object& obj() const {
        static const Object empty;
        return isObject() ? std::get<Object>(m_val) : empty;
    }
    Array&  arr() { return std::get<Array>(m_val);  }
    Object& obj() { return std::get<Object>(m_val); }

    // ---- Object access ------------------------------------------------------
    const JJson& operator[](const std::string& key) const {
        static const JJson null_val;
        if (!isObject()) return null_val;
        for (const auto& [k, v] : obj()) if (k == key) return v;
        return null_val;
    }

    JJson& operator[](const std::string& key) {
        if (!isObject()) m_val = Object{};
        auto& o = obj();
        for (auto& [k, v] : o) if (k == key) return v;
        o.emplace_back(key, JJson{});
        return o.back().second;
    }

    bool contains(const std::string& key) const {
        if (!isObject()) return false;
        for (const auto& [k, v] : obj()) if (k == key) return true;
        return false;
    }

    void erase(const std::string& key) {
        if (!isObject()) return;
        auto& o = obj();
        o.erase(std::remove_if(o.begin(), o.end(),
                               [&](const auto& p){ return p.first == key; }),
                o.end());
    }

    // ---- Array access -------------------------------------------------------
    const JJson& operator[](size_t i) const {
        static const JJson null_val;
        return isArray() && i < arr().size() ? arr()[i] : null_val;
    }
    JJson& operator[](size_t i) { return arr()[i]; }

    void push(JJson v) {
        if (!isArray()) m_val = Array{};
        arr().push_back(std::move(v));
    }

    size_t size() const {
        if (isArray())  return arr().size();
        if (isObject()) return obj().size();
        return 0;
    }

    bool empty() const { return size() == 0; }

    // ---- Serialise ----------------------------------------------------------
    std::string dump(int indent = -1) const {
        std::ostringstream out;
        _dump(out, 0, indent);
        return out.str();
    }

    bool dumpToFile(const std::string& path, int indent = 2) const {
        std::ofstream f(path);
        if (!f) return false;
        f << dump(indent);
        return f.good();
    }

    // ---- Parse --------------------------------------------------------------

    // Throws std::runtime_error on malformed input.
    static JJson parse(const std::string& src) {
        size_t pos = 0;
        _skip(src, pos);
        JJson val = _parse(src, pos);
        return val;
    }

    // Returns nullopt on any parse error — no exception.
    static std::optional<JJson> tryParse(const std::string& src) {
        try { return parse(src); } catch (...) { return std::nullopt; }
    }

    // Reads entire file then parses. Throws on I/O or parse error.
    static JJson parseFile(const std::string& path) {
        std::ifstream f(path);
        if (!f) throw std::runtime_error("JJson::parseFile: cannot open '" + path + "'");
        std::string src((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());
        return parse(src);
    }

    // Returns nullopt on any error.
    static std::optional<JJson> tryParseFile(const std::string& path) {
        try { return parseFile(path); } catch (...) { return std::nullopt; }
    }

private:
    std::variant<Null, bool, double, std::string, Array, Object> m_val;

    // ---- Serialisation ------------------------------------------------------

    static void _escapeString(std::ostringstream& out, const std::string& s) {
        out << '"';
        for (unsigned char c : s) {
            switch (c) {
                case '"':  out << "\\\""; break;
                case '\\': out << "\\\\"; break;
                case '\n': out << "\\n";  break;
                case '\r': out << "\\r";  break;
                case '\t': out << "\\t";  break;
                default:
                    if (c < 0x20) {
                        out << "\\u" << std::hex << std::setw(4)
                            << std::setfill('0') << (int)c << std::dec;
                    } else {
                        out << c;
                    }
                    break;
            }
        }
        out << '"';
    }

    void _dump(std::ostringstream& out, int depth, int indent) const {
        std::string nl   = indent >= 0 ? "\n" : "";
        std::string pad  = indent >= 0 ? std::string(size_t((depth+1)*indent), ' ') : "";
        std::string cpad = indent >= 0 ? std::string(size_t( depth   *indent), ' ') : "";

        if      (isNull())   { out << "null"; }
        else if (isBool())   { out << (boolean() ? "true" : "false"); }
        else if (isNumber()) { out << std::setprecision(15) << number(); }
        else if (isString()) { _escapeString(out, str()); }
        else if (isArray()) {
            out << '[';
            const auto& a = arr();
            for (size_t i = 0; i < a.size(); ++i) {
                if (i) out << ',';
                out << nl << pad;
                a[i]._dump(out, depth+1, indent);
            }
            if (!a.empty()) out << nl << cpad;
            out << ']';
        }
        else if (isObject()) {
            out << '{';
            const auto& o = obj();
            for (size_t i = 0; i < o.size(); ++i) {
                if (i) out << ',';
                out << nl << pad;
                _escapeString(out, o[i].first);   // keys escaped too
                out << ':';
                if (indent >= 0) out << ' ';
                o[i].second._dump(out, depth+1, indent);
            }
            if (!o.empty()) out << nl << cpad;
            out << '}';
        }
    }

    // ---- Parsing ------------------------------------------------------------

    static void _skip(const std::string& s, size_t& p) {
        while (p < s.size() && (s[p]==' '||s[p]=='\t'||s[p]=='\n'||s[p]=='\r')) ++p;
    }

    static JJson _parse(const std::string& s, size_t& p) {
        _skip(s, p);
        if (p >= s.size()) throw std::runtime_error("JSON: unexpected end of input");
        char c = s[p];
        if (c == '"') return _parseString(s, p);
        if (c == '{') return _parseObject(s, p);
        if (c == '[') return _parseArray(s, p);
        if (c == 't') return _parseLiteral(s, p, "true",  JJson(true));
        if (c == 'f') return _parseLiteral(s, p, "false", JJson(false));
        if (c == 'n') return _parseLiteral(s, p, "null",  JJson(nullptr));
        if (c == '-' || (c >= '0' && c <= '9')) return _parseNumber(s, p);
        throw std::runtime_error(std::string("JSON: unexpected char '") + c + "'");
    }

    static JJson _parseLiteral(const std::string& s, size_t& p,
                               const char* expected, JJson result) {
        size_t len = std::strlen(expected);
        if (s.size() - p < len || s.compare(p, len, expected) != 0)
            throw std::runtime_error(std::string("JSON: expected '") + expected + "'");
        p += len;
        return result;
    }

    // Encode a Unicode code point as UTF-8 bytes appended to out.
    static void _encodeUtf8(std::string& out, uint32_t cp) {
        if (cp <= 0x7F) {
            out += static_cast<char>(cp);
        } else if (cp <= 0x7FF) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp <= 0xFFFF) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6)  & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }

    static uint32_t _parseHex4(const std::string& s, size_t& p) {
        if (p + 4 > s.size()) throw std::runtime_error("JSON: incomplete \\u escape");
        uint32_t val = 0;
        for (int i = 0; i < 4; ++i, ++p) {
            char c = s[p];
            val <<= 4;
            if      (c >= '0' && c <= '9') val |= static_cast<uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f') val |= static_cast<uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') val |= static_cast<uint32_t>(c - 'A' + 10);
            else throw std::runtime_error("JSON: invalid hex digit in \\u escape");
        }
        return val;
    }

    static std::string _parseRawString(const std::string& s, size_t& p) {
        ++p; // skip opening "
        std::string out;
        while (p < s.size() && s[p] != '"') {
            if (s[p] == '\\' && p+1 < s.size()) {
                ++p;
                switch (s[p]) {
                    case '"':  out += '"';  ++p; break;
                    case '\\': out += '\\'; ++p; break;
                    case '/':  out += '/';  ++p; break;
                    case 'n':  out += '\n'; ++p; break;
                    case 'r':  out += '\r'; ++p; break;
                    case 't':  out += '\t'; ++p; break;
                    case 'b':  out += '\b'; ++p; break;
                    case 'f':  out += '\f'; ++p; break;
                    case 'u': {
                        ++p;
                        uint32_t cp = _parseHex4(s, p);
                        // Handle UTF-16 surrogate pair: \uD800-\uDBFF followed by \uDC00-\uDFFF
                        if (cp >= 0xD800 && cp <= 0xDBFF &&
                            p + 1 < s.size() && s[p] == '\\' && s[p+1] == 'u') {
                            p += 2;
                            uint32_t low = _parseHex4(s, p);
                            if (low >= 0xDC00 && low <= 0xDFFF)
                                cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                        }
                        _encodeUtf8(out, cp);
                        break;
                    }
                    default:
                        throw std::runtime_error(
                            std::string("JSON: unknown escape '\\") + s[p] + "'");
                }
            } else {
                out += s[p++];
            }
        }
        if (p < s.size()) ++p; // skip closing "
        return out;
    }

    static JJson _parseString(const std::string& s, size_t& p) {
        return JJson(_parseRawString(s, p));
    }

    static JJson _parseNumber(const std::string& s, size_t& p) {
        size_t start = p;
        if (s[p] == '-') ++p;
        while (p < s.size() && s[p] >= '0' && s[p] <= '9') ++p;
        if (p < s.size() && s[p] == '.') {
            ++p;
            while (p < s.size() && s[p] >= '0' && s[p] <= '9') ++p;
        }
        if (p < s.size() && (s[p] == 'e' || s[p] == 'E')) {
            ++p;
            if (p < s.size() && (s[p] == '+' || s[p] == '-')) ++p;
            while (p < s.size() && s[p] >= '0' && s[p] <= '9') ++p;
        }
        return JJson(std::stod(s.substr(start, p - start)));
    }

    static JJson _parseObject(const std::string& s, size_t& p) {
        ++p; // skip {
        Object obj;
        _skip(s, p);
        if (p < s.size() && s[p] == '}') { ++p; return JJson(std::move(obj)); }
        while (p < s.size()) {
            _skip(s, p);
            if (p >= s.size() || s[p] != '"')
                throw std::runtime_error("JSON: expected string key in object");
            std::string key = _parseRawString(s, p);
            _skip(s, p);
            if (p >= s.size() || s[p] != ':')
                throw std::runtime_error("JSON: expected ':' after object key");
            ++p;
            _skip(s, p);
            JJson val = _parse(s, p);
            obj.emplace_back(std::move(key), std::move(val));
            _skip(s, p);
            if (p >= s.size()) throw std::runtime_error("JSON: unterminated object");
            if (s[p] == '}') { ++p; break; }
            if (s[p] != ',') throw std::runtime_error("JSON: expected ',' or '}' in object");
            ++p;
        }
        return JJson(std::move(obj));
    }

    static JJson _parseArray(const std::string& s, size_t& p) {
        ++p; // skip [
        Array arr;
        _skip(s, p);
        if (p < s.size() && s[p] == ']') { ++p; return JJson(std::move(arr)); }
        while (p < s.size()) {
            _skip(s, p);
            arr.push_back(_parse(s, p));
            _skip(s, p);
            if (p >= s.size()) throw std::runtime_error("JSON: unterminated array");
            if (s[p] == ']') { ++p; break; }
            if (s[p] != ',') throw std::runtime_error("JSON: expected ',' or ']' in array");
            ++p;
        }
        return JJson(std::move(arr));
    }
};

} // namespace Genesis
