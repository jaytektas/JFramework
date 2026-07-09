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
#include "JWidget.h"
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
// JGuiApplication — coordinates the event loop, JSceneGraph, and the
//                JMainThreadDispatcher drain.
// ============================================================================
class JGuiApplication : public JObject, public jf::JApplication {
public:
    JGuiApplication() : JObject("JGuiApplication") {
        assert(s_instance == nullptr && "Only one JGuiApplication may exist.");
        s_instance = this;
    }

    ~JGuiApplication() {
        s_instance = nullptr;
    }

    static JGuiApplication* instance() noexcept { return s_instance; }
    JSceneGraph&    sceneGraph()  noexcept { return m_sceneGraph; }

    int exec(std::unique_ptr<jf::JPlatformWindow> nativeWindow) {
        return run(std::move(nativeWindow));   // run() inherited from jf::JApplication
    }

    // One frame of framework housekeeping: drain main-thread callbacks (Timer/SerialPort).
    // Called by the app-window runner (JAppWindow) each frame.
    void serviceFrame() {
        JMainThreadDispatcher::instance().drain();
    }

private:
    static JGuiApplication* s_instance;
    JSceneGraph   m_sceneGraph;
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
    }
};

} // inline namespace jf
