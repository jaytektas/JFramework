#pragma once

#include <ApplicationCore.h>
#include <AiControlBus.h>
#include <mutex>
#include <optional>

namespace Genesis {

/**
 * @brief Identifies the origin of an input event.
 */
enum class InputOrigin : uint8_t {
    Human,
    Agent
};

/**
 * @brief The Symmetric Arbiter merges Human and Agentic input streams.
 * It resolves collisions and manages independent focus for both sides.
 */
class SymmetricArbiter {
public:
    SymmetricArbiter(Core::Application& app, AiControlBus& aiBus)
        : m_app(app), m_aiBus(aiBus) {}

    /**
     * @brief Pushes a human input event into the arbiter.
     */
    void pushHumanInput(const Core::InputEvent& event) {
        // High priority - bypass queues for immediate tactile response
        m_humanFocus = resolveNodeAt(event.data.mouse.x, event.data.mouse.y);
        processEvent(event, InputOrigin::Human);
    }

    /**
     * @brief Polls the AI Control Bus and merges virtual inputs.
     */
    void pollAgentInput() {
        AiVirtualInput aiInput;
        if (m_aiBus.pollInboundCommand(aiInput)) {
            m_agentFocus = resolveNodeAt(aiInput.targetX, aiInput.targetY);
            
            // Map AI command to internal InputEvent
            Core::InputEvent ie{};
            ie.type = (aiInput.type == AiVirtualInput::CommandType::MouseClick) 
                       ? Core::InputEvent::Type::MouseButtonDown 
                       : Core::InputEvent::Type::KeyDown;
            
            processEvent(ie, InputOrigin::Agent);
        }
    }

    std::optional<NodeId> getHumanFocus() const { return m_humanFocus; }
    std::optional<NodeId> getAgentFocus() const { return m_agentFocus; }

private:
    void processEvent(const Core::InputEvent& event, InputOrigin origin) {
        // Logical Routing:
        // 1. If Origin == Human, target m_humanFocus.
        // 2. If Origin == Agent, target m_agentFocus.
        // 3. Collision resolution: if both target same NodeId, use timestamp and weight.
    }

    NodeId resolveNodeAt(float x, float y) {
        // Performs hit-testing via the global SceneGraph
        return 0; // Placeholder
    }

    Core::Application& m_app;
    AiControlBus& m_aiBus;

    std::optional<NodeId> m_humanFocus;
    std::optional<NodeId> m_agentFocus;
};

} // namespace Genesis
