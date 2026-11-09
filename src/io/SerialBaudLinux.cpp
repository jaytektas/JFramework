// Arbitrary serial baud rates on Linux: termios2 with BOTHER. Its own translation unit because
// <asm/termbits.h> (struct termios2, BOTHER) and glibc's <termios.h> (used by SerialPort.cpp)
// define the same names and cannot be included together.
#include <asm/termbits.h>
#include <sys/ioctl.h>
#include <cstdint>

namespace jf::detail {

// Set both directions of an open tty to `baud` exactly as asked (the driver picks the nearest
// rate the UART can make, e.g. a CP2102 has a fixed table). Returns false if the driver refuses.
bool setCustomBaud(int fd, uint32_t baud)
{
    struct termios2 t {};
    if (ioctl(fd, TCGETS2, &t) != 0) return false;
    t.c_cflag &= ~CBAUD;
    t.c_cflag |= BOTHER;
    t.c_cflag &= ~(CBAUD << IBSHIFT);
    t.c_cflag |= BOTHER << IBSHIFT;
    t.c_ispeed = baud;
    t.c_ospeed = baud;
    return ioctl(fd, TCSETS2, &t) == 0;
}

} // namespace jf::detail
