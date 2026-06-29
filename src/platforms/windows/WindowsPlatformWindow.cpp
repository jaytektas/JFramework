#include <j/core/AccessibilityBridge.h>

inline namespace jf {

bool JAccessibilityBridge::start(const std::string& appName) {
    (void)appName;
    m_running = false;
    return false;
}

void JAccessibilityBridge::stop() {
    m_running = false;
}

void JAccessibilityBridge::update(const std::vector<JAiNodeDescriptor>& nodes) {
    (void)nodes;
}

void JAccessibilityBridge::notifyFocus(uint32_t nodeIndex) {
    (void)nodeIndex;
}

void JAccessibilityBridge::notifyChecked(uint32_t nodeIndex, bool checked) {
    (void)nodeIndex;
    (void)checked;
}

} // inline namespace jf
