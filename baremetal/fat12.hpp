#pragma once
// FAT12 filesystem: read, write, create, delete, directories and long
// (VFAT) file names. Hardware-independent: it reaches the disk through the
// two sector callbacks given to fat_mount(), so it also builds on a normal
// host for testing (tools/fat12_test.cpp).
//
// Paths use '/' and are relative to the root ("/letterlock/stats.txt" and
// "letterlock/stats.txt" are the same). Names match case-insensitively.
#include <stdint.h>
#include <stddef.h>
#include <string>

struct FatDisk {
    // Read or write `count` 512-byte sectors starting at `lba`. Return false on error.
    bool (*read)(uint32_t lba, uint32_t count, void* buf);
    bool (*write)(uint32_t lba, uint32_t count, const void* buf);
    void (*idle)();          // optional: called when an operation finishes (motor off)
};

bool fat_mount(const FatDisk& disk);
bool fat_mounted();
void fat_unmount();

bool fat_read(const char* path, std::string& out);                 // whole file
bool fat_write(const char* path, const void* data, size_t len);    // create or replace
bool fat_append(const char* path, const void* data, size_t len);   // create or extend
bool fat_remove(const char* path);                                 // file or empty directory
bool fat_mkdir(const char* path);                                  // parents created too
bool fat_rename(const char* from, const char* to);                 // also moves between directories
bool fat_exists(const char* path, bool* is_dir = nullptr, uint32_t* size = nullptr);
// Calls fn for each entry in a directory (not "." or ".."); stops if fn returns false.
bool fat_list(const char* path, bool (*fn)(const char* name, uint32_t size, bool is_dir, void* ctx), void* ctx);
uint32_t fat_free_bytes();

// Local time for directory entries, as seconds since 1970 (weak; defaults to time()).
long fat_clock();
