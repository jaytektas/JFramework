#pragma once

#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>
#include <functional>
#include <cstdint>
#include <cstring>
#include "muted_logging_mock.h"

// Forward-declare dbus types so consumers don't pull in dbus headers
struct DBusConnection;
struct DBusMessage;

inline namespace jf {

/**
 * @brief Toolkit-neutral accessibility role for a widget.
 *
 * A widget reports one of these from JWidget::a11yNode(); the bridge maps it to
 * the platform accessibility role (AT-SPI on Linux) via jA11yAtSpiRoleFor().
 * Deliberately independent of any single desktop toolkit's role vocabulary.
 */
enum class JA11yRole : uint32_t {
    Widget = 0,
    Button,
    ToggleButton,
    CheckBox,
    RadioButton,
    TextField,
    SpinBox,
    ComboBox,
    Slider,
    ProgressBar,
    Label,
    List,
    ListItem,
    Tab,        // a single tab / the selected-tab surface
    TabList,    // a tab bar (group of tabs)
    Menu,
    MenuItem,
    Dialog,
    Window,
    Group,
    ScrollBar,
    Separator,
    Tree,
    Table,
};

/**
 * @brief Toolkit-neutral accessibility state bits (OR-combined into
 *        JA11yNode::stateFlags). The bridge translates these to the platform's
 *        state set (AT-SPI JAtSpiState) when answering a client.
 */
enum JA11yState : uint32_t {
    JA11yFocused    = 1u << 0,   // currently holds keyboard focus
    JA11yFocusable  = 1u << 1,   // can take keyboard focus
    JA11yDisabled   = 1u << 2,   // greyed / not sensitive
    JA11yChecked    = 1u << 3,   // checkbox/radio/toggle is on
    JA11yMixed      = 1u << 4,   // tri-state checkbox partial ("mixed")
    JA11ySelected   = 1u << 5,   // selected within a group/list
    JA11yEditable   = 1u << 6,   // text can be edited by the user
    JA11yExpanded   = 1u << 7,   // expandable node is open
    JA11yPressed    = 1u << 8,   // momentarily pressed
    JA11yReadOnly   = 1u << 9,   // editable widget locked read-only
    JA11yProtected  = 1u << 10,  // password / masked — value not exposed
};

/**
 * @brief Blit-safe, POD accessibility node — the semantic snapshot the bridge
 *        exposes to AT-SPI (role/label/value/state + geometry).
 */
struct alignas(16) JA11yNode {
    uint32_t  id{0xFFFFFFFF};
    uint32_t  stateFlags{0};      // OR of JA11yState bits
    JA11yRole roleId{JA11yRole::Widget};
    float     x{0.0f};
    float     y{0.0f};
    float     width{0.0f};
    float     height{0.0f};
    // Numeric value interface (sliders / spin boxes / progress bars). hasRange
    // says whether curValue/minValue/maxValue are meaningful for this node.
    bool      hasRange{false};
    float     curValue{0.0f};
    float     minValue{0.0f};
    float     maxValue{0.0f};
    char      role[24]{0};   // canonical role string ("JButton", "JSlider", ...)
    char      name[32]{0};   // label / accessible name
    char      value[24]{0};  // current value ("0.50", "checked", text, ...)
};

/** Copy a std::string into a fixed char buffer, always NUL-terminated. */
static inline void jA11yCopyStr(char* dst, size_t cap, const std::string& s) {
    if (!cap) return;
    size_t n = s.size() < cap - 1 ? s.size() : cap - 1;
    std::memcpy(dst, s.data(), n);
    dst[n] = '\0';
}

/** Canonical role string for a JA11yRole (feeds roleForSemanticType). */
static inline const char* jA11yRoleName(JA11yRole r) {
    switch (r) {
        case JA11yRole::Button:       return "JButton";
        case JA11yRole::ToggleButton: return "JToggleButton";
        case JA11yRole::CheckBox:     return "JCheckBox";
        case JA11yRole::RadioButton:  return "JRadioButton";
        case JA11yRole::TextField:    return "JLineEdit";
        case JA11yRole::SpinBox:      return "JSpinBox";
        case JA11yRole::ComboBox:     return "JComboBox";
        case JA11yRole::Slider:       return "JSlider";
        case JA11yRole::ProgressBar:  return "JProgressBar";
        case JA11yRole::Label:        return "JLabel";
        case JA11yRole::List:         return "JListView";
        case JA11yRole::ListItem:     return "JListItem";
        case JA11yRole::Tab:          return "JTab";
        case JA11yRole::TabList:      return "JTabBar";
        case JA11yRole::Menu:         return "JMenu";
        case JA11yRole::MenuItem:     return "JMenuItem";
        case JA11yRole::Dialog:       return "JDialog";
        case JA11yRole::Window:       return "JWindow";
        case JA11yRole::Group:        return "JGroupBox";
        case JA11yRole::ScrollBar:    return "JScrollBar";
        case JA11yRole::Separator:    return "JSeparator";
        case JA11yRole::Tree:         return "JTreeView";
        case JA11yRole::Table:        return "JDataGrid";
        case JA11yRole::Widget: default: return "JWidget";
    }
}

/**
 * @brief Maps Genesis semantic roles to AT-SPI role constants.
 *
 * These match org.a11y.atspi.ROLE_* values from the AT-SPI2 spec.
 * Screen readers (Orca) use these to describe widgets to the user.
 */
enum class JAtSpiRole : uint32_t {
    Invalid        = 0,
    JApplication    = 75,
    Frame          = 69,
    Panel          = 22,
    PushButton     = 28,
    JToggleButton   = 76,
    JCheckBox       = 7,
    JRadioButton    = 33,
    JLabel          = 19,
    Text           = 50,
    SliderRole     = 41,  // ROLE_SLIDER
    JProgressBar    = 27,
    SpinButton     = 45,
    JComboBox       = 11,
    PageTabList    = 24,
    PageTab        = 23,
    JScrollBar      = 40,
    SeparatorRole  = 38,
};

static inline JAtSpiRole roleForSemanticType(const std::string& role) {
    if (role == "JButton")      return JAtSpiRole::PushButton;
    if (role == "JToggleButton")return JAtSpiRole::JToggleButton;
    if (role == "JCheckBox")    return JAtSpiRole::JCheckBox;
    if (role == "JRadioButton") return JAtSpiRole::JRadioButton;
    if (role == "JLabel")       return JAtSpiRole::JLabel;
    if (role == "JLineEdit")    return JAtSpiRole::Text;
    if (role == "JSlider")      return JAtSpiRole::SliderRole;
    if (role == "JProgressBar") return JAtSpiRole::JProgressBar;
    if (role == "JSpinBox")     return JAtSpiRole::SpinButton;
    if (role == "JComboBox")    return JAtSpiRole::JComboBox;
    if (role == "JTabBar")      return JAtSpiRole::PageTabList;
    if (role == "JScrollBar")   return JAtSpiRole::JScrollBar;
    if (role == "JGroupBox")    return JAtSpiRole::Panel;
    if (role == "JSeparator")   return JAtSpiRole::SeparatorRole;
    if (role == "JDockWidget")  return JAtSpiRole::Frame;
    return JAtSpiRole::Panel;
}

/** Map a toolkit-neutral JA11yRole directly to its AT-SPI role constant. */
static inline JAtSpiRole jA11yAtSpiRoleFor(JA11yRole r) {
    return roleForSemanticType(jA11yRoleName(r));
}

/**
 * @brief AT-SPI state bitset constants (matches atspi-state-set.h).
 * Two uint32_t words form a 64-bit state set.
 */
enum class JAtSpiState : uint32_t {
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
 * @brief JAccessibilityBridge — publishes the Genesis widget tree on the
 * AT-SPI2 dbus accessibility bus so screen readers (Orca) can discover
 * and interact with the application.
 *
 * Usage:
 *   JAccessibilityBridge bridge;
 *   bridge.start("My App");
 *   // each frame / on state change:
 *   bridge.update(semanticNodes);
 *   // on focus change:
 *   bridge.notifyFocus(nodeIndex);
 */
class JAccessibilityBridge {
public:
    JAccessibilityBridge() = default;
    ~JAccessibilityBridge() { stop(); }

    JAccessibilityBridge(const JAccessibilityBridge&)            = delete;
    JAccessibilityBridge& operator=(const JAccessibilityBridge&) = delete;

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
    void update(const std::vector<JA11yNode>& nodes);

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

    std::vector<JA11yNode> m_nodes;
    std::mutex                  m_nodesMutex;
    std::thread                 m_thread;
    std::atomic<bool>           m_running{false};
    std::atomic<bool>           m_dirty{false};
};

} // inline namespace jf
