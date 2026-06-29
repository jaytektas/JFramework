#include <genesis/core/DockManager.h>
#include <genesis/core/DockRegistry.h>
#include <cassert>
#include <iostream>

using namespace Genesis;

void test_default_options() {
    // 1. Registry defaults
    auto& reg = JDockRegistry::instance();
    assert(reg.defaultOptions().handleHoverPad.value_or(0.f) == 4.0f);
    assert(reg.defaultOptions().enforceMinSizes.value_or(false) == true);
    assert(reg.defaultOptions().showResizeCursors.value_or(false) == true);

    // 2. Host inheritance
    JDockHost host;
    assert(host.handleHoverPad() == 4.0f);
    assert(host.enforceMinSizes() == true);
    assert(host.showResizeCursors() == true);

    std::cout << "test_default_options passed\n";
}

void test_local_overrides() {
    JDockHost host;

    // Override handleHoverPad
    host.options().handleHoverPad = 8.0f;
    assert(host.handleHoverPad() == 8.0f);
    // Registry should still be 4.0f
    assert(JDockRegistry::instance().defaultOptions().handleHoverPad.value_or(0.f) == 4.0f);

    // Override enforceMinSizes
    host.options().enforceMinSizes = false;
    assert(host.enforceMinSizes() == false);

    // Reset override
    host.options().enforceMinSizes = std::nullopt;
    assert(host.enforceMinSizes() == true);

    std::cout << "test_local_overrides passed\n";
}

void test_dynamic_registry_updates() {
    JDockHost host;

    // Verify initial inheritance
    assert(host.showResizeCursors() == true);

    // Change registry option
    JDockRegistry::instance().defaultOptions().showResizeCursors = false;
    assert(host.showResizeCursors() == false);

    // Reset registry option for other tests
    JDockRegistry::instance().defaultOptions().showResizeCursors = true;
    assert(host.showResizeCursors() == true);

    std::cout << "test_dynamic_registry_updates passed\n";
}

void test_behavioral_options() {
    JDockHost host;
    
    // Add two leaf nodes under root split
    host.setRootSplit(JSplitDir::Horizontal);
    JDockNodeId leafA = host.addLeaf(host.rootId(), "A", 0.5f);
    JDockNodeId leafB = host.addLeaf(host.rootId(), "B", 0.5f);
    
    host.computeLayout({0.f, 0.f, 200.f, 200.f});

    const auto* root = host.node(host.rootId());
    std::cout << "Root node children count: " << root->children.size() << std::endl;
    std::cout << "Root node handleRects count: " << root->handleRects.size() << std::endl;
    for (size_t i = 0; i < root->handleRects.size(); ++i) {
        const auto& r = root->handleRects[i];
        std::cout << "Handle " << i << ": x=" << r.x << ", y=" << r.y << ", w=" << r.width << ", h=" << r.height << std::endl;
    }

    // 1. Hover pads
    // Split handle will be at x = 129.333..135.333 (centered around 132.333)
    // Default hover pad is 4.f, so hit width is 3.f + 4.f = 7.f on each side, x in [125.333..139.333]
    host.options().handleHoverPad = 10.0f; // hit width: 3.f + 10.f = 13.f on each side, x in [119.333..145.333]
    
    std::cout << "getHoverCursor(122.f, 100.f): " << (int)host.getHoverCursor(122.f, 100.f) << std::endl;
    // Check hover cursor at x=122 (within 10.f hover pad, but outside default 4.f hover pad)
    host.options().showResizeCursors = true;
    assert(host.getHoverCursor(122.f, 100.f) == JDockHost::JHoverCursor::Horiz);
    
    host.options().handleHoverPad = 2.0f; // hit width: 3.f + 2.f = 5.f on each side, x in [127.333..137.333]
    assert(host.getHoverCursor(122.f, 100.f) == JDockHost::JHoverCursor::Default);

    // 2. Show/hide cursors
    host.options().showResizeCursors = false;
    host.options().handleHoverPad = 10.0f;
    assert(host.getHoverCursor(122.f, 100.f) == JDockHost::JHoverCursor::Default);

    // 3. Minimum size enforcement
    host.options().enforceMinSizes = true;
    float minW_enforced = host.minWidthNeeded();
    assert(minW_enforced > 48.f);
    
    host.options().enforceMinSizes = false;
    assert(host.minWidthNeeded() == 48.f);

    std::cout << "test_behavioral_options passed\n";
}

void test_layout_mutations() {
    JDockHost host;
    
    // Add two leaf nodes under root split
    host.setRootSplit(JSplitDir::Horizontal);
    JDockNodeId leafA = host.addLeaf(host.rootId(), "A", 0.5f);
    JDockNodeId leafB = host.addLeaf(host.rootId(), "B", 0.5f);
    
    host.computeLayout({0.f, 0.f, 206.f, 200.f}); // total horizontal split width including 6px handle: 206
    
    // Split leafA vertically to verify splitLeaf
    JDockNodeId leafA_split = host.splitLeaf(leafA, JDropPos::Bottom);
    assert(leafA_split.valid());

    // Verify edgeLeaf
    JDockNodeId edge = host.edgeLeaf();
    assert(edge.valid());

    // Verify dynamic weight modification sizing math
    JDockNode* parent = host.node(host.rootId());
    assert(parent->splitDir == JSplitDir::Horizontal);
    parent->weights[0] = 0.8f;
    parent->weights[1] = 0.2f;
    host.computeLayout({0.f, 0.f, 206.f, 200.f});
    
    assert(host.node(parent->children[0])->rect.width == 160.f);
    assert(host.node(parent->children[1])->rect.width == 40.f);

    std::cout << "test_layout_mutations passed\n";
}

int main() {
    test_default_options();
    test_local_overrides();
    test_dynamic_registry_updates();
    test_behavioral_options();
    test_layout_mutations();
    std::cout << "All dock options tests passed successfully!\n";
    return 0;
}
