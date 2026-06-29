#pragma once

// ============================================================================
// Genesis::MetaObject — JVariant-based reflection, the QObject meta-layer
// without a meta-object compiler.
//
// Two complementary facilities, both built on Genesis::JVariant:
//
// 1. JPropertyBag — a dynamic, named bag of JVariant properties with a change
//    signal. The "dynamic properties" half of QObject.
//
//        JPropertyBag bag;
//        bag.onPropertyChanged.connect([](auto k, auto v){ ... });
//        bag.setProperty("baud", 115200);
//        int b = bag.property("baud").toInt();
//
// 2. JMetaClass<T> — static, compile-time-registered reflection of a concrete
//    type's typed properties via member pointers or getter/setter pairs. This
//    is what lets generic code (serialization, model/view, binding) enumerate
//    and get/set fields on any registered type by name.
//
//        struct JSerialConfig { std::string port; int baud{9600}; bool rts{false}; };
//        JMetaClass<JSerialConfig>::instance()
//            .field("port", &JSerialConfig::port)
//            .field("baud", &JSerialConfig::baud)
//            .field("rts",  &JSerialConfig::rts);
//
//        JSerialConfig c;
//        auto& mc = JMetaClass<JSerialConfig>::instance();
//        mc.set(c, "baud", 115200);
//        JVariant b = mc.get(c, "baud");          // 115200
//        VariantMap snapshot = mc.toMap(c);      // serialize all properties
//        mc.fromMap(c, snapshot);                // apply back
//
// Pairs naturally with <genesis/core/VariantJson.h> to serialize any reflected
// object to/from JSON.
// ============================================================================

#include <genesis/core/Variant.h>
#include <genesis/core/Signal.h>

#include <string>
#include <vector>
#include <functional>

namespace Genesis {

// ---- Dynamic property bag ---------------------------------------------------
class JPropertyBag {
public:
    // Fires (key, newValue) whenever a property is set or changed.
    Core::JSignal<std::string, JVariant> onPropertyChanged;

    void setProperty(const std::string& name, JVariant value) {
        for (auto& kv : m_props) {
            if (kv.first == name) {
                if (kv.second == value) return;       // no-op on identical value
                kv.second = std::move(value);
                onPropertyChanged.emit(name, kv.second);
                return;
            }
        }
        m_props.emplace_back(name, std::move(value));
        onPropertyChanged.emit(name, m_props.back().second);
    }

    JVariant property(const std::string& name) const {
        for (auto& kv : m_props) if (kv.first == name) return kv.second;
        return JVariant{};
    }

    bool hasProperty(const std::string& name) const {
        for (auto& kv : m_props) if (kv.first == name) return true;
        return false;
    }

    bool removeProperty(const std::string& name) {
        for (auto it = m_props.begin(); it != m_props.end(); ++it) {
            if (it->first == name) { m_props.erase(it); return true; }
        }
        return false;
    }

    std::vector<std::string> propertyNames() const {
        std::vector<std::string> names;
        names.reserve(m_props.size());
        for (auto& kv : m_props) names.push_back(kv.first);
        return names;
    }

    const VariantMap& properties() const { return m_props; }
    void clear() { m_props.clear(); }

private:
    VariantMap m_props;  // insertion-ordered
};

// ---- Static reflection ------------------------------------------------------
struct JMetaProperty {
    std::string name;
    std::function<JVariant(const void*)>        get;   // never null
    std::function<bool(void*, const JVariant&)> set;   // null => read-only

    bool writable() const { return static_cast<bool>(set); }
};

template<class T>
class JMetaClass {
public:
    static JMetaClass& instance() { static JMetaClass m; return m; }

    // Register a plain data member (read/write). The member's type must be
    // JVariant-convertible (scalars, std::string, containers, or a custom type).
    template<class M>
    JMetaClass& field(std::string name, M T::* member) {
        JMetaProperty p;
        p.name = name;
        p.get  = [member](const void* obj) -> JVariant {
            return JVariant(static_cast<const T*>(obj)->*member);
        };
        p.set  = [member](void* obj, const JVariant& v) -> bool {
            auto opt = v.tryValue<M>();
            if constexpr (std::is_arithmetic_v<M>) {
                static_cast<T*>(obj)->*member = static_cast<M>(v.toDouble(static_cast<double>(static_cast<T*>(obj)->*member)));
                return true;
            } else {
                if (!opt) return false;
                static_cast<T*>(obj)->*member = *opt;
                return true;
            }
        };
        return _add(std::move(p));
    }

    // Register a getter/setter pair (e.g. R (T::*)() const  +  void (T::*)(A)).
    template<class JGetter, class JSetter>
    JMetaClass& property(std::string name, JGetter getter, JSetter setter) {
        JMetaProperty p;
        p.name = name;
        p.get  = [getter](const void* obj) -> JVariant {
            return JVariant((static_cast<const T*>(obj)->*getter)());
        };
        p.set  = [setter](void* obj, const JVariant& v) -> bool {
            using Arg = std::decay_t<typename detail_arg<JSetter>::type>;
            (static_cast<T*>(obj)->*setter)(v.value<Arg>());
            return true;
        };
        return _add(std::move(p));
    }

    // Register a read-only getter.
    template<class JGetter>
    JMetaClass& readOnly(std::string name, JGetter getter) {
        JMetaProperty p;
        p.name = name;
        p.get  = [getter](const void* obj) -> JVariant {
            return JVariant((static_cast<const T*>(obj)->*getter)());
        };
        return _add(std::move(p));
    }

    const std::vector<JMetaProperty>& properties() const { return m_props; }

    const JMetaProperty* find(const std::string& name) const {
        for (auto& p : m_props) if (p.name == name) return &p;
        return nullptr;
    }

    JVariant get(const T& obj, const std::string& name) const {
        if (const JMetaProperty* p = find(name)) return p->get(&obj);
        return JVariant{};
    }

    bool set(T& obj, const std::string& name, const JVariant& v) const {
        const JMetaProperty* p = find(name);
        if (!p || !p->set) return false;
        return p->set(&obj, v);
    }

    // Serialize every property to an insertion-ordered VariantMap.
    VariantMap toMap(const T& obj) const {
        VariantMap m;
        m.reserve(m_props.size());
        for (auto& p : m_props) m.emplace_back(p.name, p.get(&obj));
        return m;
    }

    // Apply a VariantMap back onto an object (skips unknown / read-only keys).
    void fromMap(T& obj, const VariantMap& m) const {
        for (auto& kv : m) set(obj, kv.first, kv.second);
    }

private:
    std::vector<JMetaProperty> m_props;

    JMetaClass& _add(JMetaProperty p) {
        for (auto& existing : m_props) {
            if (existing.name == p.name) { existing = std::move(p); return *this; }  // re-register overwrites
        }
        m_props.push_back(std::move(p));
        return *this;
    }

    // Extract the (single) argument type of a setter member-function pointer.
    template<class S> struct detail_arg;
    template<class C, class A> struct detail_arg<void (C::*)(A)>       { using type = A; };
    template<class C, class A> struct detail_arg<void (C::*)(A) const> { using type = A; };
};

}  // namespace Genesis
