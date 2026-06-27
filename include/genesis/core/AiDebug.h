#pragma once

// ============================================================================
// AiDebug — AI bus introspection and debugging utilities
//
// Include this header in any file that needs to inspect or log the live AI bus
// state. Does NOT need to be included in production builds — no heap unless
// you call dump functions.
//
// Usage:
//   // Dump the full semantic tree to stdout:
//   AiDebug::printSemanticTree();
//
//   // Dump a single widget's AI state:
//   AiDebug::printNode(*myButton);
//
//   // Get a structured string representation (for logging, UI panels, etc.):
//   std::string tree = AiDebug::dumpSemanticTree();
//
//   // Inspect the shared-memory AI bus from the probe process:
//   AiDebug::printBusSnapshot(region);
// ============================================================================

#include "BaseWidgets.h"
#include "AiControlBus.h"
#include "DockWidget.h"
#include <string>
#include <sstream>
#include <iomanip>
#include <iostream>
#include <cstdio>

namespace Genesis {

class AiDebug {
public:
    // ── Per-widget inspection ───────────────────────────────────────────────

    static std::string describeNode(const Widget& w) {
        auto node = w.getSemanticNode();
        auto bb   = w.getBoundingBox();

        std::ostringstream s;
        s << std::left
          << std::setw(18) << node.role
          << std::setw(22) << _trunc(node.label, 21)
          << std::setw(16) << _trunc(node.value, 15)
          << "  "
          << _flags(w)
          << "  @("
          << static_cast<int>(bb.x) << ","
          << static_cast<int>(bb.y) << " "
          << static_cast<int>(bb.width) << "×"
          << static_cast<int>(bb.height) << ")"
          << "  id=" << w.getNodeId();
        return s.str();
    }

    static void printNode(const Widget& w) {
        std::cout << "[AiDebug] " << describeNode(w) << '\n';
    }

    // ── Full tree dump ──────────────────────────────────────────────────────

    static std::string dumpSemanticTree() {
        std::ostringstream out;
        out << "═══ Genesis Semantic Tree ("
            << Widget::s_activeWidgets.size() << " widgets";
        if (!DockWidget::s_activeDocks.empty())
            out << " + " << DockWidget::s_activeDocks.size() << " docks";
        out << ") ═══\n";
        out << std::left
            << std::setw(18) << "Role"
            << std::setw(22) << "Label"
            << std::setw(16) << "Value"
            << "  Flags     Geometry      NodeId\n";
        out << std::string(90, '─') << '\n';

        for (auto* w : Widget::s_activeWidgets) {
            if (!w) continue;
            out << describeNode(*w) << '\n';
        }

        for (auto* d : DockWidget::s_activeDocks) {
            if (!d) continue;
            auto node = d->getSemanticNode();
            out << std::left
                << std::setw(18) << node.role
                << std::setw(22) << _trunc(node.label, 21)
                << std::setw(16) << _trunc(node.value, 15)
                << "  [float]"
                << "  @(" << static_cast<int>(d->x()) << ","
                << static_cast<int>(d->y()) << " "
                << static_cast<int>(d->width()) << "×"
                << static_cast<int>(d->height()) << ")"
                << "  dock\n";
        }
        return out.str();
    }

    static void printSemanticTree() {
        std::cout << dumpSemanticTree() << std::flush;
    }

    // ── AI bus shared-memory snapshot (probe/agent side) ───────────────────

    static std::string dumpBusSnapshot(const SharedBusMemory* region) {
        if (!region) return "(null region)\n";
        std::vector<AiNodeDescriptor> nodes;
        uint64_t frame = 0;
        if (!AiControlBus::snapshot(region, nodes, &frame))
            return "(snapshot failed — seqlock conflict or invalid cookie)\n";

        std::ostringstream out;
        out << "═══ AI Bus Snapshot  frame=" << frame
            << "  nodes=" << nodes.size() << " ═══\n";
        out << std::left
            << std::setw(8)  << "id"
            << std::setw(18) << "role"
            << std::setw(22) << "name"
            << std::setw(16) << "value"
            << std::setw(10) << "flags"
            << "geometry\n";
        out << std::string(90, '─') << '\n';

        for (const auto& n : nodes) {
            out << std::setw(8)  << n.id
                << std::setw(18) << _trunc(n.role,  17)
                << std::setw(22) << _trunc(n.name,  21)
                << std::setw(16) << _trunc(n.value, 15)
                << std::setw(10) << _busFlags(n.stateFlags)
                << "@(" << static_cast<int>(n.x) << ","
                << static_cast<int>(n.y) << " "
                << static_cast<int>(n.width) << "×"
                << static_cast<int>(n.height) << ")\n";
        }
        return out.str();
    }

    static void printBusSnapshot(const SharedBusMemory* region) {
        std::cout << dumpBusSnapshot(region) << std::flush;
    }

    // ── Action trace — log and dispatch a semantic action ──────────────────
    // Useful for testing: finds the widget, calls executeSemanticAction, logs result.

    static bool executeAndTrace(uint32_t nodeId, const std::string& action) {
        for (auto* w : Widget::s_activeWidgets) {
            if (!w) continue;
            if (static_cast<uint32_t>(w->getNodeId()) != nodeId) continue;
            bool ok = w->executeSemanticAction(action);
            std::cout << "[AiDebug] action \"" << action
                      << "\" on node " << nodeId
                      << " (" << w->getSemanticNode().role << " \""
                      << w->getSemanticNode().label << "\")"
                      << " → " << (ok ? "handled" : "rejected") << '\n';
            return ok;
        }
        // Try DockWidgets
        for (auto* d : DockWidget::s_activeDocks) {
            if (!d) continue;
            uint32_t id = 0xF0000000u |
                static_cast<uint32_t>(reinterpret_cast<std::uintptr_t>(d) & 0x0FFFFFFFu);
            if (id != nodeId) continue;
            bool ok = d->executeSemanticAction(action);
            std::cout << "[AiDebug] action \"" << action
                      << "\" on dock \"" << d->title() << "\""
                      << " → " << (ok ? "handled" : "rejected") << '\n';
            return ok;
        }
        std::cout << "[AiDebug] action \"" << action
                  << "\" — node " << nodeId << " not found\n";
        return false;
    }

    // ── Find widget by label or role ────────────────────────────────────────

    static Widget* findByLabel(const std::string& label) {
        for (auto* w : Widget::s_activeWidgets) {
            if (w && w->getSemanticNode().label == label) return w;
        }
        return nullptr;
    }

    static Widget* findByRole(const std::string& role) {
        for (auto* w : Widget::s_activeWidgets) {
            if (w && w->getSemanticNode().role == role) return w;
        }
        return nullptr;
    }

    static std::vector<Widget*> findAllByRole(const std::string& role) {
        std::vector<Widget*> out;
        for (auto* w : Widget::s_activeWidgets)
            if (w && w->getSemanticNode().role == role) out.push_back(w);
        return out;
    }

private:
    static std::string _trunc(const std::string& s, size_t maxLen) {
        if (s.size() <= maxLen) return s;
        return s.substr(0, maxLen - 1) + "…";
    }

    static std::string _trunc(const char* s, size_t maxLen) {
        return _trunc(std::string(s ? s : ""), maxLen);
    }

    static std::string _flags(const Widget& w) {
        std::string f;
        f += w.isEnabled()  ? 'E' : '-';
        f += w.isVisible()  ? 'V' : '-';
        f += w.isFocused()  ? 'F' : '-';
        f += (w.getState() == WidgetState::Pressed) ? 'P' : '-';
        f += w.getSemanticNode().interactable ? 'I' : '-';
        return f;
    }

    static std::string _busFlags(uint32_t f) {
        std::string s;
        s += (f & AiEnabled)      ? 'E' : '-';
        s += (f & AiVisible)      ? 'V' : '-';
        s += (f & AiFocused)      ? 'F' : '-';
        s += (f & AiChecked)      ? 'C' : '-';
        s += (f & AiPressed)      ? 'P' : '-';
        s += (f & AiInteractable) ? 'I' : '-';
        return s;
    }
};

} // namespace Genesis
