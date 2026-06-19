#pragma once

#include <string>
#include <vector>
#include <memory>
#include <cassert>
#include <iostream>

// Include our previously validated Genesis foundation framework headers
#include "Signal.h"
#include "ApplicationCore.h"
#include "SceneGraph.h"
#include "AiControlBus.h"

namespace { inline constexpr auto& LogWidget = Genesis::Log::Widgets; }

namespace Genesis {

/**
 * @brief Base lifecycle and event tracking framework tracker class.
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

/**
 * @brief Coordinates the global execution pool, flat SceneGraph, and the AI Control Bus.
 */
class GApplication : public GObject {
public:
    GApplication() : GObject("GApplication") {
        assert(s_instance == nullptr && "Fatal: Only one GApplication instance can exist safely.");
        s_instance = this;

        // Prefer a real cross-process shared-memory segment so an external AI agent can
        // read the live semantic tree / drive the UI at machine speed.  Fall back to the
        // in-process pool if POSIX shm is unavailable (keeps tests & headless robust).
        if (!m_aiBus.createSegment())
            m_aiBus.attach(&m_fallbackMemoryPool);
    }

    virtual ~GApplication() {
        s_instance = nullptr;
    }

    static GApplication* instance() noexcept { return s_instance; }
    SceneGraph& sceneGraph() noexcept { return m_sceneGraph; }
    AiControlBus& aiBus() noexcept { return m_aiBus; }
    Core::Application& runtimeLoop() noexcept { return m_runtimeLoop; }

    int exec(std::unique_ptr<Core::PlatformWindow> nativeWindow) {
        return m_runtimeLoop.run(std::move(nativeWindow));
    }

private:
    static GApplication* s_instance;
    SceneGraph m_sceneGraph;
    AiControlBus m_aiBus;
    Core::Application m_runtimeLoop;
    SharedBusMemory m_fallbackMemoryPool{}; // Fast IPC memory mapping simulation context
};

inline GApplication* GApplication::s_instance = nullptr;

/**
 * @brief High-level Widget wrapper. Maps clean object APIs onto the flat scene graph.
 */
class GWidget : public GObject {
public:
    explicit GWidget(std::string name = "") : GObject(std::move(name)) {
        m_graphRef = &GApplication::instance()->sceneGraph();
        m_nodeId = m_graphRef->createNode(objectName());
    }

    virtual ~GWidget() = default;

    NodeId nodeId() const noexcept { return m_nodeId; }

    void setFlexDirection(FlexDirection direction) {
        m_graphRef->getLayout(m_nodeId).direction = direction;
    }

    void setJustifyContent(JustifyContent justify) {
        m_graphRef->getLayout(m_nodeId).justifyContent = justify;
    }

protected:
    SceneGraph* m_graphRef;
    NodeId m_nodeId;
};

/**
 * @brief Main Window Component managing rendering constraints and layouts.
 */
class GMainWindow : public GWidget {
public:
    explicit GMainWindow(std::string title = "Genesis Window") 
        : GWidget(std::move(title)) 
    {
        setFlexDirection(FlexDirection::Column);
        setJustifyContent(JustifyContent::FlexStart);
    }

    void show() {
        qCInfo(LogWidget) << "Showing main window application surface: " << objectName() << std::endl;
        
        // Force the single-pass constraint solver layout down across our data graph arrays
        Constraints constraints{800.0f, 1920.0f, 600.0f, 1080.0f};
        m_graphRef->computeLayout(m_nodeId, constraints);
        
        // Broadcast the initial layout telemetry down to the waiting AI Agent
        GApplication::instance()->aiBus().updateTelemetry(*m_graphRef);
    }
};

} // namespace Genesis
