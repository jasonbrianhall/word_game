// Persistent files on the boot floppy. See storage.hpp.
#include "storage.hpp"
#include "fat12.hpp"
#include "floppy.hpp"
#include <stdio.h>
#include <string.h>
#include <string>

static bool enabled, mounted;

static void idle() { floppy_idle(); }
static const FatDisk disk = {floppy_read, floppy_write, idle};

// The disk can be taken out or swapped while the game runs: check before
// each use, and remount when a different disk is in.
static bool ready() {
    if (!enabled) return false;
    int m = floppy_check_media();
    if (m < 0) {
        if (mounted) printf("Storage: floppy removed\n");
        mounted = false;
    } else if (m == 1 || !mounted) {
        mounted = fat_mount(disk);
        printf(mounted ? "Storage: FAT12 floppy mounted, %u KB free\n" : "Storage: floppy isn't FAT12\n",
               fat_free_bytes() / 1024);
    }
    if (!mounted) floppy_idle();
    return mounted;
}

void storage_init(uint32_t mb_flags, uint32_t boot_device, const char* cmdline) {
    if (cmdline && strstr(cmdline, "floppy=off")) return;
    // Multiboot boot_device: BIOS drive number in the top byte; 0x00 is A:.
    if (!(mb_flags & (1 << 1)) || (boot_device >> 24) != 0x00) return;
    if (!floppy_init()) return;
    enabled = true;
    ready();
}

// ---- hooks for overrides/stream_shim.h
bool platform_file_load(const char* path, std::string& out) {
    return ready() && fat_read(path, out);
}

void platform_file_store(const char* path, const std::string& all, size_t from, bool append) {
    if (!ready()) return;
    if (const char* slash = strrchr(path, '/'); slash && slash != path)
        fat_mkdir(std::string(path, slash - path).c_str());
    bool ok = append ? fat_append(path, all.data() + from, all.size() - from)
                     : fat_write(path, all.data(), all.size());
    if (!ok) printf("Storage: couldn't save %s%s\n", path, floppy_write_protected() ? " (disk is write-protected)" : "");
}
