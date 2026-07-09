#pragma once

// ============================================================================
// jf::JVariant — the framework's universal value type.
//
// A small, dependency-free tagged union (the modern-C++ answer to QVariant).
// It is the lingua franca that lets generic code — model/view, settings,
// serialization, property binding — move typed data around without
// knowing the concrete type at compile time.
//
//   JVariant v = 42;             // Int
//   JVariant d = 3.14;           // Double
//   JVariant s = "torque";       // String
//   JVariant b = true;           // Bool
//   JVariant l = JVariant::list({1, 2, 3});
//   JVariant m = JVariant::map({{"port", "/dev/ttyUSB0"}, {"baud", 115200}});
//
//   int    i = v.toInt();        // coercing accessors (string<->number<->bool)
//   double x = v.toDouble();
//   std::string str = v.toString();
//   auto opt = v.tryValue<int>();        // std::optional<int>, no coercion guess
//
// Custom (opaque) types ride along via std::any:
//   JVariant c = JVariant::custom(MyThing{...});
//   if (auto* t = c.customPtr<MyThing>()) { ... }
//
// Leaf module: includes only the standard library. The JJson bridge lives in
// the opt-in <genesis/core/VariantJson.h>; reflection in <.../MetaObject.h>.
// ============================================================================

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include <utility>
#include <variant>
#include <any>
#include <optional>
#include <sstream>
#include <type_traits>

inline namespace jf {

class JVariant;

using VariantList = std::vector<JVariant>;
using VariantMap  = std::vector<std::pair<std::string, JVariant>>;  // insertion-ordered

enum class JVariantType : uint8_t {
    Null, Bool, Int, Double, String, List, Map, Custom
};

class JVariant {
public:
    using Null = std::monostate;

    // ---- Construction -------------------------------------------------------
    JVariant() noexcept                : m_val(Null{}) {}
    JVariant(std::nullptr_t) noexcept  : m_val(Null{}) {}
    JVariant(bool v) noexcept          : m_val(v) {}

    template<class T, std::enable_if_t<std::is_integral_v<T> && !std::is_same_v<T, bool>, int> = 0>
    JVariant(T v) : m_val(static_cast<int64_t>(v)) {}

    template<class T, std::enable_if_t<std::is_floating_point_v<T>, int> = 0>
    JVariant(T v) : m_val(static_cast<double>(v)) {}

    JVariant(const char* v)        : m_val(std::string(v)) {}
    JVariant(std::string_view v)   : m_val(std::string(v)) {}
    JVariant(std::string v)        : m_val(std::move(v)) {}
    JVariant(VariantList v)        : m_val(std::move(v)) {}
    JVariant(VariantMap v)         : m_val(std::move(v)) {}

    // Wrap an arbitrary (opaque) C++ value. Retrieve with customPtr<T>()/value<T>().
    template<class T>
    static JVariant custom(T v) {
        JVariant r;
        r.m_val = std::any(std::move(v));
        return r;
    }

    static JVariant list(VariantList v = {}) { return JVariant(std::move(v)); }
    static JVariant map(VariantMap v = {})   { return JVariant(std::move(v)); }

    // ---- JType interrogation -------------------------------------------------
    JVariantType type() const noexcept {
        switch (m_val.index()) {
            case 0: return JVariantType::Null;
            case 1: return JVariantType::Bool;
            case 2: return JVariantType::Int;
            case 3: return JVariantType::Double;
            case 4: return JVariantType::String;
            case 5: return JVariantType::List;
            case 6: return JVariantType::Map;
            default: return JVariantType::Custom;
        }
    }
    const char* typeName() const noexcept {
        switch (type()) {
            case JVariantType::Null:   return "Null";
            case JVariantType::Bool:   return "Bool";
            case JVariantType::Int:    return "Int";
            case JVariantType::Double: return "Double";
            case JVariantType::String: return "String";
            case JVariantType::List:   return "List";
            case JVariantType::Map:    return "Map";
            default:                  return "Custom";
        }
    }

    bool isNull()   const noexcept { return type() == JVariantType::Null; }
    bool isBool()   const noexcept { return type() == JVariantType::Bool; }
    bool isInt()    const noexcept { return type() == JVariantType::Int; }
    bool isDouble() const noexcept { return type() == JVariantType::Double; }
    bool isNumber() const noexcept { return isInt() || isDouble(); }
    bool isString() const noexcept { return type() == JVariantType::String; }
    bool isList()   const noexcept { return type() == JVariantType::List; }
    bool isMap()    const noexcept { return type() == JVariantType::Map; }
    bool isCustom() const noexcept { return type() == JVariantType::Custom; }
    bool isValid()  const noexcept { return !isNull(); }

    // ---- Coercing accessors (best-effort conversion) ------------------------
    bool toBool(bool def = false) const {
        switch (type()) {
            case JVariantType::Bool:   return std::get<bool>(m_val);
            case JVariantType::Int:    return std::get<int64_t>(m_val) != 0;
            case JVariantType::Double: return std::get<double>(m_val) != 0.0;
            case JVariantType::String: {
                const std::string& s = std::get<std::string>(m_val);
                if (s == "true" || s == "1" || s == "yes" || s == "on")  return true;
                if (s == "false"|| s == "0" || s == "no"  || s == "off") return false;
                return def;
            }
            default: return def;
        }
    }
    int64_t toInt(int64_t def = 0) const {
        switch (type()) {
            case JVariantType::Bool:   return std::get<bool>(m_val) ? 1 : 0;
            case JVariantType::Int:    return std::get<int64_t>(m_val);
            case JVariantType::Double: return static_cast<int64_t>(std::get<double>(m_val));
            case JVariantType::String: {
                try { size_t pos = 0; long long r = std::stoll(std::get<std::string>(m_val), &pos); return r; }
                catch (...) { return def; }
            }
            default: return def;
        }
    }
    double toDouble(double def = 0.0) const {
        switch (type()) {
            case JVariantType::Bool:   return std::get<bool>(m_val) ? 1.0 : 0.0;
            case JVariantType::Int:    return static_cast<double>(std::get<int64_t>(m_val));
            case JVariantType::Double: return std::get<double>(m_val);
            case JVariantType::String: {
                try { return std::stod(std::get<std::string>(m_val)); }
                catch (...) { return def; }
            }
            default: return def;
        }
    }
    std::string toString() const {
        switch (type()) {
            case JVariantType::Null:   return std::string{};
            case JVariantType::Bool:   return std::get<bool>(m_val) ? "true" : "false";
            case JVariantType::Int:    return std::to_string(std::get<int64_t>(m_val));
            case JVariantType::Double: {
                std::ostringstream os; os << std::get<double>(m_val); return os.str();
            }
            case JVariantType::String: return std::get<std::string>(m_val);
            case JVariantType::List:   return _dump();
            case JVariantType::Map:    return _dump();
            default:                  return std::string{};
        }
    }

    // Container access (empty references returned for the wrong type).
    const VariantList& toList() const { return isList() ? std::get<VariantList>(m_val) : _emptyList(); }
    const VariantMap&  toMap()  const { return isMap()  ? std::get<VariantMap>(m_val)  : _emptyMap(); }
    VariantList& asList() { if (!isList()) m_val = VariantList{}; return std::get<VariantList>(m_val); }
    VariantMap&  asMap()  { if (!isMap())  m_val = VariantMap{};  return std::get<VariantMap>(m_val); }

    // ---- Map / list convenience --------------------------------------------
    bool contains(const std::string& key) const {
        if (!isMap()) return false;
        for (auto& kv : std::get<VariantMap>(m_val)) if (kv.first == key) return true;
        return false;
    }
    // Read-only lookup; returns Null JVariant when absent.
    JVariant at(const std::string& key) const {
        if (isMap()) for (auto& kv : std::get<VariantMap>(m_val)) if (kv.first == key) return kv.second;
        return JVariant{};
    }
    // Insert-or-update on a Map (promotes Null -> Map).
    void set(const std::string& key, JVariant v) {
        auto& m = asMap();
        for (auto& kv : m) { if (kv.first == key) { kv.second = std::move(v); return; } }
        m.emplace_back(key, std::move(v));
    }
    size_t size() const {
        if (isList()) return std::get<VariantList>(m_val).size();
        if (isMap())  return std::get<VariantMap>(m_val).size();
        return 0;
    }

    // ---- Opaque custom payload ---------------------------------------------
    template<class T> const T* customPtr() const {
        if (!isCustom()) return nullptr;
        return std::any_cast<T>(&std::get<std::any>(m_val));
    }
    template<class T> T* customPtr() {
        if (!isCustom()) return nullptr;
        return std::any_cast<T>(&std::get<std::any>(m_val));
    }

    // ---- Generic typed accessors -------------------------------------------
    // value<T>() coerces where sensible; tryValue<T>() only succeeds on an
    // exact/lossless match (no coercion guessing).
    template<class T> T value(T def = T{}) const {
        if constexpr (std::is_same_v<T, bool>)               return toBool(def);
        else if constexpr (std::is_integral_v<T>)            return static_cast<T>(toInt(static_cast<int64_t>(def)));
        else if constexpr (std::is_floating_point_v<T>)      return static_cast<T>(toDouble(static_cast<double>(def)));
        else if constexpr (std::is_same_v<T, std::string>)   return isNull() ? def : toString();
        else if constexpr (std::is_same_v<T, VariantList>)   return isList() ? std::get<VariantList>(m_val) : def;
        else if constexpr (std::is_same_v<T, VariantMap>)    return isMap()  ? std::get<VariantMap>(m_val)  : def;
        else { const T* p = customPtr<T>(); return p ? *p : def; }
    }
    template<class T> std::optional<T> tryValue() const {
        if constexpr (std::is_same_v<T, bool>)               { if (isBool())   return std::get<bool>(m_val); }
        else if constexpr (std::is_integral_v<T>)            { if (isInt())    return static_cast<T>(std::get<int64_t>(m_val)); }
        else if constexpr (std::is_floating_point_v<T>)      { if (isDouble()) return static_cast<T>(std::get<double>(m_val));
                                                               if (isInt())    return static_cast<T>(std::get<int64_t>(m_val)); }
        else if constexpr (std::is_same_v<T, std::string>)   { if (isString()) return std::get<std::string>(m_val); }
        else if constexpr (std::is_same_v<T, VariantList>)   { if (isList())   return std::get<VariantList>(m_val); }
        else if constexpr (std::is_same_v<T, VariantMap>)    { if (isMap())    return std::get<VariantMap>(m_val); }
        else { if (const T* p = customPtr<T>()) return *p; }
        return std::nullopt;
    }
    template<class T> bool is() const { return tryValue<T>().has_value(); }

    // ---- Equality -----------------------------------------------------------
    bool operator==(const JVariant& o) const {
        // Cross-numeric comparison (Int vs Double) by value.
        if (isNumber() && o.isNumber()) {
            if (isInt() && o.isInt()) return std::get<int64_t>(m_val) == std::get<int64_t>(o.m_val);
            return toDouble() == o.toDouble();
        }
        if (type() != o.type()) return false;
        switch (type()) {
            case JVariantType::Null:   return true;
            case JVariantType::Bool:   return std::get<bool>(m_val) == std::get<bool>(o.m_val);
            case JVariantType::String: return std::get<std::string>(m_val) == std::get<std::string>(o.m_val);
            case JVariantType::List:   return std::get<VariantList>(m_val) == std::get<VariantList>(o.m_val);
            case JVariantType::Map:    return std::get<VariantMap>(m_val) == std::get<VariantMap>(o.m_val);
            default:                  return false;  // custom payloads are not comparable
        }
    }
    bool operator!=(const JVariant& o) const { return !(*this == o); }

private:
    std::variant<Null, bool, int64_t, double, std::string, VariantList, VariantMap, std::any> m_val;

    static const VariantList& _emptyList() { static const VariantList e; return e; }
    static const VariantMap&  _emptyMap()  { static const VariantMap  e; return e; }

    std::string _dump() const {
        std::ostringstream os;
        if (isList()) {
            os << '[';
            const auto& l = std::get<VariantList>(m_val);
            for (size_t i = 0; i < l.size(); ++i) { if (i) os << ','; os << l[i]._quoted(); }
            os << ']';
        } else if (isMap()) {
            os << '{';
            const auto& m = std::get<VariantMap>(m_val);
            for (size_t i = 0; i < m.size(); ++i) {
                if (i) os << ',';
                os << '"' << m[i].first << "\":" << m[i].second._quoted();
            }
            os << '}';
        }
        return os.str();
    }
    std::string _quoted() const {
        if (isString()) return '"' + std::get<std::string>(m_val) + '"';
        return toString();
    }
};

}  // inline namespace jf
