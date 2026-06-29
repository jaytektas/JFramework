// Tests for the reference scheme:
//   ReferenceResolver  — control-domain value access by NodeId path
//   Evaluator          — opt-in, precompiled, pure/reentrant/thread-safe expression eval
//   (the test itself plays the "app": a derived-value store with memo + cycle guard
//    built ON TOP of the generic framework, proving the framework needs no knowledge of it)
#include <genesis/core/ReferenceResolver.h>
#include <genesis/core/ReferenceEvaluator.h>
#include <genesis/core/SceneGraph.h>

#include <cassert>
#include <cmath>
#include <iostream>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <limits>
#include <unordered_map>
#include <unordered_set>

using namespace Genesis;

static std::string id(NodeId n) { return std::to_string(n); }
static bool near(double a, double b) { return std::fabs(a - b) < 1e-6; }

// ---- base native keys, available on every widget ---------------------------
void test_base_keys() {
    SceneGraph g;
    Button btn(g, "Calibrate");
    auto& l = g.getLayout(btn.getNodeId());
    l.boundingBox = {10, 20, 160, 36};

    const std::string b = id(btn.getNodeId());
    assert(resolveRef(b + ".role").toString() == "Button");
    assert(resolveRef(b + ".label").toString() == "Calibrate");
    assert(near(resolveRef(b + ".width").toDouble(), 160.0));
    assert(near(resolveRef(b + ".x").toDouble(), 10.0));
    assert(resolveRef(b + ".enabled").toBool() == true);
    std::cout << "test_base_keys passed\n";
}

// ---- typed per-widget overrides --------------------------------------------
void test_typed_value() {
    SceneGraph g;
    Slider s(g);
    s.setValue(0.75f);
    assert(resolveRef(id(s.getNodeId()) + ".value").isDouble());
    assert(near(resolveRef(id(s.getNodeId()) + ".value").toDouble(), 0.75));

    CheckBox cb(g, "Armed");
    cb.setChecked(true);
    assert(resolveRef(id(cb.getNodeId()) + ".checked").toBool() == true);
    assert(resolveRef(id(cb.getNodeId()) + ".value").toBool() == true);
    std::cout << "test_typed_value passed\n";
}

// ---- unknown keys / dangling ids degrade to Null, never crash --------------
void test_unknown_and_dangling() {
    SceneGraph g;
    Slider s(g);
    assert(resolveRef(id(s.getNodeId()) + ".nonexistent").isNull());
    assert(resolveRef("4294000000.value").isNull());
    assert(resolveRef("notanumber.value").isNull());
    std::cout << "test_unknown_and_dangling passed\n";
}

// A synthetic table-like widget: nested Map + indexed List descent.
class FakeTable : public Widget {
public:
    explicit FakeTable(SceneGraph& g) : Widget(g, "FakeTable") {}
    void populateRenderPrimitives(PrimitiveBuffer&) override {}
    AISemanticNode getSemanticNode() const override { return {"Table", "fuel", "", false}; }

    int cursorRow = 3;
    Variant getRef(const std::string& key) const override {
        if (key == "cursorRow") return static_cast<int64_t>(cursorRow);
        if (key == "yAxis")     return Variant::list({500, 1000, 1500, 2000, 2500});
        if (key == "y")         return Variant::map({{"sigid", 12}, {"n", 5}});
        return Widget::getRef(key);
    }
};

void test_nested_paths() {
    SceneGraph g;
    FakeTable t(g);
    const std::string p = id(t.getNodeId());
    assert(resolveRef(p + ".cursorRow").toInt() == 3);
    assert(resolveRef(p + ".yAxis[3]").toInt() == 2000);
    assert(resolveRef(p + ".y.sigid").toInt() == 12);
    assert(resolveRef(p + ".y.n").toInt() == 5);
    assert(resolveRef(p + ".yAxis[9]").isNull());
    std::cout << "test_nested_paths passed\n";
}

// A control-domain resolver: map a refName head to its NodeId, then resolveRef.
static Evaluator::Resolver controlsResolver(const std::unordered_map<std::string, NodeId>& byName) {
    return [&byName](const std::string& n) -> Variant {
        const size_t dot = n.find('.');
        const std::string head = (dot == std::string::npos) ? n : n.substr(0, dot);
        auto it = byName.find(head);
        if (it == byName.end()) return Variant{};
        return resolveRef(std::to_string(it->second) + (dot == std::string::npos ? "" : n.substr(dot)));
    };
}

// ---- the opt-in evaluator over live references -----------------------------
void test_evaluator() {
    SceneGraph g;
    Slider s(g);  s.setValue(0.5f);
    FakeTable t(g);   // cursorRow = 3
    std::unordered_map<std::string, NodeId> byName = {{"sld", s.getNodeId()}, {"tbl", t.getNodeId()}};
    Evaluator ev;
    Evaluator::Resolver R = controlsResolver(byName);

    // pure arithmetic + precedence + unary minus
    assert(near(ev.eval("2 + 3 * 4", R), 14.0));
    assert(near(ev.eval("(2 + 3) * 4", R), 20.0));
    assert(near(ev.eval("2 ^ 3 ^ 2", R), 512.0));    // right-assoc
    assert(near(ev.eval("-2 + 5", R), 3.0));
    assert(near(ev.eval("10 - -3", R), 13.0));

    // references mixed with math (bare path and bracketed form both work)
    assert(near(ev.eval("tbl.cursorRow * 3", R), 9.0));
    assert(near(ev.eval("[sld.value] + 0.25", R), 0.75));
    assert(near(ev.eval("tbl.cursorRow * 3 > 2", R), 1.0));
    assert(near(ev.eval("tbl.y.sigid", R), 12.0));
    assert(near(ev.eval("tbl.yAxis[3]", R), 2000.0));

    // functions
    assert(near(ev.eval("clamp(15, 0, 10)", R), 10.0));
    assert(near(ev.eval("select([sld.value] > 0.4, 100, 200)", R), 100.0));
    assert(near(ev.eval("lerp(0.5, 10, 20)", R), 15.0));
    assert(near(ev.eval("max(min(3,7), 2)", R), 3.0));

    // faults degrade to NaN, never throw
    assert(std::isnan(ev.eval("1 + ", R)));
    assert(std::isnan(ev.eval("ghost.value + 1", R)));   // unresolved reference
    std::cout << "test_evaluator passed\n";
}

// ---- the resolver registry: framework controls + a client-provided domain ----
void test_registry() {
    SceneGraph g;
    Slider s(g);  s.setValue(0.5f);
    FakeTable t(g);
    std::unordered_map<std::string, NodeId> byName = {{"sld", s.getNodeId()}, {"tbl", t.getNodeId()}};

    Evaluator ev;
    ev.registerResolver("controls", controlsResolver(byName), /*priority*/ 10);

    std::unordered_map<std::string, double> cfg = {{"fuel.kp", 4.0}, {"fuel.ki", 0.2}};
    ev.registerResolver("config", [&](const std::string& n) -> Variant {
        auto it = cfg.find(n);
        return it == cfg.end() ? Variant{} : Variant(it->second);
    }, /*priority*/ 5, /*sigil*/ '#');

    assert(near(ev.eval("tbl.cursorRow * fuel.kp"), 12.0));
    assert(near(ev.eval("[sld.value] + fuel.ki"), 0.7));
    assert(near(ev.eval("[#fuel.kp] + 1"), 5.0));            // sigil forces config
    assert(near(ev.eval("tbl.cursorRow * 3 > fuel.kp"), 1.0));
    assert(std::isnan(ev.eval("nope.field")));               // in no domain
    std::cout << "test_registry passed\n";
}

// ---- precompile: parse once, run many against changing live values ----------
void test_precompile() {
    SceneGraph g;
    Slider s(g);
    std::unordered_map<std::string, NodeId> byName = {{"sld", s.getNodeId()}};
    Evaluator::Resolver R = controlsResolver(byName);

    Evaluator ev;
    Evaluator::Program p = ev.compile("[sld.value] * 100");
    assert(p.ok);
    s.setValue(0.25f);  assert(near(Evaluator::run(p, R), 25.0));
    s.setValue(0.50f);  assert(near(Evaluator::run(p, R), 50.0));   // same Program, fresh value

    // the dependency set is exposed for an app to build a graph / subscribe
    assert(p.references().size() == 1 && p.references()[0] == "sld.value");

    // compile failure is reported with a caret position, not a throw
    Evaluator::Program bad = ev.compile("1 + * 2");
    assert(!bad.ok);
    assert(!bad.error.empty());
    assert(bad.errorPos >= 0);
    std::cout << "test_precompile passed\n";
}

// ---- thread-safety: one immutable Program, hammered from many threads --------
void test_threadsafe() {
    Evaluator ev;
    Evaluator::Program p = ev.compile("[%x] * 2 + 1");
    assert(p.ok);

    std::atomic<int> failures{0};
    auto worker = [&](double xv) {
        for (int i = 0; i < 50000; ++i) {
            double r = Evaluator::run(p, [xv](const std::string&) { return Variant(xv); });
            if (!near(r, xv * 2 + 1)) ++failures;
        }
    };
    std::vector<std::thread> ts;
    for (int t = 0; t < 8; ++t) ts.emplace_back(worker, static_cast<double>(t));
    for (auto& th : ts) th.join();
    assert(failures.load() == 0);   // no cross-talk -> run() is pure
    std::cout << "test_threadsafe passed\n";
}

// ---- APP layer (NOT framework): derived values referencing each other, with
//      per-call memo + cycle guard. The framework's Evaluator knows nothing of it. ----
class DerivedStore {
public:
    explicit DerivedStore(const Evaluator& ev) : m_ev(ev) {}
    void define(const std::string& name, const std::string& expr) { m_defs[name] = m_ev.compile(expr); }

    // Per-call context — owns the cycle set + memo, so evaluation is reentrant and
    // thread-safe (no mutable state lives on the node, unlike omnidyno's Channel).
    struct Ctx { std::unordered_set<std::string> active; std::unordered_map<std::string, double> memo; };

    double evaluate(const std::string& name, Ctx& ctx) const {
        auto m = ctx.memo.find(name);
        if (m != ctx.memo.end()) return m->second;                      // memo
        if (ctx.active.count(name)) return std::numeric_limits<double>::quiet_NaN();  // cycle
        auto d = m_defs.find(name);
        if (d == m_defs.end()) return std::numeric_limits<double>::quiet_NaN();       // undefined
        ctx.active.insert(name);
        double v = Evaluator::run(d->second, [this, &ctx](const std::string& tok) -> Variant {
            const std::string nm = (!tok.empty() && tok[0] == '#') ? tok.substr(1) : tok;
            double r = evaluate(nm, ctx);                              // recurse (framework reenters run)
            return std::isnan(r) ? Variant{} : Variant(r);
        });
        ctx.active.erase(name);
        ctx.memo[name] = v;
        return v;
    }
private:
    const Evaluator& m_ev;
    std::unordered_map<std::string, Evaluator::Program> m_defs;
};

void test_app_derived_cycle() {
    Evaluator ev;

    DerivedStore store(ev);
    store.define("a", "[#b] * 3");
    store.define("b", "[#c] + 1");
    store.define("c", "10");
    DerivedStore::Ctx ctx;
    assert(near(store.evaluate("a", ctx), 33.0));   // (10 + 1) * 3, c memoised

    DerivedStore cyc(ev);
    cyc.define("x", "[#y] + 1");
    cyc.define("y", "[#x] * 2");
    DerivedStore::Ctx ctx2;
    assert(std::isnan(cyc.evaluate("x", ctx2)));    // cycle -> NaN, no infinite recursion
    std::cout << "test_app_derived_cycle passed\n";
}

int main() {
    test_base_keys();
    test_typed_value();
    test_unknown_and_dangling();
    test_nested_paths();
    test_evaluator();
    test_registry();
    test_precompile();
    test_threadsafe();
    test_app_derived_cycle();
    std::cout << "All reference tests passed\n";
    return 0;
}
