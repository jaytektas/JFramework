#pragma once

// AiBusHook — zero-dependency bridge from widget interactions to the AI control bus.
//
// Thread-safety: install() MUST be called from the main thread before any other
// threads start. After that, emit() is read-only and safe to call from any thread.
// Calling install() after background threads have started is a data race.
//
// Every widget (Button, CheckBox, LineEdit, ToggleButton, …) calls AiBusHook::emit()
// without needing to know about AiControlBus or GApplication directly.
//
// Deliberately kept in its own header so it can be included by GenesisComponents.h
// without dragging in the full BaseWidgets.h (and FontEngine / stb_truetype).

#include <cstdint>
#include <functional>

namespace Genesis {

struct AiBusHook {
    /** Called by widgets on user interaction.  Set via install(); no-op if null. */
    static inline std::function<void(uint32_t nodeId,
                                     const char* signal,
                                     const char* value)> emit;

    /** Install the live bus connection.  Called from GApplication ctor. */
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
