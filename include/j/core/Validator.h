#pragma once

// ============================================================================
// JValidator — input-acceptability policy for text fields (JLineEdit et al.).
//
// A validator classifies a candidate string into one of three states:
//   Invalid       — the string can never be part of a valid entry; a keystroke
//                   that would produce it is rejected outright.
//   Intermediate  — not yet valid, but could still become valid with more
//                   typing (e.g. "" or "1" while a [10,99] range is expected).
//                   Allowed WHILE editing; not accepted on commit.
//   Acceptable    — a fully valid entry.
//
// A field wires a validator with JLineEdit::setValidator(). Per keystroke it
// rejects anything that would make the text Invalid (Intermediate is allowed so
// the user can type toward a valid value). On commit / focus-out, if the text is
// not Acceptable the field first calls fixup() (which clamps numeric text into
// range) and, if that still isn't Acceptable, REVERTS to the last committed
// acceptable text.
//
// Clean-room: the tri-state concept mirrors the well-known validator contract but
// no third-party source was consulted or copied.
// ============================================================================

#include <string>
#include <regex>
#include <cmath>
#include <cstdlib>
#include <algorithm>

inline namespace jf {

class JValidator {
public:
    enum State { Invalid, Intermediate, Acceptable };
    virtual ~JValidator() = default;

    // Classify `text`. `cursorPos` is the current caret (bytes); a validator may
    // read it but the base contract does not require adjusting it.
    virtual State validate(const std::string& text, int& cursorPos) const = 0;

    // Best-effort nudge of `text` toward Acceptable on commit (e.g. clamp a
    // number into range). Default: leave it unchanged.
    virtual void fixup(std::string& /*text*/) const {}
};

// ----------------------------------------------------------------------------
// JIntValidator — a whole-number field constrained to [min, max].
//   ""  or (min<0 and) "-"                  -> Intermediate
//   any non-digit (past an allowed sign)    -> Invalid
//   in range                                -> Acceptable
//   out of range but fewer significant
//     digits than the widest bound          -> Intermediate  (can still grow into range)
//   out of range and at/over that width     -> Invalid
// ----------------------------------------------------------------------------
class JIntValidator : public JValidator {
public:
    JIntValidator(long min, long max) : m_min(min), m_max(max) {}

    void setRange(long min, long max) { m_min = min; m_max = max; }
    long bottom() const { return m_min; }
    long top()    const { return m_max; }

    State validate(const std::string& s, int& /*cursorPos*/) const override {
        if (s.empty()) return Intermediate;
        if (s == "-")  return m_min < 0 ? Intermediate : Invalid;
        size_t i = 0;
        if (s[i] == '-') { if (m_min >= 0) return Invalid; ++i; }
        if (i >= s.size()) return Intermediate;
        for (size_t j = i; j < s.size(); ++j)
            if (s[j] < '0' || s[j] > '9') return Invalid;
        long v = 0;
        try { v = std::stol(s); } catch (...) { return Invalid; }
        if (v >= m_min && v <= m_max) return Acceptable;
        const size_t nd    = s.size() - i;                       // significant digits typed
        const size_t bound = std::max(_ndigits(m_min), _ndigits(m_max));
        return (nd < bound) ? Intermediate : Invalid;            // room to grow → keep typing
    }

    void fixup(std::string& s) const override {
        long v = 0;
        try { v = std::stol(s); } catch (...) { v = std::clamp<long>(0, m_min, m_max); }
        v = std::clamp(v, m_min, m_max);
        s = std::to_string(v);
    }

private:
    static size_t _ndigits(long x) {
        unsigned long u = static_cast<unsigned long>(x < 0 ? -x : x);
        size_t n = 1; while (u >= 10) { u /= 10; ++n; } return n;
    }
    long m_min, m_max;
};

// ----------------------------------------------------------------------------
// JDoubleValidator — a real-number field in [min, max] with at most `decimals`
// fractional digits.
//   "" / "-" / "." partials    -> Intermediate
//   too many fractional digits -> Invalid
//   malformed (bad char / two dots) -> Invalid
//   in range                   -> Acceptable
//   below range                -> Intermediate (can still grow)
//   above range                -> Invalid
// ----------------------------------------------------------------------------
class JDoubleValidator : public JValidator {
public:
    JDoubleValidator(double min, double max, int decimals)
        : m_min(min), m_max(max), m_decimals(decimals) {}

    void setRange(double min, double max) { m_min = min; m_max = max; }
    void setDecimals(int d) { m_decimals = d; }
    double bottom()  const { return m_min; }
    double top()     const { return m_max; }
    int    decimals() const { return m_decimals; }

    State validate(const std::string& s, int& /*cursorPos*/) const override {
        if (s.empty()) return Intermediate;
        size_t i = 0;
        if (s[i] == '-') { if (m_min >= 0) return Invalid; ++i; }
        bool dot = false, anyDigit = false; int dec = 0;
        for (; i < s.size(); ++i) {
            char c = s[i];
            if (c >= '0' && c <= '9') { anyDigit = true; if (dot) ++dec; }
            else if (c == '.') { if (dot || m_decimals == 0) return Invalid; dot = true; }
            else return Invalid;
        }
        if (dec > m_decimals) return Invalid;
        if (!anyDigit) return Intermediate;              // "-", ".", "-." etc.
        double v = 0.0;
        try { v = std::stod(s); } catch (...) { return Invalid; }
        if (v >= m_min && v <= m_max) return Acceptable;
        return (v < m_min) ? Intermediate : Invalid;     // below → can grow; above → dead
    }

    void fixup(std::string& s) const override {
        double v = 0.0;
        try { v = std::stod(s); } catch (...) { v = std::clamp(0.0, m_min, m_max); }
        v = std::clamp(v, m_min, m_max);
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%.*f", m_decimals, v);
        s = buf;
    }

private:
    double m_min, m_max;
    int    m_decimals;
};

// ----------------------------------------------------------------------------
// JRegexValidator — full-match acceptance against a std::regex.
//   full match -> Acceptable
//   ""         -> Intermediate
//   otherwise  -> Invalid
// NOTE: std::regex has no partial-match mode clean-room, so a non-empty
// non-matching prefix is Invalid. Regex fields therefore validate best on
// commit; per-keystroke rejection is coarse (a partly-typed pattern reads as
// Invalid). Prefer JIntValidator/JDoubleValidator for live numeric typing.
// ----------------------------------------------------------------------------
class JRegexValidator : public JValidator {
public:
    explicit JRegexValidator(std::regex re) : m_re(std::move(re)) {}

    State validate(const std::string& s, int& /*cursorPos*/) const override {
        if (std::regex_match(s, m_re)) return Acceptable;
        if (s.empty())                 return Intermediate;
        return Invalid;
    }

private:
    std::regex m_re;
};

} // inline namespace jf
