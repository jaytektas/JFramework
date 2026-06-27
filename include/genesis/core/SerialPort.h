#pragma once

#include "Signal.h"
#include "MainThreadDispatcher.h"
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <cstdint>

// Cross-platform serial port (POSIX termios on Linux/macOS, Win32 on Windows).
// Superior to QSerialPort: no Qt event loop dependency, signals fire on main thread
// via MainThreadDispatcher so callbacks can safely update widgets.

#if defined(_WIN32)
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
#else
  #include <termios.h>
  #include <fcntl.h>
  #include <unistd.h>
  #include <errno.h>
  #include <cstring>
#endif

namespace Genesis {

class SerialPort {
public:
    enum class BaudRate {
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

    enum class DataBits  { Five=5, Six=6, Seven=7, Eight=8 };
    enum class StopBits  { One, Two };
    enum class Parity    { None, Even, Odd };
    enum class FlowCtrl  { None, Hardware, Software };

    // Fires on the main thread (via MainThreadDispatcher).
    Core::Signal<std::vector<uint8_t>> onData;
    Core::Signal<std::string>          onError;

    SerialPort() = default;
    ~SerialPort() { close(); }

    SerialPort(const SerialPort&)            = delete;
    SerialPort& operator=(const SerialPort&) = delete;

    bool open(const std::string& port,
              BaudRate   baud     = BaudRate::B115200_,
              DataBits   dataBits = DataBits::Eight,
              StopBits   stopBits = StopBits::One,
              Parity     parity   = Parity::None,
              FlowCtrl   flow     = FlowCtrl::None)
    {
#if defined(_WIN32)
        std::string path = "\\\\.\\" + port;
        m_handle = CreateFileA(path.c_str(),
            GENERIC_READ | GENERIC_WRITE, 0, nullptr,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (m_handle == INVALID_HANDLE_VALUE) {
            _postError("Failed to open port: " + port);
            return false;
        }
        DCB dcb{};
        dcb.DCBlength = sizeof(DCB);
        GetCommState(m_handle, &dcb);
        dcb.BaudRate = static_cast<DWORD>(baud);
        dcb.ByteSize = static_cast<BYTE>(dataBits);
        dcb.StopBits = (stopBits == StopBits::Two) ? TWOSTOPBITS : ONESTOPBIT;
        dcb.Parity   = (parity == Parity::None) ? NOPARITY :
                       (parity == Parity::Even) ? EVENPARITY : ODDPARITY;
        dcb.fOutxCtsFlow = (flow == FlowCtrl::Hardware) ? TRUE : FALSE;
        dcb.fRtsControl  = (flow == FlowCtrl::Hardware) ? RTS_CONTROL_HANDSHAKE : RTS_CONTROL_ENABLE;
        dcb.fOutX        = (flow == FlowCtrl::Software) ? TRUE : FALSE;
        dcb.fInX         = (flow == FlowCtrl::Software) ? TRUE : FALSE;
        if (!SetCommState(m_handle, &dcb)) { _postError("SetCommState failed"); close(); return false; }
        COMMTIMEOUTS to{};
        to.ReadIntervalTimeout         = 50;
        to.ReadTotalTimeoutMultiplier  = 10;
        to.ReadTotalTimeoutConstant    = 50;
        SetCommTimeouts(m_handle, &to);
#else
        m_fd = ::open(port.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
        if (m_fd < 0) {
            _postError("Failed to open " + port + ": " + std::strerror(errno));
            return false;
        }
        struct termios tty{};
        if (tcgetattr(m_fd, &tty) != 0) {
            _postError("tcgetattr failed"); ::close(m_fd); m_fd = -1; return false;
        }
        speed_t speed = _posixBaud(baud);
        cfsetispeed(&tty, speed);
        cfsetospeed(&tty, speed);

        tty.c_cflag &= ~CSIZE;
        switch (dataBits) {
            case DataBits::Five:  tty.c_cflag |= CS5; break;
            case DataBits::Six:   tty.c_cflag |= CS6; break;
            case DataBits::Seven: tty.c_cflag |= CS7; break;
            case DataBits::Eight: tty.c_cflag |= CS8; break;
        }
        if (stopBits == StopBits::Two) tty.c_cflag |= CSTOPB; else tty.c_cflag &= ~CSTOPB;
        tty.c_cflag &= ~PARENB;
        if (parity != Parity::None) {
            tty.c_cflag |= PARENB;
            if (parity == Parity::Odd) tty.c_cflag |= PARODD;
        }
        if (flow == FlowCtrl::Hardware) tty.c_cflag |= CRTSCTS; else tty.c_cflag &= ~CRTSCTS;
        tty.c_cflag |= (CREAD | CLOCAL);
        tty.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG);
        tty.c_iflag &= ~(IXON | IXOFF | IXANY);
        if (flow == FlowCtrl::Software) tty.c_iflag |= (IXON | IXOFF);
        tty.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL);
        tty.c_oflag &= ~OPOST;
        tty.c_cc[VMIN]  = 0;
        tty.c_cc[VTIME] = 1;  // 100ms timeout
        if (tcsetattr(m_fd, TCSANOW, &tty) != 0) {
            _postError("tcsetattr failed"); ::close(m_fd); m_fd = -1; return false;
        }
        // Switch to blocking
        int flags = fcntl(m_fd, F_GETFL, 0);
        fcntl(m_fd, F_SETFL, flags & ~O_NONBLOCK);
#endif
        m_running = true;
        m_thread  = std::thread([this]{ _readLoop(); });
        return true;
    }

    void close() {
        m_running = false;
#if defined(_WIN32)
        if (m_handle != INVALID_HANDLE_VALUE) {
            CloseHandle(m_handle);
            m_handle = INVALID_HANDLE_VALUE;
        }
#else
        if (m_fd >= 0) { ::close(m_fd); m_fd = -1; }
#endif
        if (m_thread.joinable()) m_thread.join();
    }

    bool isOpen() const {
#if defined(_WIN32)
        return m_handle != INVALID_HANDLE_VALUE;
#else
        return m_fd >= 0;
#endif
    }

    bool write(const std::vector<uint8_t>& data) {
        if (!isOpen()) return false;
#if defined(_WIN32)
        DWORD written = 0;
        return WriteFile(m_handle, data.data(), static_cast<DWORD>(data.size()), &written, nullptr)
               && written == data.size();
#else
        ssize_t n = ::write(m_fd, data.data(), data.size());
        return n == static_cast<ssize_t>(data.size());
#endif
    }

    bool writeLine(const std::string& s) {
        std::vector<uint8_t> v(s.begin(), s.end());
        v.push_back('\n');
        return write(v);
    }

    // Available serial ports on this system.
    static std::vector<std::string> availablePorts() {
        std::vector<std::string> out;
#if defined(_WIN32)
        for (int i = 1; i <= 256; ++i) {
            std::string p = "COM" + std::to_string(i);
            HANDLE h = CreateFileA(("\\\\.\\" + p).c_str(),
                GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
            if (h != INVALID_HANDLE_VALUE) { CloseHandle(h); out.push_back(p); }
        }
#else
        for (const char* prefix : {"/dev/ttyUSB", "/dev/ttyACM", "/dev/ttyS"}) {
            for (int i = 0; i < 16; ++i) {
                std::string p = prefix + std::to_string(i);
                if (::access(p.c_str(), F_OK) == 0) out.push_back(p);
            }
        }
#endif
        return out;
    }

private:
    void _readLoop() {
        std::vector<uint8_t> buf(4096);
        while (m_running) {
#if defined(_WIN32)
            DWORD nRead = 0;
            if (!ReadFile(m_handle, buf.data(), static_cast<DWORD>(buf.size()), &nRead, nullptr))
                break;
            if (nRead > 0) _dispatch(std::vector<uint8_t>(buf.begin(), buf.begin() + nRead));
#else
            ssize_t n = ::read(m_fd, buf.data(), buf.size());
            if (n > 0) _dispatch(std::vector<uint8_t>(buf.begin(), buf.begin() + n));
            else if (n < 0 && errno != EINTR && errno != EAGAIN) {
                _postError(std::string("read error: ") + std::strerror(errno));
                break;
            }
#endif
        }
    }

    void _dispatch(std::vector<uint8_t> data) {
        MainThreadDispatcher::instance().post([this, d = std::move(data)]() mutable {
            onData.emit(std::move(d));
        });
    }

    void _postError(const std::string& msg) {
        MainThreadDispatcher::instance().post([this, msg]{
            onError.emit(msg);
        });
    }

#if defined(_WIN32)
    static speed_t _posixBaud(BaudRate) { return 0; } // unused on Win32
    HANDLE m_handle{INVALID_HANDLE_VALUE};
#else
    static speed_t _posixBaud(BaudRate b) {
        switch (b) {
            case BaudRate::B1200_:   return B1200;
            case BaudRate::B4800_:   return B4800;
            case BaudRate::B9600_:   return B9600;
            case BaudRate::B19200_:  return B19200;
            case BaudRate::B38400_:  return B38400;
            case BaudRate::B57600_:  return B57600;
            case BaudRate::B115200_: return B115200;
            case BaudRate::B230400_: return B230400;
            case BaudRate::B921600_: return B921600;
            default:                 return B115200;
        }
    }
    int m_fd{-1};
#endif

    std::thread       m_thread;
    std::atomic<bool> m_running{false};
};

} // namespace Genesis
