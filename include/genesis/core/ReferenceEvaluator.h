#pragma once

// ============================================================================
// Genesis::Evaluator — OPT-IN, generic, precompiled expression evaluation.
//
// A small arithmetic/logic evaluator that is deliberately a *framework* primitive:
// it knows nothing about channels, config, telemetry, derived values, memoisation
// or cycles. Those are application concepts built ON TOP by registering resolver
// domains (see registerResolver) — the evaluator only computes, calling back to the
// app for the value of any variable token.
//
//   parse ONCE:   Program p = ev.compile("[#a]*10/2.0 + [!cfg] + [%rpm]");
//   run MANY:     double v = ev.run(p);              // no regex on the hot path
//
// Threading: compile() is an edit/load-time op (writes diagnostics into the returned
// Program, not into the evaluator). run() is PURE and REENTRANT — it touches no
// evaluator state, so the same Program may be run concurrently from many threads, and
// an app resolver may recurse back into run() (that is how derived values reference
// each other). The only shared mutable state is the resolver registry: register all
// domains up front (read-only during the eval phase), or snapshot via setResolvers().
//
// Errors never throw. compile() reports {ok, errorPos, message} in the Program; run()
// returns NaN on a runtime fault (unresolved reference, malformed program) so a bad
// expression degrades one value instead of crashing the app — and NaN propagates,
// which an app cycle-guard can also use as its sentinel.
//
// Grammar: numbers; + - * / % ^ (^ right-assoc); comparisons == != < <= > >= ;
// logical && || ; unary minus; parentheses; functions
//   abs sqrt sin cos floor ceil round       (1 arg)
//   min max mod pow                          (2 args)
//   clamp(x,lo,hi) select(c,a,b) lerp(t,a,b) (3 args)
// A variable token is a bracket-delimited reference, e.g. [#name], [!cfg.kp],
// [%rpm], or an identifier-headed path (tbl.cursorRow). The text inside is handed
// verbatim to the resolver; a leading sigil ($ # @ ~) can force a specific domain.
// ============================================================================

#include "Variant.h"

#include <string>
#include <vector>
#include <stack>
#include <cmath>
#include <cstdint>
#include <limits>
#include <functional>
#include <regex>
#include <algorithm>
#include <memory>
#include <unordered_map>

namespace Genesis {

class Evaluator {
public:
    using Resolver = std::function<Variant(const std::string&)>;

    // ---- compiled program (immutable after compile; shareable across threads) ----
    enum class Op : uint8_t {
        Add, Sub, Mul, Div, Mod, Pow,
        Eq, Ne, Lt, Le, Gt, Ge, And, Or, Neg,
        Abs, Sqrt, Sin, Cos, Floor, Ceil, Round,
        Min, Max, FnMod, FnPow, Clamp, Select, Lerp,
    };
    struct Instr {
        enum class Kind : uint8_t { Num, Var, Op } kind;
        double num = 0.0;   // Kind::Num
        int    var = -1;    // Kind::Var -> index into Program::vars
        Op     op{};        // Kind::Op
    };
    struct Program {
        std::vector<Instr>       code;
        std::vector<std::string> vars;        // interned reference texts (resolved at run)
        bool        ok = false;
        int         errorPos = -1;            // byte offset in source, or -1
        std::string error;
        explicit operator bool() const { return ok; }
        const std::vector<std::string>& references() const { return vars; }  // for the app's dep graph
    };

    // ---- compile: parse + lower to a flat program, ONCE (edit / load time) ----
    Program compile(const std::string& src) const {
        Program p;
        std::string err; int pos = -1;
        std::vector<Token> toks = tokenize(src, err, pos);
        if (!err.empty()) { p.error = err; p.errorPos = pos; return p; }
        std::vector<Token> rpn = shunt(toks, err, pos);
        if (!err.empty()) { p.error = err; p.errorPos = pos; return p; }
        lower(rpn, p, err, pos);
        if (!err.empty()) { p.code.clear(); p.vars.clear(); p.error = err; p.errorPos = pos; return p; }
        p.ok = true;
        return p;
    }

    // ---- run: stack machine over the program, PURE + REENTRANT + thread-safe ----
    // Variable tokens resolve through `resolve`; returns NaN on a runtime fault.
    static double run(const Program& p, const Resolver& resolve) {
        if (!p.ok) return std::numeric_limits<double>::quiet_NaN();
        std::vector<double> st;
        st.reserve(p.code.size());
        for (const Instr& in : p.code) {
            switch (in.kind) {
                case Instr::Kind::Num:
                    st.push_back(in.num);
                    break;
                case Instr::Kind::Var: {
                    Variant v = resolve ? resolve(p.vars[in.var]) : Variant{};
                    if (v.isNull()) return std::numeric_limits<double>::quiet_NaN();
                    st.push_back(v.toDouble());
                    break;
                }
                case Instr::Kind::Op: {
                    const int n = arity(in.op);
                    if (static_cast<int>(st.size()) < n) return std::numeric_limits<double>::quiet_NaN();
                    double a = 0, b = 0, c = 0;
                    if (n >= 3) { c = st.back(); st.pop_back(); }
                    if (n >= 2) { b = st.back(); st.pop_back(); }
                    a = st.back(); st.pop_back();
                    st.push_back(apply(in.op, a, b, c));
                    break;
                }
            }
        }
        if (st.size() != 1) return std::numeric_limits<double>::quiet_NaN();
        return st.back();
    }

    // Run against the registered domains (probe order / sigils — see registerResolver).
    double run(const Program& p) const {
        return run(p, [this](const std::string& n) { return resolveToken(n); });
    }

    // ---- convenience: compile + run in one call (one-shots only) ----
    double eval(const std::string& expr, const Resolver& resolve) const {
        return run(compile(expr), resolve);
    }
    double eval(const std::string& expr) const {
        return run(compile(expr));
    }

    // ---- resolver registry — the framework extension point ----
    // Register a value DOMAIN. The app supplies the domains the framework cannot know
    // ("config", "telemetry", or its own derived-value store). Un-prefixed tokens are
    // probed across domains in descending priority (first non-Null wins); a token may
    // force one domain via its optional sigil ([#x] etc.). NOT safe to call concurrently
    // with run(): register up front, or build a snapshot and install via setResolvers().
    Evaluator& registerResolver(std::string name, Resolver fn, int priority = 0, char sigil = 0) {
        for (auto& d : m_domains) {
            if (d.name == name) { d.fn = std::move(fn); d.priority = priority; d.sigil = sigil; sortDomains(); return *this; }
        }
        m_domains.push_back({std::move(name), std::move(fn), priority, sigil});
        sortDomains();
        return *this;
    }
    void clearResolvers() { m_domains.clear(); }

private:
    enum class T { Num, Var, Func, Op, LParen, RParen, Comma };
    struct Token { T type; std::string text; int prec = 0; bool rightAssoc = false; bool unary = false; int pos = 0; };

    struct Domain { std::string name; Resolver fn; int priority = 0; char sigil = 0; };
    std::vector<Domain> m_domains;

    static bool isSigil(char c) { return c == '$' || c == '#' || c == '@' || c == '~'; }

    void sortDomains() {
        std::stable_sort(m_domains.begin(), m_domains.end(),
                         [](const Domain& a, const Domain& b) { return a.priority > b.priority; });
    }

    Variant resolveToken(const std::string& name) const {
        if (!name.empty() && isSigil(name[0])) {
            const char s = name[0];
            const std::string rest = name.substr(1);
            for (const Domain& d : m_domains) if (d.sigil == s) return d.fn ? d.fn(rest) : Variant{};
            return Variant{};
        }
        for (const Domain& d : m_domains) {
            Variant v = d.fn ? d.fn(name) : Variant{};
            if (!v.isNull()) return v;
        }
        return Variant{};
    }

    static const std::unordered_map<std::string, int>& funcArity() {
        static const std::unordered_map<std::string, int> a = {
            {"abs",1},{"sqrt",1},{"sin",1},{"cos",1},{"floor",1},{"ceil",1},{"round",1},
            {"min",2},{"max",2},{"mod",2},{"pow",2},
            {"clamp",3},{"select",3},{"lerp",3},
        };
        return a;
    }

    static int precOf(const std::string& op) {
        if (op == "||") return 1;
        if (op == "&&") return 2;
        if (op == "==" || op == "!=") return 3;
        if (op == "<" || op == "<=" || op == ">" || op == ">=") return 4;
        if (op == "+" || op == "-") return 5;
        if (op == "*" || op == "/" || op == "%") return 6;
        if (op == "^") return 7;
        return 0;
    }

    // Tokenise. `err`/`pos` left empty on success. A bracket reference [..] is one Var.
    static std::vector<Token> tokenize(const std::string& expr, std::string& err, int& pos) {
        std::vector<Token> out;
        static const std::regex re(
            R"((\[[^\]]*\])|([0-9]*\.?[0-9]+)|(==|!=|>=|<=|&&|\|\|)|([-+*/^%<>(),])|([$#@~]?[A-Za-z_][A-Za-z0-9_.\[\]]*))");
        auto begin = std::sregex_iterator(expr.begin(), expr.end(), re);
        auto end   = std::sregex_iterator();
        bool havePrev = false; T prevType{};
        for (auto it = begin; it != end; ++it) {
            const std::smatch& m = *it;
            const int p = static_cast<int>(m.position(0));
            const size_t before = out.size();
            if (m[1].matched) {                                   // [reference]
                std::string inner = m[1].str();
                inner = inner.substr(1, inner.size() - 2);        // strip [ ]
                out.push_back({T::Var, inner}); prevType = T::Var; havePrev = true;
            } else if (m[2].matched) {                            // number
                out.push_back({T::Num, m[2].str()}); prevType = T::Num; havePrev = true;
            } else if (m[3].matched || m[4].matched) {            // operator / paren / comma
                std::string s = m[3].matched ? m[3].str() : m[4].str();
                if (s == "(") { out.push_back({T::LParen, s}); prevType = T::LParen; }
                else if (s == ")") { out.push_back({T::RParen, s}); prevType = T::RParen; }
                else if (s == ",") { out.push_back({T::Comma, s}); prevType = T::Comma; }
                else {
                    const bool unaryCtx = !havePrev || prevType == T::Op || prevType == T::LParen || prevType == T::Comma;
                    if (unaryCtx && (s == "-" || s == "+")) {
                        if (s == "-") out.push_back({T::Op, "u-", 8, true, true});
                        prevType = T::Op;
                    } else {
                        out.push_back({T::Op, s, precOf(s), s == "^", false});
                        prevType = T::Op;
                    }
                }
                havePrev = true;
            } else if (m[5].matched) {                            // bare identifier path or function
                std::string s = m[5].str();
                if (funcArity().count(s)) { out.push_back({T::Func, s}); prevType = T::Func; }
                else                      { out.push_back({T::Var, s});  prevType = T::Var;  }
                havePrev = true;
            }
            if (out.size() > before) out.back().pos = p;
        }
        if (out.empty()) { err = "empty expression"; pos = 0; }
        return out;
    }

    static std::vector<Token> shunt(const std::vector<Token>& toks, std::string& err, int& pos) {
        std::vector<Token> output;
        std::stack<Token> ops;
        auto setErr = [&](const std::string& why, int p) { if (err.empty()) { err = why; pos = p; } };
        for (const Token& t : toks) {
            switch (t.type) {
                case T::Num:
                case T::Var:
                    output.push_back(t);
                    break;
                case T::Func:
                    ops.push(t);
                    break;
                case T::Comma:
                    while (!ops.empty() && ops.top().type != T::LParen) { output.push_back(ops.top()); ops.pop(); }
                    if (ops.empty()) setErr("misplaced comma", t.pos);
                    break;
                case T::Op:
                    while (!ops.empty() && ops.top().type == T::Op &&
                           ((!t.rightAssoc && ops.top().prec >= t.prec) ||
                            ( t.rightAssoc && ops.top().prec >  t.prec))) {
                        output.push_back(ops.top()); ops.pop();
                    }
                    ops.push(t);
                    break;
                case T::LParen:
                    ops.push(t);
                    break;
                case T::RParen:
                    while (!ops.empty() && ops.top().type != T::LParen) { output.push_back(ops.top()); ops.pop(); }
                    if (ops.empty()) { setErr("unbalanced ')'", t.pos); return output; }
                    ops.pop();
                    if (!ops.empty() && ops.top().type == T::Func) { output.push_back(ops.top()); ops.pop(); }
                    break;
            }
        }
        while (!ops.empty()) {
            if (ops.top().type == T::LParen) { setErr("unbalanced '('", ops.top().pos); break; }
            output.push_back(ops.top()); ops.pop();
        }
        return output;
    }

    // Lower RPN tokens to the flat program: map operator/function text to an Op enum
    // and intern variable texts, so run() never touches a string operator or re-parses.
    static void lower(const std::vector<Token>& rpn, Program& p, std::string& err, int& pos) {
        auto setErr = [&](const std::string& why, int pp) { if (err.empty()) { err = why; pos = pp; } };
        auto intern = [&](const std::string& name) -> int {
            for (size_t i = 0; i < p.vars.size(); ++i) if (p.vars[i] == name) return static_cast<int>(i);
            p.vars.push_back(name);
            return static_cast<int>(p.vars.size() - 1);
        };
        int depth = 0;          // simulated stack depth — validates operand arity at COMPILE time
        int lastPos = 0;
        for (const Token& t : rpn) {
            lastPos = t.pos;
            switch (t.type) {
                case T::Num: {
                    Instr in; in.kind = Instr::Kind::Num; in.num = std::stod(t.text);
                    p.code.push_back(in); ++depth; break;
                }
                case T::Var: {
                    Instr in; in.kind = Instr::Kind::Var; in.var = intern(t.text);
                    p.code.push_back(in); ++depth; break;
                }
                case T::Op:
                case T::Func: {
                    Op o;
                    if (t.type == T::Op ? !opFor(t, o) : !funcOp(t.text, o)) {
                        setErr("bad " + std::string(t.type == T::Op ? "operator '" : "function '") + t.text + "'", t.pos);
                        return;
                    }
                    const int n = arity(o);
                    if (depth < n) { setErr("missing operand", t.pos); return; }
                    depth = depth - n + 1;
                    Instr in; in.kind = Instr::Kind::Op; in.op = o;
                    p.code.push_back(in); break;
                }
                default:
                    setErr("unexpected token", t.pos); return;
            }
        }
        if (depth != 1) setErr("malformed expression", lastPos);
    }

    static bool opFor(const Token& t, Op& o) {
        if (t.unary && t.text == "u-") { o = Op::Neg; return true; }
        const std::string& s = t.text;
        if (s == "+") o = Op::Add; else if (s == "-") o = Op::Sub; else if (s == "*") o = Op::Mul;
        else if (s == "/") o = Op::Div; else if (s == "%") o = Op::Mod; else if (s == "^") o = Op::Pow;
        else if (s == "==") o = Op::Eq; else if (s == "!=") o = Op::Ne; else if (s == "<") o = Op::Lt;
        else if (s == "<=") o = Op::Le; else if (s == ">") o = Op::Gt; else if (s == ">=") o = Op::Ge;
        else if (s == "&&") o = Op::And; else if (s == "||") o = Op::Or;
        else return false;
        return true;
    }

    static bool funcOp(const std::string& f, Op& o) {
        if (f == "abs") o = Op::Abs; else if (f == "sqrt") o = Op::Sqrt; else if (f == "sin") o = Op::Sin;
        else if (f == "cos") o = Op::Cos; else if (f == "floor") o = Op::Floor; else if (f == "ceil") o = Op::Ceil;
        else if (f == "round") o = Op::Round; else if (f == "min") o = Op::Min; else if (f == "max") o = Op::Max;
        else if (f == "mod") o = Op::FnMod; else if (f == "pow") o = Op::FnPow; else if (f == "clamp") o = Op::Clamp;
        else if (f == "select") o = Op::Select; else if (f == "lerp") o = Op::Lerp;
        else return false;
        return true;
    }

    static int arity(Op o) {
        switch (o) {
            case Op::Neg: case Op::Abs: case Op::Sqrt: case Op::Sin: case Op::Cos:
            case Op::Floor: case Op::Ceil: case Op::Round: return 1;
            case Op::Clamp: case Op::Select: case Op::Lerp: return 3;
            default: return 2;
        }
    }

    static double apply(Op o, double a, double b, double c) {
        switch (o) {
            case Op::Add: return a + b;     case Op::Sub: return a - b;     case Op::Mul: return a * b;
            case Op::Div: return b == 0.0 ? 0.0 : a / b;
            case Op::Mod: return b == 0.0 ? 0.0 : std::fmod(a, b);
            case Op::Pow: return std::pow(a, b);
            case Op::Eq:  return a == b ? 1.0 : 0.0;   case Op::Ne: return a != b ? 1.0 : 0.0;
            case Op::Lt:  return a <  b ? 1.0 : 0.0;   case Op::Le: return a <= b ? 1.0 : 0.0;
            case Op::Gt:  return a >  b ? 1.0 : 0.0;   case Op::Ge: return a >= b ? 1.0 : 0.0;
            case Op::And: return (a != 0.0 && b != 0.0) ? 1.0 : 0.0;
            case Op::Or:  return (a != 0.0 || b != 0.0) ? 1.0 : 0.0;
            case Op::Neg: return -a;
            case Op::Abs: return std::fabs(a);  case Op::Sqrt: return std::sqrt(a);
            case Op::Sin: return std::sin(a);   case Op::Cos:  return std::cos(a);
            case Op::Floor:return std::floor(a);case Op::Ceil: return std::ceil(a);
            case Op::Round:return std::round(a);
            case Op::Min: return std::min(a, b);    case Op::Max: return std::max(a, b);
            case Op::FnMod:return b == 0.0 ? 0.0 : std::fmod(a, b);
            case Op::FnPow:return std::pow(a, b);
            case Op::Clamp: return std::min(std::max(a, b), c);
            case Op::Select:return a != 0.0 ? b : c;
            case Op::Lerp:  return b + a * (c - b);
        }
        return 0.0;
    }
};

}  // namespace Genesis
