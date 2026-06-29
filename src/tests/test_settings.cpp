#include <genesis/config/Settings.h>
#include <cassert>
#include <iostream>
#include <cstdio>
#include <filesystem>

using namespace Genesis;

static std::filesystem::path tmpFile() {
    return std::filesystem::temp_directory_path() / "genesis_test_settings.ini";
}

void test_set_get() {
    auto& s = JSettings::instance();
    s.clear();
    s.set("name",    std::string("Genesis"));
    s.set("count",   42);
    s.set("ratio",   3.14);
    s.set("enabled", true);

    assert(s.get<std::string>("name")    == "Genesis");
    assert(s.get<int>("count")           == 42);
    assert(s.get<double>("ratio")        == 3.14);
    assert(s.get<bool>("enabled")        == true);
    assert(s.get<std::string>("missing", "def") == "def");
    assert(s.get<int>("missing", 99)     == 99);
    std::cout << "test_set_get passed\n";
}

void test_has_remove() {
    auto& s = JSettings::instance();
    s.clear();
    s.set("x", 1);
    assert(s.has("x"));
    s.remove("x");
    assert(!s.has("x"));
    std::cout << "test_has_remove passed\n";
}

void test_persist_round_trip() {
    auto path = tmpFile();
    std::filesystem::remove(path);

    {
        auto& s = JSettings::instance();
        s.clear();
        s.setPath(path);
        s.set("serial.port", std::string("/dev/ttyUSB0"));
        s.set("serial.baud",  115200);
        s.set("ui.theme",    std::string("dark"));
        s.set("ui.scale",    2.0);
        s.save();
    }

    assert(std::filesystem::exists(path));

    {
        auto& s = JSettings::instance();
        s.clear();
        s.setPath(path);
        s.load();
        assert(s.get<std::string>("serial.port") == "/dev/ttyUSB0");
        assert(s.get<int>("serial.baud")         == 115200);
        assert(s.get<std::string>("ui.theme")    == "dark");
        assert(std::abs(s.get<double>("ui.scale") - 2.0) < 0.001);
    }

    std::filesystem::remove(path);
    std::cout << "test_persist_round_trip passed\n";
}

void test_bool_variants() {
    auto& s = JSettings::instance();
    s.clear();
    s.set("a", true);
    s.set("b", false);
    assert(s.get<bool>("a") == true);
    assert(s.get<bool>("b") == false);

    // "1" and "yes" are truthy
    s.set("c", std::string("1"));
    s.set("d", std::string("yes"));
    assert(s.get<bool>("c") == true);
    assert(s.get<bool>("d") == true);
    std::cout << "test_bool_variants passed\n";
}

int main() {
    test_set_get();
    test_has_remove();
    test_persist_round_trip();
    test_bool_variants();
    std::cout << "All JSettings tests passed!\n";
    return 0;
}
