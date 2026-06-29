#include <genesis/core/MainThreadDispatcher.h>
#include <genesis/core/Timer.h>
#include <cassert>
#include <iostream>
#include <thread>
#include <chrono>

using namespace jf;

void test_post_and_drain() {
    auto& disp = JMainThreadDispatcher::instance();
    disp.drain();  // flush any prior state
    int count = 0;
    disp.post([&]{ count += 1; });
    disp.post([&]{ count += 10; });
    assert(count == 0);
    int ran = disp.drain();
    assert(ran == 2);
    assert(count == 11);
    assert(disp.drain() == 0);
    std::cout << "test_post_and_drain passed\n";
}

void test_has_pending() {
    auto& disp = JMainThreadDispatcher::instance();
    disp.drain();
    assert(!disp.hasPending());
    disp.post([]{ });
    assert(disp.hasPending());
    disp.drain();
    assert(!disp.hasPending());
    std::cout << "test_has_pending passed\n";
}

void test_cross_thread_post() {
    auto& disp = JMainThreadDispatcher::instance();
    disp.drain();
    int count = 0;
    std::thread t([&]{
        for (int i = 0; i < 5; ++i)
            disp.post([&]{ ++count; });
    });
    t.join();
    disp.drain();
    assert(count == 5);
    std::cout << "test_cross_thread_post passed\n";
}

void test_timer_posts_to_main_thread() {
    // JTimer ticks are posted through JMainThreadDispatcher::instance()
    int ticks = 0;
    JMainThreadDispatcher::instance().drain();  // flush any prior state

    JTimer timer(std::chrono::milliseconds(20), JTimer::JMode::Repeating);
    timer.onTick.connect([&]{ ++ticks; });

    // Let the timer fire a few times
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    timer.stop();

    int drained = JMainThreadDispatcher::instance().drain();
    // We don't care about exact count, just that at least one tick was posted
    assert(drained >= 2);
    assert(ticks >= 2);
    std::cout << "test_timer_posts_to_main_thread passed (ticks=" << ticks << ")\n";
}

void test_single_shot_timer() {
    JMainThreadDispatcher::instance().drain();
    int count = 0;
    JTimer t(std::chrono::milliseconds(30), JTimer::JMode::SingleShot);
    t.onTick.connect([&]{ ++count; });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    JMainThreadDispatcher::instance().drain();
    assert(count == 1);
    assert(!t.isRunning());
    std::cout << "test_single_shot_timer passed\n";
}

int main() {
    test_post_and_drain();
    test_has_pending();
    test_cross_thread_post();
    test_timer_posts_to_main_thread();
    test_single_shot_timer();
    std::cout << "All JMainThreadDispatcher/JTimer tests passed!\n";
    return 0;
}
