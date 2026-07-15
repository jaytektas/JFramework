#pragma once

// ============================================================================
// jf::jMakePropertyEditor — the generic PLUMBING that binds one JProperty to an
// editor control. Framework-owned and presentation-free:
//
//   * maps the property's value type + meta to the right built-in control
//     (choices => combo, Bool => checkbox, Int => spin box, Double => double
//     spin box, String => line edit),
//   * wires the control's change signal back through the property's setter, and
//   * returns a pull() that refreshes the control from the property's LIVE value.
//
// The PRESENTATION — where rows go, labels, chrome, which properties to show,
// grouping — stays with the APPLICATION, which arranges the returned widgets
// however it likes. Plumbing here; layout there. That split is the flexibility:
// one binding, many presentations.
//
//   for (const JProperty& p : target->properties().all()) {
//       JPropertyEditor e = jMakePropertyEditor(graph, p);
//       e.widget->setBounds(myRowRect);   // app owns layout
//       e.pull();                          // reflect the current value
//       e.widget->populateRenderPrimitives(buf);
//   }
// ============================================================================

#include "JPropertyModel.h"
#include "JCheckBox.h"
#include "JSpinBox.h"
#include "JDoubleSpinBox.h"
#include "JLineEdit.h"
#include "JComboBox.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

inline namespace jf {

// A property bound to a control: the owned widget + a pull() that syncs it from the live value.
struct JPropertyEditor {
    std::unique_ptr<JWidget> widget;      // the bound control
    std::function<void()>    pull;        // refresh the control's display from the property's value
    char                     kind = '?';  // b/i/d/s/c — the chosen editor kind, if the caller cares
};

// Build + two-way-bind an editor control for one property. The property's get/set closures are copied
// (each is bound to its owning instance), so the returned editor is valid for that instance's lifetime.
inline JPropertyEditor jMakePropertyEditor(JSceneGraph& graph, const JProperty& p) {
    auto get = p.get;
    auto set = p.set;
    auto put = [set](const JVariant& v) { if (set) set(v); };   // read-only props (no setter) just display
    JPropertyEditor e;

    if (!p.meta.choices.empty()) {
        std::vector<std::string> items;
        for (const JVariant& c : p.meta.choices) items.push_back(c.toString());
        const bool asString = get().isString();               // choices store either the label or the index
        auto* cb = new JComboBox(graph, items);
        cb->onIndexChanged.connect([put, items, asString](int i) {
            if (asString && i >= 0 && i < static_cast<int>(items.size())) put(JVariant(items[i]));
            else put(JVariant(i));
        });
        e.kind = 'c';
        e.pull = [cb, get, items] {
            const JVariant v = get();
            if (v.isString()) { for (int i = 0; i < static_cast<int>(items.size()); ++i) if (items[i] == v.toString()) { cb->setCurrentIndex(i); break; } }
            else cb->setCurrentIndex(static_cast<int>(v.toInt()));
        };
        e.widget.reset(cb);
    } else if (get().isBool()) {
        auto* ck = new JCheckBox(graph, "");
        ck->onStateChanged.connect([put](bool v) { put(JVariant(v)); });
        e.kind = 'b';
        e.pull = [ck, get] { ck->setChecked(get().toBool()); };
        e.widget.reset(ck);
    } else if (get().isInt()) {
        const int mn = p.meta.min.isNull() ? -1000000 : static_cast<int>(p.meta.min.toInt());
        const int mx = p.meta.max.isNull() ?  1000000 : static_cast<int>(p.meta.max.toInt());
        auto* sp = new JSpinBox(graph, mn, mx);
        sp->onValueChanged.connect([put](int v) { put(JVariant(v)); });
        e.kind = 'i';
        e.pull = [sp, get] { sp->setValue(static_cast<int>(get().toInt())); };
        e.widget.reset(sp);
    } else if (get().isDouble()) {
        const double mn = p.meta.min.isNull()  ? -1.0e9 : p.meta.min.toDouble();
        const double mx = p.meta.max.isNull()  ?  1.0e9 : p.meta.max.toDouble();
        const double st = p.meta.step.isNull() ?  0.1   : p.meta.step.toDouble();
        const int    dc = p.meta.decimals > 0  ? p.meta.decimals : 2;
        auto* sp = new JDoubleSpinBox(graph, mn, mx, st, dc);
        sp->onValueChanged.connect([put](double v) { put(JVariant(v)); });
        e.kind = 'd';
        e.pull = [sp, get] { sp->setValue(get().toDouble()); };
        e.widget.reset(sp);
    } else {
        auto* le = new JLineEdit(graph, "");
        le->onTextChanged.connect([put](const std::string& t) { put(JVariant(t)); });
        e.kind = 's';
        e.pull = [le, get] { const std::string s = get().toString(); if (le->text() != s) le->setText(s); };
        e.widget.reset(le);
    }
    return e;
}

}  // inline namespace jf
