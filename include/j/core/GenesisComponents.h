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

namespace { inline constexpr auto& LogWidget = jf::Log::Widgets; }

inline namespace jf {

/**
 * JObject — base lifecycle and slot-tracking.
 */
class JObject : public jf::JSlotTracker {
public:
    explicit JObject(std::string name = "") : m_objectName(std::move(name)) {}
    virtual ~JObject() = default;

    JObject(const JObject&) = delete;
    JObject& operator=(const JObject&) = delete;

    const std::string& objectName() const noexcept { return m_objectName; }

private:
    std::string m_objectName;
};

// ============================================================================
// JGuiApplication — coordinates the event loop, JSceneGraph, AI JControl Bus,
//                JMainThreadDispatcher drain, and full semantic publishing.
// ============================================================================
class JGuiApplication : public JObject, public jf::JApplication {
public:
    JGuiApplication() : JObject("JGuiApplication") {
        assert(s_instance == nullptr && "Only one JGuiApplication may exist.");
        s_instance = this;

        // Prefer cross-process shared-memory so an external AI agent can attach.
        // Fall back to the in-process pool for headless / test runs.
        if (!m_aiBus.createSegment())
            m_aiBus.attach(&m_fallbackPool);

        // Wire every widget interaction onto the outbound AI signal bus.
        JAiBusHook::install([this](uint32_t nodeId, const char* sig, const char* val) {
            m_aiBus.publishSignal(nodeId, sig, val);
        });

        // Per-frame GUI work is done in onFrameTick() (overridden below) — the base
        // loop drains the dispatcher and calls it, leaving onFrameUpdate for the user.
    }

    ~JGuiApplication() {
        JAiBusHook::install(nullptr);
        s_instance = nullptr;
    }

    static JGuiApplication* instance() noexcept { return s_instance; }
    JSceneGraph&    sceneGraph()  noexcept { return m_sceneGraph; }
    JAiControlBus&  aiBus()       noexcept { return m_aiBus; }

    void publishSignal(uint32_t nodeId, const char* signal, const char* value) {
        m_aiBus.publishSignal(nodeId, signal, value);
    }

    // ---- Rich semantic snapshot ------------------------------------------
    // Builds a full JAiNodeDescriptor array from JWidget::s_activeWidgets and
    // publishes it over the AI control bus (seqlock-protected, readable from
    // any process that has mapped the shared segment).
    void publishSemanticSnapshot() {
        m_descriptors.clear();
        for (auto* w : JWidget::s_activeWidgets) {
            if (!w) continue;
            auto node = w->getSemanticNode();
            auto bb   = w->getBoundingBox();

            JAiNodeDescriptor d{};
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
            if (w->getState() == JWidgetState::Pressed)  d.stateFlags |= AiPressed;
            if (node.interactable)                      d.stateFlags |= AiInteractable;
            // Infer AiChecked from semantic value (JCheckBox, JToggleButton, JRadioButton)
            if (node.value == "true" || node.value == "checked")
                d.stateFlags |= AiChecked;

            aiSetField(d.role,  sizeof(d.role),  node.role);
            aiSetField(d.name,  sizeof(d.name),  node.label);
            aiSetField(d.value, sizeof(d.value), node.value);

            m_descriptors.push_back(d);
        }
        // Also publish floating DockWidgets (live outside the JSceneGraph layout tree)
        for (auto* dock : JDockWidget::s_activeDocks) {
            if (!dock) continue;
            auto node = dock->getSemanticNode();
            JAiNodeDescriptor d{};
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

    int exec(std::unique_ptr<jf::JPlatformWindow> nativeWindow) {
        return run(std::move(nativeWindow));   // run() inherited from jf::JApplication
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
        for (auto* w : JWidget::s_activeWidgets) {
            if (!w) continue;
            if (static_cast<uint32_t>(w->getNodeId()) == targetId) {
                result = w->executeSemanticAction(action) ? 1 : 0;
                break;
            }
        }
        m_aiBus.ackAction(seq, result);
    }

    static JGuiApplication* s_instance;
    JSceneGraph   m_sceneGraph;
    JAiControlBus m_aiBus;
    JSharedBusMemory   m_fallbackPool{};
    std::vector<JAiNodeDescriptor> m_descriptors;  // reused each frame to avoid alloc
};

inline JGuiApplication* JGuiApplication::s_instance = nullptr;

// ============================================================================
// JMainWindow — top-level window. One widget base only: it IS a JWidget (the
// parallel GWidget base is gone). It pulls the shared JSceneGraph from the
// application singleton, so a caller still just constructs it by title.
// ============================================================================
class JMainWindow : public JWidget {
public:
    explicit JMainWindow(std::string title = "Genesis JWindow")
        : JWidget(JGuiApplication::instance()->sceneGraph(), std::move(title))
    {
        m_graph.getLayout(m_nodeId).direction      = JFlexDirection::Column;
        m_graph.getLayout(m_nodeId).justifyContent = JJustifyContent::FlexStart;
    }

    // A window is a container; its children paint themselves.
    void populateRenderPrimitives(JPrimitiveBuffer&) override {}

    void show() {
        qCInfo(LogWidget) << "Showing main window: " << m_debugName << std::endl;
        JConstraints constraints{800.0f, 1920.0f, 600.0f, 1080.0f};
        m_graph.computeLayout(m_nodeId, constraints);
        // Publish initial snapshot immediately (before the first frame fires).
        JGuiApplication::instance()->publishSemanticSnapshot();
    }
};

} // inline namespace jf
