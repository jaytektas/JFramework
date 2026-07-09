#include <j/core/ApplicationCore.h>
#include <j/core/SceneGraph.h>
#include <j/core/BaseWidgets.h>
#include <j/graphics/RenderPrimitive.h>
#include <j/graphics/GpuHal.h>
#include <iostream>
#include <iomanip>
#include <vector>
#include <thread>

using namespace jf;

/**
 * @brief Mock Headless JWindow for the Validation App.
 * Simulates a 1920x1080 display.
 */
class HeadlessWindow : public jf::JPlatformWindow {
public:
    void pollNativeEvents() override {
        // In a real app, this pulls from X11/Wayland/Win32
    }
    void swapBuffers() override {
        m_frameCount++;
    }
    void setVSync(bool enabled) override { (void)enabled; }
    bool shouldClose() const override { return m_frameCount > 100; } // Run for 100 frames for validation

private:
    uint32_t m_frameCount{0};
};

/**
 * @brief Performance JControl Panel JApplication.
 * Demonstrates the interlocking of all Genesis systems.
 */
class PerformancePanel {
public:
    explicit PerformancePanel(JSceneGraph& graph)
        : m_graph(graph)
    {
        setupUI();
    }

    void setupUI() {
        // 1. Root Container (Dark Background)
        m_rootId = m_graph.createNode("AppRoot");
        auto& rootLayout = m_graph.getLayout(m_rootId);
        rootLayout.direction = JFlexDirection::Column;
        rootLayout.padding = 20.0f;
        rootLayout.boundingBox = {0, 0, 1920, 1080};

        // 2. Metrics JRow
        m_metricsRowId = m_graph.createNode("MetricsRow");
        m_graph.getLayout(m_metricsRowId).direction = JFlexDirection::JRow;
        m_graph.addChild(m_rootId, m_metricsRowId);

        // 3. Performance JSlider
        m_loadSlider = std::make_unique<JSlider>(m_graph);
        m_graph.addChild(m_rootId, m_loadSlider->getNodeId());
        m_graph.getLayout(m_loadSlider->getNodeId()).boundingBox = {0, 0, 400, 40};

        m_loadSlider->onValueChanged.connect([this](float val) {
            m_targetLoad = val;
            qCInfo(jf::Log::Widgets) << "System Target Load Adjusted: " << (val * 100.0f) << "%" << std::endl;
        });

        // 4. Reset JButton
        m_resetBtn = std::make_unique<JButton>(m_graph, "Reset Metrics");
        m_graph.addChild(m_rootId, m_resetBtn->getNodeId());
        m_graph.getLayout(m_resetBtn->getNodeId()).boundingBox = {0, 0, 200, 50};

        m_resetBtn->onClicked.connect([this]() {
            m_loadSlider->setValue(0.5f);
            qCInfo(jf::Log::Widgets) << "Performance Metrics Reset to Baseline." << std::endl;
        });
    }

    void update(double deltaTime) {
        // Simulate some dynamic metric updates
        m_currentMetrics += (m_targetLoad - m_currentMetrics) * (float)deltaTime * 2.0f;
        
        // Update layout tree
        JConstraints constraints{1920, 1920, 1080, 1080};
        m_graph.computeLayout(m_rootId, constraints);
    }

    void render(JPrimitiveBuffer& buffer) {
        buffer.clear();
        
        // In a real app, we'd traverse the widget tree
        m_loadSlider->populateRenderPrimitives(buffer);
        m_resetBtn->populateRenderPrimitives(buffer);
        
        // Print status to console (Headless feedback)
        std::cout << "\r[GENESIS RUNTIME] Frame Sync | Nodes: " << std::setw(2) << m_graph.totalNodes()
                  << " | Load: " << std::fixed << std::setprecision(1) << (m_currentMetrics * 100.0f) << "%   " << std::flush;
    }

    void simulateClick() {
        const auto& bounds = m_graph.getLayout(m_resetBtn->getNodeId()).boundingBox;
        m_resetBtn->handleMousePress(bounds.x + 1, bounds.y + 1);
    }

private:
    JSceneGraph& m_graph;

    NodeId m_rootId;
    NodeId m_metricsRowId;
    
    std::unique_ptr<JSlider> m_loadSlider;
    std::unique_ptr<JButton> m_resetBtn;
    
    float m_targetLoad{0.5f};
    float m_currentMetrics{0.5f};
};

int main() {
    qCInfo(LogEngineCore) << "Starting Genesis Validation JApplication..." << std::endl;

    jf::JApplication app;
    JSceneGraph graph;

    PerformancePanel panel(graph);
    JPrimitiveBuffer renderBuffer;

    // Run the app with a headless window
    auto window = std::make_unique<HeadlessWindow>();
    
    // We can't easily run the real loop and block, so we'll simulate the frames
    // to show it works.
    
    double deltaTime = 0.016; // ~60fps
    for(int i = 0; i < 100; ++i) {
        panel.update(deltaTime);
        panel.render(renderBuffer);
        
        // Simulate a JButton Click at Frame 25
        if (i == 25) {
            std::cout << std::endl;
            panel.simulateClick();
        }

        // Simulate a task being posted to main thread
        if (i == 50) {
            app.postToMainThread([]() {
                qCInfo(LogEngineCore) << "Async Background Task Executed on Main Thread." << std::endl;
            });
        }
        
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }

    std::cout << std::endl;
    qCInfo(LogEngineCore) << "Validation JApplication completed successfully." << std::endl;
    
    return 0;
}
