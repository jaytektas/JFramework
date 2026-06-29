#pragma once

// ============================================================================
// Genesis::resolveRef — the control-domain half of the reference scheme.
//
// A reference is a dotted path whose HEAD is a NodeId (the durable reference key
// of a widget) and whose TAIL walks that widget's getRef() and descends into any
// Map/List it returns:
//
//     "42.value"              -> widget 42's typed value
//     "42.width"              -> widget 42's layout width
//     "42.y.sigid"            -> widget 42, "y" axis Map, "sigid" entry
//     "42.yAxis[3]"           -> widget 42, "yAxis" List, element 3
//
// Framework-free: it sits on Genesis::JVariant + the JWidget registry, nothing GUI-
// backend specific, so it ports with the widgets rather than the renderer. Reads
// only — writes are a separate, explicit path (see the evaluator's assignment form).
//
// NodeId is the canonical key here. A friendly refName -> NodeId mapping is a higher
// layer's job (the picker); this resolver speaks ids.
// ============================================================================

#include "BaseWidgets.h"
#include "Variant.h"

#include <string>
#include <vector>
#include <cstdlib>

namespace Genesis {

// Linear scan of the active-widget registry. Isolated here so it can become an index
// if it ever shows up in a profile; the rest of the system only sees resolveRef().
inline JWidget* widgetByNodeId(NodeId id) {
    for (JWidget* w : JWidget::s_activeWidgets)
        if (w->getNodeId() == id) return w;
    return nullptr;
}

namespace detail {

// Split "a.b[2].c" into ["a", "b[2]", "c"] (on '.').
inline std::vector<std::string> splitRefPath(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == '.') { out.push_back(cur); cur.clear(); }
        else            cur += c;
    }
    out.push_back(cur);
    return out;
}

// Apply the trailing [N][M]... list indices of `seg` (starting at byte `lb`) to `v`,
// which must be a List at each step. Returns Null on an out-of-range / non-list step.
inline JVariant applyIndexSuffix(JVariant v, const std::string& seg, size_t lb) {
    size_t p = lb;
    while (p < seg.size() && seg[p] == '[') {
        const size_t rb = seg.find(']', p);
        if (rb == std::string::npos) return JVariant{};
        const long idx = std::strtol(seg.c_str() + p + 1, nullptr, 10);
        if (!v.isList() || idx < 0 || static_cast<size_t>(idx) >= v.toList().size())
            return JVariant{};
        v = v.toList()[static_cast<size_t>(idx)];
        p = rb + 1;
    }
    return v;
}

}  // namespace detail

// Resolve a pre-split tail (segments after the NodeId) against a widget.
inline JVariant resolveRefPath(JWidget* root, const std::vector<std::string>& segs) {
    if (!root) return JVariant{};
    JVariant cur;
    bool have = false;
    for (const std::string& seg : segs) {
        // The key is the segment minus any [..] index suffix; resolve the key against
        // the current context (the widget for the first segment, a Map thereafter),
        // then apply the index suffix to whatever it produced.
        const size_t lb = seg.find('[');
        const std::string key = (lb == std::string::npos) ? seg : seg.substr(0, lb);
        JVariant v = !have ? root->getRef(key)
                          : (cur.isMap() ? cur.at(key) : JVariant{});
        if (lb != std::string::npos)
            v = detail::applyIndexSuffix(v, seg, lb);
        cur = v;
        have = true;
    }
    return have ? cur : JVariant{};
}

// Resolve a full "<nodeId>.tail" reference string. A bare "<nodeId>" yields the
// widget's "value". Returns Null for a bad/dangling id or an unresolvable path.
inline JVariant resolveRef(const std::string& expr) {
    const size_t dot = expr.find('.');
    const std::string head = (dot == std::string::npos) ? expr : expr.substr(0, dot);
    char* end = nullptr;
    const unsigned long id = std::strtoul(head.c_str(), &end, 10);
    if (end == head.c_str() || *end != '\0') return JVariant{};   // head wasn't a number
    JWidget* w = widgetByNodeId(static_cast<NodeId>(id));
    if (!w) return JVariant{};
    if (dot == std::string::npos) return w->getRef("value");
    return resolveRefPath(w, detail::splitRefPath(expr.substr(dot + 1)));
}

}  // namespace Genesis
