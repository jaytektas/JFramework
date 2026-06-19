#pragma once

#include <string>
#include <memory>
#include "BaseWidgets.h"
#include "StyleEngine.h"

namespace Genesis {

/**
 * @brief Opaque identifiers for window styling properties.
 */
namespace WindowStyle {
    constexpr StyleKey<Color> TitleBarColor{ 0x20 };
    constexpr StyleKey<Color> BorderColor{ 0x21 };
    constexpr StyleKey<float> TitleBarHeight{ 0x22 };
}

/**
 * @brief Base Window Skin interface.
 * Allows for completely bespoke visual representations of a window frame.
 */
class WindowSkin {
public:
    virtual ~WindowSkin() = default;
    virtual void drawFrame(PrimitiveBuffer& buffer, const Rect& bounds, const StyleEngine& styles, NodeId nodeId) = 0;
};

/**
 * @brief Default 'Fallback' Skin.
 * Provides a clean, dark-themed industrial look for initial bootstrapping.
 */
class FallbackWindowSkin : public WindowSkin {
public:
    void drawFrame(PrimitiveBuffer& buffer, const Rect& bounds, const StyleEngine& styles, NodeId nodeId) override {
        Color defaultTitleColor = { 30, 30, 32, 255 };
        Color defaultBorderColor = { 60, 60, 65, 255 };
        float titleHeight = styles.lookup(nodeId, WindowStyle::TitleBarHeight, 32.0f);
        
        // 1. Draw Main Window Body
        uint8_t bgColor[4] = { 18, 18, 20, 255 };
        buffer.pushRectangle(bounds.x, bounds.y, bounds.width, bounds.height, bgColor, 8.0f);

        // 2. Draw Title Bar Area
        Color tColor = styles.lookup(nodeId, WindowStyle::TitleBarColor, defaultTitleColor);
        uint8_t titleFill[4] = { tColor.r, tColor.g, tColor.b, tColor.a };
        buffer.pushRectangle(bounds.x, bounds.y, bounds.width, titleHeight, titleFill, 8.0f);
        
        // 3. Draw Accent Border
        Color bColor = styles.lookup(nodeId, WindowStyle::BorderColor, defaultBorderColor);
        uint8_t borderFill[4] = { bColor.r, bColor.g, bColor.b, bColor.a };
        // We use the primitive buffer's border width feature for the quad
        buffer.pushRectangle(bounds.x, bounds.y, bounds.width, bounds.height, bgColor, 8.0f, 1.5f, borderFill);
    }
};

/**
 * @brief The Top-Level Window Component.
 * Orchestrates the relationship between logical content nodes and visual skinning.
 */
class Window : public Widget {
public:
    Window(SceneGraph& graph, const std::string& title)
        : Widget(graph, "Window: " + title), m_title(title) 
    {
        m_skin = std::make_unique<FallbackWindowSkin>();
        
        auto& layout = m_graph.getLayout(m_nodeId);
        layout.direction = FlexDirection::Column;
        layout.padding = 4.0f;
    }

    void setSkin(std::unique_ptr<WindowSkin> newSkin) {
        m_skin = std::move(newSkin);
        m_graph.invalidateNode(m_nodeId, DirtySelf);
    }

    void populateRenderPrimitives(PrimitiveBuffer&) override {}

    void renderWithStyles(PrimitiveBuffer& buffer, const StyleEngine& styles) {
        const auto& layout = m_graph.getLayoutConst(m_nodeId);
        m_skin->drawFrame(buffer, layout.boundingBox, styles, m_nodeId);
    }

private:
    std::string m_title;
    std::unique_ptr<WindowSkin> m_skin;
};

} // namespace Genesis
