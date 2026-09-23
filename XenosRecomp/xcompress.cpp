#include "xcompress.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <string>

#include <fmt/format.h>

#include "thirdparty/libmspack/system.h"
#include "thirdparty/libmspack/lzx.h"

// The Xbox 360 compression container is a small big endian header followed by the compressed
// blocks. Every block is prefixed by its compressed size and decompresses to at most
// 'uncompressedBlockSize' bytes, where the last block holds the remainder of the file.
namespace
{

constexpr uint32_t XCOMPRESS_SIGNATURE = 0x0FF512EE;
constexpr size_t XCOMPRESS_HEADER_SIZE = 48;
constexpr uint16_t XCOMPRESS_UNCOMPRESSED_BLOCK = 0xFF00;

// Refuse to allocate more than this for a single file, so that a malformed header cannot make the
// recompiler allocate an absurd amount of memory.
constexpr uint64_t MAX_UNCOMPRESSED_SIZE = uint64_t(2) << 30;

uint16_t readBigEndian16(const uint8_t* data)
{
    return uint16_t((uint16_t(data[0]) << 8) | uint16_t(data[1]));
}

uint32_t readBigEndian32(const uint8_t* data)
{
    return (uint32_t(data[0]) << 24) | (uint32_t(data[1]) << 16) | (uint32_t(data[2]) << 8) | uint32_t(data[3]);
}

uint64_t readBigEndian64(const uint8_t* data)
{
    return (uint64_t(readBigEndian32(data)) << 32) | uint64_t(readBigEndian32(data + 4));
}

// The compressed data of a block is stored in segments, each prefixed by a big endian size. A size
// with the 0xFF00 flag set is followed by an extra byte and then by the size of the uncompressed
// segment that comes after it.
struct ReadStream
{
    const uint8_t* data = nullptr;
    size_t size = 0;
    size_t position = 0;
    size_t remaining = 0;
};

struct WriteStream
{
    uint8_t* data = nullptr;
    size_t size = 0;
    size_t position = 0;
    size_t dropped = 0;
};

int readStream(mspack_file* file, void* buffer, int bytes)
{
    auto stream = reinterpret_cast<ReadStream*>(file);
    auto output = reinterpret_cast<uint8_t*>(buffer);
    int copied = 0;

    while (copied < bytes)
    {
        if (stream->remaining == 0)
        {
            if (stream->position + 2 > stream->size)
                break;

            uint16_t segmentSize = readBigEndian16(stream->data + stream->position);
            stream->position += 2;

            if ((segmentSize & XCOMPRESS_UNCOMPRESSED_BLOCK) == XCOMPRESS_UNCOMPRESSED_BLOCK)
            {
                if (stream->position + 3 > stream->size)
                    break;

                stream->position += 1;
                segmentSize = readBigEndian16(stream->data + stream->position);
                stream->position += 2;
            }

            // Never read past the end of the block, even if the segment claims to be longer.
            stream->remaining = std::min<size_t>(segmentSize, stream->size - stream->position);

            if (stream->remaining == 0)
                break;
        }

        const size_t toCopy = std::min<size_t>(stream->remaining, size_t(bytes - copied));

        std::memcpy(output + copied, stream->data + stream->position, toCopy);

        stream->position += toCopy;
        stream->remaining -= toCopy;
        copied += int(toCopy);
    }

    return copied;
}

int writeStream(mspack_file* file, void* buffer, int bytes)
{
    auto stream = reinterpret_cast<WriteStream*>(file);
    const size_t toCopy = std::min<size_t>(size_t(bytes), stream->size - stream->position);

    std::memcpy(stream->data + stream->position, buffer, toCopy);

    stream->position += toCopy;
    stream->dropped += size_t(bytes) - toCopy;

    return int(toCopy);
}

void* mspackAlloc(mspack_system* self, size_t bytes)
{
    return operator new(bytes, std::nothrow);
}

void mspackFree(void* pointer)
{
    operator delete(pointer);
}

void mspackCopy(void* source, void* destination, size_t bytes)
{
    std::memcpy(destination, source, bytes);
}

const char* describeError(int status)
{
    switch (status)
    {
    case MSPACK_ERR_READ: return "the compressed data is truncated";
    case MSPACK_ERR_NOMEMORY: return "there was not enough memory";
    case MSPACK_ERR_SIGNATURE: return "the header of the compressed data is invalid";
    case MSPACK_ERR_DATAFORMAT: return "the compressed data has an invalid format";
    case MSPACK_ERR_CRUNCH:
    case MSPACK_ERR_DECRUNCH: return "the compressed data is corrupted";
    default: return "the data could not be decompressed";
    }
}

mspack_system g_lzxSystem =
{
    nullptr,
    nullptr,
    readStream,
    writeStream,
    nullptr,
    nullptr,
    nullptr,
    mspackAlloc,
    mspackFree,
    mspackCopy
};

}

bool XCompressContainer::isContainer(const uint8_t* data, size_t size)
{
    return size >= XCOMPRESS_HEADER_SIZE && readBigEndian32(data) == XCOMPRESS_SIGNATURE;
}

XCompressContainer::Result XCompressContainer::decompress(const uint8_t* data, size_t size)
{
    if (!isContainer(data, size))
        throw std::runtime_error("The file is not an Xbox 360 compressed file.");

    const uint32_t windowSize = readBigEndian32(data + 0x10);
    const uint32_t partitionSize = readBigEndian32(data + 0x14);
    const uint64_t uncompressedSize = readBigEndian64(data + 0x18);
    const uint32_t uncompressedBlockSize = readBigEndian32(data + 0x28);

    if (uncompressedSize > MAX_UNCOMPRESSED_SIZE)
        throw std::runtime_error(fmt::format("The Xbox 360 compressed file claims to hold {} bytes.", uncompressedSize));

    if (uncompressedBlockSize == 0 || partitionSize == 0)
        throw std::runtime_error("The header of the Xbox 360 compressed file is invalid.");

    // The window size is stored as a power of two, while LZX wants its index.
    if (windowSize < (1u << 15) || windowSize > (1u << 21) || (windowSize & (windowSize - 1)) != 0)
        throw std::runtime_error(fmt::format("The Xbox 360 compressed file uses an unsupported window size of {} bytes.", windowSize));

    uint32_t windowBits = 0;
    for (uint32_t value = windowSize; (value & 1) == 0; value >>= 1)
        windowBits++;

    Result result;
    result.data.resize(size_t(uncompressedSize));

    WriteStream output;
    output.data = result.data.data();
    output.size = result.data.size();

    const uint8_t* source = data + XCOMPRESS_HEADER_SIZE;
    size_t remaining = size - XCOMPRESS_HEADER_SIZE;

    while (remaining >= sizeof(uint32_t) && output.position < output.size)
    {
        const uint32_t compressedBlockSize = readBigEndian32(source);
        source += sizeof(uint32_t);
        remaining -= sizeof(uint32_t);

        if (compressedBlockSize > remaining)
        {
            // The file is cut off. Everything that was decoded so far is still usable, so the
            // blocks before the cut are kept instead of dropping the whole file.
            if (result.blockCount == 0)
            {
                throw std::runtime_error(fmt::format(
                    "The Xbox 360 compressed file ends in the middle of block {} ({} of {} bytes available).",
                    result.blockCount, remaining, compressedBlockSize));
            }

            break;
        }

        const size_t uncompressedBlockBytes = std::min<size_t>(uncompressedBlockSize, output.size - output.position);

        ReadStream input;
        input.data = source;
        input.size = compressedBlockSize;

        lzxd_stream* lzx = lzxd_init(&g_lzxSystem, reinterpret_cast<mspack_file*>(&input),
            reinterpret_cast<mspack_file*>(&output), int(windowBits), 0, int(partitionSize),
            off_t(uncompressedBlockBytes), 0);

        if (lzx == nullptr)
            throw std::runtime_error("The LZX decoder could not be initialized for the Xbox 360 compressed file.");

        const int status = lzxd_decompress(lzx, off_t(uncompressedBlockBytes));
        lzxd_free(lzx);

        if (status != MSPACK_ERR_OK)
        {
            // Keep the blocks that were decoded correctly, the caller reports the file as
            // incomplete.
            if (result.blockCount != 0)
                break;

            throw std::runtime_error(fmt::format("Block {} of the Xbox 360 compressed file is invalid: {}.",
                result.blockCount, describeError(status)));
        }

        if (output.dropped != 0)
            throw std::runtime_error("The Xbox 360 compressed file produced more data than it claims to hold.");

        source += compressedBlockSize;
        remaining -= compressedBlockSize;

        result.blockCount++;
    }

    result.complete = output.position == output.size;
    result.data.resize(output.position);
    return result;
}
