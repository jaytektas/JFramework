// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jason Roughley <pis.controller@gmail.com>

#pragma once

// JVolumes — the drives currently mounted on this machine, and which of them are removable.
//
// The companion to JSerialPort::availablePorts(): the same question ("what devices are attached
// right now") asked about storage instead of serial. An app that waits for a card reader, a USB
// stick or an embedded device's mass-storage mode to appear needs this and nothing more, and every
// app that needs it would otherwise write the same two backends — /proc/mounts plus
// /sys/block/<disk>/removable on Linux, GetLogicalDrives + GetDriveType on Windows.
//
// The public interface carries no platform types; both backends live in Volumes.cpp.
//
// POLLED, not evented. There is no portable "a volume appeared" notification (Linux wants udev or
// a DBus connection to udisks2, Windows wants a window handle to receive WM_DEVICECHANGE), and a
// list this cheap to build does not justify either dependency. Call it on a timer.

#include <cstdint>
#include <string>
#include <vector>

inline namespace jf {

struct JVolumeInfo {
    std::string mountPoint;    // "/run/media/jay/ECU" or "E:\"  — where to open files
    std::string label;         // volume label, empty when the filesystem carries none
    std::string device;        // "/dev/sdb1" on Linux; empty on Windows
    std::string filesystem;    // "vfat", "exfat", "FAT32", …
    bool        removable{false};
    uint64_t    totalBytes{0};
    uint64_t    freeBytes{0};
};

class JVolumes {
public:
    // Everything mounted that looks like a real filesystem — pseudo and system mounts (proc, sysfs,
    // cgroup, tmpfs on Linux; anything not a drive letter on Windows) are left out, because no
    // caller has ever wanted them and including them makes the list useless at a glance.
    static std::vector<JVolumeInfo> mountedVolumes();

    // …narrowed to the removable ones. The usual question: which of these is the card the user just
    // plugged in.
    static std::vector<JVolumeInfo> removableVolumes();
};

} // namespace jf
