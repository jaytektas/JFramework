#include <genesis/core/GenesisComponents.h>

// --- Mock Platform Window For Verification Testing Loop ---
class MockPlatformWindow : public Core::PlatformWindow {
public:
    MockPlatformWindow() : m_frames(0) {}
    void pollNativeEvents() override {}
    void swapBuffers() override { m_frames++; }
    void setVSync(bool) override {}
    bool shouldClose() const override { return m_frames > 2; } // Simulate quick runtime exit loop
private:
    int m_frames;
};

/**
 * @brief Execution test verifying clean API replacements for standard toolkits.
 */
int main() {
    Genesis::GApplication app;
    
    Genesis::GMainWindow mainWindow("Genesis Main Performance Deck");
    mainWindow.show();

    // Pasting this single block replaces standard Qt application orchestration paths cleanly
    auto mockWindow = std::make_unique<MockPlatformWindow>();
    int ret = app.exec(std::move(mockWindow));
    
    std::cout << "[GENESIS RUNTIME] Application exited with code: " << ret << std::endl;
    return ret;
}
