#include <genesis/config/Json.h>
#include <cassert>
#include <iostream>
#include <cmath>

using namespace jf;

void test_parse_primitives() {
    assert(JJson::parse("null").isNull());
    assert(JJson::parse("true").boolean() == true);
    assert(JJson::parse("false").boolean() == false);
    assert(JJson::parse("42").number() == 42.0);
    assert(JJson::parse("3.14").number<double>() == 3.14);
    assert(JJson::parse("-7").number<int>() == -7);
    assert(JJson::parse("\"hello\"").str() == "hello");
    std::cout << "test_parse_primitives passed\n";
}

void test_parse_object() {
    auto j = JJson::parse(R"({"port":"/dev/ttyUSB0","baud":115200,"enabled":true})");
    assert(j.isObject());
    assert(j["port"].str() == "/dev/ttyUSB0");
    assert(j["baud"].number<int>() == 115200);
    assert(j["enabled"].boolean() == true);
    assert(j.contains("port"));
    assert(!j.contains("missing"));
    const JJson& cj = j;
    assert(cj["missing"].isNull());  // const operator[] doesn't insert
    std::cout << "test_parse_object passed\n";
}

void test_parse_array() {
    auto j = JJson::parse(R"([1, "two", true, null])");
    assert(j.isArray());
    assert(j.size() == 4);
    assert(j[0].number<int>() == 1);
    assert(j[1].str() == "two");
    assert(j[2].boolean() == true);
    assert(j[3].isNull());
    std::cout << "test_parse_array passed\n";
}

void test_parse_nested() {
    auto j = JJson::parse(R"({"sensors":[{"id":1,"val":3.14},{"id":2,"val":2.72}]})");
    assert(j["sensors"][0]["id"].number<int>() == 1);
    assert(j["sensors"][1]["val"].number<double>() == 2.72);
    std::cout << "test_parse_nested passed\n";
}

void test_build_and_dump() {
    JJson obj = JJson::object();
    obj["name"] = JJson("Genesis");
    obj["version"] = JJson(1.0);
    obj["active"] = JJson(true);

    JJson arr = JJson::array();
    arr.push(JJson(10.0));
    arr.push(JJson(20.0));
    obj["ports"] = arr;

    std::string s = obj.dump();
    // round-trip
    auto j2 = JJson::parse(s);
    assert(j2["name"].str() == "Genesis");
    assert(j2["version"].number<int>() == 1);
    assert(j2["active"].boolean() == true);
    assert(j2["ports"][0].number<int>() == 10);
    assert(j2["ports"][1].number<int>() == 20);
    std::cout << "test_build_and_dump passed\n";
}

void test_escape_handling() {
    auto j = JJson::parse(R"({"msg":"line1\nline2\ttabbed"})");
    assert(j["msg"].str().find('\n') != std::string::npos);
    assert(j["msg"].str().find('\t') != std::string::npos);

    JJson obj = JJson::object();
    obj["q"] = JJson("say \"hi\"");
    std::string s = obj.dump();
    auto j2 = JJson::parse(s);
    assert(j2["q"].str() == "say \"hi\"");
    std::cout << "test_escape_handling passed\n";
}

void test_pretty_print() {
    auto j = JJson::parse(R"({"a":1,"b":[2,3]})");
    std::string pretty = j.dump(2);
    // Must contain a newline and indentation
    assert(pretty.find('\n') != std::string::npos);
    assert(pretty.find("  ") != std::string::npos);
    // Must still round-trip
    auto j2 = JJson::parse(pretty);
    assert(j2["a"].number<int>() == 1);
    std::cout << "test_pretty_print passed\n";
}

int main() {
    test_parse_primitives();
    test_parse_object();
    test_parse_array();
    test_parse_nested();
    test_build_and_dump();
    test_escape_handling();
    test_pretty_print();
    std::cout << "All JJson tests passed!\n";
    return 0;
}
