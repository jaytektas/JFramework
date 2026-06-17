#include <genesis/core/ApplicationCore.h>
#include <genesis/core/SceneGraph.h>
#include <genesis/core/BaseWidgets.h>
#include <genesis/core/AiControlBus.h>
#include <genesis/graphics/RenderPrimitive.h>
#include <genesis/graphics/GpuHal.h>
#include <iostream>
#include <iomanip>
#include <vector>
#include <thread>

using namespace Genesis;

/**
 * @brief Mock Headless Window for the Validation App.
 * Simulates a 1920x1080 display.
 */
class HeadlessWindow : public Core::PlatformWindow {
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
 * @brief Performance Control Panel Application.
 * Demonstrates the interlocking of all Genesis systems.
 */
class PerformancePanel {
public:
    PerformancePanel(SceneGraph& graph, AiControlBus& aiBus) 
        : m_graph(graph), m_aiBus(aiBus) 
    {
        setupUI();
    }

    void setupUI() {
        // 1. Root Container (Dark Background)
        m_rootId = m_graph.createNode("AppRoot");
        auto& rootLayout = m_graph.getLayout(m_rootId);
        rootLayout.direction = FlexDirection::Column;
        rootLayout.padding = 20.0f;
        rootLayout.boundingBox = {0, 0, 1920, 1080};

        // 2. Metrics Row
        m_metricsRowId = m_graph.createNode("MetricsRow");
        m_graph.getLayout(m_metricsRowId).direction = FlexDirection::Row;
        m_graph.addChild(m_rootId, m_metricsRowId);

        // 3. Performance Slider
        m_loadSlider = std::make_unique<Slider>(m_graph);
        m_graph.addChild(m_rootId, m_loadSlider->getNodeId());
        m_graph.getLayout(m_loadSlider->getNodeId()).boundingBox = {0, 0, 400, 40};

        m_loadSlider->onValueChanged.connect([this](float val) {
            m_targetLoad = val;
            qCInfo(LogWidgetSystem) << "System Target Load Adjusted: " << (val * 100.0f) << "%" << std::endl;
        });

        // 4. Reset Button
        m_resetBtn = std::make_unique<Button>(m_graph, "Reset Metrics");
        m_graph.addChild(m_rootId, m_resetBtn->getNodeId());
        m_graph.getLayout(m_resetBtn->getNodeId()).boundingBox = {0, 0, 200, 50};

        m_resetBtn->onClicked.connect([this]() {
            m_loadSlider->setNormalizedValue(0.5f);
            qCInfo(LogWidgetSystem) << "Performance Metrics Reset to Baseline." << std::endl;
        });
    }

    void update(double deltaTime) {
        // Simulate some dynamic metric updates
        m_currentMetrics += (m_targetLoad - m_currentMetrics) * (float)deltaTime * 2.0f;
        
        // Update layout tree
        Constraints constraints{1920, 1920, 1080, 1080};
        m_graph.computeLayout(m_rootId, constraints);

        // Sync to AI Control Bus
        m_aiBus.updateTelemetry(m_graph);
    }

    void render(PrimitiveBuffer& buffer) {
        buffer.clear();
        
        // In a real app, we'd traverse the widget tree
        m_loadSlider->populateRenderPrimitives(buffer);
        m_resetBtn->populateRenderPrimitives(buffer);
        
        // Print status to console (Headless feedback)
        std::cout << "\r[GENESIS RUNTIME] Frame Sync | AI Nodes: " << std::setw(2) << m_graph.totalNodes() 
                  << " | Load: " << std::fixed << std::setprecision(1) << (m_currentMetrics * 100.0f) << "%   " << std::flush;
    }

    void simulateClick() {
        const auto& bounds = m_graph.getLayout(m_resetBtn->getNodeId()).boundingBox;
        m_resetBtn->handleMousePress(bounds.x + 1, bounds.y + 1);
    }

private:
    SceneGraph& m_graph;
    AiControlBus& m_aiBus;
    
    NodeId m_rootId;
    NodeId m_metricsRowId;
    
    std::unique_ptr<Slider> m_loadSlider;
    std::unique_ptr<Button> m_resetBtn;
    
    float m_targetLoad{0.5f};
    float m_currentMetrics{0.5f};
};

int main() {
    qCInfo(LogEngineCore) << "Starting Genesis Validation Application..." << std::endl;

    Core::Application app;
    SceneGraph graph;
    AiControlBus aiBus;
    
    // Setup Shared Memory for AI Bus (Simulated for this demo)
    SharedBusMemory sharedMem;
    aiBus.attach(&sharedMem);

    PerformancePanel panel(graph, aiBus);
    PrimitiveBuffer renderBuffer;

    // Run the app with a headless window
    auto window = std::make_unique<HeadlessWindow>();
    
    // We can't easily run the real loop and block, so we'll simulate the frames
    // to show it works.
    
    double deltaTime = 0.016; // ~60fps
    for(int i = 0; i < 100; ++i) {
        panel.update(deltaTime);
        panel.render(renderBuffer);
        
        // Simulate a Button Click at Frame 25
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
    qCInfo(LogEngineCore) << "Validation Application completed successfully." << std::endl;
    
    return 0;
}
