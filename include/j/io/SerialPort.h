#pragma once

#include <j/core/Signal.h>
#include <j/core/MainThreadDispatcher.h>
#include <j/core/AiBusHook.h>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>
#include <cstdint>

// Cross-platform serial port (POSIX termios on Linux/macOS, Win32 on Windows).
// Signals fire on the main thread via JMainThreadDispatcher — safe for widget updates.

#if defined(_WIN32)
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
#else
  #include <termios.h>
  #include <fcntl.h>
  #include <unistd.h>
  #include <dirent.h>
  #include <errno.h>
  #include <cstring>
  #include <sys/ioctl.h>
#endif

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

    JSerialPort() = default;
    ~JSerialPort() { close(); }

    JSerialPort(const JSerialPort&)            = delete;
    JSerialPort& operator=(const JSerialPort&) = delete;

    bool open(const std::string& port,
              JBaudRate  baud     = JBaudRate::B115200_,
              JDataBits  dataBits = JDataBits::Eight,
              JStopBits  stopBits = JStopBits::One,
              JParity    parity   = JParity::None,
              JFlowCtrl  flow     = JFlowCtrl::None)
    {
#if defined(_WIN32)
        // Create an event so we can cancel blocking reads on close.
        m_cancelEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);

        std::string path = "\\\\.\\" + port;
        m_handle = CreateFileA(path.c_str(),
            GENERIC_READ | GENERIC_WRITE, 0, nullptr,
            OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
        if (m_handle == INVALID_HANDLE_VALUE) {
            CloseHandle(m_cancelEvent); m_cancelEvent = nullptr;
            _postError("Failed to open port: " + port);
            return false;
        }
        DCB dcb{};
        dcb.DCBlength = sizeof(DCB);
        GetCommState(m_handle, &dcb);
        dcb.JBaudRate = static_cast<DWORD>(baud);
        dcb.ByteSize = static_cast<BYTE>(dataBits);
        dcb.JStopBits = (stopBits == JStopBits::Two) ? TWOSTOPBITS : ONESTOPBIT;
        dcb.JParity   = (parity == JParity::None) ? NOPARITY :
                       (parity == JParity::Even) ? EVENPARITY : ODDPARITY;
        dcb.fOutxCtsFlow = (flow == JFlowCtrl::Hardware) ? TRUE : FALSE;
        dcb.fRtsControl  = (flow == JFlowCtrl::Hardware) ? RTS_CONTROL_HANDSHAKE : RTS_CONTROL_ENABLE;
        dcb.fOutX        = (flow == JFlowCtrl::Software) ? TRUE : FALSE;
        dcb.fInX         = (flow == JFlowCtrl::Software) ? TRUE : FALSE;
        if (!SetCommState(m_handle, &dcb)) {
            _postError("SetCommState failed"); _closeHandles(); return false;
        }
        // No timeout on overlapped reads — completion drives the loop.
        COMMTIMEOUTS to{};
        to.ReadIntervalTimeout = MAXDWORD;
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
            case JDataBits::Five:  tty.c_cflag |= CS5; break;
            case JDataBits::Six:   tty.c_cflag |= CS6; break;
            case JDataBits::Seven: tty.c_cflag |= CS7; break;
            case JDataBits::Eight: tty.c_cflag |= CS8; break;
        }
        if (stopBits == JStopBits::Two) tty.c_cflag |= CSTOPB; else tty.c_cflag &= ~CSTOPB;
        tty.c_cflag &= ~PARENB;
        if (parity != JParity::None) {
            tty.c_cflag |= PARENB;
            if (parity == JParity::Odd) tty.c_cflag |= PARODD;
        }
        if (flow == JFlowCtrl::Hardware) tty.c_cflag |= CRTSCTS; else tty.c_cflag &= ~CRTSCTS;
        tty.c_cflag |= (CREAD | CLOCAL);
        tty.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG);
        tty.c_iflag &= ~(IXON | IXOFF | IXANY);
        if (flow == JFlowCtrl::Software) tty.c_iflag |= (IXON | IXOFF);
        tty.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL);
        tty.c_oflag &= ~OPOST;
        tty.c_cc[VMIN]  = 1;   // block until at least 1 byte
        tty.c_cc[VTIME] = 1;   // or 100 ms
        if (tcsetattr(m_fd, TCSANOW, &tty) != 0) {
            _postError("tcsetattr failed"); ::close(m_fd); m_fd = -1; return false;
        }
        // Switch back to blocking now that VMIN/VTIME are set.
        int flags = fcntl(m_fd, F_GETFL, 0);
        fcntl(m_fd, F_SETFL, flags & ~O_NONBLOCK);

        // Create a pipe so close() can wake the blocking read.
        if (pipe(m_pipeFd) != 0) {
            _postError("pipe failed"); ::close(m_fd); m_fd = -1; return false;
        }
#endif
        flushInput();                 // start clean — drop any stale bytes from a prior session
        m_port    = port;
        m_running = true;
        m_thread  = std::thread([this]{ _readLoop(); });
        if (JAiBusHook::emit) JAiBusHook::emit(0, "serial.open", port.c_str());
        return true;
    }

    void close() {
        if (!m_running.exchange(false)) return;
        if (JAiBusHook::emit && !m_port.empty())
            JAiBusHook::emit(0, "serial.close", m_port.c_str());
        m_port.clear();
        _wakeReadThread();
        if (m_thread.joinable()) m_thread.join();
        _closeHandles();
    }

    bool isOpen() const {
#if defined(_WIN32)
        return m_handle != INVALID_HANDLE_VALUE;
#else
        return m_fd >= 0;
#endif
    }

    // Discard any bytes the OS has already buffered on the receive side. open() calls this so a
    // stale or mid-frame buffer left from a previous session can't corrupt the first parse; callers
    // may also use it to resync after a protocol error.
    void flushInput() {
#if defined(_WIN32)
        if (m_handle != INVALID_HANDLE_VALUE) PurgeComm(m_handle, PURGE_RXCLEAR);
#else
        if (m_fd >= 0) tcflush(m_fd, TCIFLUSH);
#endif
    }

    // Write bytes — thread-safe, handles partial writes internally.
    bool write(const std::vector<uint8_t>& data) {
        if (!isOpen() || data.empty()) return false;
        std::lock_guard<std::mutex> lk(m_writeMutex);
#if defined(_WIN32)
        OVERLAPPED ov{};
        ov.hEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);
        DWORD written = 0;
        bool ok = true;
        size_t offset = 0;
        while (offset < data.size()) {
            DWORD toWrite = static_cast<DWORD>(data.size() - offset);
            if (!WriteFile(m_handle, data.data() + offset, toWrite, nullptr, &ov)) {
                if (GetLastError() != ERROR_IO_PENDING) { ok = false; break; }
            }
            if (!GetOverlappedResult(m_handle, &ov, &written, TRUE)) { ok = false; break; }
            offset += written;
            ResetEvent(ov.hEvent);
        }
        CloseHandle(ov.hEvent);
        return ok;
#else
        size_t offset = 0;
        while (offset < data.size()) {
            ssize_t n = ::write(m_fd, data.data() + offset, data.size() - offset);
            if (n < 0) {
                if (errno == EINTR) continue;
                _postError(std::string("write error: ") + std::strerror(errno));
                return false;
            }
            offset += static_cast<size_t>(n);
        }
        return true;
#endif
    }

    bool writeLine(const std::string& s) {
        std::vector<uint8_t> v(s.begin(), s.end());
        v.push_back('\n');
        return write(v);
    }

    // Discard buffered input/output.
    void flush() {
        if (!isOpen()) return;
#if defined(_WIN32)
        PurgeComm(m_handle, PURGE_RXCLEAR | PURGE_TXCLEAR);
#else
        tcflush(m_fd, TCIOFLUSH);
#endif
    }

    // Enumerate available ports with descriptions, manufacturer, and VID/PID.
    static std::vector<JSerialPortInfo> availablePorts() {
        std::vector<JSerialPortInfo> out;
#if defined(_WIN32)
        HKEY key;
        if (RegOpenKeyExA(HKEY_LOCAL_MACHINE,
                "HARDWARE\\DEVICEMAP\\SERIALCOMM", 0, KEY_READ, &key) == ERROR_SUCCESS) {
            char name[256], value[256];
            DWORD idx = 0, nameLen, valueLen, type;
            while (true) {
                nameLen = sizeof(name); valueLen = sizeof(value);
                if (RegEnumValueA(key, idx++, name, &nameLen, nullptr,
                                  &type, (LPBYTE)value, &valueLen) != ERROR_SUCCESS) break;
                JSerialPortInfo info;
                info.port        = value;
                info.description = name;
                out.push_back(std::move(info));
            }
            RegCloseKey(key);
        }
#else
        const char* sysPath = "/sys/class/tty";
        DIR* dir = opendir(sysPath);
        if (!dir) return out;
        struct dirent* ent;
        while ((ent = readdir(dir)) != nullptr) {
            std::string name = ent->d_name;
            if (name == "." || name == "..") continue;
            bool isUsb = (name.rfind("ttyUSB", 0) == 0 || name.rfind("ttyACM", 0) == 0);
            std::string devPath = std::string(sysPath) + "/" + name + "/device";
            bool hasDevice = (access(devPath.c_str(), F_OK) == 0);
            if (!isUsb && !hasDevice) continue;
            std::string devNode = "/dev/" + name;
            if (access(devNode.c_str(), F_OK) != 0) continue;
            JSerialPortInfo info;
            info.port         = devNode;
            std::string base  = std::string(sysPath) + "/" + name;
            info.description  = _sysfsAttr(base, "device/../product");
            info.manufacturer = _sysfsAttr(base, "device/../manufacturer");
            info.serialNumber = _sysfsAttr(base, "device/../serial");
            std::string vidStr = _sysfsAttr(base, "device/../idVendor");
            std::string pidStr = _sysfsAttr(base, "device/../idProduct");
            if (!vidStr.empty() && !pidStr.empty()) {
                info.vendorId  = static_cast<uint16_t>(std::stoul(vidStr, nullptr, 16));
                info.productId = static_cast<uint16_t>(std::stoul(pidStr, nullptr, 16));
                info.hasVidPid = true;
            }
            if (info.description.empty()) info.description = name;
            out.push_back(std::move(info));
        }
        closedir(dir);
        std::sort(out.begin(), out.end(),
                  [](const JSerialPortInfo& a, const JSerialPortInfo& b){ return a.port < b.port; });
#endif
        return out;
    }

private:
    void _readLoop() {
        std::vector<uint8_t> buf(4096);
#if defined(_WIN32)
        OVERLAPPED ov{};
        ov.hEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);
        HANDLE waitHandles[2] = { ov.hEvent, m_cancelEvent };
        while (m_running) {
            DWORD nRead = 0;
            ResetEvent(ov.hEvent);
            if (!ReadFile(m_handle, buf.data(), static_cast<DWORD>(buf.size()), nullptr, &ov)) {
                if (GetLastError() != ERROR_IO_PENDING) { _signalDisconnect(); break; }
            }
            DWORD wait = WaitForMultipleObjects(2, waitHandles, FALSE, INFINITE);
            if (wait == WAIT_OBJECT_0) {
                if (!GetOverlappedResult(m_handle, &ov, &nRead, FALSE)) { _signalDisconnect(); break; }
                if (nRead > 0) _dispatch(std::vector<uint8_t>(buf.begin(), buf.begin() + nRead));
            } else { CancelIo(m_handle); break; }
        }
        CloseHandle(ov.hEvent);
#else
        while (m_running) {
            fd_set fds;
            FD_ZERO(&fds);
            FD_SET(m_fd, &fds);
            FD_SET(m_pipeFd[0], &fds);
            int maxFd = (m_pipeFd[0] > m_fd) ? m_pipeFd[0] : m_fd;
            int ret = select(maxFd + 1, &fds, nullptr, nullptr, nullptr);
            if (ret < 0) { if (errno == EINTR) continue; _signalDisconnect(); break; }
            if (FD_ISSET(m_pipeFd[0], &fds)) break;
            if (FD_ISSET(m_fd, &fds)) {
                ssize_t n = ::read(m_fd, buf.data(), buf.size());
                if (n > 0) _dispatch(std::vector<uint8_t>(buf.begin(), buf.begin() + n));
                else if (n == 0 || (n < 0 && errno != EINTR && errno != EAGAIN)) { _signalDisconnect(); break; }
            }
        }
#endif
    }

    void _wakeReadThread() {
#if defined(_WIN32)
        if (m_cancelEvent) SetEvent(m_cancelEvent);
#else
        if (m_pipeFd[1] >= 0) { uint8_t b = 0; ::write(m_pipeFd[1], &b, 1); }
#endif
    }

    void _closeHandles() {
#if defined(_WIN32)
        if (m_handle != INVALID_HANDLE_VALUE) { CloseHandle(m_handle); m_handle = INVALID_HANDLE_VALUE; }
        if (m_cancelEvent)                    { CloseHandle(m_cancelEvent); m_cancelEvent = nullptr; }
#else
        if (m_fd >= 0)        { ::close(m_fd);        m_fd = -1; }
        if (m_pipeFd[0] >= 0) { ::close(m_pipeFd[0]); m_pipeFd[0] = -1; }
        if (m_pipeFd[1] >= 0) { ::close(m_pipeFd[1]); m_pipeFd[1] = -1; }
#endif
    }

    void _dispatch(std::vector<uint8_t> data) {
        if (JAiBusHook::emit) {
            std::string len = std::to_string(data.size()) + "B";
            JAiBusHook::emit(0, "serial.data", len.c_str());
        }
        JMainThreadDispatcher::instance().post([this, d = std::move(data)]() mutable {
            onData.emit(std::move(d));
        });
    }

    void _postError(const std::string& msg) {
        JMainThreadDispatcher::instance().post([this, msg]{ onError.emit(msg); });
    }

    void _signalDisconnect() {
        JMainThreadDispatcher::instance().post([this]{ onDisconnect.emit(); });
    }

#if !defined(_WIN32)
    static std::string _sysfsAttr(const std::string& base, const std::string& rel) {
        std::string path = base + "/" + rel;
        FILE* f = fopen(path.c_str(), "r");
        if (!f) return {};
        char buf[256] = {};
        if (fgets(buf, sizeof(buf), f)) {
            std::string s = buf;
            while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' '))
                s.pop_back();
            fclose(f);
            return s;
        }
        fclose(f);
        return {};
    }
#endif

#if defined(_WIN32)
    HANDLE            m_handle{INVALID_HANDLE_VALUE};
    HANDLE            m_cancelEvent{nullptr};
    static speed_t    _posixBaud(JBaudRate) { return 0; }
#else
    int               m_fd{-1};
    int               m_pipeFd[2]{-1, -1};

    static speed_t _posixBaud(JBaudRate b) {
        switch (b) {
            case JBaudRate::B1200_:   return B1200;
            case JBaudRate::B4800_:   return B4800;
            case JBaudRate::B9600_:   return B9600;
            case JBaudRate::B19200_:  return B19200;
            case JBaudRate::B38400_:  return B38400;
            case JBaudRate::B57600_:  return B57600;
            case JBaudRate::B115200_: return B115200;
            case JBaudRate::B230400_: return B230400;
            case JBaudRate::B921600_: return B921600;
            default:                 return B115200;
        }
    }
#endif

    std::string       m_port;
    std::thread       m_thread;
    std::atomic<bool> m_running{false};
    std::mutex        m_writeMutex;
};

} // inline namespace jf
