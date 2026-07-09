#pragma once

// ============================================================================
// jf::JKeySequence — a portable keyboard chord: a modifier mask + one JKey.
//
// Clean-room command-key primitive. It carries NO platform types — just the
// framework's own JKeyEvent::JKey plus a small bitmask of Ctrl/Shift/Alt — so
// an action can declare "Ctrl+Shift+S" once and have it match uniformly wher-
// ever key events arrive. Parse/format go through a single canonical string
// grammar ("Ctrl+Shift+S", "Alt+F4", "Esc") so format(parse(x)) round-trips.
//
//   JKeySequence save = JKeySequence::parse("Ctrl+S");
//   if (save.matches(keyEvent)) ...            // ctrl held, key == S
//   std::string label = save.toString();       // "Ctrl+S"
//
// Complements MenuSystem.h's JMenuShortcut (which stays as-is for menu rows);
// JKeySequence is the richer, string-native chord the JAction system speaks.
// ============================================================================

#include "KeyEvent.h"

#include <cstdint>
#include <string>
#include <cctype>
#include <functional>

inline namespace jf {

struct JKeySequence {
    // Modifier bitmask — deliberately independent of any platform enum.
    enum Mod : uint8_t {
        None  = 0,
        Ctrl  = 1u << 0,
        Shift = 1u << 1,
        Alt   = 1u << 2,
    };

    uint8_t           mods{None};
    JKeyEvent::JKey   key{JKeyEvent::JKey::Unknown};

    constexpr JKeySequence() = default;
    constexpr JKeySequence(uint8_t m, JKeyEvent::JKey k) : mods(m), key(k) {}

    bool valid() const noexcept { return key != JKeyEvent::JKey::Unknown; }

    bool hasCtrl()  const noexcept { return mods & Ctrl; }
    bool hasShift() const noexcept { return mods & Shift; }
    bool hasAlt()   const noexcept { return mods & Alt; }

    // A live key event matches when the base key is equal AND the modifier
    // state matches exactly (an accelerator must not fire on a superset chord).
    bool matches(const JKeyEvent& ke) const noexcept {
        if (key == JKeyEvent::JKey::Unknown || ke.key != key) return false;
        const uint8_t evMods =
            (ke.ctrl  ? Ctrl  : 0u) |
            (ke.shift ? Shift : 0u) |
            (ke.alt   ? Alt   : 0u);
        return evMods == mods;
    }

    bool operator==(const JKeySequence& o) const noexcept {
        return mods == o.mods && key == o.key;
    }
    bool operator!=(const JKeySequence& o) const noexcept { return !(*this == o); }

    // A stable integer key for hashing / map indexing: mods in the low byte,
    // the JKey value above it.
    uint64_t packed() const noexcept {
        return (static_cast<uint64_t>(static_cast<uint32_t>(key)) << 8) | mods;
    }

    // Canonical "Ctrl+Shift+S" form. Empty when invalid.
    std::string toString() const {
        if (key == JKeyEvent::JKey::Unknown) return {};
        std::string s;
        if (mods & Ctrl)  s += "Ctrl+";
        if (mods & Alt)   s += "Alt+";
        if (mods & Shift) s += "Shift+";
        s += keyName(key);
        return s;
    }

    // Parse "Ctrl+Shift+S" (case-insensitive on modifiers/tokens) into a chord.
    // Unknown tokens leave the sequence invalid. Whitespace around tokens is
    // tolerated. The LAST non-modifier token is taken as the base key.
    static JKeySequence parse(const std::string& spec) {
        JKeySequence seq;
        size_t i = 0;
        const size_t n = spec.size();
        while (i < n) {
            // Skip separators / whitespace.
            while (i < n && (spec[i] == '+' || std::isspace(static_cast<unsigned char>(spec[i])))) ++i;
            size_t start = i;
            while (i < n && spec[i] != '+') ++i;
            size_t end = i;
            // Trim trailing whitespace on the token.
            while (end > start && std::isspace(static_cast<unsigned char>(spec[end - 1]))) --end;
            if (end <= start) continue;
            std::string tok = spec.substr(start, end - start);
            std::string low = toLower(tok);
            if (low == "ctrl" || low == "control" || low == "ctl") {
                seq.mods |= Ctrl;
            } else if (low == "shift") {
                seq.mods |= Shift;
            } else if (low == "alt" || low == "meta" || low == "option") {
                seq.mods |= Alt;
            } else {
                seq.key = parseKeyName(tok);
            }
        }
        return seq;
    }

private:
    static std::string toLower(std::string s) {
        for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    }

    // JKey -> canonical token. Kept in sync with parseKeyName below.
    static std::string keyName(JKeyEvent::JKey k) {
        using K = JKeyEvent::JKey;
        switch (k) {
            case K::Tab:      return "Tab";
            case K::BackTab:  return "BackTab";
            case K::Return:   return "Enter";
            case K::Space:    return "Space";
            case K::Escape:   return "Esc";
            case K::Backspace:return "Backspace";
            case K::Delete:   return "Del";
            case K::Left:     return "Left";
            case K::Right:    return "Right";
            case K::Up:       return "Up";
            case K::Down:     return "Down";
            case K::Home:     return "Home";
            case K::End:      return "End";
            case K::PageUp:   return "PageUp";
            case K::PageDown: return "PageDown";
            default: break;
        }
        if (k >= K::F1 && k <= K::F12)
            return "F" + std::to_string(1 + (static_cast<uint32_t>(k) - static_cast<uint32_t>(K::F1)));
        if (k >= K::A && k <= K::Z)
            return std::string(1, static_cast<char>('A' + (static_cast<uint32_t>(k) - static_cast<uint32_t>(K::A))));
        if (k >= K::_0 && k <= K::_9)
            return std::string(1, static_cast<char>('0' + (static_cast<uint32_t>(k) - static_cast<uint32_t>(K::_0))));
        return {};
    }

    // Token -> JKey (case-insensitive on named keys; single A-Z / 0-9 literal).
    static JKeyEvent::JKey parseKeyName(const std::string& tok) {
        using K = JKeyEvent::JKey;
        if (tok.empty()) return K::Unknown;
        std::string low = toLower(tok);
        if (low == "tab")      return K::Tab;
        if (low == "backtab")  return K::BackTab;
        if (low == "enter" || low == "return") return K::Return;
        if (low == "space")    return K::Space;
        if (low == "esc" || low == "escape")   return K::Escape;
        if (low == "backspace")return K::Backspace;
        if (low == "del" || low == "delete")   return K::Delete;
        if (low == "left")     return K::Left;
        if (low == "right")    return K::Right;
        if (low == "up")       return K::Up;
        if (low == "down")     return K::Down;
        if (low == "home")     return K::Home;
        if (low == "end")      return K::End;
        if (low == "pageup" || low == "pgup")     return K::PageUp;
        if (low == "pagedown" || low == "pgdn" || low == "pgdown") return K::PageDown;
        // Function keys F1..F12.
        if ((low[0] == 'f') && low.size() >= 2) {
            bool allDigits = true;
            for (size_t j = 1; j < low.size(); ++j)
                if (!std::isdigit(static_cast<unsigned char>(low[j]))) { allDigits = false; break; }
            if (allDigits) {
                int fn = std::stoi(low.substr(1));
                if (fn >= 1 && fn <= 12)
                    return static_cast<K>(static_cast<uint32_t>(K::F1) + (fn - 1));
            }
        }
        // Single letter / digit literal.
        if (tok.size() == 1) {
            char c = tok[0];
            if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
            if (c >= 'A' && c <= 'Z')
                return static_cast<K>(static_cast<uint32_t>(K::A) + (c - 'A'));
            if (c >= '0' && c <= '9')
                return static_cast<K>(static_cast<uint32_t>(K::_0) + (c - '0'));
        }
        return K::Unknown;
    }
};

} // inline namespace jf

// Hash support so JKeySequence can key an unordered_map.
namespace std {
template <>
struct hash<jf::JKeySequence> {
    size_t operator()(const jf::JKeySequence& s) const noexcept {
        return std::hash<uint64_t>{}(s.packed());
    }
};
} // namespace std
