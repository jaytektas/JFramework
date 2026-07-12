#pragma once

// ============================================================================
// jf::JPropertyModel — the introspective, editable property surface.
//
// The QWidget/Q_PROPERTY analog, minus the meta-object compiler. A flat, ordered
// list of named JVariant properties, each a live getter + optional setter +
// editor metadata (category, bounds, choices, read-only). A single generic
// property editor consumes any model: it enumerates the properties, renders a
// row per entry (the JVariant type + meta picks the editor widget), and writes
// edits straight back through the setter.
//
// Every JWidget exposes one (see JWidget::properties()). A subclass overrides
// collectProperties(), chains to its base, and registers its own members:
//
//     void collectProperties(JPropertyModel& m) override {
//         JWidget::collectProperties(m);                 // geometry, enabled, ...
//         m.add("checked", this, &ACheckBox::m_checked, {.category="Behavior"});
//         m.add("label",   this, &ACheckBox::m_label,   {.category="Appearance"});
//     }
//
// This is DISTINCT from two neighbouring facilities:
//   * JWidget::getRef() — resolves reference PATHS (uid.value, uid.bins[3]) for
//     bindings and the AI bus; descends into maps/lists. Not for editing.
//   * JPropertyBag (MetaObject.h) — opt-in, app-level DYNAMIC properties added
//     at runtime by name. This model is the STATIC, per-class declared surface.
//
// Values live in the object's real typed members; the model only describes how
// to reach them. Each property's closures are bound to one instance, so a model
// is valid only for as long as its owning object lives.
// ============================================================================

#include <j/core/Variant.h>

#include <functional>
#include <string>
#include <vector>
#include <type_traits>

inline namespace jf {

// ---- Editor-facing metadata for a single property ---------------------------
// The complete description a generic property editor needs to render one row: how
// to group and label it, which editor widget to use, and the value's bounds. This
// is the JVariant-native equivalent of Qt's per-property metadata (name/READ/WRITE
// + a designer's propertyMetaData() map) — carried as data, resolved with no moc.
struct JPropertyMeta {
    std::string category;          // editor grouping ("" => ungrouped / General)
    std::string label;             // human label ("" => use the property name)
    std::string def;               // authored default VALUE (string form); "" => no default
    std::string tooltip;           // help text
    // Explicit editor kind. "" => the editor derives one from the value type
    // (Bool => checkbox, number => spin box, String => line edit, choices => combo).
    // Set it when the value type is ambiguous: "color" | "font" | "signal" | "unit"
    // | "file" | "choices" | any app-defined kind the editor knows how to build.
    std::string editor;
    bool        designable = true; // present in a property editor at all?
    bool        writable   = true; // allow editing (also gated by having a setter)
    bool        inheritable = false; // editor offers a "clear" that resets to the scheme/default
    JVariant    min;               // numeric lower bound (Null => unbounded)
    JVariant    max;               // numeric upper bound (Null => unbounded)
    JVariant    step;              // numeric increment  (Null => editor default)
    int         decimals = 0;      // >0 => fractional editor; 0 => integer
    std::string suffix;            // unit suffix shown after the value (" ms", " px")
    int         order = 0;         // editor sort key; the model itself stays insertion-ordered
    VariantList choices;           // enumerated choices (empty => free value)
};

// ---- One named property: live getter, optional setter, and its metadata -----
struct JProperty {
    std::string                          name;
    std::function<JVariant()>            get;   // never null
    std::function<bool(const JVariant&)> set;   // null => read-only
    JPropertyMeta                        meta;

    bool writable() const { return static_cast<bool>(set) && meta.writable; }
};

namespace jprop_detail {
    // Single-argument type of a setter member-function pointer.
    template<class S> struct setter_arg;
    template<class C, class A> struct setter_arg<void (C::*)(A)>          { using type = A; };
    template<class C, class A> struct setter_arg<void (C::*)(A) const>    { using type = A; };
    template<class C, class A> struct setter_arg<void (C::*)(A) noexcept> { using type = A; };

    // A JVariant from any value: native ctor where one exists, enums as ints,
    // otherwise ride along opaquely via JVariant::custom.
    template<class X>
    JVariant toVariant(const X& x) {
        if constexpr (std::is_enum_v<X>)              return JVariant(static_cast<int64_t>(x));
        else if constexpr (requires { JVariant(x); }) return JVariant(x);
        else                                          return JVariant::custom(x);
    }

    // Assign a JVariant back onto a typed lvalue, coercing where sensible.
    template<class M>
    bool fromVariant(M& dst, const JVariant& v) {
        if constexpr (std::is_same_v<M, bool>)             { dst = v.toBool(dst); return true; }
        else if constexpr (std::is_enum_v<M>)              { dst = static_cast<M>(v.toInt(static_cast<int64_t>(dst))); return true; }
        else if constexpr (std::is_integral_v<M>)          { dst = static_cast<M>(v.toInt(static_cast<int64_t>(dst))); return true; }
        else if constexpr (std::is_floating_point_v<M>)    { dst = static_cast<M>(v.toDouble(static_cast<double>(dst))); return true; }
        else if constexpr (std::is_same_v<M, std::string>) { dst = v.toString(); return true; }
        else { auto opt = v.tryValue<M>(); if (!opt) return false; dst = *opt; return true; }
    }
}  // namespace jprop_detail

// ---- The model --------------------------------------------------------------
class JPropertyModel {
public:
    // Register a plain data member (read/write). obj is the owning instance.
    template<class T, class M, std::enable_if_t<std::is_object_v<M>, int> = 0>
    JPropertyModel& add(std::string name, T* obj, M T::* member, JPropertyMeta meta = {}) {
        JProperty p;
        p.name = std::move(name);
        p.meta = std::move(meta);
        p.get  = [obj, member] { return jprop_detail::toVariant(obj->*member); };
        p.set  = [obj, member](const JVariant& v) { return jprop_detail::fromVariant(obj->*member, v); };
        return _add(std::move(p));
    }

    // Register a getter/setter pair (R (T::*)() const  +  void (T::*)(A)).
    template<class T, class G, class S, std::enable_if_t<std::is_member_function_pointer_v<S>, int> = 0>
    JPropertyModel& add(std::string name, T* obj, G getter, S setter, JPropertyMeta meta = {}) {
        using Arg = std::decay_t<typename jprop_detail::setter_arg<S>::type>;
        JProperty p;
        p.name = std::move(name);
        p.meta = std::move(meta);
        p.get  = [obj, getter] { return jprop_detail::toVariant((obj->*getter)()); };
        p.set  = [obj, setter](const JVariant& v) { (obj->*setter)(v.value<Arg>()); return true; };
        return _add(std::move(p));
    }

    // Register a read-only getter.
    template<class T, class G>
    JPropertyModel& addReadOnly(std::string name, T* obj, G getter, JPropertyMeta meta = {}) {
        JProperty p;
        p.name = std::move(name);
        p.meta = std::move(meta);
        p.get  = [obj, getter] { return jprop_detail::toVariant((obj->*getter)()); };
        return _add(std::move(p));
    }

    // Register a fully custom property (arbitrary getter/setter closures).
    JPropertyModel& add(JProperty p) { return _add(std::move(p)); }

    // ---- Access -------------------------------------------------------------
    const std::vector<JProperty>& all() const { return m_props; }

    const JProperty* find(const std::string& name) const {
        for (auto& p : m_props) if (p.name == name) return &p;
        return nullptr;
    }

    JVariant get(const std::string& name) const {
        if (const JProperty* p = find(name)) return p->get();
        return JVariant{};
    }

    bool set(const std::string& name, const JVariant& v) {
        const JProperty* p = find(name);
        if (!p || !p->writable()) return false;
        return p->set(v);
    }

    std::vector<std::string> names() const {
        std::vector<std::string> n;
        n.reserve(m_props.size());
        for (auto& p : m_props) n.push_back(p.name);
        return n;
    }

    // Snapshot every property's current value (insertion-ordered).
    VariantMap toMap() const {
        VariantMap m;
        m.reserve(m_props.size());
        for (auto& p : m_props) m.emplace_back(p.name, p.get());
        return m;
    }

    // Suppress a property (e.g. an inherited base row a subclass doesn't want). Returns true if removed.
    bool remove(const std::string& name) {
        for (auto it = m_props.begin(); it != m_props.end(); ++it)
            if (it->name == name) { m_props.erase(it); return true; }
        return false;
    }

    bool   empty() const { return m_props.empty(); }
    size_t size()  const { return m_props.size(); }
    void   clear()       { m_props.clear(); }

private:
    std::vector<JProperty> m_props;  // registration order == editor order

    JPropertyModel& _add(JProperty p) {
        for (auto& e : m_props) if (e.name == p.name) { e = std::move(p); return *this; }  // re-register overwrites
        m_props.push_back(std::move(p));
        return *this;
    }
};

}  // inline namespace jf
