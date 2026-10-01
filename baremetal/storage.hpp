#pragma once
// Persistent files for the stream shim (overrides/stream_shim.h): the FAT12
// boot floppy when the game was booted from one, otherwise nothing (files
// then live in RAM only).
#include <stdint.h>

void storage_init(uint32_t mb_flags, uint32_t boot_device, const char* cmdline);
