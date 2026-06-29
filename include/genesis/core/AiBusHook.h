#pragma once

// JAiBusHook — zero-dependency bridge from widget interactions to the AI control bus.
//
// Thread-safety: install() MUST be called from the main thread before any other
// threads start. After that, emit() is read-only and safe to call from any thread.
// Calling install() after background threads have started is a data race.
//
// Every widget (JButton, JCheckBox, JLineEdit, JToggleButton, …) calls JAiBusHook::emit()
// without needing to know about JAiControlBus or JGuiApplication directly.
//
// Deliberately kept in its own header so it can be included by GenesisComponents.h
// without dragging in the full BaseWidgets.h (and JFontEngine / stb_truetype).

#include <cstdint>
#include <functional>

namespace Genesis {

struct JAiBusHook {
    /** Called by widgets on user interaction.  Set via install(); no-op if null. */
    static inline std::function<void(uint32_t nodeId,
                                     const char* signal,
                                     const char* value)> emit;

    /** Install the live bus connection.  Called from JGuiApplication ctor. */
    static void install(std::function<void(uint32_t, const char*, const char*)> fn) {
        emit = std::move(fn);
    }

    /** Well-known signal name constants — avoids magic strings at call sites. */
    static constexpr const char* kClick        = "click";
    static constexpr const char* kToggled      = "toggled";
    static constexpr const char* kChecked      = "checked";
    static constexpr const char* kUnchecked    = "unchecked";
    static constexpr const char* kTextChanged  = "text_changed";
    static constexpr const char* kSelected     = "selected";
    static constexpr const char* kValueChanged = "value_changed";
};

} // namespace Genesis
