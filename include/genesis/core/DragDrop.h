#pragma once

// Thread-safety: MAIN THREAD ONLY.
// Drag-and-drop state is tied to mouse events which arrive on the main thread.
// Do not call any DragDrop methods from background threads.

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <typeindex>
#include <vector>

namespace Genesis {

// ============================================================================
// Typed Drag & Drop — Step 5
//
// No MIME strings. The payload is a typed C++ object. Type mismatches are
// caught at compile time via template specialisation, not runtime string checks.
//
// Sender:
//   DragDrop::start<FileList>(myFiles, myWidget, dragCursorLabel);
//
// Receiver (in handleMouseRelease):
//   DragDrop::accept<FileList>([](const FileList& files, float x, float y) {
//       importFiles(files);
//   });
//
// Or in handleMouseMove, to highlight a valid drop target:
//   if (DragDrop::canAccept<FileList>()) { showDropHighlight(); }
// ============================================================================
class DragDrop {
public:
    // ---- Payload wrapper ---------------------------------------------------
    struct PayloadBase {
        virtual ~PayloadBase() = default;
        virtual std::type_index type() const = 0;
        virtual std::string     label() const = 0;
    };

    template<typename T>
    struct Payload : PayloadBase {
        T value;
        std::string m_label;
        explicit Payload(T v, std::string lbl)
            : value(std::move(v)), m_label(std::move(lbl)) {}
        std::type_index type()  const override { return std::type_index(typeid(T)); }
        std::string     label() const override { return m_label; }
    };

    // ---- Drag session ------------------------------------------------------
    struct Session {
        std::unique_ptr<PayloadBase> payload;
        float startX{0}, startY{0};
        float curX{0},   curY{0};
        bool  active{false};
    };

    // ---- Global state (one active drag at a time) -------------------------
    static Session& current() {
        static Session s;
        return s;
    }

    // Start a drag with a typed payload. Call from handleMousePress when the
    // user initiates a drag gesture (e.g., after 4px of movement).
    template<typename T>
    static void start(T value, float startX, float startY,
                      std::string label = "drag") {
        auto& s = current();
        s.payload = std::make_unique<Payload<T>>(std::move(value), std::move(label));
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
        const T& val = static_cast<Payload<T>*>(s.payload.get())->value;
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

struct FileListPayload {
    std::vector<std::string> paths;
};

struct TextPayload {
    std::string text;
};

// Widget reorder payload — used by DockManager, ListView, TreeView
struct WidgetIdPayload {
    uint32_t nodeId{0};
    std::string debugName;
};

} // namespace Genesis
