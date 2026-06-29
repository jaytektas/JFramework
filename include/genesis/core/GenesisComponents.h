#pragma once

#include <string>
#include <vector>
#include <memory>
#include <cassert>
#include <iostream>
#include <cstring>

#include "Signal.h"
#include "ApplicationCore.h"
#include "SceneGraph.h"
#include "AiControlBus.h"
#include "AiBusHook.h"
#include "BaseWidgets.h"
#include "DockWidget.h"
#include "MainThreadDispatcher.h"

namespace { inline constexpr auto& LogWidget = Genesis::Log::Widgets; }

namespace Genesis {

/**
 * GObject — base lifecycle and slot-tracking.
 */
class GObject : public Core::SlotTracker {
public:
    explicit GObject(std::string name = "") : m_objectName(std::move(name)) {}
    virtual ~GObject() = default;

    GObject(const GObject&) = delete;
    GObject& operator=(const GObject&) = delete;

    const std::string& objectName() const noexcept { return m_objectName; }

private:
    std::string m_objectName;
};

// ============================================================================
// GApplication — coordinates the event loop, SceneGraph, AI Control Bus,
//                MainThreadDispatcher drain, and full semantic publishing.
// ============================================================================
class GApplication : public GObject, public Core::Application {
public:
    GApplication() : GObject("GApplication") {
        assert(s_instance == nullptr && "Only one GApplication may exist.");
        s_instance = this;

        // Prefer cross-process shared-memory so an external AI agent can attach.
        // Fall back to the in-process pool for headless / test runs.
        if (!m_aiBus.createSegment())
            m_aiBus.attach(&m_fallbackPool);

        // Wire every widget interaction onto the outbound AI signal bus.
        AiBusHook::install([this](uint32_t nodeId, const char* sig, const char* val) {
            m_aiBus.publishSignal(nodeId, sig, val);
        });

        // Per-frame GUI work is done in onFrameTick() (overridden below) — the base
        // loop drains the dispatcher and calls it, leaving onFrameUpdate for the user.
    }

    ~GApplication() {
        AiBusHook::install(nullptr);
        s_instance = nullptr;
    }

    static GApplication* instance() noexcept { return s_instance; }
    SceneGraph&    sceneGraph()  noexcept { return m_sceneGraph; }
    AiControlBus&  aiBus()       noexcept { return m_aiBus; }

    void publishSignal(uint32_t nodeId, const char* signal, const char* value) {
        m_aiBus.publishSignal(nodeId, signal, value);
    }

    // ---- Rich semantic snapshot ------------------------------------------
    // Builds a full AiNodeDescriptor array from Widget::s_activeWidgets and
    // publishes it over the AI control bus (seqlock-protected, readable from
    // any process that has mapped the shared segment).
    void publishSemanticSnapshot() {
        m_descriptors.clear();
        for (auto* w : Widget::s_activeWidgets) {
            if (!w) continue;
            auto node = w->getSemanticNode();
            auto bb   = w->getBoundingBox();

            AiNodeDescriptor d{};
            d.id     = static_cast<uint32_t>(w->getNodeId());
            d.x      = bb.x;
            d.y      = bb.y;
            d.width  = bb.width;
            d.height = bb.height;

            // Populate state flags
            d.stateFlags = 0;
            if (w->isEnabled())                         d.stateFlags |= AiEnabled;
            if (w->isVisible())                         d.stateFlags |= AiVisible;
            if (w->isFocused())                         d.stateFlags |= AiFocused;
            if (w->getState() == WidgetState::Pressed)  d.stateFlags |= AiPressed;
            if (node.interactable)                      d.stateFlags |= AiInteractable;
            // Infer AiChecked from semantic value (CheckBox, ToggleButton, RadioButton)
            if (node.value == "true" || node.value == "checked")
                d.stateFlags |= AiChecked;

            aiSetField(d.role,  sizeof(d.role),  node.role);
            aiSetField(d.name,  sizeof(d.name),  node.label);
            aiSetField(d.value, sizeof(d.value), node.value);

            m_descriptors.push_back(d);
        }
        // Also publish floating DockWidgets (live outside the SceneGraph layout tree)
        for (auto* dock : DockWidget::s_activeDocks) {
            if (!dock) continue;
            auto node = dock->getSemanticNode();
            AiNodeDescriptor d{};
            d.id     = 0xF0000000u | static_cast<uint32_t>(
                           reinterpret_cast<std::uintptr_t>(dock) & 0x0FFFFFFFu);
            d.x      = dock->x();
            d.y      = dock->y();
            d.width  = dock->width();
            d.height = dock->height();
            d.stateFlags = AiEnabled | AiVisible | AiInteractable;
            if (dock->isPinned()) d.stateFlags |= AiChecked;
            aiSetField(d.role,  sizeof(d.role),  node.role);
            aiSetField(d.name,  sizeof(d.name),  node.label);
            aiSetField(d.value, sizeof(d.value), node.value);
            m_descriptors.push_back(d);
        }

        m_aiBus.publishNodes(m_descriptors.data(),
                             static_cast<uint32_t>(m_descriptors.size()));
    }

    int exec(std::unique_ptr<Core::PlatformWindow> nativeWindow) {
        return run(std::move(nativeWindow));   // run() inherited from Core::Application
    }

protected:
    // GUI per-frame work: publish the rich semantic snapshot + service any pending AI
    // action. The base loop calls this each frame after draining the dispatcher.
    void onFrameTick(double) override {
        publishSemanticSnapshot();
        _pollAndDispatchAction();
    }

private:
    // Poll for an inbound semantic action from the AI agent and dispatch it
    // to the target widget on the UI thread.
    void _pollAndDispatchAction() {
        uint32_t targetId = 0;
        char     action[64] = {};
        uint32_t seq  = 0;
        if (!m_aiBus.pollAction(targetId, action, sizeof(action), seq)) return;

        int result = -1;  // default: bad target
        for (auto* w : Widget::s_activeWidgets) {
            if (!w) continue;
            if (static_cast<uint32_t>(w->getNodeId()) == targetId) {
                result = w->executeSemanticAction(action) ? 1 : 0;
                break;
            }
        }
        m_aiBus.ackAction(seq, result);
    }

    static GApplication* s_instance;
    SceneGraph   m_sceneGraph;
    AiControlBus m_aiBus;
    SharedBusMemory   m_fallbackPool{};
    std::vector<AiNodeDescriptor> m_descriptors;  // reused each frame to avoid alloc
};

inline GApplication* GApplication::s_instance = nullptr;

// ============================================================================
// GMainWindow — top-level window. One widget base only: it IS a Widget (the
// parallel GWidget base is gone). It pulls the shared SceneGraph from the
// application singleton, so a caller still just constructs it by title.
// ============================================================================
class GMainWindow : public Widget {
public:
    explicit GMainWindow(std::string title = "Genesis Window")
        : Widget(GApplication::instance()->sceneGraph(), std::move(title))
    {
        m_graph.getLayout(m_nodeId).direction      = FlexDirection::Column;
        m_graph.getLayout(m_nodeId).justifyContent = JustifyContent::FlexStart;
    }

    // A window is a container; its children paint themselves.
    void populateRenderPrimitives(PrimitiveBuffer&) override {}

    void show() {
        qCInfo(LogWidget) << "Showing main window: " << m_debugName << std::endl;
        Constraints constraints{800.0f, 1920.0f, 600.0f, 1080.0f};
        m_graph.computeLayout(m_nodeId, constraints);
        // Publish initial snapshot immediately (before the first frame fires).
        GApplication::instance()->publishSemanticSnapshot();
    }
};

} // namespace Genesis
