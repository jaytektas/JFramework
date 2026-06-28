#include <genesis/db/Database.h>
#include <cassert>
#include <iostream>
#include <vector>
#include <string>

using namespace Genesis;
using B = Database::Bind;

void test_open_memory() {
    Database db;
    assert(db.open(":memory:"));
    assert(db.isOpen());
    db.close();
    assert(!db.isOpen());
    std::cout << "test_open_memory passed\n";
}

void test_create_insert_query() {
    Database db;
    db.open(":memory:");
    assert(db.exec("CREATE TABLE sensors (id INTEGER PRIMARY KEY, name TEXT, value REAL)"));
    assert(db.exec("INSERT INTO sensors VALUES (1, 'rpm',  3500.0)", {}));
    assert(db.exec("INSERT INTO sensors VALUES (2, 'temp', 98.6)",   {}));

    std::vector<std::string> names;
    std::vector<double>      vals;
    int rows = db.query("SELECT name, value FROM sensors ORDER BY id", {},
        [&](const Database::Row& r) {
            names.push_back(r.text(0));
            vals.push_back(r.real(1));
        });
    assert(rows == 2);
    assert(names[0] == "rpm"  && vals[0] == 3500.0);
    assert(names[1] == "temp" && vals[1] == 98.6);
    std::cout << "test_create_insert_query passed\n";
}

void test_parameterised_query() {
    Database db;
    db.open(":memory:");
    db.exec("CREATE TABLE t (id INTEGER, val TEXT)");
    db.exec("INSERT INTO t VALUES (?, ?)", {B::from(1), B::from("hello")});
    db.exec("INSERT INTO t VALUES (?, ?)", {B::from(2), B::from("world")});

    std::string got;
    db.query("SELECT val FROM t WHERE id = ?", {B::from(1)},
        [&](const Database::Row& r){ got = r.text(0); });
    assert(got == "hello");
    std::cout << "test_parameterised_query passed\n";
}

void test_last_insert_row_id() {
    Database db;
    db.open(":memory:");
    db.exec("CREATE TABLE t (id INTEGER PRIMARY KEY AUTOINCREMENT, name TEXT)");
    db.exec("INSERT INTO t (name) VALUES (?)", {B::from("first")});
    assert(db.lastInsertRowId() == 1);
    db.exec("INSERT INTO t (name) VALUES (?)", {B::from("second")});
    assert(db.lastInsertRowId() == 2);
    std::cout << "test_last_insert_row_id passed\n";
}

void test_transaction_commit() {
    Database db;
    db.open(":memory:");
    db.exec("CREATE TABLE t (x INTEGER)");
    bool ok = db.transaction([&]{
        db.exec("INSERT INTO t VALUES (10)", {});
        db.exec("INSERT INTO t VALUES (20)", {});
        return true;
    });
    assert(ok);
    int sum = 0;
    db.query("SELECT x FROM t", {}, [&](const Database::Row& r){ sum += (int)r.integer(0); });
    assert(sum == 30);
    std::cout << "test_transaction_commit passed\n";
}

void test_transaction_rollback() {
    Database db;
    db.open(":memory:");
    db.exec("CREATE TABLE t (x INTEGER)");
    db.exec("INSERT INTO t VALUES (5)", {});

    bool ok = db.transaction([&]{
        db.exec("INSERT INTO t VALUES (100)", {});
        return false;  // request rollback
    });
    assert(!ok);

    int count = 0;
    db.query("SELECT x FROM t", {}, [&](const Database::Row&){ ++count; });
    assert(count == 1);  // only the original row
    std::cout << "test_transaction_rollback passed\n";
}

void test_null_bind() {
    Database db;
    db.open(":memory:");
    db.exec("CREATE TABLE t (id INTEGER, v TEXT)");
    db.exec("INSERT INTO t VALUES (?, ?)", {B::from(1), B::null()});
    db.query("SELECT v FROM t WHERE id=1", {},
        [](const Database::Row& r){ assert(r.isNull(0)); });
    std::cout << "test_null_bind passed\n";
}

int main() {
    test_open_memory();
    test_create_insert_query();
    test_parameterised_query();
    test_last_insert_row_id();
    test_transaction_commit();
    test_transaction_rollback();
    test_null_bind();
    std::cout << "All Database tests passed!\n";
    return 0;
}
