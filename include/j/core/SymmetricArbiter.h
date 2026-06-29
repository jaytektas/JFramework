#pragma once

#include <ApplicationCore.h>
#include <AiControlBus.h>
#include <mutex>
#include <optional>

inline namespace jf {

/**
 * @brief Identifies the origin of an input event.
 */
enum class JInputOrigin : uint8_t {
    Human,
    Agent
};

/**
 * @brief The Symmetric Arbiter merges Human and Agentic input streams.
 * It resolves collisions and manages independent focus for both sides.
 */
class JSymmetricArbiter {
public:
    JSymmetricArbiter(jf::JApplication& app, JAiControlBus& aiBus)
        : m_app(app), m_aiBus(aiBus) {}

    /**
     * @brief Pushes a human input event into the arbiter.
     */
    void pushHumanInput(const jf::JInputEvent& event) {
        // High priority - bypass queues for immediate tactile response
        m_humanFocus = resolveNodeAt(event.data.mouse.x, event.data.mouse.y);
        processEvent(event, JInputOrigin::Human);
    }

    /**
     * @brief Polls the AI JControl Bus and merges virtual inputs.
     */
    void pollAgentInput() {
        JAiVirtualInput aiInput;
        if (m_aiBus.pollInboundCommand(aiInput)) {
            m_agentFocus = resolveNodeAt(aiInput.targetX, aiInput.targetY);
            
            // Map AI command to internal JInputEvent
            jf::JInputEvent ie{};
            ie.type = (aiInput.type == JAiVirtualInput::JCommandType::MouseClick) 
                       ? jf::JInputEvent::JType::MouseButtonDown 
                       : jf::JInputEvent::JType::KeyDown;
            
            processEvent(ie, JInputOrigin::Agent);
        }
    }

    std::optional<NodeId> getHumanFocus() const { return m_humanFocus; }
    std::optional<NodeId> getAgentFocus() const { return m_agentFocus; }

private:
    void processEvent(const jf::JInputEvent& event, JInputOrigin origin) {
        // Logical Routing:
        // 1. If Origin == Human, target m_humanFocus.
        // 2. If Origin == Agent, target m_agentFocus.
        // 3. Collision resolution: if both target same NodeId, use timestamp and weight.
    }

    NodeId resolveNodeAt(float x, float y) {
        // Performs hit-testing via the global JSceneGraph
        return 0; // Placeholder
    }

    jf::JApplication& m_app;
    JAiControlBus& m_aiBus;

    std::optional<NodeId> m_humanFocus;
    std::optional<NodeId> m_agentFocus;
};

} // inline namespace jf
