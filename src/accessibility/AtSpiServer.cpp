/**
 * AT-SPI2 accessibility bridge — connects Genesis to screen readers via dbus.
 * Responds to Accessible.GetName/GetRole/GetState so Orca can describe widgets.
 */

#include <genesis/core/AccessibilityBridge.h>
#include <genesis/core/muted_logging_mock.h>
#include <dbus/dbus.h>
#include <cstring>

namespace { inline constexpr auto& LogA11y = Genesis::Log::AI; }

static constexpr const char* ATSPI_IFACE_ACCESSIBLE  = "org.a11y.atspi.Accessible";
static constexpr const char* ATSPI_IFACE_APPLICATION  = "org.a11y.atspi.Application";
static constexpr const char* ATSPI_BUS_REGISTRY       = "org.a11y.atspi.Registry";
static constexpr const char* ATSPI_PATH_REGISTRY      = "/org/a11y/atspi/registry";
static constexpr const char* ATSPI_PATH_ROOT          = "/org/a11y/atspi/accessible/root";
static constexpr const char* INTROSPECT_IFACE         = "org.freedesktop.DBus.Introspectable";

static const char* INTROSPECT_XML = R"xml(
<!DOCTYPE node PUBLIC "-//freedesktop//DTD D-BUS Object Introspection 1.0//EN"
  "http://www.freedesktop.org/standards/dbus/1.0/introspect.dtd">
<node>
  <interface name="org.a11y.atspi.Accessible">
    <method name="GetName"><arg direction="out" type="s"/></method>
    <method name="GetRole"><arg direction="out" type="u"/></method>
    <method name="GetRoleName"><arg direction="out" type="s"/></method>
    <method name="GetState"><arg direction="out" type="au"/></method>
    <method name="GetChildCount"><arg direction="out" type="i"/></method>
    <method name="GetIndexInParent"><arg direction="out" type="i"/></method>
    <method name="GetParent"><arg direction="out" type="(so)"/></method>
    <method name="GetAttributes"><arg direction="out" type="a{ss}"/></method>
  </interface>
  <interface name="org.a11y.atspi.Application">
    <method name="GetToolkitName"><arg direction="out" type="s"/></method>
    <method name="GetVersion"><arg direction="out" type="s"/></method>
    <method name="GetLocale"><arg direction="in" type="u"/><arg direction="out" type="s"/></method>
  </interface>
  <interface name="org.freedesktop.DBus.Introspectable">
    <method name="Introspect"><arg direction="out" type="s"/></method>
  </interface>
</node>)xml";

namespace Genesis {

// ---- helpers ----------------------------------------------------------------

static void sendStringReply(DBusConnection* conn, DBusMessage* msg, const char* s) {
    DBusMessage* r = dbus_message_new_method_return(msg);
    if (!r) return;
    dbus_message_append_args(r, DBUS_TYPE_STRING, &s, DBUS_TYPE_INVALID);
    dbus_connection_send(conn, r, nullptr);
    dbus_message_unref(r);
}

static void sendUint32Reply(DBusConnection* conn, DBusMessage* msg, uint32_t v) {
    DBusMessage* r = dbus_message_new_method_return(msg);
    if (!r) return;
    dbus_message_append_args(r, DBUS_TYPE_UINT32, &v, DBUS_TYPE_INVALID);
    dbus_connection_send(conn, r, nullptr);
    dbus_message_unref(r);
}

static void sendStateReply(DBusConnection* conn, DBusMessage* msg, uint32_t w0, uint32_t w1) {
    DBusMessage* r = dbus_message_new_method_return(msg);
    if (!r) return;
    DBusMessageIter iter, arr;
    dbus_message_iter_init_append(r, &iter);
    dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY,
                                     DBUS_TYPE_UINT32_AS_STRING, &arr);
    dbus_message_iter_append_basic(&arr, DBUS_TYPE_UINT32, &w0);
    dbus_message_iter_append_basic(&arr, DBUS_TYPE_UINT32, &w1);
    dbus_message_iter_close_container(&iter, &arr);
    dbus_connection_send(conn, r, nullptr);
    dbus_message_unref(r);
}

static void sendInt32Reply(DBusConnection* conn, DBusMessage* msg, int32_t v) {
    DBusMessage* r = dbus_message_new_method_return(msg);
    if (!r) return;
    dbus_message_append_args(r, DBUS_TYPE_INT32, &v, DBUS_TYPE_INVALID);
    dbus_connection_send(conn, r, nullptr);
    dbus_message_unref(r);
}

// ---- AccessibilityBridge ----------------------------------------------------

bool AccessibilityBridge::start(const std::string& appName) {
    if (!_connectToA11yBus()) {
        qCWarning(LogA11y) << "AT-SPI: cannot connect — screen reader support disabled\n";
        return false;
    }
    _registerApplication(appName);
    m_running = true;
    m_thread  = std::thread([this] { _handleMessages(); });
    qCInfo(LogA11y) << "AT-SPI bridge started as '" << m_busName << "'\n";
    return true;
}

void AccessibilityBridge::stop() {
    m_running = false;
    if (m_thread.joinable()) m_thread.join();
    if (m_a11yBus) {
        dbus_connection_close(m_a11yBus);
        dbus_connection_unref(m_a11yBus);
        m_a11yBus = nullptr;
    }
    if (m_sessionBus) {
        dbus_connection_unref(m_sessionBus);
        m_sessionBus = nullptr;
    }
}

void AccessibilityBridge::update(const std::vector<AiNodeDescriptor>& nodes) {
    std::lock_guard<std::mutex> lk(m_nodesMutex);
    m_nodes = nodes;
    m_dirty = true;
}

void AccessibilityBridge::notifyFocus(uint32_t idx) {
    _sendEvent("object:state-changed:focused", idx, 1, 0);
}

void AccessibilityBridge::notifyChecked(uint32_t idx, bool checked) {
    _sendEvent("object:state-changed:checked", idx, checked ? 1 : 0, 0);
}

bool AccessibilityBridge::_connectToA11yBus() {
    DBusError err; dbus_error_init(&err);

    m_sessionBus = dbus_bus_get(DBUS_BUS_SESSION, &err);
    if (!m_sessionBus || dbus_error_is_set(&err)) {
        dbus_error_free(&err);
        return false;
    }

    DBusMessage* req = dbus_message_new_method_call(
        "org.a11y.Bus", "/org/a11y/bus", "org.a11y.Bus", "GetAddress");
    if (!req) return false;

    DBusMessage* resp = dbus_connection_send_with_reply_and_block(
        m_sessionBus, req, 1000, &err);
    dbus_message_unref(req);
    if (!resp || dbus_error_is_set(&err)) { dbus_error_free(&err); return false; }

    const char* addr = nullptr;
    dbus_message_get_args(resp, &err, DBUS_TYPE_STRING, &addr, DBUS_TYPE_INVALID);
    std::string busAddr = addr ? std::string(addr) : "";
    dbus_message_unref(resp);
    dbus_error_free(&err);

    if (busAddr.empty()) return false;

    m_a11yBus = dbus_connection_open_private(busAddr.c_str(), &err);
    if (!m_a11yBus || dbus_error_is_set(&err)) { dbus_error_free(&err); return false; }

    if (!dbus_bus_register(m_a11yBus, &err) || dbus_error_is_set(&err)) {
        dbus_error_free(&err); return false;
    }
    return true;
}

void AccessibilityBridge::_registerApplication(const std::string& appName) {
    if (!m_a11yBus) return;
    DBusError err; dbus_error_init(&err);

    m_busName = "org.a11y.atspi.accessible." + appName;
    dbus_bus_request_name(m_a11yBus, m_busName.c_str(),
                          DBUS_NAME_FLAG_REPLACE_EXISTING, &err);
    dbus_error_free(&err);

    DBusMessage* msg = dbus_message_new_method_call(
        ATSPI_BUS_REGISTRY, ATSPI_PATH_REGISTRY,
        ATSPI_BUS_REGISTRY, "RegisterApplication");
    if (!msg) return;

    const char* path = ATSPI_PATH_ROOT;
    dbus_message_append_args(msg, DBUS_TYPE_OBJECT_PATH, &path, DBUS_TYPE_INVALID);
    dbus_connection_send(m_a11yBus, msg, nullptr);
    dbus_message_unref(msg);
    dbus_connection_flush(m_a11yBus);
}

void AccessibilityBridge::_handleMessages() {
    while (m_running) {
        if (!m_a11yBus) break;

        // Non-blocking poll — 10 ms timeout so we can check m_running frequently
        dbus_connection_read_write(m_a11yBus, 10);

        DBusMessage* msg;
        while ((msg = dbus_connection_pop_message(m_a11yBus)) != nullptr) {
            const char* iface  = dbus_message_get_interface(msg);
            const char* member = dbus_message_get_member(msg);
            const char* path   = dbus_message_get_path(msg);

            if (!iface || !member || !path) { dbus_message_unref(msg); continue; }

            const std::string sIface(iface), sMember(member), sp(path);
            const std::string prefix = "/org/a11y/atspi/accessible/";

            // Parse node index (root = -1, leaf = 0..N-1)
            int32_t nodeIdx = -1;
            if (sp.size() > prefix.size() && sp.rfind(prefix, 0) == 0) {
                try { nodeIdx = std::stoi(sp.substr(prefix.size())); } catch (...) {}
            }

            if (sIface == INTROSPECT_IFACE && sMember == "Introspect") {
                sendStringReply(m_a11yBus, msg, INTROSPECT_XML);

            } else if (sIface == ATSPI_IFACE_APPLICATION) {
                if      (sMember == "GetToolkitName") sendStringReply(m_a11yBus, msg, "Genesis UI");
                else if (sMember == "GetVersion")     sendStringReply(m_a11yBus, msg, "0.1.0");
                else if (sMember == "GetLocale")      sendStringReply(m_a11yBus, msg, "en_US");

            } else if (sIface == ATSPI_IFACE_ACCESSIBLE) {
                // Snapshot under lock to avoid data race with update()
                std::string name;
                uint32_t role   = static_cast<uint32_t>(AtSpiRole::Application);
                uint32_t state0 = static_cast<uint32_t>(AtSpiState::Enabled)
                                | static_cast<uint32_t>(AtSpiState::Visible)
                                | static_cast<uint32_t>(AtSpiState::Showing)
                                | static_cast<uint32_t>(AtSpiState::Sensitive);
                int32_t childCount = 0;

                {
                    std::lock_guard<std::mutex> lk(m_nodesMutex);
                    if (nodeIdx >= 0 && nodeIdx < static_cast<int32_t>(m_nodes.size())) {
                        name       = m_nodes[nodeIdx].name;
                        role       = static_cast<uint32_t>(AtSpiRole::Panel);
                        childCount = 0;
                    } else {
                        // Root application object
                        name       = "Genesis UI";
                        childCount = static_cast<int32_t>(m_nodes.size());
                    }
                }

                if      (sMember == "GetName")          sendStringReply(m_a11yBus, msg, name.c_str());
                else if (sMember == "GetRole")          sendUint32Reply(m_a11yBus, msg, role);
                else if (sMember == "GetRoleName")      sendStringReply(m_a11yBus, msg, "panel");
                else if (sMember == "GetState")         sendStateReply(m_a11yBus, msg, state0, 0u);
                else if (sMember == "GetChildCount")    sendInt32Reply(m_a11yBus, msg, childCount);
                else if (sMember == "GetIndexInParent") sendInt32Reply(m_a11yBus, msg, nodeIdx);
                else if (sMember == "GetParent") {
                    DBusMessage* r = dbus_message_new_method_return(msg);
                    if (r) {
                        const char* pBus  = m_busName.c_str();
                        const char* pPath = ATSPI_PATH_ROOT;
                        dbus_message_append_args(r,
                            DBUS_TYPE_STRING, &pBus,
                            DBUS_TYPE_OBJECT_PATH, &pPath,
                            DBUS_TYPE_INVALID);
                        dbus_connection_send(m_a11yBus, r, nullptr);
                        dbus_message_unref(r);
                    }
                }
                else if (sMember == "GetAttributes") {
                    DBusMessage* r = dbus_message_new_method_return(msg);
                    if (r) {
                        DBusMessageIter iter, arr;
                        dbus_message_iter_init_append(r, &iter);
                        dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "{ss}", &arr);
                        dbus_message_iter_close_container(&iter, &arr);
                        dbus_connection_send(m_a11yBus, r, nullptr);
                        dbus_message_unref(r);
                    }
                }
            }

            dbus_message_unref(msg);
        }
    }
}

bool AccessibilityBridge::_replyGetName(DBusMessage* msg, const std::string& name) {
    sendStringReply(m_a11yBus, msg, name.c_str());
    return true;
}
bool AccessibilityBridge::_replyGetRole(DBusMessage* msg, uint32_t role) {
    sendUint32Reply(m_a11yBus, msg, role);
    return true;
}
bool AccessibilityBridge::_replyGetState(DBusMessage* msg, uint32_t w0, uint32_t w1) {
    sendStateReply(m_a11yBus, msg, w0, w1);
    return true;
}
bool AccessibilityBridge::_replyGetChildren(DBusMessage* msg, const std::vector<uint32_t>&) {
    DBusMessage* r = dbus_message_new_method_return(msg);
    if (!r) return false;
    // Return empty array for now — children via GetChildCount is sufficient for Orca
    DBusMessageIter iter, arr;
    dbus_message_iter_init_append(r, &iter);
    dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "(so)", &arr);
    dbus_message_iter_close_container(&iter, &arr);
    dbus_connection_send(m_a11yBus, r, nullptr);
    dbus_message_unref(r);
    return true;
}
bool AccessibilityBridge::_replyIntrospect(DBusMessage* msg) {
    sendStringReply(m_a11yBus, msg, INTROSPECT_XML);
    return true;
}

void AccessibilityBridge::_sendEvent(const std::string& eventType,
                                     uint32_t nodeIndex,
                                     uint32_t detail1, uint32_t detail2) {
    if (!m_a11yBus || !m_running) return;
    std::string path = "/org/a11y/atspi/accessible/" + std::to_string(nodeIndex);
    const char* pPath = path.c_str();

    DBusMessage* sig = dbus_message_new_signal(pPath,
        "org.a11y.atspi.Event.Object", "StateChanged");
    if (!sig) return;

    const char* evType  = eventType.c_str();
    const char* toolkit = "Genesis UI";
    int32_t d1 = static_cast<int32_t>(detail1);
    int32_t d2 = static_cast<int32_t>(detail2);
    int32_t zero = 0;

    DBusMessageIter iter, var;
    dbus_message_iter_init_append(sig, &iter);
    dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING,  &evType);
    dbus_message_iter_append_basic(&iter, DBUS_TYPE_INT32,   &d1);
    dbus_message_iter_append_basic(&iter, DBUS_TYPE_INT32,   &d2);
    dbus_message_iter_open_container(&iter, DBUS_TYPE_VARIANT,
                                     DBUS_TYPE_INT32_AS_STRING, &var);
    dbus_message_iter_append_basic(&var, DBUS_TYPE_INT32, &zero);
    dbus_message_iter_close_container(&iter, &var);
    dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING,  &toolkit);

    dbus_connection_send(m_a11yBus, sig, nullptr);
    dbus_message_unref(sig);
}

} // namespace Genesis
