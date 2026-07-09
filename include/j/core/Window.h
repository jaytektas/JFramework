#pragma once

#include <string>
#include <memory>
#include "JWidget.h"
#include "JStyleEngine.h"

inline namespace jf {

/**
 * @brief Opaque identifiers for window styling properties.
 */
namespace WindowStyle {
    constexpr JStyleKey<JColor> TitleBarColor{ 0x20 };
    constexpr JStyleKey<JColor> BorderColor{ 0x21 };
    constexpr JStyleKey<float> TitleBarHeight{ 0x22 };
}

/**
 * @brief Base JWindow Skin interface.
 * Allows for completely bespoke visual representations of a window frame.
 */
class JWindowSkin {
public:
    virtual ~JWindowSkin() = default;
    virtual void drawFrame(JPrimitiveBuffer& buffer, const JRect& bounds, const JStyleEngine& styles, NodeId nodeId) = 0;
};

/**
 * @brief Default 'Fallback' Skin.
 * Provides a clean, dark-themed industrial look for initial bootstrapping.
 */
class JFallbackWindowSkin : public JWindowSkin {
public:
    void drawFrame(JPrimitiveBuffer& buffer, const JRect& bounds, const JStyleEngine& styles, NodeId nodeId) override {
        JColor defaultTitleColor = { 30, 30, 32, 255 };
        JColor defaultBorderColor = { 60, 60, 65, 255 };
        float titleHeight = styles.lookup(nodeId, WindowStyle::TitleBarHeight, 32.0f);
        
        // 1. Draw Main JWindow Body
        uint8_t bgColor[4] = { 18, 18, 20, 255 };
        buffer.pushRectangle(bounds.x, bounds.y, bounds.width, bounds.height, bgColor, 8.0f);

        // 2. Draw Title Bar Area
        JColor tColor = styles.lookup(nodeId, WindowStyle::TitleBarColor, defaultTitleColor);
        uint8_t titleFill[4] = { tColor.r, tColor.g, tColor.b, tColor.a };
        buffer.pushRectangle(bounds.x, bounds.y, bounds.width, titleHeight, titleFill, 8.0f);
        
        // 3. Draw Accent Border
        JColor bColor = styles.lookup(nodeId, WindowStyle::BorderColor, defaultBorderColor);
        uint8_t borderFill[4] = { bColor.r, bColor.g, bColor.b, bColor.a };
        // We use the primitive buffer's border width feature for the quad
        buffer.pushRectangle(bounds.x, bounds.y, bounds.width, bounds.height, bgColor, 8.0f, 1.5f, borderFill);
    }
};

/**
 * @brief The Top-Level JWindow Component.
 * Orchestrates the relationship between logical content nodes and visual skinning.
 */
class JWindow : public JWidget {
public:
    JWindow(JSceneGraph& graph, const std::string& title)
        : JWidget(graph, "JWindow: " + title), m_title(title) 
    {
        m_skin = std::make_unique<JFallbackWindowSkin>();
        
        auto& layout = m_graph.getLayout(m_nodeId);
        layout.direction = JFlexDirection::Column;
        layout.padding = 4.0f;
    }

    void setSkin(std::unique_ptr<JWindowSkin> newSkin) {
        m_skin = std::move(newSkin);
        m_graph.invalidateNode(m_nodeId, DirtySelf);
    }

    void populateRenderPrimitives(JPrimitiveBuffer&) override {}

    void renderWithStyles(JPrimitiveBuffer& buffer, const JStyleEngine& styles) {
        const auto& layout = m_graph.getLayoutConst(m_nodeId);
        m_skin->drawFrame(buffer, layout.boundingBox, styles, m_nodeId);
    }

private:
    std::string m_title;
    std::unique_ptr<JWindowSkin> m_skin;
};

} // inline namespace jf
