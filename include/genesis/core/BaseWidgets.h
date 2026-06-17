#pragma once

#include <string>
#include <cstdint>
#include <iostream>
#include <algorithm>
#include "Signal.h"        
#include "SceneGraph.h"    
#include "../graphics/RenderPrimitive.h" 

// --- Custom Logging Integration Mock ---
#ifndef qCInfo
#define qCInfo(category) std::cout << "[INFO] "
struct MockCategoryWidgets {};
inline MockCategoryWidgets LogWidgetSystem;
#endif

namespace Genesis {

/**
 * @brief Canonical interactive states applied to user controls.
 */
enum class WidgetState : uint32_t {
    Normal,
    Hovered,
    Pressed
};

/**
 * @brief Base Logical Interaction Component.
 */
class Widget : public Core::SlotTracker {
public:
    Widget(SceneGraph& sceneGraph, const std::string& debugName = "") 
        : m_sceneGraph(sceneGraph), m_state(WidgetState::Normal) 
    {
        m_nodeId = m_sceneGraph.createNode(debugName);
    }
    
    virtual ~Widget() = default;

    Widget(const Widget&) = delete;
    Widget& operator=(const Widget&) = delete;

    NodeId getNodeId() const noexcept { return m_nodeId; }
    WidgetState getState() const noexcept { return m_state; }

    void setHandleState(WidgetState newState) {
        m_state = newState;
    }

    /**
     * @brief Transforms logical state properties straight into the shared rendering stream.
     */
    virtual void populateRenderPrimitives(PrimitiveBuffer& renderBuffer) = 0;

protected:
    SceneGraph& m_sceneGraph;
    NodeId m_nodeId;
    WidgetState m_state;
};

/**
 * @brief High-performance, compile-ready Interactive Button Widget.
 */
class Button : public Widget {
public:
    Core::Signal<> onClicked;

    Button(SceneGraph& sceneGraph, const std::string& text) 
        : Widget(sceneGraph, "Button: " + text), m_label(text) 
    {
        auto& layout = m_sceneGraph.getLayout(m_nodeId);
        layout.direction = FlexDirection::Row;
    }

    void handleMousePress(float mx, float my) {
        const auto& bounds = m_sceneGraph.getLayout(m_nodeId).boundingBox;
        if (mx >= bounds.x && mx <= bounds.x + bounds.width &&
            my >= bounds.y && my <= bounds.y + bounds.height) {
            setHandleState(WidgetState::Pressed);
            qCInfo(LogWidgetSystem) << "Button interaction boundary triggered natively." << std::endl;
            onClicked.emit(); 
        }
    }

    void populateRenderPrimitives(PrimitiveBuffer& renderBuffer) override {
        const auto& layout = m_sceneGraph.getLayout(m_nodeId);
        
        uint8_t fillColor[4] = {40, 40, 40, 255};      
        if (m_state == WidgetState::Hovered) {
            fillColor[0] = 60; fillColor[1] = 60; fillColor[2] = 60;
        } else if (m_state == WidgetState::Pressed) {
            fillColor[0] = 20; fillColor[1] = 100; fillColor[2] = 240; 
        }

        renderBuffer.pushRectangle(
            layout.boundingBox.x, 
            layout.boundingBox.y, 
            layout.boundingBox.width, 
            layout.boundingBox.height, 
            fillColor, 
            6.0f 
        );
    }

private:
    std::string m_label;
};

/**
 * @brief High-performance Linear Range Slider Control.
 */
class Slider : public Widget {
public:
    Core::Signal<float> onValueChanged; 

    Slider(SceneGraph& sceneGraph) 
        : Widget(sceneGraph, "Slider Track"), m_normalizedValue(0.5f) {}

    void setNormalizedValue(float val) {
        m_normalizedValue = std::clamp(val, 0.0f, 1.0f);
        onValueChanged.emit(m_normalizedValue);
    }

    void populateRenderPrimitives(PrimitiveBuffer& renderBuffer) override {
        const auto& layout = m_sceneGraph.getLayout(m_nodeId);
        
        uint8_t trackColor[4] = {24, 24, 26, 255};
        renderBuffer.pushRectangle(
            layout.boundingBox.x,
            layout.boundingBox.y + (layout.boundingBox.height * 0.4f),
            layout.boundingBox.width,
            layout.boundingBox.height * 0.2f,
            trackColor,
            2.0f
        );

        uint8_t thumbColor[4] = {240, 240, 245, 255};
        float thumbWidth = 14.0f;
        float thumbX = layout.boundingBox.x + (m_normalizedValue * (layout.boundingBox.width - thumbWidth));

        renderBuffer.pushRectangle(
            thumbX,
            layout.boundingBox.y,
            thumbWidth,
            layout.boundingBox.height,
            thumbColor,
            4.0f
        );
    }

private:
    float m_normalizedValue;
};

} // namespace Genesis
