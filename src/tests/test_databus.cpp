#include <j/core/DataBus.h>
#include <j/core/SceneGraph.h>
#include <j/core/BaseWidgets.h>
#include <cassert>
#include <iostream>

using namespace jf;

// Minimal JCanvasWidget-like subclass for testing bind()
class TestWidget : public JWidget {
public:
    double dval{0};
    float  fval{0};
    int    ival{0};
    std::string sval;

    TestWidget(JSceneGraph& g) : JWidget(g, "TestWidget") {}
    void populateRenderPrimitives(JPrimitiveBuffer&) override {}
    JAISemanticNode getSemanticNode() const override { return {"TestWidget","","",false}; }
    bool executeSemanticAction(const std::string&) override { return false; }
};

void test_publish_subscribe_double() {
    auto& db = JDataBus::instance(); db.clear();
    double last = -1.0;
    db.subscribe("ch.rpm", [&](double v){ last = v; });
    db.publish("ch.rpm", 3500.0);
    assert(last == 3500.0);
    db.publish("ch.rpm", 0.0);
    assert(last == 0.0);
    std::cout << "test_publish_subscribe_double passed\n";
}

void test_publish_subscribe_string() {
    auto& db = JDataBus::instance(); db.clear();
    std::string got;
    db.subscribe("ch.status", [&](const std::string& v){ got = v; });
    db.publish("ch.status", std::string("OK"));
    assert(got == "OK");
    std::cout << "test_publish_subscribe_string passed\n";
}

void test_last_value_cache() {
    auto& db = JDataBus::instance(); db.clear();
    db.publish("sensor.temp", 98.6);
    assert(db.lastDouble("sensor.temp") == 98.6);
    assert(db.lastDouble("sensor.missing", -1.0) == -1.0);

    db.publish("device.name", std::string("probe-1"));
    assert(db.lastString("device.name") == "probe-1");
    assert(db.lastString("device.none", "def") == "def");
    std::cout << "test_last_value_cache passed\n";
}

void test_bind_double() {
    JSceneGraph graph;
    TestWidget w(graph);
    auto& l = graph.getLayout(w.getNodeId());
    l.boundingBox = {0,0,100,30};

    auto& db = JDataBus::instance(); db.clear();
    db.bind("motor.rpm", &w.dval, &w);
    db.publish("motor.rpm", 1234.5);
    assert(w.dval == 1234.5);
    std::cout << "test_bind_double passed\n";
}

void test_bind_float() {
    JSceneGraph graph;
    TestWidget w(graph);
    auto& db = JDataBus::instance(); db.clear();
    db.bind("gauge.pct", &w.fval, &w);
    db.publish("gauge.pct", 0.75);
    assert(std::abs(w.fval - 0.75f) < 1e-5f);
    std::cout << "test_bind_float passed\n";
}

void test_bind_int() {
    JSceneGraph graph;
    TestWidget w(graph);
    auto& db = JDataBus::instance(); db.clear();
    db.bind("adc.raw", &w.ival, &w);
    db.publish("adc.raw", 1023.0);
    assert(w.ival == 1023);
    std::cout << "test_bind_int passed\n";
}

void test_bind_string() {
    JSceneGraph graph;
    TestWidget w(graph);
    auto& db = JDataBus::instance(); db.clear();
    db.bind("status", &w.sval, &w);
    db.publish("status", std::string("running"));
    assert(w.sval == "running");
    std::cout << "test_bind_string passed\n";
}

void test_unsubscribe() {
    auto& db = JDataBus::instance(); db.clear();
    int count = 0;
    auto id = db.subscribe("ch", [&](double){ ++count; });
    db.publish("ch", 1.0);
    assert(count == 1);
    db.unsubscribe("ch", id);
    db.publish("ch", 2.0);
    assert(count == 1);
    std::cout << "test_unsubscribe passed\n";
}

void test_multiple_subscribers() {
    auto& db = JDataBus::instance(); db.clear();
    int a = 0, b = 0;
    db.subscribe("ch", [&](double){ ++a; });
    db.subscribe("ch", [&](double){ ++b; });
    db.publish("ch", 1.0);
    assert(a == 1 && b == 1);
    std::cout << "test_multiple_subscribers passed\n";
}

int main() {
    test_publish_subscribe_double();
    test_publish_subscribe_string();
    test_last_value_cache();
    test_bind_double();
    test_bind_float();
    test_bind_int();
    test_bind_string();
    test_unsubscribe();
    test_multiple_subscribers();
    std::cout << "All JDataBus tests passed!\n";
    return 0;
}
