// Integration tests for Variant adoption across Settings + models (goal 1-3).
#include <genesis/config/Settings.h>
#include <genesis/model/TableModel.h>
#include <genesis/model/SortFilterModel.h>
#include <genesis/model/TreeModel.h>
#include <genesis/core/VariantJson.h>
#include <cassert>
#include <iostream>
#include <filesystem>

using namespace Genesis;

// (1) Settings now stores typed Variants and JSON persistence preserves types.
void test_settings_typed_json() {
    auto path = std::filesystem::temp_directory_path() / "genesis_settings_typed.json";
    std::filesystem::remove(path);

    Settings a;
    a.setPath(path);
    a.set("serial.port", "/dev/ttyUSB0");   // string
    a.set("serial.baud", 115200);            // int
    a.set("ui.scale", 2.5);                  // double
    a.set("ui.dark", true);                  // bool
    assert(a.value("serial.baud").isInt());
    assert(a.value("ui.scale").isDouble());
    assert(a.saveJson());

    Settings b;
    b.setPath(path).loadJson();
    assert(b.value("serial.baud").isInt() && b.get<int>("serial.baud") == 115200);
    assert(b.value("ui.scale").isDouble() && b.get<double>("ui.scale") == 2.5);
    assert(b.value("ui.dark").isBool() && b.get<bool>("ui.dark") == true);
    assert(b.get<std::string>("serial.port") == "/dev/ttyUSB0");
    // Legacy coercion still works (string API unchanged).
    assert(b.get<std::string>("serial.baud") == "115200");
    std::filesystem::remove(path);
    std::cout << "test_settings_typed_json passed\n";
}

// (2a) TableModel numeric-aware sort (numeric strings sort by value, not lexically).
void test_table_numeric_sort() {
    TableModel m;
    m.setHeaders({"name", "value"});
    m.append({"b", "9"});
    m.append({"a", "100"});
    m.append({"c", "42"});
    m.sort(1, true);                        // ascending by numeric column
    assert(m.cell(0, 1) == "9");
    assert(m.cell(1, 1) == "42");
    assert(m.cell(2, 1) == "100");          // lexical sort would put "100" first
    std::cout << "test_table_numeric_sort passed\n";
}

// (2b) Typed cells round-trip via the Variant API.
void test_table_typed_cells() {
    TableModel m;
    m.setHeaders({"name", "power"});
    m.appendVar({"Engine", 320.5});
    assert(m.cellVar(0, 1).isDouble() && m.cellVar(0, 1).toDouble() == 320.5);
    assert(m.cell(0, 1) == "320.5");        // string view still works
    std::cout << "test_table_typed_cells passed\n";
}

// (2c) SortFilterModel sorts numerically over typed cells (rebuild is synchronous).
void test_sortfilter_numeric() {
    TableModel m;
    m.setHeaders({"id", "rpm"});
    m.appendVar({"x", 9});
    m.appendVar({"y", 100});
    m.appendVar({"z", 42});
    SortFilterModel proxy(m);
    proxy.sort(1, false);                    // descending numeric
    auto rows = proxy.rows();
    assert(rows.size() == 3);
    assert(rows[0][1] == "100" && rows[1][1] == "42" && rows[2][1] == "9");
    std::cout << "test_sortfilter_numeric passed\n";
}

// (2d) TreeItem carries a typed Variant payload.
void test_tree_typed_value() {
    TreeModel t;
    t.insert("", {"run1", "Run 1"});
    assert(t.setValue("run1", 3.14));
    const TreeItem* n = t.find("run1");
    assert(n && n->value.isDouble() && n->value.toDouble() == 3.14);
    assert(n->label == "Run 1" && n->tag.empty());  // existing fields intact
    std::cout << "test_tree_typed_value passed\n";
}

// (3) Reflected-object serialize/deserialize via MetaClass + JSON.
struct DeviceCfg { std::string name; int channel{0}; double gain{1.0}; bool active{false}; };

void test_object_serialization() {
    MetaClass<DeviceCfg>::instance()
        .field("name",    &DeviceCfg::name)
        .field("channel", &DeviceCfg::channel)
        .field("gain",    &DeviceCfg::gain)
        .field("active",  &DeviceCfg::active);

    DeviceCfg a{"dyno", 3, 2.5, true};
    Json j = serialize(a);
    assert(j["name"].str() == "dyno");
    assert(j["channel"].number<int>() == 3);

    DeviceCfg b;
    deserialize(b, Json::parse(j.dump()));
    assert(b.name == "dyno" && b.channel == 3 && b.gain == 2.5 && b.active == true);
    std::cout << "test_object_serialization passed\n";
}

int main() {
    test_settings_typed_json();
    test_table_numeric_sort();
    test_table_typed_cells();
    test_sortfilter_numeric();
    test_tree_typed_value();
    test_object_serialization();
    std::cout << "All Variant integration tests passed.\n";
    return 0;
}
