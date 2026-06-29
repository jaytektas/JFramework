#include <genesis/model/TableModel.h>
#include <genesis/model/TreeModel.h>
#include <genesis/model/SortFilterModel.h>
#include <genesis/core/MainThreadDispatcher.h>
#include <cassert>
#include <iostream>
#include <thread>
#include <chrono>

using namespace jf;

// ---------------------------------------------------------------------------
// JTableModel
// ---------------------------------------------------------------------------

void test_table_basic() {
    JTableModel m;
    m.setHeaders({"Name", "Value"});
    assert(m.columnCount() == 2);
    assert(m.rowCount() == 0);

    m.append({"Torque", "42"});
    m.append({"RPM", "3000"});
    assert(m.rowCount() == 2);
    assert(m.cell(0, 0) == "Torque");
    assert(m.cell(1, 1) == "3000");

    m.set(0, {"Torque", "45"});
    assert(m.cell(0, 1) == "45");

    m.set(1, 0, "Speed");
    assert(m.cell(1, 0) == "Speed");

    m.remove(0);
    assert(m.rowCount() == 1);
    assert(m.cell(0, 0) == "Speed");

    m.insert(0, {"First", "1"});
    assert(m.cell(0, 0) == "First");
    assert(m.cell(1, 0) == "Speed");

    m.clear();
    assert(m.rowCount() == 0);
    std::cout << "test_table_basic passed\n";
}

void test_table_sort() {
    JTableModel m;
    m.setHeaders({"JKey"});
    m.append({"banana"});
    m.append({"apple"});
    m.append({"cherry"});

    m.sort(0);
    assert(m.cell(0, 0) == "apple");
    assert(m.cell(1, 0) == "banana");
    assert(m.cell(2, 0) == "cherry");

    m.sort(0, false);
    assert(m.cell(0, 0) == "cherry");
    assert(m.cell(2, 0) == "apple");
    std::cout << "test_table_sort passed\n";
}

void test_table_batch() {
    JTableModel m;
    int changeCount = 0;
    m.onChanged.connect([&]{ ++changeCount; });

    m.beginBatch();
    for (int i = 0; i < 5; ++i)
        m.append({std::to_string(i)});
    m.endBatch();

    // Drain the single posted notification.
    JMainThreadDispatcher::instance().drain();
    assert(changeCount == 1);
    assert(m.rowCount() == 5);
    std::cout << "test_table_batch passed\n";
}

void test_table_onchanged_fires_on_main_thread() {
    JTableModel m;
    bool fired = false;
    m.onChanged.connect([&]{ fired = true; });

    // Mutate from a background thread.
    std::thread([&m]{ m.append({"x"}); }).join();

    // Not fired yet — posted to main thread dispatcher.
    assert(!fired);
    JMainThreadDispatcher::instance().drain();
    assert(fired);
    std::cout << "test_table_onchanged_fires_on_main_thread passed\n";
}

// ---------------------------------------------------------------------------
// JSortFilterModel
// ---------------------------------------------------------------------------

void test_sortfilter_filter() {
    JTableModel m;
    m.setHeaders({"Value"});
    m.append({"10"});
    m.append({"5"});
    m.append({"20"});
    m.append({"1"});

    JSortFilterModel proxy(m);
    proxy.setFilter([](const auto& row){ return row[0] >= "10"; }); // lexicographic ≥ "10"
    // "10", "20" pass; "5", "1" don't pass lexicographically... actually "5" > "10"
    // Let's use a length filter instead
    proxy.clearFilter();
    proxy.setFilter([](const auto& row){ return row[0].size() == 2; }); // "10", "20"
    assert(proxy.rowCount() == 2);
    assert(proxy.row(0)[0] == "10");
    assert(proxy.row(1)[0] == "20");

    proxy.clearFilter();
    assert(proxy.rowCount() == 4);
    std::cout << "test_sortfilter_filter passed\n";
}

void test_sortfilter_sort() {
    JTableModel m;
    m.setHeaders({"Name"});
    m.append({"Charlie"});
    m.append({"Alice"});
    m.append({"Bob"});

    JSortFilterModel proxy(m);
    proxy.sort(0);
    assert(proxy.row(0)[0] == "Alice");
    assert(proxy.row(1)[0] == "Bob");
    assert(proxy.row(2)[0] == "Charlie");

    proxy.sort(0, false);
    assert(proxy.row(0)[0] == "Charlie");

    proxy.clearSort();
    assert(proxy.row(0)[0] == "Charlie"); // original source order
    std::cout << "test_sortfilter_sort passed\n";
}

void test_sortfilter_live_update() {
    JTableModel m;
    m.setHeaders({"X"});
    m.append({"a"});
    m.append({"b"});

    JSortFilterModel proxy(m);
    assert(proxy.rowCount() == 2);

    // Add a row to source — proxy should update on next drain.
    int proxyChanged = 0;
    proxy.onChanged.connect([&]{ ++proxyChanged; });

    m.append({"c"});
    JMainThreadDispatcher::instance().drain();
    assert(proxy.rowCount() == 3);
    assert(proxyChanged >= 1);
    std::cout << "test_sortfilter_live_update passed\n";
}

void test_sortfilter_source_row_mapping() {
    JTableModel m;
    m.setHeaders({"V"});
    m.append({"b"});
    m.append({"a"});
    m.append({"c"});

    JSortFilterModel proxy(m);
    proxy.sort(0);  // sorted: a(1), b(0), c(2)

    assert(proxy.sourceRow(0) == 1); // "a" is source row 1
    assert(proxy.sourceRow(1) == 0); // "b" is source row 0
    assert(proxy.sourceRow(2) == 2); // "c" is source row 2
    std::cout << "test_sortfilter_source_row_mapping passed\n";
}

// ---------------------------------------------------------------------------
// JTreeModel
// ---------------------------------------------------------------------------

void test_tree_insert_find_remove() {
    JTreeModel m;
    m.insert("", {"lib0", "Library A"});
    m.insert("", {"lib1", "Library B"});
    m.insert("lib0", {"run0", "Run 1"});
    m.insert("lib0", {"run1", "Run 2"});

    assert(m.root().children.size() == 2);
    assert(m.find("lib0") != nullptr);
    assert(m.find("run1") != nullptr);
    assert(m.find("lib0")->children.size() == 2);
    assert(m.find("xyz") == nullptr);

    m.remove("run0");
    assert(m.find("lib0")->children.size() == 1);
    assert(m.find("run0") == nullptr);

    m.remove("lib1");
    assert(m.root().children.size() == 1);

    m.clear();
    assert(m.root().children.empty());
    std::cout << "test_tree_insert_find_remove passed\n";
}

void test_tree_set_label_expanded() {
    JTreeModel m;
    m.insert("", {"n0", "Original"});
    m.setLabel("n0", "Renamed");
    assert(m.find("n0")->label == "Renamed");

    assert(!m.find("n0")->expanded);
    m.setExpanded("n0", true);
    assert(m.find("n0")->expanded);
    std::cout << "test_tree_set_label_expanded passed\n";
}

void test_tree_tag() {
    JTreeModel m;
    m.insert("", {"n0", "Node", "payload:42"});
    assert(m.find("n0")->tag == "payload:42");
    m.setTag("n0", "payload:99");
    assert(m.find("n0")->tag == "payload:99");
    std::cout << "test_tree_tag passed\n";
}

void test_tree_onchanged() {
    JTreeModel m;
    int changed = 0;
    m.onChanged.connect([&]{ ++changed; });

    m.insert("", {"n0", "Node"});
    JMainThreadDispatcher::instance().drain();
    assert(changed >= 1);
    std::cout << "test_tree_onchanged passed\n";
}

// ---------------------------------------------------------------------------
// JSlotTracker disconnect (JSignal.h regression)
// ---------------------------------------------------------------------------

void test_signal_connect_returns_disconnect() {
    jf::JSignal<int> sig;
    int count = 0;
    auto disc = sig.connect([&](int){ ++count; });

    sig.emit(1);
    assert(count == 1);

    disc(); // disconnect
    sig.emit(2);
    assert(count == 1); // should not have incremented
    std::cout << "test_signal_connect_returns_disconnect passed\n";
}

void test_slot_tracker_disconnect() {
    jf::JSignal<> sig;
    int count = 0;
    {
        jf::JSlotTracker tracker;
        tracker.addConnection(sig.connect([&]{ ++count; }));
        sig.emit();
        assert(count == 1);
    } // tracker destroyed → slot disconnected
    sig.emit();
    assert(count == 1);
    std::cout << "test_slot_tracker_disconnect passed\n";
}

// ---------------------------------------------------------------------------

int main() {
    test_table_basic();
    test_table_sort();
    test_table_batch();
    test_table_onchanged_fires_on_main_thread();

    test_sortfilter_filter();
    test_sortfilter_sort();
    test_sortfilter_live_update();
    test_sortfilter_source_row_mapping();

    test_tree_insert_find_remove();
    test_tree_set_label_expanded();
    test_tree_tag();
    test_tree_onchanged();

    test_signal_connect_returns_disconnect();
    test_slot_tracker_disconnect();

    std::cout << "All model tests passed!\n";
    return 0;
}
