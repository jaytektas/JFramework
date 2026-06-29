#include <genesis/core/Variant.h>
#include <genesis/core/MetaObject.h>
#include <genesis/core/VariantJson.h>
#include <cassert>
#include <iostream>
#include <string>

using namespace Genesis;

void test_scalars_and_type() {
    JVariant n;                 assert(n.isNull() && !n.isValid());
    JVariant b = true;          assert(b.isBool()   && b.type() == JVariantType::Bool);
    JVariant i = 42;            assert(i.isInt()    && i.toInt() == 42);
    JVariant d = 3.5;           assert(i.isInt() && d.isDouble() && d.toDouble() == 3.5);
    JVariant s = "torque";      assert(s.isString() && s.toString() == "torque");
    // bool/const char* pitfalls must not mis-resolve
    assert(JVariant("x").isString());
    assert(JVariant(0).isInt());
    assert(JVariant(false).isBool());
    std::cout << "test_scalars_and_type passed\n";
}

void test_coercion() {
    assert(JVariant("115200").toInt() == 115200);
    assert(JVariant("3.14").toDouble() == 3.14);
    assert(JVariant("true").toBool() == true);
    assert(JVariant("off").toBool() == false);
    assert(JVariant(1).toBool() == true);
    assert(JVariant(42).toString() == "42");
    assert(JVariant(true).toString() == "true");
    // tryValue does NOT coerce
    assert(JVariant("5").tryValue<int>() == std::nullopt);
    assert(JVariant(5).tryValue<int>() == 5);
    assert(JVariant(5).tryValue<double>() == 5.0);   // int widens to double losslessly
    std::cout << "test_coercion passed\n";
}

void test_containers() {
    JVariant l = JVariant::list({1, "two", true});
    assert(l.isList() && l.size() == 3);
    assert(l.toList()[0].toInt() == 1);
    assert(l.toList()[1].toString() == "two");

    JVariant m = JVariant::map({{"port", "/dev/ttyUSB0"}, {"baud", 115200}});
    assert(m.isMap() && m.size() == 2);
    assert(m.contains("port") && !m.contains("missing"));
    assert(m.at("baud").toInt() == 115200);
    assert(m.at("missing").isNull());
    m.set("baud", 9600);                  // update existing
    assert(m.at("baud").toInt() == 9600);
    m.set("rts", true);                   // insert new
    assert(m.size() == 3 && m.at("rts").toBool());
    std::cout << "test_containers passed\n";
}

void test_custom_and_equality() {
    struct Pt { int x, y; };
    JVariant c = JVariant::custom(Pt{3, 4});
    assert(c.isCustom());
    const Pt* p = c.customPtr<Pt>();
    assert(p && p->x == 3 && p->y == 4);
    assert(c.customPtr<int>() == nullptr);

    assert(JVariant(5) == JVariant(5));
    assert(JVariant(5) == JVariant(5.0));          // cross-numeric
    assert(JVariant("a") != JVariant("b"));
    assert(JVariant() == JVariant());
    assert(JVariant::list({1, 2}) == JVariant::list({1, 2}));
    std::cout << "test_custom_and_equality passed\n";
}

void test_property_bag() {
    JPropertyBag bag;
    int hits = 0;
    std::string lastKey;
    bag.onPropertyChanged.connect([&](std::string k, JVariant) { ++hits; lastKey = k; });
    bag.setProperty("baud", 115200);
    bag.setProperty("baud", 115200);   // identical -> no signal
    bag.setProperty("port", "/dev/ttyUSB0");
    assert(hits == 2 && lastKey == "port");
    assert(bag.property("baud").toInt() == 115200);
    assert(bag.hasProperty("port") && !bag.hasProperty("nope"));
    assert(bag.propertyNames().size() == 2);
    assert(bag.removeProperty("baud") && !bag.hasProperty("baud"));
    std::cout << "test_property_bag passed\n";
}

struct JSerialConfig { std::string port; int baud{9600}; bool rts{false}; };

void test_meta_class() {
    auto& mc = JMetaClass<JSerialConfig>::instance();
    mc.field("port", &JSerialConfig::port)
      .field("baud", &JSerialConfig::baud)
      .field("rts",  &JSerialConfig::rts);

    JSerialConfig c;
    assert(mc.get(c, "baud").toInt() == 9600);
    assert(mc.set(c, "baud", 115200));
    assert(c.baud == 115200);
    assert(mc.set(c, "port", "/dev/ttyUSB0") && c.port == "/dev/ttyUSB0");
    assert(mc.set(c, "rts", true) && c.rts == true);
    assert(!mc.set(c, "unknown", 1));        // unknown property rejected
    assert(mc.get(c, "unknown").isNull());

    VariantMap snap = mc.toMap(c);
    assert(snap.size() == 3);

    JSerialConfig c2;
    mc.fromMap(c2, snap);
    assert(c2.baud == 115200 && c2.port == "/dev/ttyUSB0" && c2.rts == true);
    std::cout << "test_meta_class passed\n";
}

void test_json_bridge() {
    JVariant v = fromJson(JJson::parse(R"({"port":"/dev/ttyUSB0","baud":115200,"gain":3.14,"on":true,"tags":[1,2,3]})"));
    assert(v.isMap());
    assert(v.at("port").toString() == "/dev/ttyUSB0");
    assert(v.at("baud").isInt() && v.at("baud").toInt() == 115200);   // integral -> Int
    assert(v.at("gain").isDouble() && v.at("gain").toDouble() == 3.14);
    assert(v.at("on").toBool() == true);
    assert(v.at("tags").isList() && v.at("tags").size() == 3);

    // Round-trip back to JSON and re-parse.
    JJson j = toJson(v);
    JVariant v2 = fromJson(JJson::parse(j.dump()));
    assert(v2.at("baud").toInt() == 115200);
    assert(v2.at("tags").toList()[2].toInt() == 3);

    // Reflected object -> JSON.
    JSerialConfig c{"/dev/ttyACM0", 57600, true};
    JJson cj = toJson(JVariant(JMetaClass<JSerialConfig>::instance().toMap(c)));
    assert(cj["port"].str() == "/dev/ttyACM0");
    assert(cj["baud"].number<int>() == 57600);
    std::cout << "test_json_bridge passed\n";
}

int main() {
    test_scalars_and_type();
    test_coercion();
    test_containers();
    test_custom_and_equality();
    test_property_bag();
    test_meta_class();
    test_json_bridge();
    std::cout << "All JVariant/MetaObject tests passed.\n";
    return 0;
}
