#pragma once

// Thread-safety: MAIN THREAD ONLY.
// Drag-and-drop state is tied to mouse events which arrive on the main thread.
// Do not call any JDragDrop methods from background threads.

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <typeindex>
#include <utility>
#include <vector>

inline namespace jf {

// ============================================================================
// Typed Drag & Drop — Step 5
//
// No MIME strings. The payload is a typed C++ object. JType mismatches are
// caught at compile time via template specialisation, not runtime string checks.
//
// Sender:
//   JDragDrop::start<FileList>(myFiles, myWidget, dragCursorLabel);
//
// Receiver (in handleMouseRelease):
//   JDragDrop::accept<FileList>([](const FileList& files, float x, float y) {
//       importFiles(files);
//   });
//
// Or in handleMouseMove, to highlight a valid drop target:
//   if (JDragDrop::canAccept<FileList>()) { showDropHighlight(); }
// ============================================================================
class JDragDrop {
public:
    // ---- JPayload wrapper ---------------------------------------------------
    struct JPayloadBase {
        virtual ~JPayloadBase() = default;
        virtual std::type_index type() const = 0;
        virtual std::string     label() const = 0;
    };

    template<typename T>
    struct JPayload : JPayloadBase {
        T value;
        std::string m_label;
        explicit JPayload(T v, std::string lbl)
            : value(std::move(v)), m_label(std::move(lbl)) {}
        std::type_index type()  const override { return std::type_index(typeid(T)); }
        std::string     label() const override { return m_label; }
    };

    // ---- Drag session ------------------------------------------------------
    struct JSession {
        std::unique_ptr<JPayloadBase> payload;
        float startX{0}, startY{0};
        float curX{0},   curY{0};
        bool  active{false};
    };

    // ---- Global state (one active drag at a time) -------------------------
    static JSession& current() {
        static JSession s;
        return s;
    }

    // Start a drag with a typed payload. Call from handleMousePress when the
    // user initiates a drag gesture (e.g., after 4px of movement).
    template<typename T>
    static void start(T value, float startX, float startY,
                      std::string label = "drag") {
        auto& s = current();
        s.payload = std::make_unique<JPayload<T>>(std::move(value), std::move(label));
        s.startX  = startX;
        s.startY  = startY;
        s.curX    = startX;
        s.curY    = startY;
        s.active  = true;
    }

    // Update cursor position during drag. Call from handleMouseMove.
    static void update(float x, float y) {
        auto& s = current();
        if (s.active) { s.curX = x; s.curY = y; }
    }

    // Cancel with no drop.
    static void cancel() {
        auto& s = current();
        s.active = false;
        s.payload.reset();
    }

    // Query — true if there's an active drag of the given type.
    template<typename T>
    static bool canAccept() {
        auto& s = current();
        return s.active && s.payload &&
               s.payload->type() == std::type_index(typeid(T));
    }

    // Consume the drop. Returns true if there was a matching active drag.
    // The callback receives the payload value and drop coordinates.
    // After accept() returns true the session is cleared.
    template<typename T>
    static bool accept(float dropX, float dropY,
                       std::function<void(const T&, float x, float y)> handler) {
        if (!canAccept<T>()) return false;
        auto& s = current();
        const T& val = static_cast<JPayload<T>*>(s.payload.get())->value;
        handler(val, dropX, dropY);
        cancel();
        return true;
    }

    // Convenience: accept without caring about drop coordinates.
    template<typename T>
    static bool accept(std::function<void(const T&)> handler) {
        return accept<T>(0.f, 0.f,
            [&](const T& v, float, float){ handler(v); });
    }

    // Drag in progress?
    static bool isDragging()   { return current().active; }
    static std::string label() {
        auto& s = current();
        return (s.active && s.payload) ? s.payload->label() : std::string{};
    }
    static float cursorX()     { return current().curX; }
    static float cursorY()     { return current().curY; }
};

// ---- Common payload types -------------------------------------------------

struct JFileListPayload {
    std::vector<std::string> paths;
};

struct JTextPayload {
    std::string text;
};

// JWidget reorder payload — used by DockManager, JListView, JTreeView
struct JWidgetIdPayload {
    uint32_t nodeId{0};
    std::string debugName;
};

// ============================================================================
// General widget-to-widget drag & drop — format-tagged payloads
//
// A parallel, self-describing model that sits ALONGSIDE the typed JDragDrop
// above. Where JDragDrop carries one C++ object known to both ends, this model
// carries a bag of named string payloads (a "format" is any short key such as
// "text/plain" or "application/x-jf-node"), so a source and a target that were
// written independently can still negotiate a drop by advertised format.
//
// Flow:
//   source->startDrag(mime, JDropAction::Move);   // opens jCurrentDrag()
//   ...each frame the host runner calls:
//   jDragTick(mouseX, mouseY, pressed, released); // routes enter/move/leave/drop
//
// Routing (jDragTick) delivers, on the top-most visible widget under the cursor
// whose canDrop(mime) is true: onDragEnter → onDragMove* → (onDragLeave on exit)
// and, on button release over an accepting target, onDrop. A widget that returns
// canDrop()==false is never entered and never receives onDrop.
// ============================================================================

class JWidget;   // routed over, defined in BaseWidgets.h

// What the drop would do with the payload. Ignore = no valid target / rejected.
enum class JDropAction { Ignore, Copy, Move, Link };

// A bag of format-tagged string payloads. Insertion order is preserved so
// formats() reports the source's preference order.
class JMimeData {
public:
    // Set (or replace) the payload for a format key.
    void setData(const std::string& format, std::string payload) {
        for (auto& kv : m_data) {
            if (kv.first == format) { kv.second = std::move(payload); return; }
        }
        m_data.emplace_back(format, std::move(payload));
    }
    bool hasFormat(const std::string& format) const {
        for (auto& kv : m_data) if (kv.first == format) return true;
        return false;
    }
    // Payload for a format, or "" if absent (use hasFormat to disambiguate empty).
    std::string data(const std::string& format) const {
        for (auto& kv : m_data) if (kv.first == format) return kv.second;
        return {};
    }
    // Advertised format keys, in the order they were set.
    std::vector<std::string> formats() const {
        std::vector<std::string> out;
        out.reserve(m_data.size());
        for (auto& kv : m_data) out.push_back(kv.first);
        return out;
    }
    bool empty() const { return m_data.empty(); }

    // Convenience for the ubiquitous plain-text format.
    static constexpr const char* kText = "text/plain";
    bool        hasText() const { return hasFormat(kText); }
    std::string text()    const { return data(kText); }
    void        setText(std::string t) { setData(kText, std::move(t)); }

private:
    std::vector<std::pair<std::string, std::string>> m_data;
};

// The live state of one in-flight drag. There is a single framework-owned
// session at a time (see jCurrentDrag()).
struct JDragSession {
    JWidget*    source{nullptr};   // who started the drag
    JMimeData   mime;              // the payload being dragged
    JDropAction supported{JDropAction::Copy};  // action(s) the source permits
    JDropAction proposed{JDropAction::Ignore}; // action the current target would take
    float       x{0.f}, y{0.f};    // current cursor position (screen space)
    float       hotspotX{0.f}, hotspotY{0.f};  // grab offset within the source
    JWidget*    over{nullptr};     // target currently entered (accepted canDrop), or null
    bool        active{false};
};

// The one framework-owned active drag session.
inline JDragSession& jCurrentDrag() { static JDragSession s; return s; }
inline bool          jDragActive()  { return jCurrentDrag().active; }

// Open a new drag session with `source` as origin. Resets the previous session.
// Only touches JDragSession fields, so it is safe to define here (JWidget need
// not be complete). JWidget::startDrag() forwards to this.
inline JDragSession& jBeginDrag(JWidget* source, JMimeData mime,
                                JDropAction supported,
                                float hotspotX = 0.f, float hotspotY = 0.f) {
    JDragSession& s = jCurrentDrag();
    s.source    = source;
    s.mime      = std::move(mime);
    s.supported = supported;
    s.proposed  = JDropAction::Ignore;
    s.over      = nullptr;
    s.hotspotX  = hotspotX;
    s.hotspotY  = hotspotY;
    s.active    = true;
    return s;
}

// Cancel the active session with no drop.
inline void jCancelDrag() {
    JDragSession& s = jCurrentDrag();
    s.active = false;
    s.over   = nullptr;
    s.proposed = JDropAction::Ignore;
}

// Per-frame driver the host runner calls once with the current pointer state.
// Routes enter/move/leave to the top-most accepting widget under (mx,my) and,
// on `released`, delivers onDrop. Returns true if a drop was delivered.
// DEFINED OUT-OF-LINE in BaseWidgets.h, where JWidget is a complete type and
// its virtual hooks + s_activeWidgets are visible (same idiom as
// JWidget::drawFocusRing). `pressed` updates the hotspot cursor only.
bool jDragTick(float mx, float my, bool pressed, bool released);

} // inline namespace jf
