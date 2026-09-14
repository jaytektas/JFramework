// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jason Roughley <pis.controller@gmail.com>

#include <j/io/SerialPort.h>
#include <j/core/MainThreadDispatcher.h>

#include <thread>
#include <atomic>
#include <mutex>
#include <algorithm>
#include <cstring>

// Everything platform-specific is confined to this translation unit — the public
// SerialPort.h header carries no HANDLE / termios / windows.h. See CLAUDE.md
// "Platform boundary": platform types never leak past the .cpp.
#if defined(_WIN32)
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
  #include <setupapi.h>          // port ENUMERATION: the registry knows the COM names, not the devices
  #include <initguid.h>          // BEFORE devguid.h: this is what turns the GUID declarations into
  #include <devguid.h>           // definitions, and without it GUID_DEVCLASS_PORTS fails to link
#else
  #include <termios.h>
  #include <fcntl.h>
  #include <unistd.h>
  #include <dirent.h>
  #include <errno.h>
  #include <cstring>
  #include <cstdio>
  #include <sys/ioctl.h>
  #include <sys/select.h>
#endif

inline namespace jf {

// ---- Opaque implementation -------------------------------------------------
struct JSerialPort::Impl {
    explicit Impl(JSerialPort& o) : owner(o) {}
    ~Impl() { close(); }

    JSerialPort&      owner;
    std::string       m_port;
    std::thread       m_thread;
    std::atomic<bool> m_running{false};
    std::mutex        m_writeMutex;

#if defined(_WIN32)
    HANDLE m_handle{INVALID_HANDLE_VALUE};
    HANDLE m_cancelEvent{nullptr};
    static DWORD _winBaud(JBaudRate b) { return static_cast<DWORD>(b); }

    // The system's own words for a failure code — "Access is denied." rather than "5". The two that
    // matter on a serial port are ACCESS_DENIED (something else has it open) and FILE_NOT_FOUND (it
    // is gone), and both are worth reading rather than decoding.
    static std::string _winError(DWORD e) {
        char* msg = nullptr;
        const DWORD n = FormatMessageA(
            FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
            nullptr, e, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), (LPSTR)&msg, 0, nullptr);
        std::string out = (n && msg) ? std::string(msg, n) : ("error " + std::to_string(e));
        if (msg) LocalFree(msg);
        while (!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == ' ')) out.pop_back();
        if (out.empty()) out = "error " + std::to_string(e);
        return out;
    }
#else
    int m_fd{-1};
    int m_pipeFd[2]{-1, -1};

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
            default:                  return B115200;
        }
    }

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

    bool open(const std::string& port, JBaudRate baud, JDataBits dataBits,
              JStopBits stopBits, JParity parity, JFlowCtrl flow) {
#if defined(_WIN32)
        m_cancelEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);

        std::string path = "\\\\.\\" + port;
        m_handle = CreateFileA(path.c_str(),
            GENERIC_READ | GENERIC_WRITE, 0, nullptr,
            OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
        if (m_handle == INVALID_HANDLE_VALUE) {
            const DWORD e = GetLastError();
            CloseHandle(m_cancelEvent); m_cancelEvent = nullptr;
            // WHY it failed, not just that it did. The POSIX branch has always said strerror(errno);
            // this one said "Failed to open port: COM5" and nothing else, which cannot tell "another
            // program holds it" from "it is no longer there" — the two things you actually do
            // something about.
            _postError("Failed to open " + port + ": " + _winError(e));
            return false;
        }
        DCB dcb{};
        dcb.DCBlength = sizeof(DCB);
        // CHECKED, because an unchecked failure here leaves dcb zeroed and the SetCommState below
        // fails on a structure we filled in ourselves — reported as a baud-rate problem when the
        // truth is that the port never answered.
        if (!GetCommState(m_handle, &dcb)) {
            _postError("Cannot read the port settings of " + port + ": " + _winError(GetLastError()));
            _closeHandles();
            return false;
        }
        dcb.BaudRate = _winBaud(baud);
        dcb.ByteSize = static_cast<BYTE>(dataBits);
        dcb.StopBits = (stopBits == JStopBits::Two) ? TWOSTOPBITS : ONESTOPBIT;
        dcb.Parity   = (parity == JParity::None) ? NOPARITY :
                       (parity == JParity::Even) ? EVENPARITY : ODDPARITY;
        dcb.fOutxCtsFlow = (flow == JFlowCtrl::Hardware) ? TRUE : FALSE;
        dcb.fRtsControl  = (flow == JFlowCtrl::Hardware) ? RTS_CONTROL_HANDSHAKE : RTS_CONTROL_ENABLE;
        dcb.fOutX        = (flow == JFlowCtrl::Software) ? TRUE : FALSE;
        dcb.fInX         = (flow == JFlowCtrl::Software) ? TRUE : FALSE;
        if (!SetCommState(m_handle, &dcb)) {
            _postError("Cannot configure " + port + ": " + _winError(GetLastError()));
            _closeHandles(); return false;
        }
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
        int flags = fcntl(m_fd, F_GETFL, 0);
        fcntl(m_fd, F_SETFL, flags & ~O_NONBLOCK);

        if (pipe(m_pipeFd) != 0) {
            _postError("pipe failed"); ::close(m_fd); m_fd = -1; return false;
        }
#endif
        flushInput();                 // start clean — drop any stale bytes
        m_port    = port;
        m_running = true;
        m_thread  = std::thread([this]{ _readLoop(); });
        return true;
    }

    void close() {
        if (!m_running.exchange(false)) return;
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

    void flushInput() {
#if defined(_WIN32)
        if (m_handle != INVALID_HANDLE_VALUE) PurgeComm(m_handle, PURGE_RXCLEAR);
#else
        if (m_fd >= 0) tcflush(m_fd, TCIFLUSH);
#endif
    }

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

    void flush() {
        if (!isOpen()) return;
#if defined(_WIN32)
        PurgeComm(m_handle, PURGE_RXCLEAR | PURGE_TXCLEAR);
#else
        tcflush(m_fd, TCIOFLUSH);
#endif
    }

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
        if (m_pipeFd[1] >= 0) { uint8_t b = 0; ssize_t r = ::write(m_pipeFd[1], &b, 1); (void)r; }
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
        JSerialPort* o = &owner;
        JMainThreadDispatcher::instance().post([o, d = std::move(data)]() mutable {
            o->onData.emit(std::move(d));
        });
    }

    void _postError(const std::string& msg) {
        JSerialPort* o = &owner;
        JMainThreadDispatcher::instance().post([o, msg]{ o->onError.emit(msg); });
    }

    void _signalDisconnect() {
        JSerialPort* o = &owner;
        JMainThreadDispatcher::instance().post([o]{ o->onDisconnect.emit(); });
    }
};

// ---- Public surface: forwards to Impl --------------------------------------
JSerialPort::JSerialPort()  : m_impl(std::make_unique<Impl>(*this)) {}
JSerialPort::~JSerialPort() = default;

bool JSerialPort::open(const std::string& port, JBaudRate baud, JDataBits dataBits,
                       JStopBits stopBits, JParity parity, JFlowCtrl flow) {
    return m_impl->open(port, baud, dataBits, stopBits, parity, flow);
}

void JSerialPort::close()             { m_impl->close(); }
bool JSerialPort::isOpen() const      { return m_impl->isOpen(); }
void JSerialPort::flushInput()        { m_impl->flushInput(); }
bool JSerialPort::write(const std::vector<uint8_t>& data) { return m_impl->write(data); }
void JSerialPort::flush()             { m_impl->flush(); }

bool JSerialPort::writeLine(const std::string& s) {
    std::vector<uint8_t> v(s.begin(), s.end());
    v.push_back('\n');
    return write(v);
}

std::vector<JSerialPortInfo> JSerialPort::availablePorts() {
    std::vector<JSerialPortInfo> out;
#if defined(_WIN32)
    // SETUP API FIRST, because HARDWARE\DEVICEMAP\SERIALCOMM knows only that a COM number exists —
    // not what is behind it. Without a vendor and product id every USB adapter looks alike, and a
    // caller that wants "the one that is my hardware" has nothing to choose on. The device class is
    // what carries VID/PID, the manufacturer, and a name a person would recognise.
    const HDEVINFO set = SetupDiGetClassDevsA(&GUID_DEVCLASS_PORTS, nullptr, nullptr, DIGCF_PRESENT);
    if (set != INVALID_HANDLE_VALUE) {
        SP_DEVINFO_DATA dev{};
        dev.cbSize = sizeof(dev);
        for (DWORD i = 0; SetupDiEnumDeviceInfo(set, i, &dev); ++i) {
            // The COM name lives under the device's own key, not in the property store.
            char portName[64] = {0};
            const HKEY devKey = SetupDiOpenDevRegKey(set, &dev, DICS_FLAG_GLOBAL, 0,
                                                     DIREG_DEV, KEY_READ);
            if (devKey == INVALID_HANDLE_VALUE) continue;
            DWORD len = sizeof(portName) - 1, type = 0;   // -1: the registry does not promise a NUL,
            const LONG r = RegQueryValueExA(devKey, "PortName", nullptr, &type,
                                            (LPBYTE)portName, &len);
            RegCloseKey(devKey);
            portName[sizeof(portName) - 1] = '\0';        // and a value that fills the buffer would
                                                          // otherwise be read past its end
            // LPT ports are in this class too, and they are not serial ports.
            if (r != ERROR_SUCCESS || type != REG_SZ || std::strncmp(portName, "COM", 3) != 0)
                continue;

            JSerialPortInfo info;
            info.port = portName;

            char buf[512] = {0};
            if (SetupDiGetDeviceRegistryPropertyA(set, &dev, SPDRP_FRIENDLYNAME, nullptr,
                                                  (PBYTE)buf, sizeof(buf), nullptr))
                info.description = buf;
            if (SetupDiGetDeviceRegistryPropertyA(set, &dev, SPDRP_MFG, nullptr,
                                                  (PBYTE)buf, sizeof(buf), nullptr))
                info.manufacturer = buf;

            // "USB\VID_0483&PID_5740\<serial>" — the instance id carries all three, so it is parsed
            // rather than queried three ways. A built-in COM port has no VID_ in it and keeps
            // hasVidPid false, which is exactly what it should report.
            char id[512] = {0};
            if (SetupDiGetDeviceInstanceIdA(set, &dev, id, sizeof(id), nullptr)) {
                const std::string s(id);
                const size_t v = s.find("VID_"), p = s.find("PID_");
                if (v != std::string::npos && p != std::string::npos &&
                    v + 8 <= s.size() && p + 8 <= s.size()) {
                    try {
                        info.vendorId  = static_cast<uint16_t>(std::stoul(s.substr(v + 4, 4), nullptr, 16));
                        info.productId = static_cast<uint16_t>(std::stoul(s.substr(p + 4, 4), nullptr, 16));
                        info.hasVidPid = true;
                    } catch (...) { /* a malformed id is a port without ids, not a failure to enumerate */ }
                }
                // The tail after the last backslash is the device's serial number when it has one.
                const size_t last = s.rfind('\\');
                if (info.hasVidPid && last != std::string::npos && last + 1 < s.size() &&
                    s.find('&', last) == std::string::npos)
                    info.serialNumber = s.substr(last + 1);
            }
            if (info.description.empty()) info.description = info.port;
            out.push_back(std::move(info));
        }
        SetupDiDestroyDeviceInfoList(set);
    }

    // Anything SERIALCOMM lists that the device class did not (a port with no PnP device behind it —
    // some virtual and Bluetooth ports) is still a port somebody may want to open.
    HKEY key;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE,
            "HARDWARE\\DEVICEMAP\\SERIALCOMM", 0, KEY_READ, &key) == ERROR_SUCCESS) {
        char name[256], value[256];
        DWORD idx = 0, nameLen, valueLen, type;
        while (true) {
            nameLen = sizeof(name); valueLen = sizeof(value);
            if (RegEnumValueA(key, idx++, name, &nameLen, nullptr,
                              &type, (LPBYTE)value, &valueLen) != ERROR_SUCCESS) break;
            if (std::any_of(out.begin(), out.end(),
                            [&](const JSerialPortInfo& e) { return e.port == value; })) continue;
            JSerialPortInfo info;
            info.port        = value;
            info.description = name;
            out.push_back(std::move(info));
        }
        RegCloseKey(key);
    }

    // COM10 after COM9, not before it: the numeric tail is what a person is reading.
    std::sort(out.begin(), out.end(), [](const JSerialPortInfo& a, const JSerialPortInfo& b) {
        auto num = [](const std::string& p) {
            return p.size() > 3 ? std::strtol(p.c_str() + 3, nullptr, 10) : 0L;
        };
        return num(a.port) < num(b.port);
    });
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
        info.description  = Impl::_sysfsAttr(base, "device/../product");
        info.manufacturer = Impl::_sysfsAttr(base, "device/../manufacturer");
        info.serialNumber = Impl::_sysfsAttr(base, "device/../serial");
        std::string vidStr = Impl::_sysfsAttr(base, "device/../idVendor");
        std::string pidStr = Impl::_sysfsAttr(base, "device/../idProduct");
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

} // inline namespace jf
