#include <genesis/core/ApplicationCore.h>
#include <cassert>
#include <thread>
#include <vector>

class MockWindow : public Core::PlatformWindow {
public:
    int framesProcessed = 0;
    int maxFrames = 10;
    bool vsyncEnabled = false;

    void pollNativeEvents() override {
        // Simulating some events
    }

    void swapBuffers() override {
        framesProcessed++;
    }

    void setVSync(bool enabled) override {
        vsyncEnabled = enabled;
    }

    bool shouldClose() const override {
        return framesProcessed >= maxFrames;
    }
};

void test_application_loop() {
    Core::Application app;
    auto window = std::make_unique<MockWindow>();
    auto* windowPtr = window.get();
    
    int result = app.run(std::move(window));
    
    assert(result == 0);
    assert(windowPtr->framesProcessed == 10);
    assert(windowPtr->vsyncEnabled == true);
    
    std::cout << "test_application_loop passed" << std::endl;
}

void test_task_dispatching() {
    Core::Application app;
    auto window = std::make_unique<MockWindow>();
    window->maxFrames = 5;
    
    int counter = 0;
    
    // Dispatch a task before running
    app.postToMainThread([&counter]() {
        counter++;
    });
    
    // Dispatch a task from another thread during execution
    std::thread worker([&app]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        app.postToMainThread([&app]() {
            // This will run on main thread
            app.shutdown(); // Force early exit
        });
    });
    
    app.run(std::move(window));
    worker.join();
    
    assert(counter == 1);
    
    std::cout << "test_task_dispatching passed" << std::endl;
}

int main() {
    test_application_loop();
    test_task_dispatching();
    std::cout << "All tests passed!" << std::endl;
    return 0;
}
