#pragma once

#include <stdexcept>

#include "shader.h"
#include "shader_code.h"

// Thrown when a shader uses something the recompiler cannot translate.
// Callers are expected to catch this and report the shader as failed instead
// of terminating the whole recompilation process.
struct ShaderRecompileError : std::runtime_error
{
    using std::runtime_error::runtime_error;
};

// The Xbox 360 addresses boolean constants with an 8 bit register index, but
// only 128 of them are usable by each shader stage. Recompilation runtimes pack
// the boolean constants of both stages into a single 32 bit value, where bits
// 0-15 hold the vertex shader booleans and bits 16-31 the pixel shader ones.
//
// The register index in a shader instruction is not necessarily the same as the
// index stored in the constant table reflection data, so all boolean register
// indices are normalized to a packed bit index here. This is what makes
// shaders which jump on registers such as b128/b129 (pixel shader booleans) work
// instead of producing references to undeclared identifiers.
constexpr uint32_t BOOLEAN_REGISTERS_PER_STAGE = 128;
constexpr uint32_t PACKED_BOOLEAN_BITS = 32;

constexpr uint32_t getPackedBooleanIndex(uint32_t boolAddress, bool isPixelShader)
{
    return (boolAddress % BOOLEAN_REGISTERS_PER_STAGE) + (isPixelShader ? 16u : 0u);
}

struct StringBuffer
{
    std::string out;

    template<class... Args>
    void print(fmt::format_string<Args...> fmt, Args&&... args)
    {
        fmt::vformat_to(std::back_inserter(out), fmt.get(), fmt::make_format_args(args...));
    }

    template<class... Args>
    void println(fmt::format_string<Args...> fmt, Args&&... args)
    {
        fmt::vformat_to(std::back_inserter(out), fmt.get(), fmt::make_format_args(args...));
        out += '\n';
    }
};

struct ShaderRecompiler : StringBuffer
{
    uint32_t indentation = 0;
    bool isPixelShader = false;
    const uint8_t* constantTableData = nullptr;
    std::unordered_map<uint32_t, VertexElement> vertexElements;
    std::unordered_map<uint32_t, std::string> interpolators;
    std::unordered_map<uint32_t, const ConstantInfo*> float4Constants;
    std::unordered_map<uint32_t, std::string> boolConstants; // keyed by packed boolean bit index
    std::unordered_map<uint32_t, const char*> samplers;
    std::unordered_map<uint32_t, uint32_t> ifEndLabels;
    std::vector<std::string> warnings;
    uint32_t specConstantsMask = 0;

#ifdef UNLEASHED_RECOMP
    bool hasMtxProjection = false;
    bool hasMtxPrevInvViewProjection = false;
#endif

    void indent()
    {
        for (uint32_t i = 0; i < indentation; i++)
            out += '\t';
    }

    uint32_t printDstSwizzle(uint32_t dstSwizzle, bool operand);
    void printDstSwizzle01(uint32_t dstRegister, uint32_t dstSwizzle);

    void recompile(const VertexFetchInstruction& instr, uint32_t address);
    void recompile(const TextureFetchInstruction& instr, bool bicubic);
    void recompile(const AluInstruction& instr);

    // 'shaderDataSize' is the number of bytes that are readable from 'shaderData'. Every offset
    // stored in the shader container is validated against it before it is dereferenced.
    void recompile(const uint8_t* shaderData, size_t shaderDataSize, const std::string_view& include);
};
