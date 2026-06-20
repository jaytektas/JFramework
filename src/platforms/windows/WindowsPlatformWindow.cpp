#include <genesis/core/AccessibilityBridge.h>

namespace Genesis {

bool AccessibilityBridge::start(const std::string& appName) {
    (void)appName;
    m_running = false;
    return false;
}

void AccessibilityBridge::stop() {
    m_running = false;
}

void AccessibilityBridge::update(const std::vector<AiNodeDescriptor>& nodes) {
    (void)nodes;
}

void AccessibilityBridge::notifyFocus(uint32_t nodeIndex) {
    (void)nodeIndex;
}

void AccessibilityBridge::notifyChecked(uint32_t nodeIndex, bool checked) {
    (void)nodeIndex;
    (void)checked;
}

} // namespace Genesis
