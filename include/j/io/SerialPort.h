#pragma once

#include <j/core/Signal.h>
#include <string>
#include <vector>
#include <cstdint>
#include <memory>

// Cross-platform serial port. The public interface carries no platform types —
// the POSIX termios / Win32 backend lives entirely in SerialPort.cpp behind an
// opaque Impl (pimpl). Signals fire on the main thread via JMainThreadDispatcher,
// so they are safe to connect straight to widget updates.

inline namespace jf {

// ---- Port info returned by availablePorts() --------------------------------
struct JSerialPortInfo {
    std::string port;           // "/dev/ttyUSB0" or "COM3"
    std::string description;    // human-readable device name
    std::string manufacturer;
    std::string serialNumber;
    uint16_t    vendorId{0};
    uint16_t    productId{0};
    bool        hasVidPid{false};
};

class JSerialPort {
public:
    enum class JBaudRate {
        B1200_   = 1200,
        B4800_   = 4800,
        B9600_   = 9600,
        B19200_  = 19200,
        B38400_  = 38400,
        B57600_  = 57600,
        B115200_ = 115200,
        B230400_ = 230400,
        B921600_ = 921600,
    };

    enum class JDataBits { Five=5, Six=6, Seven=7, Eight=8 };
    enum class JStopBits { One, Two };
    enum class JParity   { None, Even, Odd };
    enum class JFlowCtrl { None, Hardware, Software };

    // All signals fire on the main thread via JMainThreadDispatcher.
    jf::JSignal<std::vector<uint8_t>> onData;
    jf::JSignal<std::string>          onError;
    jf::JSignal<>                     onDisconnect; // cable pull or device removal

    JSerialPort();
    ~JSerialPort();

    JSerialPort(const JSerialPort&)            = delete;
    JSerialPort& operator=(const JSerialPort&) = delete;

    bool open(const std::string& port,
              JBaudRate  baud     = JBaudRate::B115200_,
              JDataBits  dataBits = JDataBits::Eight,
              JStopBits  stopBits = JStopBits::One,
              JParity    parity   = JParity::None,
              JFlowCtrl  flow     = JFlowCtrl::None);

    void close();
    bool isOpen() const;

    // Discard any bytes the OS has already buffered on the receive side. open()
    // calls this so a stale or mid-frame buffer from a previous session can't
    // corrupt the first parse; callers may also use it to resync after an error.
    void flushInput();

    // Write bytes — thread-safe, handles partial writes internally.
    bool write(const std::vector<uint8_t>& data);
    bool writeLine(const std::string& s);

    // Discard buffered input and output.
    void flush();

    // Enumerate available ports with descriptions, manufacturer, and VID/PID.
    static std::vector<JSerialPortInfo> availablePorts();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // inline namespace jf
