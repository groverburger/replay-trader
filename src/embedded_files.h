// Files compiled into the app (built-in scripts, the scripting reference): see cmake/embed_files.cmake.
#pragma once

#include <cstddef>

struct EmbeddedFile {
    const char* path;  // "indicators/ema.lua", "strategies/orb.lua", "lib/prelude.lua", "README.md"
    const char* data;
    size_t size;
};
extern const EmbeddedFile kEmbeddedFiles[];
extern const int kEmbeddedFileCount;
