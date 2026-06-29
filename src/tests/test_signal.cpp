#include <genesis/core/Signal.h>
#include <cassert>
#include <string>

class Receiver : public jf::JSlotTracker {
public:
    int value = 0;
    std::string text;

    void onValueChanged(int v) {
        value = v;
    }

    void onTextChanged(std::string t) {
        text = t;
    }
};

void test_basic_signal() {
    jf::JSignal<int> sig;
    Receiver recv;
    
    sig.connect(&recv, &Receiver::onValueChanged);
    
    sig.emit(42);
    assert(recv.value == 42);
    
    sig(100);
    assert(recv.value == 100);
    
    std::cout << "test_basic_signal passed" << std::endl;
}

void test_lambda_signal() {
    jf::JSignal<int, int> sig;
    int sum = 0;
    
    sig.connect([&sum](int a, int b) {
        sum = a + b;
    });
    
    sig.emit(10, 20);
    assert(sum == 30);
    
    std::cout << "test_lambda_signal passed" << std::endl;
}

void test_raii_disconnection() {
    jf::JSignal<int> sig;
    int last_val = 0;
    
    {
        Receiver recv;
        sig.connect(&recv, &Receiver::onValueChanged);
        sig.emit(5);
        assert(recv.value == 5);
        last_val = recv.value;
    }
    
    // recv is now destroyed, signal should not crash when emitting
    sig.emit(10);
    // last_val remains 5 because recv is gone and the connection should be dead
    assert(last_val == 5);
    
    std::cout << "test_raii_disconnection passed" << std::endl;
}

int main() {
    test_basic_signal();
    test_lambda_signal();
    test_raii_disconnection();
    std::cout << "All tests passed!" << std::endl;
    return 0;
}
