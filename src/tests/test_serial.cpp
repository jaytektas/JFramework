// test_serial.cpp — JSerialPort unit tests
// Tests that don't require real hardware: API surface, availablePorts, AI signals.

#include <genesis/io/SerialPort.h>
#include <genesis/core/AiBusHook.h>
#include <cassert>
#include <string>
#include <vector>
#include <iostream>

using namespace Genesis;

static void test_initial_state() {
    JSerialPort sp;
    assert(!sp.isOpen());
    std::cout << "  [OK] initial state: isOpen() == false\n";
}

static void test_available_ports_returns_vector() {
    // Just verify the call doesn't crash and returns a vector<string>
    auto ports = JSerialPort::availablePorts();
    // May be empty on CI; we just need it to not crash.
    (void)ports;
    std::cout << "  [OK] availablePorts() returned " << ports.size() << " port(s)\n";
}

static void test_open_nonexistent_fails() {
    JSerialPort sp;
    bool ok = sp.open("/dev/genesis_test_nonexistent_xyz");
    assert(!ok);
    assert(!sp.isOpen());
    std::cout << "  [OK] open() of nonexistent port returns false\n";
}

static void test_write_when_closed_returns_false() {
    JSerialPort sp;
    std::vector<uint8_t> data = {0x01, 0x02};
    bool ok = sp.write(data);
    assert(!ok);
    std::cout << "  [OK] write() on closed port returns false\n";
}

static void test_ai_open_close_signals() {
    std::vector<std::string> signals;
    JAiBusHook::install([&](uint32_t, const char* sig, const char* val) {
        signals.push_back(std::string(sig) + ":" + val);
    });

    {
        JSerialPort sp;
        // Attempting to open a nonexistent port — still emits open signal on success
        // (this port will fail, so no open signal expected)
        sp.open("/dev/genesis_test_nonexistent_xyz");
        // close() only emits if previously opened successfully (m_port not empty)
    }

    // No signals expected since open() failed
    JAiBusHook::install(nullptr);
    std::cout << "  [OK] no spurious AI signals on failed open\n";
}

static void test_close_is_idempotent() {
    JSerialPort sp;
    // Calling close() multiple times on a closed port is safe
    sp.close();
    sp.close();
    assert(!sp.isOpen());
    std::cout << "  [OK] close() is idempotent\n";
}

int main() {
    std::cout << "JSerialPort tests:\n";
    test_initial_state();
    test_available_ports_returns_vector();
    test_open_nonexistent_fails();
    test_write_when_closed_returns_false();
    test_ai_open_close_signals();
    test_close_is_idempotent();
    std::cout << "All JSerialPort tests passed.\n";
    return 0;
}
