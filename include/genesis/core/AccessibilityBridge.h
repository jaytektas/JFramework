#pragma once

#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>
#include <functional>
#include "muted_logging_mock.h"
#include "AiControlBus.h"   // AISemanticNode + SceneGraph

// Forward-declare dbus types so consumers don't pull in dbus headers
struct DBusConnection;
struct DBusMessage;

namespace Genesis {

/**
 * @brief Maps Genesis AISemanticNode roles to AT-SPI role constants.
 *
 * These match org.a11y.atspi.ROLE_* values from the AT-SPI2 spec.
 * Screen readers (Orca) use these to describe widgets to the user.
 */
enum class AtSpiRole : uint32_t {
    Invalid        = 0,
    Application    = 75,
    Frame          = 69,
    Panel          = 22,
    PushButton     = 28,
    ToggleButton   = 76,
    CheckBox       = 7,
    RadioButton    = 33,
    Label          = 19,
    Text           = 50,
    SliderRole     = 41,  // ROLE_SLIDER
    ProgressBar    = 27,
    SpinButton     = 45,
    ComboBox       = 11,
    PageTabList    = 24,
    PageTab        = 23,
    ScrollBar      = 40,
    SeparatorRole  = 38,
};

static inline AtSpiRole roleForSemanticType(const std::string& role) {
    if (role == "Button")      return AtSpiRole::PushButton;
    if (role == "ToggleButton")return AtSpiRole::ToggleButton;
    if (role == "CheckBox")    return AtSpiRole::CheckBox;
    if (role == "RadioButton") return AtSpiRole::RadioButton;
    if (role == "Label")       return AtSpiRole::Label;
    if (role == "LineEdit")    return AtSpiRole::Text;
    if (role == "Slider")      return AtSpiRole::SliderRole;
    if (role == "ProgressBar") return AtSpiRole::ProgressBar;
    if (role == "SpinBox")     return AtSpiRole::SpinButton;
    if (role == "ComboBox")    return AtSpiRole::ComboBox;
    if (role == "TabBar")      return AtSpiRole::PageTabList;
    if (role == "ScrollBar")   return AtSpiRole::ScrollBar;
    if (role == "GroupBox")    return AtSpiRole::Panel;
    if (role == "Separator")   return AtSpiRole::SeparatorRole;
    if (role == "DockWidget")  return AtSpiRole::Frame;
    return AtSpiRole::Panel;
}

/**
 * @brief AT-SPI state bitset constants (matches atspi-state-set.h).
 * Two uint32_t words form a 64-bit state set.
 */
enum class AtSpiState : uint32_t {
    Enabled     = 1u << 14,
    Visible     = 1u << 16,
    Showing     = 1u << 15,
    Focusable   = 1u << 11,
    Focused     = 1u << 12,
    Pressed     = 1u << 7,
    Checked     = 1u << 4,
    Selected    = 1u << 0,
    Editable    = 1u << 6,
    Sensitive   = 1u << 28,
};

/**
 * @brief AccessibilityBridge — publishes the Genesis widget tree on the
 * AT-SPI2 dbus accessibility bus so screen readers (Orca) can discover
 * and interact with the application.
 *
 * Usage:
 *   AccessibilityBridge bridge;
 *   bridge.start("My App");
 *   // each frame / on state change:
 *   bridge.update(semanticNodes);
 *   // on focus change:
 *   bridge.notifyFocus(nodeIndex);
 */
class AccessibilityBridge {
public:
    AccessibilityBridge() = default;
    ~AccessibilityBridge() { stop(); }

    AccessibilityBridge(const AccessibilityBridge&)            = delete;
    AccessibilityBridge& operator=(const AccessibilityBridge&) = delete;

    /** Connect to the AT-SPI bus and register this application. */
    bool start(const std::string& appName);

    /** Tear down the dbus connection and background thread. */
    void stop();

    bool isRunning() const { return m_running; }

    /**
     * Push a fresh semantic snapshot to AT-SPI.  Call whenever the widget
     * tree changes (new widget added, state change, etc.).
     * The bridge diffs against the previous snapshot and sends minimal events.
     */
    void update(const std::vector<AiNodeDescriptor>& nodes);

    /** Emit object:state-changed:focused for the given node index. */
    void notifyFocus(uint32_t nodeIndex);

    /** Emit object:state-changed:checked for a checkbox/radio. */
    void notifyChecked(uint32_t nodeIndex, bool checked);

private:
    bool _connectToA11yBus();
    void _registerApplication(const std::string& appName);
    void _handleMessages();
    bool _replyGetName(DBusMessage* msg, const std::string& name);
    bool _replyGetRole(DBusMessage* msg, uint32_t role);
    bool _replyGetState(DBusMessage* msg, uint32_t stateWord0, uint32_t stateWord1);
    bool _replyGetChildren(DBusMessage* msg, const std::vector<uint32_t>& childIndices);
    bool _replyIntrospect(DBusMessage* msg);
    void _sendEvent(const std::string& eventType, uint32_t nodeIndex,
                    uint32_t detail1, uint32_t detail2);

    DBusConnection*             m_sessionBus{nullptr};
    DBusConnection*             m_a11yBus{nullptr};
    std::string                 m_busName;
    std::string                 m_appPath{"/org/a11y/atspi/accessible/root"};

    std::vector<AiNodeDescriptor> m_nodes;
    std::mutex                  m_nodesMutex;
    std::thread                 m_thread;
    std::atomic<bool>           m_running{false};
    std::atomic<bool>           m_dirty{false};
};

} // namespace Genesis
