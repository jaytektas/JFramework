// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jason Roughley <pis.controller@gmail.com>

// JVolumes — see the header. Two backends, no shared code worth sharing between them.

#include <j/io/Volumes.h>

#include <algorithm>
#include <cctype>

#if defined(_WIN32)
  #include <windows.h>
#else
  #include <sys/statvfs.h>
  #include <cstdio>
  #include <cstring>
  #include <filesystem>
  #include <map>
#endif

inline namespace jf {

#if defined(_WIN32)

// ---- Windows -------------------------------------------------------------------------------
// Drive letters, which is the whole of what a "volume" means to a user here. A drive with no medium
// (an empty card reader) answers GetVolumeInformation with a failure and is skipped: the letter
// exists, the volume does not, and listing it would offer a folder that cannot be opened.
static std::vector<JVolumeInfo> _enumerate(bool removableOnly) {
    std::vector<JVolumeInfo> out;
    const DWORD mask = GetLogicalDrives();
    for (int i = 0; i < 26; ++i) {
        if (!(mask & (1u << i))) continue;
        char root[4] = { char('A' + i), ':', '\\', '\0' };
        const UINT type = GetDriveTypeA(root);
        if (type != DRIVE_REMOVABLE && type != DRIVE_FIXED) continue;   // no network/CD/RAM drives
        const bool removable = (type == DRIVE_REMOVABLE);
        if (removableOnly && !removable) continue;

        char label[MAX_PATH + 1] = {};
        char fs[MAX_PATH + 1]    = {};
        if (!GetVolumeInformationA(root, label, MAX_PATH, nullptr, nullptr, nullptr, fs, MAX_PATH))
            continue;                       // a letter with no medium in it

        JVolumeInfo v;
        v.mountPoint = root;
        v.label      = label;
        v.filesystem = fs;
        v.removable  = removable;
        ULARGE_INTEGER freeAvail{}, total{}, freeTotal{};
        if (GetDiskFreeSpaceExA(root, &freeAvail, &total, &freeTotal)) {
            v.totalBytes = total.QuadPart;
            v.freeBytes  = freeAvail.QuadPart;
        }
        out.push_back(std::move(v));
    }
    return out;
}

#else

// ---- Linux ---------------------------------------------------------------------------------
namespace {

// /proc/mounts escapes space, tab, newline and backslash as octal. A mount point under
// /run/media/<user>/ very often contains a space, so a reader that does not undo this hands back a
// path that does not exist.
std::string unescape(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 3 < s.size() &&
            s[i+1] >= '0' && s[i+1] <= '7' &&
            s[i+2] >= '0' && s[i+2] <= '7' &&
            s[i+3] >= '0' && s[i+3] <= '7') {
            out += char((s[i+1] - '0') * 64 + (s[i+2] - '0') * 8 + (s[i+3] - '0'));
            i += 3;
        } else {
            out += s[i];
        }
    }
    return out;
}

// The DISK a partition belongs to, which is where the removable flag lives — /sys/block holds
// disks, not partitions. Derived by trimming rather than by pattern: sdb1 -> sdb, nvme0n1p2 ->
// nvme0n1, mmcblk0p1 -> mmcblk0 all fall out of "strip trailing digits, then a trailing p, until
// /sys/block/<name> exists", and a naming scheme nobody here has heard of still resolves if it
// follows the same shape.
std::string diskOf(const std::string& devPath) {
    const size_t slash = devPath.rfind('/');
    std::string name = (slash == std::string::npos) ? devPath : devPath.substr(slash + 1);
    namespace fs = std::filesystem;
    std::error_code ec;
    for (int guard = 0; guard < 8 && !name.empty(); ++guard) {
        if (fs::exists("/sys/block/" + name, ec)) return name;
        while (!name.empty() && std::isdigit(static_cast<unsigned char>(name.back()))) name.pop_back();
        if (!name.empty() && name.back() == 'p') name.pop_back();
        else if (fs::exists("/sys/block/" + name, ec)) return name;
        else if (name.empty()) break;
        else continue;
    }
    return name;
}

bool isRemovable(const std::string& devPath) {
    const std::string disk = diskOf(devPath);
    if (disk.empty()) return false;
    std::FILE* f = std::fopen(("/sys/block/" + disk + "/removable").c_str(), "r");
    if (!f) return false;
    int v = 0;
    const bool ok = (std::fscanf(f, "%d", &v) == 1);
    std::fclose(f);
    return ok && v != 0;
}

// Labels come from the by-label symlink farm, which is the only place a label is exposed without
// linking blkid. Built once per call: a handful of readlinks against a directory that usually holds
// two or three entries.
std::map<std::string, std::string> labelsByDevice() {
    namespace fs = std::filesystem;
    std::map<std::string, std::string> out;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator("/dev/disk/by-label", ec)) {
        std::error_code re;
        const fs::path target = fs::canonical(e.path(), re);
        if (re) continue;
        out[target.string()] = unescape(e.path().filename().string());
    }
    return out;
}

}  // namespace

static std::vector<JVolumeInfo> _enumerate(bool removableOnly) {
    std::vector<JVolumeInfo> out;
    std::FILE* mounts = std::fopen("/proc/mounts", "r");
    if (!mounts) return out;

    const std::map<std::string, std::string> labels = labelsByDevice();
    char line[4096];
    while (std::fgets(line, sizeof(line), mounts)) {
        char dev[1024] = {}, point[1024] = {}, type[256] = {};
        if (std::sscanf(line, "%1023s %1023s %255s", dev, point, type) != 3) continue;
        // A real device, not a pseudo filesystem. Everything the caller could open files on comes
        // from /dev; proc, sysfs, cgroup, tmpfs and the overlay zoo never do.
        if (std::strncmp(dev, "/dev/", 5) != 0) continue;
        // …and /dev is not enough on its own. A desktop with snaps mounts thirty-odd read-only
        // squashfs images from /dev/loopN, every one of which passes the test above and none of
        // which is a drive in any sense a user means. Loop devices and FUSE mounts go with them.
        if (std::strcmp(type, "squashfs") == 0) continue;
        if (std::strncmp(dev, "/dev/loop", 9) == 0) continue;
        if (std::strcmp(dev, "/dev/fuse") == 0) continue;

        JVolumeInfo v;
        v.device     = unescape(dev);
        v.mountPoint = unescape(point);
        v.filesystem = type;
        v.removable  = isRemovable(v.device);
        if (removableOnly && !v.removable) continue;
        const auto it = labels.find(v.device);
        if (it != labels.end()) v.label = it->second;

        struct statvfs st {};
        if (statvfs(v.mountPoint.c_str(), &st) == 0) {
            v.totalBytes = static_cast<uint64_t>(st.f_blocks) * st.f_frsize;
            v.freeBytes  = static_cast<uint64_t>(st.f_bavail) * st.f_frsize;
        }
        out.push_back(std::move(v));
    }
    std::fclose(mounts);
    return out;
}

#endif

std::vector<JVolumeInfo> JVolumes::mountedVolumes()   { return _enumerate(false); }
std::vector<JVolumeInfo> JVolumes::removableVolumes() { return _enumerate(true);  }

} // namespace jf
