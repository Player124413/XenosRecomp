#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

// Xbox 360 compressed files, as written by XMemCompress. The shader archives of Sonic Unleashed
// and Sonic Generations are stored this way ("shader.ar.00", "shader.ar.01", ...), so they have to
// be decompressed before the shaders inside of them can be recompiled.
//
// Only the LZX variant is supported, which is the one used by the games. Files that use the older
// LZX delta format are left to the caller.
class XCompressContainer
{
public:
    // Checks whether a file starts with the header of an Xbox 360 compressed file.
    static bool isContainer(const uint8_t* data, size_t size);

    struct Result
    {
        std::vector<uint8_t> data;
        // False when the container ends before all of its uncompressed data was produced, which
        // happens when only one part of an archive that was split in the middle of a compression
        // stream is handed to the recompiler.
        bool complete = false;
        uint32_t blockCount = 0;
    };

    // Decompresses a whole container. Throws std::runtime_error when the container is malformed.
    static Result decompress(const uint8_t* data, size_t size);
};
