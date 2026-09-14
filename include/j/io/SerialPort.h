// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jason Roughley <pis.controller@gmail.com>

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

    // ---- Taking delivery on YOUR thread -------------------------------------------------------
    //
    // Normally the reader thread posts every chunk to the main thread and onData fires there. That
    // is what makes the port safe to use from widget code, and it is the right default — but it
    // ties the RATE of a request/reply exchange to the main loop, because the reply cannot be seen
    // until the main thread comes round again. A transfer of hundreds of chunks then runs at the
    // main loop's pace rather than the link's.
    //
    // A claim redirects the SAME reader thread into an internal queue, and readClaimed() blocks on
    // it, so one worker can drive a whole transfer — write, wait, read, repeat — at wire speed with
    // the UI untouched. There is no second read path and no duplicated platform code: only the
    // destination of the bytes changes.
    //
    // Exactly one claimant at a time; claim() returns false if the port is closed or already
    // claimed. While claimed, onData does NOT fire — so the owner of the port must make sure
    // nothing else is mid-exchange before claiming (for a one-frame-in-flight protocol, that means
    // stopping the poll first). release() returns to main-thread delivery and hands back anything
    // still queued, so a reply that arrived as the transfer ended is not lost.
    bool claim();
    void release();
    bool isClaimed() const;

    // Blocking take of whatever the reader thread has collected, up to timeoutMs. Returns empty on
    // timeout — which is the caller's signal that the link has gone quiet, not an error. Waits on a
    // condition variable, so a byte arriving in 200 us is returned in 200 us.
    std::vector<uint8_t> readClaimed(int timeoutMs);

    // Discard buffered input and output.
    void flush();

    // Enumerate available ports with descriptions, manufacturer, and VID/PID.
    static std::vector<JSerialPortInfo> availablePorts();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // inline namespace jf
