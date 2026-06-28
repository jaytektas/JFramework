#pragma once

// ============================================================================
// Genesis::VariantJson — opt-in bridge between Variant and Json.
//
// Keeps Variant a dependency-free leaf: include this header only where you
// actually need to cross between the universal value type and JSON.
//
//   Json j     = toJson(variant);
//   Variant v  = fromJson(Json::parse(src));
//
// Combined with MetaClass<T>, this serializes any reflected object:
//   Json j = toJson(Variant(MetaClass<T>::instance().toMap(obj)));
//
// Note: Json has no integer type (all numbers are double). On Json -> Variant,
// integral-valued numbers become Variant Int and everything else Double, which
// is the ergonomic choice for config/settings round-trips. Custom (opaque)
// Variant payloads cannot be serialized and map to JSON null.
// ============================================================================

#include <genesis/core/Variant.h>
#include <genesis/core/MetaObject.h>
#include <genesis/config/Json.h>
#include <cmath>
#include <limits>

namespace Genesis {

inline Json toJson(const Variant& v) {
    switch (v.type()) {
        case VariantType::Null:   return Json(nullptr);
        case VariantType::Bool:   return Json(v.toBool());
        case VariantType::Int:    return Json(static_cast<int64_t>(v.toInt()));
        case VariantType::Double: return Json(v.toDouble());
        case VariantType::String: return Json(v.toString());
        case VariantType::List: {
            Json arr = Json::array();
            for (const auto& e : v.toList()) arr.arr().push_back(toJson(e));
            return arr;
        }
        case VariantType::Map: {
            Json obj = Json::object();
            for (const auto& kv : v.toMap()) obj[kv.first] = toJson(kv.second);
            return obj;
        }
        default: return Json(nullptr);  // opaque custom payloads aren't serializable
    }
}

inline Variant fromJson(const Json& j) {
    if (j.isNull())   return Variant{};
    if (j.isBool())   return Variant(j.boolean());
    if (j.isNumber()) {
        double d = j.number<double>();
        // Preserve integers as Variant Int when the value is exactly integral.
        if (std::isfinite(d) && std::floor(d) == d &&
            d >= static_cast<double>(std::numeric_limits<int64_t>::min()) &&
            d <= static_cast<double>(std::numeric_limits<int64_t>::max())) {
            return Variant(static_cast<int64_t>(d));
        }
        return Variant(d);
    }
    if (j.isString()) return Variant(j.str());
    if (j.isArray()) {
        VariantList list;
        list.reserve(j.size());
        for (const auto& e : j.arr()) list.push_back(fromJson(e));
        return Variant(std::move(list));
    }
    if (j.isObject()) {
        VariantMap map;
        map.reserve(j.size());
        for (const auto& kv : j.obj()) map.emplace_back(kv.first, fromJson(kv.second));
        return Variant(std::move(map));
    }
    return Variant{};
}

// ---- Reflected-object serialization ----------------------------------------
// Any type registered with MetaClass<T> serializes to/from JSON for free.

template<class T>
inline Json serialize(const T& obj) {
    return toJson(Variant(MetaClass<T>::instance().toMap(obj)));
}

template<class T>
inline void deserialize(T& obj, const Json& j) {
    Variant v = fromJson(j);
    if (v.isMap()) MetaClass<T>::instance().fromMap(obj, v.toMap());
}

}  // namespace Genesis
