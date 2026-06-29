#pragma once

// ============================================================================
// Genesis::VariantJson — opt-in bridge between JVariant and JJson.
//
// Keeps JVariant a dependency-free leaf: include this header only where you
// actually need to cross between the universal value type and JSON.
//
//   JJson j     = toJson(variant);
//   JVariant v  = fromJson(JJson::parse(src));
//
// Combined with JMetaClass<T>, this serializes any reflected object:
//   JJson j = toJson(JVariant(JMetaClass<T>::instance().toMap(obj)));
//
// Note: JJson has no integer type (all numbers are double). On JJson -> JVariant,
// integral-valued numbers become JVariant Int and everything else Double, which
// is the ergonomic choice for config/settings round-trips. Custom (opaque)
// JVariant payloads cannot be serialized and map to JSON null.
// ============================================================================

#include <genesis/core/Variant.h>
#include <genesis/core/MetaObject.h>
#include <genesis/config/Json.h>
#include <cmath>
#include <limits>

namespace Genesis {

inline JJson toJson(const JVariant& v) {
    switch (v.type()) {
        case JVariantType::Null:   return JJson(nullptr);
        case JVariantType::Bool:   return JJson(v.toBool());
        case JVariantType::Int:    return JJson(static_cast<int64_t>(v.toInt()));
        case JVariantType::Double: return JJson(v.toDouble());
        case JVariantType::String: return JJson(v.toString());
        case JVariantType::List: {
            JJson arr = JJson::array();
            for (const auto& e : v.toList()) arr.arr().push_back(toJson(e));
            return arr;
        }
        case JVariantType::Map: {
            JJson obj = JJson::object();
            for (const auto& kv : v.toMap()) obj[kv.first] = toJson(kv.second);
            return obj;
        }
        default: return JJson(nullptr);  // opaque custom payloads aren't serializable
    }
}

inline JVariant fromJson(const JJson& j) {
    if (j.isNull())   return JVariant{};
    if (j.isBool())   return JVariant(j.boolean());
    if (j.isNumber()) {
        double d = j.number<double>();
        // Preserve integers as JVariant Int when the value is exactly integral.
        if (std::isfinite(d) && std::floor(d) == d &&
            d >= static_cast<double>(std::numeric_limits<int64_t>::min()) &&
            d <= static_cast<double>(std::numeric_limits<int64_t>::max())) {
            return JVariant(static_cast<int64_t>(d));
        }
        return JVariant(d);
    }
    if (j.isString()) return JVariant(j.str());
    if (j.isArray()) {
        VariantList list;
        list.reserve(j.size());
        for (const auto& e : j.arr()) list.push_back(fromJson(e));
        return JVariant(std::move(list));
    }
    if (j.isObject()) {
        VariantMap map;
        map.reserve(j.size());
        for (const auto& kv : j.obj()) map.emplace_back(kv.first, fromJson(kv.second));
        return JVariant(std::move(map));
    }
    return JVariant{};
}

// ---- Reflected-object serialization ----------------------------------------
// Any type registered with JMetaClass<T> serializes to/from JSON for free.

template<class T>
inline JJson serialize(const T& obj) {
    return toJson(JVariant(JMetaClass<T>::instance().toMap(obj)));
}

template<class T>
inline void deserialize(T& obj, const JJson& j) {
    JVariant v = fromJson(j);
    if (v.isMap()) JMetaClass<T>::instance().fromMap(obj, v.toMap());
}

}  // namespace Genesis
