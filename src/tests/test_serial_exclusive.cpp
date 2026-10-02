// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jason Roughley <pis.controller@gmail.com>

// A serial port has one owner: a second open of a port already open fails and says the port is in
// use, rather than both openers silently sharing (and halving) what arrives. Linux only — Windows
// opens COM ports exclusively by itself. Uses a pseudo-terminal, so no hardware is needed.
#include <j/io/SerialPort.h>

#include <cassert>

#if !defined(_WIN32)
#include <pty.h>
#include <unistd.h>
#endif

using namespace jf;

int main() {
#if !defined(_WIN32)
    int master = -1, slave = -1;
    char name[256] = {};
    const int rc = openpty(&master, &slave, name, nullptr, nullptr);
    assert(rc == 0);
    ::close(slave);   // the ports below open it by name

    JSerialPort first, second;
    assert(first.open(name));
    assert(!second.open(name));
    assert(second.lastError().find("in use") != std::string::npos);

    first.close();
    assert(second.open(name));   // free again once the owner lets go
    second.close();
    ::close(master);
#endif
    return 0;
}
