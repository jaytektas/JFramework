#pragma once

// ============================================================================
// Genesis::Json — lightweight read/write JSON (Step 10)
//
// Superior to Qt's QJsonDocument: no boxing via QJsonValue/QJsonObject layers,
// no byte array round-trip. std::variant-based node tree, parse from string,
// stringify to string. No external dependencies.
//
// Usage:
//   auto root = Json::parse(R"({"port":"/dev/ttyUSB0","baud":115200})");
//   std::string port = root["port"].str();
//   int baud = root["baud"].number<int>();
//
//   Json obj = Json::object();
//   obj["port"] = Json("/dev/ttyUSB0");
//   obj["baud"] = Json(115200.0);
//   std::string s = obj.dump(2);   // pretty-print with 2-space indent
// ============================================================================

#include <string>
#include <vector>
#include <unordered_map>
#include <variant>
#include <stdexcept>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <cstdint>

namespace Genesis {

class Json {
public:
    using Null   = std::monostate;
    using Array  = std::vector<Json>;
    using Object = std::vector<std::pair<std::string, Json>>;  // ordered

    Json()                    : m_val(Null{})           {}
    Json(std::nullptr_t)      : m_val(Null{})           {}
    Json(bool v)              : m_val(v)                {}
    Json(double v)            : m_val(v)                {}
    Json(int v)               : m_val(static_cast<double>(v)){}
    Json(int64_t v)           : m_val(static_cast<double>(v)){}
    Json(const char* v)       : m_val(std::string(v))   {}
    Json(std::string v)       : m_val(std::move(v))     {}
    Json(Array v)             : m_val(std::move(v))     {}
    Json(Object v)            : m_val(std::move(v))     {}

    static Json object() { return Json(Object{}); }
    static Json array()  { return Json(Array{}); }

    // ---- Type checks ---------------------------------------------------
    bool isNull()   const { return std::holds_alternative<Null>(m_val);         }
    bool isBool()   const { return std::holds_alternative<bool>(m_val);         }
    bool isNumber() const { return std::holds_alternative<double>(m_val);       }
    bool isString() const { return std::holds_alternative<std::string>(m_val);  }
    bool isArray()  const { return std::holds_alternative<Array>(m_val);        }
    bool isObject() const { return std::holds_alternative<Object>(m_val);       }

    // ---- Value accessors -----------------------------------------------
    bool              boolean(bool def = false) const {
        if (isBool()) return std::get<bool>(m_val);
        if (isNumber()) return std::get<double>(m_val) != 0.0;
        return def;
    }
    double            number(double def = 0.0) const {
        if (isNumber()) return std::get<double>(m_val);
        return def;
    }
    template<typename T>
    T number(T def = T{}) const { return static_cast<T>(number(static_cast<double>(def))); }

    const std::string& str() const {
        static const std::string empty;
        return isString() ? std::get<std::string>(m_val) : empty;
    }
    const Array&  arr() const { return std::get<Array>(m_val); }
    const Object& obj() const { return std::get<Object>(m_val); }
    Array&        arr()       { return std::get<Array>(m_val); }
    Object&       obj()       { return std::get<Object>(m_val); }

    // ---- Object access -------------------------------------------------
    const Json& operator[](const std::string& key) const {
        static const Json null_val;
        if (!isObject()) return null_val;
        for (const auto& [k, v] : obj()) if (k == key) return v;
        return null_val;
    }

    Json& operator[](const std::string& key) {
        if (!isObject()) m_val = Object{};
        auto& o = obj();
        for (auto& [k, v] : o) if (k == key) return v;
        o.emplace_back(key, Json{});
        return o.back().second;
    }

    bool contains(const std::string& key) const {
        if (!isObject()) return false;
        for (const auto& [k, v] : obj()) if (k == key) return true;
        return false;
    }

    // ---- Array access --------------------------------------------------
    const Json& operator[](size_t i) const {
        static const Json null_val;
        return isArray() && i < arr().size() ? arr()[i] : null_val;
    }
    Json& operator[](size_t i) { return arr()[i]; }
    void  push(Json v) { arr().push_back(std::move(v)); }
    size_t size() const {
        if (isArray())  return arr().size();
        if (isObject()) return obj().size();
        return 0;
    }

    // ---- Serialise -----------------------------------------------------
    std::string dump(int indent = -1) const {
        std::ostringstream out;
        _dump(out, 0, indent);
        return out.str();
    }

    // ---- Parse ---------------------------------------------------------
    static Json parse(const std::string& src) {
        size_t pos = 0;
        _skip(src, pos);
        Json val = _parse(src, pos);
        return val;
    }

private:
    std::variant<Null, bool, double, std::string, Array, Object> m_val;

    // Serialisation
    void _dump(std::ostringstream& out, int depth, int indent) const {
        std::string nl  = indent >= 0 ? "\n" : "";
        std::string pad = indent >= 0 ? std::string(size_t((depth+1)*indent), ' ') : "";
        std::string cpad= indent >= 0 ? std::string(size_t(depth*indent), ' ') : "";

        if (isNull())   { out << "null"; }
        else if (isBool())  { out << (boolean() ? "true" : "false"); }
        else if (isNumber()){ out << std::setprecision(15) << number(); }
        else if (isString()){
            out << '"';
            for (char c : str()) {
                if      (c == '"')  out << "\\\"";
                else if (c == '\\') out << "\\\\";
                else if (c == '\n') out << "\\n";
                else if (c == '\r') out << "\\r";
                else if (c == '\t') out << "\\t";
                else                out << c;
            }
            out << '"';
        }
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
                out << nl << pad << '"' << o[i].first << '"' << ':';
                if (indent >= 0) out << ' ';
                o[i].second._dump(out, depth+1, indent);
            }
            if (!o.empty()) out << nl << cpad;
            out << '}';
        }
    }

    // Parsing
    static void _skip(const std::string& s, size_t& p) {
        while (p < s.size() && (s[p]==' '||s[p]=='\t'||s[p]=='\n'||s[p]=='\r')) ++p;
    }

    static Json _parse(const std::string& s, size_t& p) {
        _skip(s, p);
        if (p >= s.size()) throw std::runtime_error("JSON: unexpected end");
        char c = s[p];
        if (c == '"') return _parseString(s, p);
        if (c == '{') return _parseObject(s, p);
        if (c == '[') return _parseArray(s, p);
        if (c == 't') { p += 4; return Json(true);  }
        if (c == 'f') { p += 5; return Json(false); }
        if (c == 'n') { p += 4; return Json(nullptr); }
        if (c == '-' || (c >= '0' && c <= '9')) return _parseNumber(s, p);
        throw std::runtime_error(std::string("JSON: unexpected char '") + c + "'");
    }

    static std::string _parseRawString(const std::string& s, size_t& p) {
        ++p; // skip opening "
        std::string out;
        while (p < s.size() && s[p] != '"') {
            if (s[p] == '\\' && p+1 < s.size()) {
                ++p;
                switch (s[p]) {
                    case '"':  out += '"'; break;
                    case '\\': out += '\\'; break;
                    case '/':  out += '/'; break;
                    case 'n':  out += '\n'; break;
                    case 'r':  out += '\r'; break;
                    case 't':  out += '\t'; break;
                    default:   out += s[p]; break;
                }
            } else { out += s[p]; }
            ++p;
        }
        if (p < s.size()) ++p; // skip closing "
        return out;
    }

    static Json _parseString(const std::string& s, size_t& p) {
        return Json(_parseRawString(s, p));
    }

    static Json _parseNumber(const std::string& s, size_t& p) {
        size_t start = p;
        if (s[p] == '-') ++p;
        while (p < s.size() && (s[p]>='0'&&s[p]<='9')) ++p;
        if (p < s.size() && s[p] == '.') { ++p; while (p < s.size() && (s[p]>='0'&&s[p]<='9')) ++p; }
        if (p < s.size() && (s[p]=='e'||s[p]=='E')) {
            ++p;
            if (p < s.size() && (s[p]=='+'||s[p]=='-')) ++p;
            while (p < s.size() && (s[p]>='0'&&s[p]<='9')) ++p;
        }
        return Json(std::stod(s.substr(start, p - start)));
    }

    static Json _parseObject(const std::string& s, size_t& p) {
        ++p; // skip {
        Object obj;
        _skip(s, p);
        if (p < s.size() && s[p] == '}') { ++p; return Json(std::move(obj)); }
        while (p < s.size()) {
            _skip(s, p);
            std::string key = _parseRawString(s, p);
            _skip(s, p);
            if (p < s.size() && s[p] == ':') ++p;
            _skip(s, p);
            Json val = _parse(s, p);
            obj.emplace_back(std::move(key), std::move(val));
            _skip(s, p);
            if (p >= s.size() || s[p] == '}') { ++p; break; }
            if (s[p] == ',') ++p;
        }
        return Json(std::move(obj));
    }

    static Json _parseArray(const std::string& s, size_t& p) {
        ++p; // skip [
        Array arr;
        _skip(s, p);
        if (p < s.size() && s[p] == ']') { ++p; return Json(std::move(arr)); }
        while (p < s.size()) {
            _skip(s, p);
            arr.push_back(_parse(s, p));
            _skip(s, p);
            if (p >= s.size() || s[p] == ']') { ++p; break; }
            if (s[p] == ',') ++p;
        }
        return Json(std::move(arr));
    }
};

} // namespace Genesis
