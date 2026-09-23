#include "shader_recompiler.h"
#include "shader_common.h"

static constexpr char SWIZZLES[] = 
{ 
    'x',
    'y', 
    'z', 
    'w', 
    '0', 
    '1',
    '_',
    '_'
};

static constexpr const char* USAGE_TYPES[] =
{
    "float4", // POSITION
    "float4", // BLENDWEIGHT
    "uint4", // BLENDINDICES
    "float4", // NORMAL
    "float4", // PSIZE
    "float4", // TEXCOORD
    "float4", // TANGENT
    "float4", // BINORMAL
    "float4", // TESSFACTOR
    "float4", // POSITIONT
    "float4", // COLOR
    "float4", // FOG
    "float4", // DEPTH
    "float4", // SAMPLE

    // Usages 14 and 15 do not exist, but the field is four bits wide. Shaders that use them are
    // rejected when the name of the usage is looked up.
    "float4",
    "float4",
};

static constexpr const char* USAGE_VARIABLES[] =
{
    "Position",
    "BlendWeight",
    "BlendIndices",
    "Normal",
    "PointSize",
    "TexCoord",
    "Tangent",
    "Binormal",
    "TessFactor",
    "PositionT",
    "Color",
    "Fog",
    "Depth",
    "Sample"
};

static constexpr const char* USAGE_SEMANTICS[] =
{
    "POSITION",
    "BLENDWEIGHT",
    "BLENDINDICES",
    "NORMAL",
    "PSIZE",
    "TEXCOORD",
    "TANGENT",
    "BINORMAL",
    "TESSFACTOR",
    "POSITIONT",
    "COLOR",
    "FOG",
    "DEPTH",
    "SAMPLE"
};

struct DeclUsageLocation
{
    DeclUsage usage;
    uint32_t usageIndex;
    uint32_t location;
};

// The Vulkan input locations of the vertex elements. The runtime that binds the vertex buffers
// of the game uses the same numbers, so both sides have to agree on this table: a shader that
// declares [[vk::location(N)]] for an element is fed by the input at location N, and a mismatch
// binds the wrong data without failing anywhere.
//
// The numbering follows the order in which a vertex declaration lists its elements:
// POSITION0, NORMAL0, TANGENT0, BINORMAL0, TEXCOORD0..3, COLOR0, BLENDINDICES0, BLENDWEIGHT0,
// COLOR1 and then TEXCOORD4..23 at 12..31. This matches the locations of hedge-dev/XenosRecomp
// and of the runtimes that were built against it (UnleashedRecomp and the XenonRecomp ports,
// whose vertex declaration code carries the same list).
//
// NOTE: Change as necessary, but the runtime has to be changed together with it. An earlier
// revision of this fork packed the elements (TEXCOORD0..3 at 13..16), which no runtime uses.
static constexpr DeclUsageLocation USAGE_LOCATIONS[] =
{
    { DeclUsage::Position, 0, 0 },
    { DeclUsage::Normal, 0, 1 },
    { DeclUsage::Tangent, 0, 2 },
    { DeclUsage::Binormal, 0, 3 },
    { DeclUsage::TexCoord, 0, 4 },
    { DeclUsage::TexCoord, 1, 5 },
    { DeclUsage::TexCoord, 2, 6 },
    { DeclUsage::TexCoord, 3, 7 },
    { DeclUsage::Color, 0, 8 },
    { DeclUsage::BlendIndices, 0, 9 },
    { DeclUsage::BlendWeight, 0, 10 },
    { DeclUsage::Color, 1, 11 },
    { DeclUsage::TexCoord, 4, 12 },
    { DeclUsage::TexCoord, 5, 13 },
    { DeclUsage::TexCoord, 6, 14 },
    { DeclUsage::TexCoord, 7, 15 },
    // Kept for compatibility with hedge-dev/XenosRecomp, where POSITION1 shares the location of
    // TEXCOORD7, although no runtime asks for a second position that way.
    { DeclUsage::Position, 1, 15 },
    // TEXCOORD8..23 continue at 16..31, which is the last range the runtimes know about.
    { DeclUsage::TexCoord, 8, 16 },
    { DeclUsage::TexCoord, 9, 17 },
    { DeclUsage::TexCoord, 10, 18 },
    { DeclUsage::TexCoord, 11, 19 },
    { DeclUsage::TexCoord, 12, 20 },
    { DeclUsage::TexCoord, 13, 21 },
    { DeclUsage::TexCoord, 14, 22 },
    { DeclUsage::TexCoord, 15, 23 },
    { DeclUsage::TexCoord, 16, 24 },
    { DeclUsage::TexCoord, 17, 25 },
    { DeclUsage::TexCoord, 18, 26 },
    { DeclUsage::TexCoord, 19, 27 },
    { DeclUsage::TexCoord, 20, 28 },
    { DeclUsage::TexCoord, 21, 29 },
    { DeclUsage::TexCoord, 22, 30 },
    { DeclUsage::TexCoord, 23, 31 },
};

static constexpr std::pair<DeclUsage, size_t> INTERPOLATORS[] =
{
    { DeclUsage::TexCoord, 0 },
    { DeclUsage::TexCoord, 1 },
    { DeclUsage::TexCoord, 2 },
    { DeclUsage::TexCoord, 3 },
    { DeclUsage::TexCoord, 4 },
    { DeclUsage::TexCoord, 5 },
    { DeclUsage::TexCoord, 6 },
    { DeclUsage::TexCoord, 7 },
    { DeclUsage::TexCoord, 8 },
    { DeclUsage::TexCoord, 9 },
    { DeclUsage::TexCoord, 10 },
    { DeclUsage::TexCoord, 11 },
    { DeclUsage::TexCoord, 12 },
    { DeclUsage::TexCoord, 13 },
    { DeclUsage::TexCoord, 14 },
    { DeclUsage::TexCoord, 15 },
    { DeclUsage::Color, 0 },
    { DeclUsage::Color, 1 }
};

static constexpr std::string_view TEXTURE_DIMENSIONS[] = 
{
    "2D",
    "2DArray",
    "Cube" 
};

// The usage fields of vertex elements and interpolators are stored as four bit values, while the
// tables below only describe the usages that are actually understood. Everything else is rejected
// instead of reading past the end of the tables.
static const char* usageVariable(DeclUsage usage)
{
    if (uint32_t(usage) >= std::size(USAGE_VARIABLES))
        throw ShaderRecompileError(fmt::format("Shader uses an unknown vertex or interpolator usage {}.", uint32_t(usage)));

    return USAGE_VARIABLES[uint32_t(usage)];
}

static const char* usageSemantic(DeclUsage usage)
{
    if (uint32_t(usage) >= std::size(USAGE_SEMANTICS))
        throw ShaderRecompileError(fmt::format("Shader uses an unknown vertex or interpolator usage {}.", uint32_t(usage)));

    return USAGE_SEMANTICS[uint32_t(usage)];
}

static FetchDestinationSwizzle getDestSwizzle(uint32_t dstSwizzle, uint32_t index)
{
    return FetchDestinationSwizzle((dstSwizzle >> (index * 3)) & 0x7);
}

uint32_t ShaderRecompiler::printDstSwizzle(uint32_t dstSwizzle, bool operand)
{
    uint32_t size = 0;

    for (size_t i = 0; i < 4; i++)
    {
        const auto swizzle = getDestSwizzle(dstSwizzle, i);
        if (swizzle >= FetchDestinationSwizzle::X && swizzle <= FetchDestinationSwizzle::W)
        {
            out += SWIZZLES[operand ? uint32_t(swizzle) : i];
            size++;
        }
    }

    return size;
}

void ShaderRecompiler::printDstSwizzle01(uint32_t dstRegister, uint32_t dstSwizzle)
{
    for (size_t i = 0; i < 4; i++)
    {
        const auto swizzle = getDestSwizzle(dstSwizzle, i);
        if (swizzle == FetchDestinationSwizzle::Zero)
        {
            indent();
            println("r{}.{} = 0.0;", dstRegister, SWIZZLES[i]);
        }
        else if (swizzle == FetchDestinationSwizzle::One)
        {
            indent();
            println("r{}.{} = 1.0;", dstRegister, SWIZZLES[i]);
        }
    }
}

void ShaderRecompiler::recompile(const VertexFetchInstruction& instr, uint32_t address)
{
    if (instr.isPredicated)
    {
        indent();
        println("if ({}p0)", instr.predicateCondition ? "" : "!");

        indent();
        out += "{\n";
        ++indentation;
    }

    indent();
    print("r{}.", instr.dstRegister);
    uint32_t size = printDstSwizzle(instr.dstSwizzle, false);

    out += " = ";

    if (size <= 1)
        out += "(float)(";
    else
        print("(float{})(", size);

    auto findResult = vertexElements.find(address);
    if (findResult == vertexElements.end())
    {
        throw ShaderRecompileError(fmt::format(
            "Vertex fetch at address {} refers to a vertex element that is not part of the vertex declaration.",
            address));
    }

    switch (findResult->second.usage)
    {
    case DeclUsage::Normal:
        print("swapFloats(g_SwappedNormals, ");
        break;
    case DeclUsage::Tangent:
        print("swapFloats(g_SwappedTangents, ");
        break;
    case DeclUsage::Binormal:
        print("swapFloats(g_SwappedBinormals, ");
        break;
    case DeclUsage::BlendWeight:
        print("swapFloats(g_SwappedBlendWeights, ");
        break;
    case DeclUsage::TexCoord:
        print("swapFloats(g_SwappedTexcoords, ");
        break;
    }

    print("(input.i{}{})", usageVariable(findResult->second.usage), uint32_t(findResult->second.usageIndex));

    switch (findResult->second.usage)
    {
    case DeclUsage::Normal:
    case DeclUsage::Tangent:
    case DeclUsage::Binormal:
    case DeclUsage::BlendWeight:
    case DeclUsage::TexCoord:
        print(", {})", uint32_t(findResult->second.usageIndex));
        break;
    }

    out += ").";
    printDstSwizzle(instr.dstSwizzle, true);

    out += ";\n";

    printDstSwizzle01(instr.dstRegister, instr.dstSwizzle);

    if (instr.isPredicated)
    {
        --indentation;
        indent();
        out += "}\n";
    }
}

void ShaderRecompiler::recompile(const TextureFetchInstruction& instr, bool bicubic)
{
    if (instr.opcode != FetchOpcode::TextureFetch && instr.opcode != FetchOpcode::GetTextureWeights)
        return;

    if (instr.isPredicated)
    {
        indent();
        println("if ({}p0)", instr.predCondition ? "" : "!");

        indent();
        out += "{\n";
        ++indentation;
    }

    auto printSrcRegister = [&](size_t componentCount)
        {
            print("r{}.", instr.srcRegister);

            for (size_t i = 0; i < componentCount; i++)
                out += SWIZZLES[((instr.srcSwizzle >> (i * 2))) & 0x3];
        };

    std::string constName;
    const char* constNamePtr = nullptr;
#ifdef UNLEASHED_RECOMP
    bool subtractFromOne = false;
#endif

    auto findResult = samplers.find(instr.constIndex);
    if (findResult != samplers.end())
    {
        constNamePtr = findResult->second;

    #ifdef UNLEASHED_RECOMP
        subtractFromOne = hasMtxPrevInvViewProjection && strcmp(constNamePtr, "sampZBuffer") == 0;
    #endif
    }
    else
    {
        constName = fmt::format("s{}", instr.constIndex);
        constNamePtr = constName.c_str();
    }

#ifdef UNLEASHED_RECOMP
    if (instr.constIndex == 0 && instr.dimension == TextureDimension::Texture2D)
    {
        indent();
        println("pixelCoord = getPixelCoord(");
        println("#ifdef __air__");
        indent();
        println("g_Texture2DDescriptorHeap,");
        println("#endif");
        indent();
        print("{}_Texture2DDescriptorIndex, ", constNamePtr);
        printSrcRegister(2);
        out += ");\n";
    }
#endif

    indent();
    print("r{}.", instr.dstRegister);
    printDstSwizzle(instr.dstSwizzle, false);

    out += " = ";
    switch (instr.opcode)
    {
    case FetchOpcode::TextureFetch:
    {
    #ifdef UNLEASHED_RECOMP
        if (subtractFromOne)
            out += "1.0 - ";
    #endif

        out += "tfetch";
        break;
    }
    case FetchOpcode::GetTextureWeights:
    {
        out += "getWeights";
        break;
    }
    }

    std::string_view dimension;
    uint32_t componentCount = 0;

    switch (instr.dimension)
    {
    case TextureDimension::Texture1D:
        dimension = "1D";
        componentCount = 1;
        break;
    case TextureDimension::Texture2D:
        dimension = "2D";
        componentCount = 2;
        break;
    case TextureDimension::Texture3D:
        dimension = "2DArray";
        componentCount = 3;
        break;
    case TextureDimension::TextureCube:
        dimension = "Cube";
        componentCount = 3;
        break;
    }

    out += dimension;

#ifdef UNLEASHED_RECOMP
    if (bicubic)
        out += "Bicubic";
#endif

    println("(");

    println("#ifdef __air__");
    indent();
    println("\tg_Texture{}DescriptorHeap,", dimension);
    indent();
    println("\tg_SamplerDescriptorHeap,");
    println("#endif");

    indent();
    print("\t{0}_Texture{1}DescriptorIndex, {0}_SamplerDescriptorIndex, ", constNamePtr, dimension);
    printSrcRegister(componentCount);

    switch (instr.dimension)
    {
        case TextureDimension::Texture2D:
            print(", float2({}, {})", instr.offsetX * 0.5f, instr.offsetY * 0.5f);
            break;
        case TextureDimension::Texture3D:
            print(", float3({}, {}, {})", instr.offsetX * 0.5f, instr.offsetY * 0.5f, instr.offsetZ * 0.5f);
            break;
    }

    out += ").";

    printDstSwizzle(instr.dstSwizzle, true);

    out += ";\n";

    printDstSwizzle01(instr.dstRegister, instr.dstSwizzle);

    if (instr.isPredicated)
    {
        --indentation;
        indent();
        out += "}\n";
    }
}

void ShaderRecompiler::recompile(const AluInstruction& instr)
{
    if (instr.isPredicated)
    {
        indent();
        println("if ({}p0)", instr.predicateCondition ? "" : "!");

        indent(); 
        out += "{\n";
        ++indentation;
    }

    enum
    {
        VECTOR_0,
        VECTOR_1,
        VECTOR_2,
        SCALAR_0,
        SCALAR_1,
        SCALAR_CONSTANT_0,
        SCALAR_CONSTANT_1
    };

    struct OperationResult
    {
        std::string expression;
        size_t componentCount;
    };

    auto op = [&](size_t operand)
        {
            size_t reg = 0;
            size_t swizzle = 0;
            bool select = true;
            bool negate = false;
            bool abs = false;

            switch (operand)
            {
            case SCALAR_CONSTANT_0:
                reg = instr.src3Register;
                swizzle = instr.src3Swizzle;
                select = false;
                negate = instr.src3Negate;
                abs = instr.absConstants;
                break;

            case SCALAR_CONSTANT_1:
                reg = (uint32_t(instr.scalarOpcode) & 1) | (instr.src3Select << 1) | (instr.src3Swizzle & 0x3C);
                swizzle = instr.src3Swizzle;
                select = true;
                negate = instr.src3Negate;
                abs = instr.absConstants;
                break;

            default:
                switch (operand)
                {
                case VECTOR_0:
                    reg = instr.src1Register;
                    swizzle = instr.src1Swizzle;
                    select = instr.src1Select;
                    negate = instr.src1Negate;
                    break;
                case VECTOR_1:
                    reg = instr.src2Register;
                    swizzle = instr.src2Swizzle;
                    select = instr.src2Select;
                    negate = instr.src2Negate;
                    break;
                case VECTOR_2:
                case SCALAR_0:
                case SCALAR_1:
                    reg = instr.src3Register;
                    swizzle = instr.src3Swizzle;
                    select = instr.src3Select;
                    negate = instr.src3Negate;
                    break;
                }

                if (select)
                {
                    abs = (reg & 0x80) != 0;
                    reg &= 0x3F;
                }
                else
                {
                    abs = instr.absConstants;
                }

                break;
            }

            std::string regFormatted;

            if (select)
            {
                regFormatted = fmt::format("r{}", reg);
            }
            else
            {
                auto findResult = float4Constants.find(reg);
                if (findResult != float4Constants.end())
                {
                    const char* constantName = reinterpret_cast<const char*>(constantTableData + findResult->second->name);
                    if (findResult->second->registerCount > 1)
                    {
                    #ifdef UNLEASHED_RECOMP
                        if (hasMtxProjection && strcmp(constantName, "g_MtxProjection") == 0)
                        {
                            regFormatted = fmt::format("(iterationIndex == 0 ? mtxProjectionReverseZ[{0}] : mtxProjection[{0}])",
                                reg - findResult->second->registerIndex);
                        }
                        else
                    #endif
                        {
                            regFormatted = fmt::format("{}({}{})", constantName,
                                reg - findResult->second->registerIndex, instr.const0Relative ? (instr.constAddressRegisterRelative ? " + a0" : " + aL") : "");
                        }
                    }
                    else
                    {
                        if (instr.const0Relative || instr.const1Relative)
                        {
                            throw ShaderRecompileError(fmt::format(
                                "Dynamic constant indexing on '{}' (c{}), which is not declared as an array, is not supported.",
                                constantName, reg));
                        }

                        regFormatted = constantName;
                    }
                }
                else
                {
                    if (instr.const0Relative || instr.const1Relative)
                    {
                        throw ShaderRecompileError(fmt::format(
                            "Dynamic constant indexing on c{}, which is not present in the reflection data, is not supported.", reg));
                    }

                    regFormatted = fmt::format("c{}", reg);
                }
            }

            OperationResult opResult {};

            if (negate)
                opResult.expression += '-';

            if (abs)
                opResult.expression += "abs(";

            opResult.expression += regFormatted;
            opResult.expression += '.';

            switch (operand)
            {
            case VECTOR_0:
            case VECTOR_1:
            case VECTOR_2:
            {
                uint32_t mask;

                switch (instr.vectorOpcode)
                {
                case AluVectorOpcode::Dp2Add:
                    mask = (operand == VECTOR_2) ? 0b1 : 0b11;
                    break;

                case AluVectorOpcode::Dp3:
                    mask = 0b111;
                    break;

                case AluVectorOpcode::Dp4:
                case AluVectorOpcode::Max4:
                    mask = 0b1111;
                    break;

                default:
                    mask = instr.vectorWriteMask != 0 ? instr.vectorWriteMask : 0b1;
                    break;
                }

                for (size_t i = 0; i < 4; i++)
                {
                    if ((mask >> i) & 0x1) {
                        opResult.componentCount++;
                        opResult.expression += SWIZZLES[((swizzle >> (i * 2)) + i) & 0x3];
                    }
                }

                break;
            }

            case SCALAR_0:
            case SCALAR_CONSTANT_0:
                opResult.componentCount = 1;
                opResult.expression += SWIZZLES[((swizzle >> 6) + 3) & 0x3];
                break;

            case SCALAR_1:
            case SCALAR_CONSTANT_1:
                opResult.componentCount = 1;
                opResult.expression += SWIZZLES[swizzle & 0x3];
                break;
            }

            if (abs)
                opResult.expression += ")";

            return opResult;
        };

    switch (instr.vectorOpcode)
    {
    case AluVectorOpcode::KillEq:
        indent();
        println("clip(any({} == {}) ? -1 : 1);", op(VECTOR_0).expression, op(VECTOR_1).expression);
        break;
    
    case AluVectorOpcode::KillGt:
        indent();
        println("clip(any({} > {}) ? -1 : 1);", op(VECTOR_0).expression, op(VECTOR_1).expression);
        break;
    
    case AluVectorOpcode::KillGe:
        indent();
        println("clip(any({} >= {}) ? -1 : 1);", op(VECTOR_0).expression, op(VECTOR_1).expression);
        break;
    
    case AluVectorOpcode::KillNe:
        indent();
        println("clip(any({} != {}) ? -1 : 1);", op(VECTOR_0).expression, op(VECTOR_1).expression);
        break;
    }

    bool closeIfBracket = false;

    std::string_view exportRegister;
    bool vectorRegister = true;

    if (instr.exportData)
    {
        if (isPixelShader)
        {
            switch (ExportRegister(instr.vectorDest))
            {
            case ExportRegister::PSColor0:
                exportRegister = "output.oC0";
                break;        
            case ExportRegister::PSColor1:
                exportRegister = "output.oC1";
                break;        
            case ExportRegister::PSColor2:
                exportRegister = "output.oC2";
                break;            
            case ExportRegister::PSColor3:
                exportRegister = "output.oC3";
                break;           
            case ExportRegister::PSDepth:
                exportRegister = "output.oDepth";
                vectorRegister = false;
                break;
            }
        }
        else
        {
            switch (ExportRegister(instr.vectorDest))
            {
            case ExportRegister::VSPosition:
                exportRegister = "output.oPos";

            #ifdef UNLEASHED_RECOMP
                if (hasMtxProjection)
                {
                    indent();
                    out += "if ((g_SpecConstants() & SPEC_CONSTANT_REVERSE_Z) == 0 || iterationIndex == 0)\n";
                    indent();
                    out += "{\n";
                    ++indentation;

                    closeIfBracket = true;
                }
            #endif

                break;

            default:
            {
                auto findResult = interpolators.find(instr.vectorDest);
                if (findResult == interpolators.end())
                {
                    throw ShaderRecompileError(fmt::format(
                        "Export of register r{} has no matching vertex shader output interpolator.",
                        uint32_t(instr.vectorDest)));
                }

                exportRegister = findResult->second;
                break;
            }
            }
        }
    }

    if (instr.vectorOpcode >= AluVectorOpcode::SetpEqPush && instr.vectorOpcode <= AluVectorOpcode::SetpGePush)
    {
        indent();
        print("p0 = {} == 0.0 && {} ", op(VECTOR_0).expression, op(VECTOR_1).expression);

        switch (instr.vectorOpcode)
        {
        case AluVectorOpcode::SetpEqPush:
            out += "==";
            break;
        case AluVectorOpcode::SetpNePush:
            out += "!=";
            break;
        case AluVectorOpcode::SetpGtPush:
            out += ">";
            break;
        case AluVectorOpcode::SetpGePush:
            out += ">=";
            break;
        }

        out += " 0.0;\n";
    }
    else if (instr.vectorOpcode >= AluVectorOpcode::MaxA)
    {
        indent();
        println("a0 = (int)clamp(floor(({}).w + 0.5), -256.0, 255.0);", op(VECTOR_0).expression);
    }

    uint32_t vectorWriteMask = instr.vectorWriteMask;
    if (instr.exportData)
        vectorWriteMask &= ~instr.scalarWriteMask;

    if (vectorWriteMask != 0)
    {
        indent();
        if (!exportRegister.empty())
        {
            out += exportRegister;
            if (vectorRegister)
                out += '.';
        }
        else
        {
            print("r{}.", instr.vectorDest);
        }

        uint32_t vectorWriteSize = 0;

        for (size_t i = 0; i < 4; i++)
        {
            if ((vectorWriteMask >> i) & 0x1)
            {
                if (vectorRegister)
                    out += SWIZZLES[i];
                vectorWriteSize++;
            }
        }

        out += " = ";

        if (vectorWriteSize > 1)
            print("(float{})((", vectorWriteSize);
        else
            out += "(float)((";

        if (instr.vectorSaturate)
            out += "saturate(";

        size_t operationResultComponentCount;

        switch (instr.vectorOpcode)
        {
        case AluVectorOpcode::Add:
            {
                auto v0 = op(VECTOR_0);
                auto v1 = op(VECTOR_1);
                operationResultComponentCount = std::max(v0.componentCount, v1.componentCount);

                print("{} + {}", v0.expression, v1.expression);
                break;
            }

        case AluVectorOpcode::Mul:
            {
                auto v0 = op(VECTOR_0);
                auto v1 = op(VECTOR_1);
                operationResultComponentCount = std::max(v0.componentCount, v1.componentCount);

                print("{} * {}", v0.expression, v1.expression);
                break;
            }

        case AluVectorOpcode::Max:
        case AluVectorOpcode::MaxA:
            {
                auto v0 = op(VECTOR_0);
                auto v1 = op(VECTOR_1);
                operationResultComponentCount = std::max(v0.componentCount, v1.componentCount);

                print("max({}, {})", v0.expression, v1.expression);
                break;
            }

        case AluVectorOpcode::Min:
            {
                auto v0 = op(VECTOR_0);
                auto v1 = op(VECTOR_1);
                operationResultComponentCount = std::max(v0.componentCount, v1.componentCount);

                print("min({}, {})", v0.expression, v1.expression);
                break;
            }

        case AluVectorOpcode::Seq:
            {
                auto v0 = op(VECTOR_0);
                auto v1 = op(VECTOR_1);
                operationResultComponentCount = std::max(v0.componentCount, v1.componentCount);

                print("{} == {}", v0.expression, v1.expression);
                break;
            }

        case AluVectorOpcode::Sgt:
            {
                auto v0 = op(VECTOR_0);
                auto v1 = op(VECTOR_1);
                operationResultComponentCount = std::max(v0.componentCount, v1.componentCount);

                print("{} > {}", v0.expression, v1.expression);
                break;
            }

        case AluVectorOpcode::Sge:
            {
                auto v0 = op(VECTOR_0);
                auto v1 = op(VECTOR_1);
                operationResultComponentCount = std::max(v0.componentCount, v1.componentCount);

                print("{} >= {}", v0.expression, v1.expression);
                break;
            }

        case AluVectorOpcode::Sne:
            {
                auto v0 = op(VECTOR_0);
                auto v1 = op(VECTOR_1);
                operationResultComponentCount = std::max(v0.componentCount, v1.componentCount);

                print("{} != {}", v0.expression, v1.expression);
                break;
            }

        case AluVectorOpcode::Frc:
            {
                auto v0 = op(VECTOR_0);
                operationResultComponentCount = v0.componentCount;

                print("frac({})", v0.expression);
                break;
            }

        case AluVectorOpcode::Trunc:
            {
                auto v0 = op(VECTOR_0);
                operationResultComponentCount = v0.componentCount;

                print("trunc({})", v0.expression);
                break;
            }

        case AluVectorOpcode::Floor:
            {
                auto v0 = op(VECTOR_0);
                operationResultComponentCount = v0.componentCount;

                print("floor({})", v0.expression);
                break;
            }

        case AluVectorOpcode::Mad:
            {
                auto v0 = op(VECTOR_0);
                auto v1 = op(VECTOR_1);
                auto v2 = op(VECTOR_2);
                operationResultComponentCount = std::max(std::max(v0.componentCount, v1.componentCount), v2.componentCount);

                print("{} * {} + {}", v0.expression, v1.expression, v2.expression);
                break;
            }

        case AluVectorOpcode::CndEq:
            {
                auto v0 = op(VECTOR_0);
                auto v1 = op(VECTOR_1);
                auto v2 = op(VECTOR_2);
                operationResultComponentCount = std::max(v1.componentCount, v2.componentCount);

                print("selectWrapper({} == 0.0, {}, {})", v0.expression, v1.expression, v2.expression);
                break;
            }

        case AluVectorOpcode::CndGe:
            {
                auto v0 = op(VECTOR_0);
                auto v1 = op(VECTOR_1);
                auto v2 = op(VECTOR_2);
                operationResultComponentCount = std::max(v1.componentCount, v2.componentCount);

                print("selectWrapper({} >= 0.0, {}, {})", v0.expression, v1.expression, v2.expression);
                break;
            }

        case AluVectorOpcode::CndGt:
            {
                auto v0 = op(VECTOR_0);
                auto v1 = op(VECTOR_1);
                auto v2 = op(VECTOR_2);
                operationResultComponentCount = std::max(v1.componentCount, v2.componentCount);

                print("selectWrapper({} > 0.0, {}, {})", v0.expression, v1.expression, v2.expression);
                break;
            }

        case AluVectorOpcode::Dp4:
        case AluVectorOpcode::Dp3:
            operationResultComponentCount = 1;
            print("dot({}, {})", op(VECTOR_0).expression, op(VECTOR_1).expression);
            break;

        case AluVectorOpcode::Dp2Add:
            {
                auto v2 = op(VECTOR_2);
                operationResultComponentCount = v2.componentCount;

                print("dot({}, {}) + {}", op(VECTOR_0).expression, op(VECTOR_1).expression, v2.expression);
                break;
            }

        case AluVectorOpcode::Cube:
            operationResultComponentCount = 4;
            print("cube({})", op(VECTOR_0).expression);
            break;

        case AluVectorOpcode::Max4:
            operationResultComponentCount = 4;
            print("max4({})", op(VECTOR_0).expression);
            break;

        case AluVectorOpcode::SetpEqPush:
        case AluVectorOpcode::SetpNePush:
        case AluVectorOpcode::SetpGtPush:
        case AluVectorOpcode::SetpGePush:
            {
                auto v0 = op(VECTOR_0);
                operationResultComponentCount = v0.componentCount;

                print("p0 ? 0.0 : {} + 1.0", v0.expression);
                break;
            }

        case AluVectorOpcode::KillEq:
            operationResultComponentCount = 1;
            print("any({} == {})", op(VECTOR_0).expression, op(VECTOR_1).expression);
            break;

        case AluVectorOpcode::KillGt:
            operationResultComponentCount = 1;
            print("any({} > {})", op(VECTOR_0).expression, op(VECTOR_1).expression);
            break;

        case AluVectorOpcode::KillGe:
            operationResultComponentCount = 1;
            print("any({} >= {})", op(VECTOR_0).expression, op(VECTOR_1).expression);
            break;

        case AluVectorOpcode::KillNe:
            operationResultComponentCount = 1;
            print("any({} != {})", op(VECTOR_0).expression, op(VECTOR_1).expression);
            break;

        case AluVectorOpcode::Dst:
            operationResultComponentCount = 4;
            print("dst({}, {})", op(VECTOR_0).expression, op(VECTOR_1).expression);
            break;
        }

		out += ")";

        if (operationResultComponentCount > vectorWriteSize) {
            if (vectorWriteSize == 1) {
                out += ".x";
            } else if (vectorWriteSize == 2) {
                out += ".xy";
            } else if (vectorWriteSize == 3) {
                out += ".xyz";
            }
        }

        out += ")";

        if (instr.vectorSaturate)
            out += ')';

        out += ";\n";
    }

    if (instr.scalarOpcode != AluScalarOpcode::RetainPrev)
    {
        if (instr.scalarOpcode >= AluScalarOpcode::SetpEq && instr.scalarOpcode <= AluScalarOpcode::SetpRstr)
        {
            indent();
            out += "p0 = ";

            switch (instr.scalarOpcode)
            {
            case AluScalarOpcode::SetpEq:
                print("{} == 0.0", op(SCALAR_0).expression);
                break;

            case AluScalarOpcode::SetpNe:
                print("{} != 0.0", op(SCALAR_0).expression);
                break;

            case AluScalarOpcode::SetpGt:
                print("{} > 0.0", op(SCALAR_0).expression);
                break;

            case AluScalarOpcode::SetpGe:
                print("{} >= 0.0", op(SCALAR_0).expression);
                break;

            case AluScalarOpcode::SetpInv:
                print("{} == 1.0", op(SCALAR_0).expression);
                break;

            case AluScalarOpcode::SetpPop:
                print("{} - 1.0 <= 0.0", op(SCALAR_0).expression);
                break;

            case AluScalarOpcode::SetpClr:
                out += "false";
                break;

            case AluScalarOpcode::SetpRstr:
                print("{} == 0.0", op(SCALAR_0).expression);
                break;
            }

            out += ";\n";
        }

        indent();
        out += "ps = ";
        if (instr.scalarSaturate)
            out += "saturate((float)(";

        switch (instr.scalarOpcode)
        {
        case AluScalarOpcode::Adds:
            print("{} + {}", op(SCALAR_0).expression, op(SCALAR_1).expression);
            break;

        case AluScalarOpcode::AddsPrev:
            print("{} + ps", op(SCALAR_0).expression);
            break;

        case AluScalarOpcode::Muls:
            print("{} * {}", op(SCALAR_0).expression, op(SCALAR_1).expression);
            break;

        case AluScalarOpcode::MulsPrev:
        case AluScalarOpcode::MulsPrev2:
            print("{} * ps", op(SCALAR_0).expression);
            break;

        case AluScalarOpcode::Maxs:
        case AluScalarOpcode::MaxAs:
        case AluScalarOpcode::MaxAsf:
            print("max({}, {})", op(SCALAR_0).expression, op(SCALAR_1).expression);
            break;

        case AluScalarOpcode::Mins:
            print("min({}, {})", op(SCALAR_0).expression, op(SCALAR_1).expression);
            break;

        case AluScalarOpcode::Seqs:
            print("{} == 0.0", op(SCALAR_0).expression);
            break;

        case AluScalarOpcode::Sgts:
            print("{} > 0.0", op(SCALAR_0).expression);
            break;

        case AluScalarOpcode::Sges:
            print("{} >= 0.0", op(SCALAR_0).expression);
            break;

        case AluScalarOpcode::Snes:
            print("{} != 0.0", op(SCALAR_0).expression);
            break;

        case AluScalarOpcode::Frcs:
            print("frac({})", op(SCALAR_0).expression);
            break;

        case AluScalarOpcode::Truncs:
            print("trunc({})", op(SCALAR_0).expression);
            break;

        case AluScalarOpcode::Floors:
            print("floor({})", op(SCALAR_0).expression);
            break;

        case AluScalarOpcode::Exp:
            print("exp2({})", op(SCALAR_0).expression);
            break;

        case AluScalarOpcode::Logc:
        case AluScalarOpcode::Log:
            print("clamp(log2({}), -FLT_MAX, FLT_MAX)", op(SCALAR_0).expression);
            break;

        case AluScalarOpcode::Rcpc:
        case AluScalarOpcode::Rcpf:
        case AluScalarOpcode::Rcp:
            print("clamp(rcp({}), -FLT_MAX, FLT_MAX)", op(SCALAR_0).expression);
            break;

        case AluScalarOpcode::Rsqc:
        case AluScalarOpcode::Rsqf:
        case AluScalarOpcode::Rsq:
            print("clamp(rsqrt({}), -FLT_MAX, FLT_MAX)", op(SCALAR_0).expression);
            break;

        case AluScalarOpcode::Subs:
            print("{} - {}", op(SCALAR_0).expression, op(SCALAR_1).expression);
            break;

        case AluScalarOpcode::SubsPrev:
            print("{} - ps", op(SCALAR_0).expression);
            break;

        case AluScalarOpcode::SetpEq:
        case AluScalarOpcode::SetpNe:
        case AluScalarOpcode::SetpGt:
        case AluScalarOpcode::SetpGe:
            out += "p0 ? 0.0 : 1.0";
            break;

        case AluScalarOpcode::SetpInv:
            print("p0 ? 0.0 : {0} == 0.0 ? 1.0 : {0}", op(SCALAR_0).expression);
            break;

        case AluScalarOpcode::SetpPop:
            print("p0 ? 0.0 : ({} - 1.0)", op(SCALAR_0).expression);
            break;

        case AluScalarOpcode::SetpClr:
            out += "FLT_MAX";
            break;

        case AluScalarOpcode::SetpRstr:
            print("p0 ? 0.0 : {}", op(SCALAR_0).expression);
            break;

        case AluScalarOpcode::KillsEq:
            print("{} == 0.0", op(SCALAR_0).expression);
            break;

        case AluScalarOpcode::KillsGt:
            print("{} > 0.0", op(SCALAR_0).expression);
            break;

        case AluScalarOpcode::KillsGe:
            print("{} >= 0.0", op(SCALAR_0).expression);
            break;

        case AluScalarOpcode::KillsNe:
            print("{} != 0.0", op(SCALAR_0).expression);
            break;

        case AluScalarOpcode::KillsOne:
            print("{} == 1.0", op(SCALAR_0).expression);
            break;

        case AluScalarOpcode::Sqrt:
            print("sqrt({})", op(SCALAR_0).expression);
            break;

        case AluScalarOpcode::Mulsc0:
        case AluScalarOpcode::Mulsc1:
            print("{} * {}", op(SCALAR_CONSTANT_0).expression, op(SCALAR_CONSTANT_1).expression);
            break;

        case AluScalarOpcode::Addsc0:
        case AluScalarOpcode::Addsc1:
            print("{} + {}", op(SCALAR_CONSTANT_0).expression, op(SCALAR_CONSTANT_1).expression);
            break;

        case AluScalarOpcode::Subsc0:
        case AluScalarOpcode::Subsc1:
            print("{} - {}", op(SCALAR_CONSTANT_0).expression, op(SCALAR_CONSTANT_1).expression);
            break;

        case AluScalarOpcode::Sin:
            print("sin({})", op(SCALAR_0).expression);
            break;

        case AluScalarOpcode::Cos:
            print("cos({})", op(SCALAR_0).expression);
            break;
        }

        if (instr.scalarSaturate)
            out += "))";

        out += ";\n";

        switch (instr.scalarOpcode)
        {
        case AluScalarOpcode::MaxAs:
            indent();
            println("a0 = (int)clamp(floor({} + 0.5), -256.0, 255.0);", op(SCALAR_0).expression);
            break;     
        case AluScalarOpcode::MaxAsf:
            indent();
            println("a0 = (int)clamp(floor({}), -256.0, 255.0);", op(SCALAR_0).expression);
            break;
        }
    }

    uint32_t scalarWriteMask = instr.scalarWriteMask;
    if (instr.exportData)
        scalarWriteMask &= ~instr.vectorWriteMask;

    if (scalarWriteMask != 0)
    {
        indent();
        if (!exportRegister.empty())
        {
            out += exportRegister;
            if (vectorRegister)
                out += '.';
        }
        else
        {
            print("r{}.", instr.scalarDest);
        }

        for (size_t i = 0; i < 4; i++)
        {
            if (((scalarWriteMask >> i) & 0x1) && vectorRegister)
                out += SWIZZLES[i];
        }

        out += " = ps;\n";
    }

    if (instr.exportData)
    {
        uint32_t zeroMask = instr.scalarDestRelative ? (0b1111 & ~(instr.vectorWriteMask | instr.scalarWriteMask)) : 0;
        uint32_t oneMask = instr.vectorWriteMask & instr.scalarWriteMask;

        for (size_t i = 0; i < 4; i++)
        {
            uint32_t mask = 1 << i;
            if (zeroMask & mask)
            {
                indent();
                println("{}.{} = 0.0;", exportRegister, SWIZZLES[i]);
            }
            else if (oneMask & mask)
            {
                indent();
                println("{}.{} = 1.0;", exportRegister, SWIZZLES[i]);
            }
        }
    }

    if (instr.scalarOpcode >= AluScalarOpcode::KillsEq && instr.scalarOpcode <= AluScalarOpcode::KillsOne)
    {
        indent();
        out += "clip(ps != 0.0 ? -1 : 1);\n";
    }

    if (closeIfBracket)
    {
        --indentation;
        indent();
        out += "}\n";
    }

    if (instr.isPredicated)
    {
        --indentation;
        indent();
        out += "}\n";
    }
}

// Shader containers are untrusted data: every offset stored in them is validated against the size
// of the container before it is dereferenced, so that a corrupted or unsupported shader is reported
// as failed instead of reading out of bounds.
static bool isInBounds(size_t offset, size_t size, size_t total)
{
    return offset <= total && size <= total - offset;
}

static bool isNullTerminatedWithin(const uint8_t* data, size_t offset, size_t size)
{
    if (offset >= size)
        return false;

    for (size_t i = offset; i < size; i++)
    {
        if (data[i] == '\0')
            return true;
    }

    return false;
}

void ShaderRecompiler::recompile(const uint8_t* shaderData, size_t shaderDataSize, const std::string_view& include)
{
    const auto shaderContainer = reinterpret_cast<const ShaderContainer*>(shaderData);

    if ((shaderContainer->flags & 0xFFFFFF00) != 0x102A1100)
    {
        throw ShaderRecompileError(fmt::format(
            "Invalid shader container flags 0x{:X}.", shaderContainer->flags.get()));
    }

    const size_t containerSize = shaderDataSize;
    const uint32_t virtualSize = shaderContainer->virtualSize.get();
    const uint32_t physicalSize = shaderContainer->physicalSize.get();

    if (!isInBounds(virtualSize, physicalSize, containerSize))
    {
        throw ShaderRecompileError("The shader container describes more data than it holds.");
    }

    if (shaderContainer->constantTableOffset == 0)
    {
        throw ShaderRecompileError("Shader container does not contain a constant table.");
    }

    out += include;
    out += '\n';

    isPixelShader = (shaderContainer->flags & 0x1) == 0;

    // Offsets stored inside the constant table are relative to the table itself.
    const size_t constantTableDataOffset = size_t(shaderContainer->constantTableOffset.get()) + sizeof(be<uint32_t>);

    if (!isInBounds(constantTableDataOffset, sizeof(ConstantTable), containerSize))
    {
        throw ShaderRecompileError("The constant table of this shader is out of bounds.");
    }

    const auto constantTableContainer = reinterpret_cast<const ConstantTableContainer*>(shaderData + shaderContainer->constantTableOffset.get());
    constantTableData = reinterpret_cast<const uint8_t*>(&constantTableContainer->constantTable);

    const size_t constantTableSize = containerSize - constantTableDataOffset;
    const uint32_t numConstants = constantTableContainer->constantTable.constants.get();
    const size_t constantInfoOffset = constantTableContainer->constantTable.constantInfo.get();

    if (!isInBounds(constantInfoOffset, size_t(numConstants) * sizeof(ConstantInfo), constantTableSize))
    {
        throw ShaderRecompileError("The constant table of this shader is out of bounds.");
    }

    for (uint32_t i = 0; i < numConstants; i++)
    {
        const auto constantInfo = reinterpret_cast<const ConstantInfo*>(constantTableData + constantInfoOffset + i * sizeof(ConstantInfo));

        // Constant names are read as C strings in several places, so they have to be null
        // terminated within the constant table.
        if (!isNullTerminatedWithin(constantTableData, constantInfo->name.get(), constantTableSize))
        {
            throw ShaderRecompileError(fmt::format(
                "Constant {} of this shader has an out of bounds name.", i));
        }
    }

    out += "#ifdef __spirv__\n\n";

#ifdef UNLEASHED_RECOMP
    bool isMetaInstancer = false;
    bool hasIndexCount = false;
#endif

    for (uint32_t i = 0; i < constantTableContainer->constantTable.constants; i++)
    {
        const auto constantInfo = reinterpret_cast<const ConstantInfo*>(
            constantTableData + constantTableContainer->constantTable.constantInfo + i * sizeof(ConstantInfo));

        const char* constantName = reinterpret_cast<const char*>(constantTableData + constantInfo->name);

    #ifdef UNLEASHED_RECOMP
        if (!isPixelShader)
        {
            if (strcmp(constantName, "g_MtxProjection") == 0)
                hasMtxProjection = true;
            else if (strcmp(constantName, "g_InstanceTypes") == 0)
                isMetaInstancer = true;
            else if (strcmp(constantName, "g_IndexCount") == 0)
                hasIndexCount = true;
        }
        else
        {
            if (strcmp(constantName, "g_MtxPrevInvViewProjection") == 0)
                hasMtxPrevInvViewProjection = true;
        }
    #endif

        switch (constantInfo->registerSet)
        {
        case RegisterSet::Float4:
        {
            const char* shaderName = isPixelShader ? "Pixel" : "Vertex";

            if (constantInfo->registerCount > 1)
            {
                uint32_t tailCount = (isPixelShader ? 224 : 256) - constantInfo->registerIndex;

                println("#define {}(INDEX) selectWrapper((INDEX) < {}, vk::RawBufferLoad<float4>(g_PushConstants.{}ShaderConstants + ({} + min(INDEX, {})) * 16, 0x10), 0.0)",
                    constantName, tailCount, shaderName, constantInfo->registerIndex.get(), tailCount - 1);
            }
            else
            {
                println("#define {} vk::RawBufferLoad<float4>(g_PushConstants.{}ShaderConstants + {}, 0x10)",
                    constantName, shaderName, constantInfo->registerIndex * 16);
            }
            
            for (uint16_t j = 0; j < constantInfo->registerCount; j++)
                float4Constants.emplace(constantInfo->registerIndex + j, constantInfo);

            break;
        }

        case RegisterSet::Sampler:
        {
            for (size_t j = 0; j < std::size(TEXTURE_DIMENSIONS); j++)
            {
                println("#define {}_Texture{}DescriptorIndex vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + {})",
                    constantName, TEXTURE_DIMENSIONS[j], j * 64 + constantInfo->registerIndex * 4);
            }

            println("#define {}_SamplerDescriptorIndex vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + {})",
                constantName, std::size(TEXTURE_DIMENSIONS) * 64 + constantInfo->registerIndex * 4);

            samplers.emplace(constantInfo->registerIndex, constantName);
            break;
        }

        }
    }

    out += "\n#elif defined(__air__)\n\n";

    for (uint32_t i = 0; i < constantTableContainer->constantTable.constants; i++)
    {
        const auto constantInfo = reinterpret_cast<const ConstantInfo*>(
            constantTableData + constantTableContainer->constantTable.constantInfo + i * sizeof(ConstantInfo));

        const char* constantName = reinterpret_cast<const char*>(constantTableData + constantInfo->name);

    #ifdef UNLEASHED_RECOMP
        if (!isPixelShader)
        {
            if (strcmp(constantName, "g_MtxProjection") == 0)
                hasMtxProjection = true;
            else if (strcmp(constantName, "g_InstanceTypes") == 0)
                isMetaInstancer = true;
            else if (strcmp(constantName, "g_IndexCount") == 0)
                hasIndexCount = true;
        }
        else
        {
            if (strcmp(constantName, "g_MtxPrevInvViewProjection") == 0)
                hasMtxPrevInvViewProjection = true;
        }
    #endif

        switch (constantInfo->registerSet)
        {
        case RegisterSet::Float4:
        {
            const char* shaderName = isPixelShader ? "Pixel" : "Vertex";

            if (constantInfo->registerCount > 1)
            {
                uint32_t tailCount = (isPixelShader ? 224 : 256) - constantInfo->registerIndex;

                println("#define {}(INDEX) selectWrapper((INDEX) < {}, (*(reinterpret_cast<device float4*>(g_PushConstants.{}ShaderConstants + ({} + min(INDEX, {})) * 16))), 0.0)",
                    constantName, tailCount, shaderName, constantInfo->registerIndex.get(), tailCount - 1);
            }
            else
            {
                println("#define {} (*(reinterpret_cast<device float4*>(g_PushConstants.{}ShaderConstants + {})))",
                    constantName, shaderName, constantInfo->registerIndex * 16);
            }

            for (uint16_t j = 0; j < constantInfo->registerCount; j++)
                float4Constants.emplace(constantInfo->registerIndex + j, constantInfo);

            break;
        }

        case RegisterSet::Sampler:
        {
            for (size_t j = 0; j < std::size(TEXTURE_DIMENSIONS); j++)
            {
                println("#define {}_Texture{}DescriptorIndex (*(reinterpret_cast<device uint*>(g_PushConstants.SharedConstants + {})))",
                    constantName, TEXTURE_DIMENSIONS[j], j * 64 + constantInfo->registerIndex * 4);
            }

            println("#define {}_SamplerDescriptorIndex (*(reinterpret_cast<device uint*>(g_PushConstants.SharedConstants + {})))",
                constantName, std::size(TEXTURE_DIMENSIONS) * 64 + constantInfo->registerIndex * 4);

            samplers.emplace(constantInfo->registerIndex, constantName);
            break;
        }

        }
    }

    out += "\n#else\n\n";

    println("cbuffer {}ShaderConstants : register(b{}, space4)", isPixelShader ? "Pixel" : "Vertex", isPixelShader ? 1 : 0);
    out += "{\n";

    for (uint32_t i = 0; i < constantTableContainer->constantTable.constants; i++)
    {
        const auto constantInfo = reinterpret_cast<const ConstantInfo*>(
            constantTableData + constantTableContainer->constantTable.constantInfo + i * sizeof(ConstantInfo));

        if (constantInfo->registerSet == RegisterSet::Float4)
        {
            const char* constantName = reinterpret_cast<const char*>(constantTableData + constantInfo->name);

            print("\tfloat4 {}", constantName);

            if (constantInfo->registerCount > 1)
                print("[{}]", constantInfo->registerCount.get());

            println(" : packoffset(c{});", constantInfo->registerIndex.get());

            if (constantInfo->registerCount > 1)
            {
                uint32_t tailCount = (isPixelShader ? 224 : 256) - constantInfo->registerIndex;
                println("#define {0}(INDEX) selectWrapper((INDEX) < {1}, {0}[min(INDEX, {2})], 0.0)", constantName, tailCount, tailCount - 1);
            }
        }
    }

    out += "};\n\n";

    out += "cbuffer SharedConstants : register(b2, space4)\n";
    out += "{\n";

    for (uint32_t i = 0; i < constantTableContainer->constantTable.constants; i++)
    {
        const auto constantInfo = reinterpret_cast<const ConstantInfo*>(
            constantTableData + constantTableContainer->constantTable.constantInfo + i * sizeof(ConstantInfo));

        if (constantInfo->registerSet == RegisterSet::Sampler)
        {
            const char* constantName = reinterpret_cast<const char*>(constantTableData + constantInfo->name);

            for (size_t j = 0; j < std::size(TEXTURE_DIMENSIONS); j++)
            {
                println("\tuint {}_Texture{}DescriptorIndex : packoffset(c{}.{});",
                    constantName, TEXTURE_DIMENSIONS[j], j * 4 + constantInfo->registerIndex / 4, SWIZZLES[constantInfo->registerIndex % 4]);
            }

            println("\tuint {}_SamplerDescriptorIndex : packoffset(c{}.{});",
                constantName, 4 * std::size(TEXTURE_DIMENSIONS) + constantInfo->registerIndex / 4, SWIZZLES[constantInfo->registerIndex % 4]);
        }
    }

    out += "\tDEFINE_SHARED_CONSTANTS();\n";
    out += "};\n\n";

    out += "#endif\n";

    for (uint32_t i = 0; i < constantTableContainer->constantTable.constants; i++)
    {
        const auto constantInfo = reinterpret_cast<const ConstantInfo*>(
            constantTableData + constantTableContainer->constantTable.constantInfo + i * sizeof(ConstantInfo));

        if (constantInfo->registerSet == RegisterSet::Bool)
        {
            const char* constantName = reinterpret_cast<const char*>(constantTableData + constantInfo->name);
            const uint32_t registerCount = std::max<uint32_t>(constantInfo->registerCount.get(), 1);

            for (uint32_t j = 0; j < registerCount; j++)
            {
                const uint32_t boolAddress = constantInfo->registerIndex.get() + j;
                const uint32_t boolIndex = getPackedBooleanIndex(boolAddress, isPixelShader);

                if (boolIndex >= PACKED_BOOLEAN_BITS)
                {
                    warnings.push_back(fmt::format(
                        "Boolean constant '{}{}' uses register b{}, which is outside of the {}-bit packed boolean range "
                        "supported by the shader common header.",
                        constantName, registerCount > 1 ? fmt::format("[{}]", j) : "", boolAddress, PACKED_BOOLEAN_BITS));
                    continue;
                }

                // Only the first element of a boolean array can be expressed as a macro.
                // The remaining elements are still registered below so that conditional
                // jumps referencing them resolve to the correct bit instead of producing
                // an undeclared identifier.
                if (j == 0)
                    println("#define {} (1u << {})", constantName, boolIndex);

                boolConstants.emplace(boolIndex, registerCount > 1 ? fmt::format("{}[{}]", constantName, j) : std::string(constantName));
            }
        }
    }

    out += '\n';

    if (!isInBounds(shaderContainer->shaderOffset.get(), sizeof(Shader), containerSize))
    {
        throw ShaderRecompileError("The shader of this container is out of bounds.");
    }

    const auto shader = reinterpret_cast<const Shader*>(shaderData + shaderContainer->shaderOffset.get());

    // The vertex elements and interpolators are stored inline after the shader structure, so the
    // amount of entries the shader claims to have has to fit into the container. Every read below
    // then stays in bounds, as long as the indices used are the ones validated here.
    uint32_t inlineDwords = (shader->interpolatorInfo.get() >> 5) & 0x1F;

    if (!isPixelShader)
    {
        const auto vertexShader = reinterpret_cast<const VertexShader*>(shader);
        inlineDwords += vertexShader->field18.get() + vertexShader->vertexElementCount.get();
    }

    const size_t inlineOffset = size_t(shaderContainer->shaderOffset.get()) +
        (isPixelShader ? sizeof(PixelShader) : sizeof(VertexShader));

    if (!isInBounds(inlineOffset, size_t(inlineDwords) * sizeof(uint32_t), containerSize))
    {
        throw ShaderRecompileError("The interpolators and vertex elements of this shader are out of bounds.");
    }

    println("struct {}", isPixelShader ? "Interpolators" : "VertexShaderInput");
    out += "{\n";

    if (isPixelShader)
    {
        out += "#ifdef __air__\n";

        out += "\tfloat4 iPos [[position]];\n";

        for (auto& [usage, usageIndex] : INTERPOLATORS)
            println("\tfloat4 i{0}{1} [[user({2}{1})]];", usageVariable(usage), usageIndex, usageSemantic(usage));

        out += "#else\n";

        out += "\tfloat4 iPos : SV_Position;\n";

        for (auto& [usage, usageIndex] : INTERPOLATORS)
            println("\tfloat4 i{0}{1} : {2}{1};", usageVariable(usage), usageIndex, usageSemantic(usage));

        out += "#endif\n";
    }
    else
    {
        auto vertexShader = reinterpret_cast<const VertexShader*>(shader);

        out += "#ifdef __air__\n";

        for (uint32_t i = 0; i < vertexShader->vertexElementCount; i++)
        {
            union
            {
                VertexElement vertexElement;
                uint32_t value;
            };

            value = vertexShader->vertexElementsAndInterpolators[vertexShader->field18 + i];

            const char* usageType = USAGE_TYPES[uint32_t(vertexElement.usage)];

#ifdef UNLEASHED_RECOMP
            if ((vertexElement.usage == DeclUsage::TexCoord && vertexElement.usageIndex == 2 && isMetaInstancer) ||
                (vertexElement.usage == DeclUsage::Position && vertexElement.usageIndex == 1))
            {
                usageType = "uint4";
            }
#endif

            out += '\t';

            print("{0} i{1}{2}", usageType, usageVariable(vertexElement.usage),
                uint32_t(vertexElement.usageIndex));

            bool foundUsage = false;
            for (auto& usageLocation : USAGE_LOCATIONS)
            {
                if (usageLocation.usage == vertexElement.usage && usageLocation.usageIndex == vertexElement.usageIndex)
                {
                    println(" [[attribute({})]];", usageLocation.location);
                    foundUsage = true;
                    break;
                }
            }

            if (!foundUsage) {
                throw ShaderRecompileError(fmt::format(
                    "Vertex element usage {} {} has no assigned location in USAGE_LOCATIONS.",
                    usageVariable(vertexElement.usage), uint32_t(vertexElement.usageIndex)));
            }

            vertexElements.emplace(uint32_t(vertexElement.address), vertexElement);
        }

        out += "#else\n";

        for (uint32_t i = 0; i < vertexShader->vertexElementCount; i++)
        {
            union
            {
                VertexElement vertexElement;
                uint32_t value;
            };

            value = vertexShader->vertexElementsAndInterpolators[vertexShader->field18 + i];

            const char* usageType = USAGE_TYPES[uint32_t(vertexElement.usage)];

#ifdef UNLEASHED_RECOMP
            if ((vertexElement.usage == DeclUsage::TexCoord && vertexElement.usageIndex == 2 && isMetaInstancer) ||
                (vertexElement.usage == DeclUsage::Position && vertexElement.usageIndex == 1))
            {
                usageType = "uint4";
            }
#endif

            out += '\t';

            for (auto& usageLocation : USAGE_LOCATIONS)
            {
                if (usageLocation.usage == vertexElement.usage && usageLocation.usageIndex == vertexElement.usageIndex)
                {
                    print("[[vk::location({})]] ", usageLocation.location);
                    break;
                }
            }

            println("{0} i{1}{2} : {3}{2};", usageType, usageVariable(vertexElement.usage),
                uint32_t(vertexElement.usageIndex), usageSemantic(vertexElement.usage));
        }

        out += "#endif\n";
    }

    out += "};\n";

    println("struct {}", isPixelShader ? "PixelShaderOutput" : "Interpolators");
    out += "{\n";

    if (isPixelShader)
    {
        out += "#ifdef __air__\n";

        auto pixelShader = reinterpret_cast<const PixelShader*>(shader);
        if (pixelShader->outputs & PIXEL_SHADER_OUTPUT_COLOR0)
            out += "\tfloat4 oC0 [[color(0)]];\n";
        if (pixelShader->outputs & PIXEL_SHADER_OUTPUT_COLOR1)
            out += "\tfloat4 oC1 [[color(1)]];\n";
        if (pixelShader->outputs & PIXEL_SHADER_OUTPUT_COLOR2)
            out += "\tfloat4 oC2 [[color(2)]];\n";
        if (pixelShader->outputs & PIXEL_SHADER_OUTPUT_COLOR3)
            out += "\tfloat4 oC3 [[color(3)]];\n";
        if (pixelShader->outputs & PIXEL_SHADER_OUTPUT_DEPTH)
            out += "\tfloat oDepth [[depth(any)]];\n";

        out += "#else\n";

        if (pixelShader->outputs & PIXEL_SHADER_OUTPUT_COLOR0)
            out += "\tfloat4 oC0 : SV_Target0;\n";
        if (pixelShader->outputs & PIXEL_SHADER_OUTPUT_COLOR1)
            out += "\tfloat4 oC1 : SV_Target1;\n";
        if (pixelShader->outputs & PIXEL_SHADER_OUTPUT_COLOR2)
            out += "\tfloat4 oC2 : SV_Target2;\n";
        if (pixelShader->outputs & PIXEL_SHADER_OUTPUT_COLOR3)
            out += "\tfloat4 oC3 : SV_Target3;\n";
        if (pixelShader->outputs & PIXEL_SHADER_OUTPUT_DEPTH)
            out += "\tfloat oDepth : SV_Depth;\n";

        out += "#endif\n";
    }
    else
    {
        out += "#ifdef __air__\n";

        out += "\tfloat4 oPos [[position]] [[invariant]];\n";

        for (auto& [usage, usageIndex] : INTERPOLATORS)
            print("\tfloat4 o{0}{1} [[user({2}{1})]];\n", usageVariable(usage), usageIndex, usageSemantic(usage));

        out += "\tfloat clipDistance [[clip_distance]];\n";

        out += "#else\n";

        out += "\tprecise float4 oPos : SV_Position;\n";

        for (auto& [usage, usageIndex] : INTERPOLATORS)
            print("\tfloat4 o{0}{1} : {2}{1};\n", usageVariable(usage), usageIndex, usageSemantic(usage));

        out += "\tfloat clipDistance : SV_ClipDistance;\n";

        out += "#endif\n";
    }

    out += "};\n";

    out += "#ifdef __air__\n";

    if (isPixelShader)
        out += "[[fragment]]\n";
    else
        out += "[[vertex]]\n";

    out += "#elif !defined(__spirv__)\n";

    if (isPixelShader)
        out += "[shader(\"pixel\")]\n";
    else
        out += "[shader(\"vertex\")]\n";

    out += "#endif\n";

    println("{} shaderMain(", isPixelShader ? "PixelShaderOutput" : "Interpolators");

    if (isPixelShader)
    {
        out += "#ifdef __air__\n";

        out += "\tInterpolators input [[stage_in]],\n";
        out += "\tbool iFace [[front_facing]],\n";

        out += "\tconstant Texture2DDescriptorHeap* g_Texture2DDescriptorHeap [[buffer(0)]],\n";
        out += "\tconstant Texture2DArrayDescriptorHeap* g_Texture2DArrayDescriptorHeap [[buffer(1)]],\n";
        out += "\tconstant TextureCubeDescriptorHeap* g_TextureCubeDescriptorHeap [[buffer(2)]],\n";
        out += "\tconstant SamplerDescriptorHeap* g_SamplerDescriptorHeap [[buffer(3)]],\n";
#ifdef MARATHON_RECOMP
        out += "\tdevice AtomicUintBuffer* g_ConditionalSurveyBuffer [[buffer(4)]],\n";
#endif
        out += "\tconstant PushConstants& g_PushConstants [[buffer(8)]]\n";

        out += "#else\n";

        out += "\tInterpolators input,\n";

        out += "#ifdef __spirv__\n";
        out += "\tin bool iFace : SV_IsFrontFace\n";
        out += "#else\n";
        out += "\tin uint iFace : SV_IsFrontFace\n";
        out += "#endif\n";

        out += "\n#endif\n";
    }
    else
    {
        out += "#ifdef __air__\n";
        out += "\tconstant PushConstants& g_PushConstants [[buffer(8)]],\n";
        out += "\tVertexShaderInput input [[stage_in]]\n";
        out += "#else\n";
        out += "\tVertexShaderInput input\n";
        out += "#endif\n";

    #ifdef UNLEASHED_RECOMP
        if (hasIndexCount)
        {
            out += "\t,\n";
            out += "#ifdef __air__\n";
            out += "\tuint iVertexId [[vertex_id]],\n";
            out += "\tuint iInstanceId [[instance_id]]\n";
            out += "#else\n";
            out += "\tin uint iVertexId : SV_VertexID,\n";
            out += "\tin uint iInstanceId : SV_InstanceID\n";
            out += "#endif\n";
        }
    #endif
    }

    out += ")\n";
    out += "{\n";

    std::string outputName = isPixelShader ? "PixelShaderOutput" : "Interpolators";

    out += "#ifdef __air__\n";
    println("\t{0} output = {0}{{}};", outputName);
    out += "#else\n";
    println("\t{0} output = ({0})0;", outputName);
    out += "#endif\n";

#ifdef UNLEASHED_RECOMP
    if (hasMtxProjection)
    {
        specConstantsMask |= SPEC_CONSTANT_REVERSE_Z;

        out += "\toutput.oPos = 0.0;\n";

        out += "\tfloat4x4 mtxProjection = float4x4(g_MtxProjection(0), g_MtxProjection(1), g_MtxProjection(2), g_MtxProjection(3));\n";
        out += "\tfloat4x4 mtxProjectionReverseZ = mul(mtxProjection, float4x4(1, 0, 0, 0, 0, 1, 0, 0, 0, 0, -1, 0, 0, 0, 1, 1));\n";

        out += "\tUNROLL for (int iterationIndex = 0; iterationIndex < 2; iterationIndex++)\n";
        out += "\t{\n";
    }
#endif

    if (shaderContainer->definitionTableOffset != NULL)
    {
        if (!isInBounds(shaderContainer->definitionTableOffset.get(), sizeof(DefinitionTable), containerSize))
        {
            throw ShaderRecompileError("The definition table of this shader is out of bounds.");
        }

        auto definitionTable = reinterpret_cast<const DefinitionTable*>(shaderData + shaderContainer->definitionTableOffset.get());
        auto definitions = definitionTable->definitions;

        // The definition tables are null terminated lists of definitions, so the reads below are
        // stopped at the end of the container instead of trusting the data to be well formed.
        const auto definitionsInBounds = [&](const void* pointer, size_t size)
        {
            return isInBounds(size_t(reinterpret_cast<const uint8_t*>(pointer) - shaderData), size, containerSize);
        };

        while (true)
        {
            if (!definitionsInBounds(definitions, sizeof(uint32_t)))
                throw ShaderRecompileError("The definition table of this shader is truncated.");

            if (*definitions == 0)
                break;

            auto definition = reinterpret_cast<const Float4Definition*>(definitions);

            if (!definitionsInBounds(definition, sizeof(Float4Definition)) ||
                !isInBounds(definition->physicalOffset.get(), size_t((definition->count.get() + 3) / 4) * 16, physicalSize))
                throw ShaderRecompileError("The definition table of this shader is out of bounds.");

            auto value = reinterpret_cast<const be<uint32_t>*>(shaderData + virtualSize + definition->physicalOffset.get());
            for (uint16_t i = 0; i < (definition->count + 3) / 4; i++)
            {
                println("#ifdef __air__");
                println("\tfloat4 c{} = as_type<float4>(uint4(0x{:X}, 0x{:X}, 0x{:X}, 0x{:X}));",
                    definition->registerIndex + i - (isPixelShader ? 256 : 0), value[0].get(), value[1].get(), value[2].get(), value[3].get());
                println("#else");
                println("\tfloat4 c{} = asfloat(uint4(0x{:X}, 0x{:X}, 0x{:X}, 0x{:X}));",
                    definition->registerIndex + i - (isPixelShader ? 256 : 0), value[0].get(), value[1].get(), value[2].get(), value[3].get());
                println("#endif");

                value += 4;
            }
            definitions += 2;
        }
        ++definitions;

        while (true)
        {
            if (!definitionsInBounds(definitions, sizeof(uint32_t)))
                throw ShaderRecompileError("The definition table of this shader is truncated.");

            if (*definitions == 0)
                break;

            auto definition = reinterpret_cast<const Int4Definition*>(definitions);

            if (!definitionsInBounds(definition, sizeof(Int4Definition)) ||
                !definitionsInBounds(definition->values, size_t(definition->count.get()) * sizeof(be<uint32_t>)))
                throw ShaderRecompileError("The definition table of this shader is out of bounds.");

            for (uint16_t i = 0; i < definition->count; i++)
            {
                union
                {
                    uint32_t value;
                    // The struct is named because GCC rejects an unnamed struct inside a union.
                    struct
                    {
                        int8_t x;
                        int8_t y;
                        int8_t z;
                        int8_t w;
                    } bytes;
                };

                value = definition->values[i].get();

                println("\tint4 i{} = int4({}, {}, {}, {});",
                    (definition->registerIndex - 8992) / 4 + i, bytes.x, bytes.y, bytes.z, bytes.w);
            }
            definitions += 2;
            definitions += definition->count;
        }

        out += "\n";
    }

    bool printedRegisters[32]{};

    uint32_t interpolatorCount = (shader->interpolatorInfo >> 5) & 0x1F;

    for (uint32_t i = 0; i < interpolatorCount; i++)
    {
        union
        {
            Interpolator interpolator;
            uint32_t value;
        };
    
        if (isPixelShader)
        {
            value = reinterpret_cast<const PixelShader*>(shader)->interpolators[i];
            println("\tfloat4 r{} = input.i{}{};", uint32_t(interpolator.reg), usageVariable(interpolator.usage), uint32_t(interpolator.usageIndex));
            printedRegisters[interpolator.reg] = true;
        }
        else
        {
            auto vertexShader = reinterpret_cast<const VertexShader*>(shader);
            value = vertexShader->vertexElementsAndInterpolators[vertexShader->field18 + vertexShader->vertexElementCount + i];
            interpolators.emplace(i, fmt::format("output.o{}{}", usageVariable(interpolator.usage), uint32_t(interpolator.usageIndex)));
        }
    }

    if (!isPixelShader)
    {
    #ifdef UNLEASHED_RECOMP
        if (!hasMtxProjection)
            out += "\toutput.oPos = 0.0;\n";
    #endif

        for (auto& [usage, usageIndex] : INTERPOLATORS)
            println("\toutput.o{}{} = 0.0;", usageVariable(usage), usageIndex);

        out += "\n";
    }

    for (size_t i = 0; i < 32; i++)
    {
        if (!printedRegisters[i])
        {
            print("\tfloat4 r{} = ", i);
            if (isPixelShader && i == ((shader->fieldC >> 8) & 0xFF))
            {
                out += "float4((input.iPos.xy - 0.5) * float2(iFace ? 1.0 : -1.0, 1.0), 0.0, 0.0);\n";
            }
        #ifdef UNLEASHED_RECOMP
            else if (!isPixelShader && hasIndexCount && i == 0)
            {
                out += "float4(iVertexId + g_IndexCount.x * iInstanceId, 0.0, 0.0, 0.0);\n";
            }
        #endif
            else
            {
                out += "0.0;\n";
            }
        }
    }

    out += "\tint a0 = 0;\n";
    out += "\tint aL = 0;\n";
    out += "\tbool p0 = false;\n";
    out += "\tfloat ps = 0.0;\n";
    if (isPixelShader)
    {
#ifdef UNLEASHED_RECOMP
        out += "\tfloat2 pixelCoord = 0.0;\n";
#endif
#ifdef MARATHON_RECOMP
        specConstantsMask |= SPEC_CONSTANT_CONDITIONAL_RENDERING;

        out += "\tBRANCH if ((g_SpecConstants() & SPEC_CONSTANT_CONDITIONAL_RENDERING))\n";
        out += "\t{\n";

        out += "\t\tuint sampleCount = atomicLoadUint(g_ConditionalSurveyBuffer, g_conditionalSurveyIndex);\n";
        out += "\t\tBRANCH if (sampleCount == 0)\n";
        out += "\t\t{\n";

        println("#ifdef __air__");
        println("\t\t\tdiscard_fragment();");
        println("#else");
        println("\t\t\tdiscard;");
        println("#endif");

        out += "\t\t}\n";

        out += "\t}\n";
#endif
    }

    // The microcode of the shader lives in the physical area of the container.
    if (!isInBounds(shader->physicalOffset.get(), shader->size.get(), physicalSize))
    {
        throw ShaderRecompileError("The microcode of this shader is out of bounds.");
    }

    const be<uint32_t>* code = reinterpret_cast<const be<uint32_t>*>(shaderData + virtualSize + shader->physicalOffset.get());

    union
    {
        ControlFlowInstruction controlFlow[2];
        // The struct is named because GCC rejects an unnamed struct inside a union.
        struct
        {
            uint32_t code0;
            uint32_t code1;
            uint32_t code2;
            uint32_t code3;
        } codes;
    };

    auto controlFlowCode = code;
    uint32_t instrAddress = 0;
    uint32_t instrSize = shader->size;
    bool simpleControlFlow = true;

    // Unsupported or approximated control flow constructs are collected here so that they
    // end up in the recompilation report instead of silently affecting the generated shader.
    std::set<uint32_t> conditionalExecRegisters;
    bool hasFunctionCalls = false;

    while (instrAddress < instrSize)
    {
        codes.code0 = controlFlowCode[0];
        codes.code1 = controlFlowCode[1] & 0xFFFF;
        codes.code2 = (controlFlowCode[1] >> 16) | (controlFlowCode[2] << 16);
        codes.code3 = controlFlowCode[2] >> 16;

        for (auto& cfInstr : controlFlow)
        {
            uint32_t address = 0;

            switch (cfInstr.opcode)
            {
            case ControlFlowOpcode::Exec:
            case ControlFlowOpcode::ExecEnd:
                address = cfInstr.exec.address;
                break;

            case ControlFlowOpcode::CondExec:
            case ControlFlowOpcode::CondExecEnd:
            case ControlFlowOpcode::CondExecPredClean:
            case ControlFlowOpcode::CondExecPredCleanEnd:
                address = cfInstr.condExec.address;
                conditionalExecRegisters.insert(cfInstr.condExec.boolAddress);
                break;

            case ControlFlowOpcode::CondExecPred:
            case ControlFlowOpcode::CondExecPredEnd:
                address = cfInstr.condExecPred.address;
                break;

            case ControlFlowOpcode::CondCall:
            case ControlFlowOpcode::Return:
                hasFunctionCalls = true;
                break;

            case ControlFlowOpcode::CondJmp:
            {
                if (cfInstr.condJmp.isUnconditional || cfInstr.condJmp.direction)
                    simpleControlFlow = false;
                else
                    ++ifEndLabels[cfInstr.condJmp.address];

                break;
            }
            }

            if (address != 0)
                instrSize = std::min<uint32_t>(instrSize, address * 12);
        }

        controlFlowCode += 3;
        instrAddress += 12;
    }

    if (!conditionalExecRegisters.empty())
    {
        std::string registers;
        size_t count = 0;

        for (uint32_t boolAddress : conditionalExecRegisters)
        {
            if (count++ == 8)
            {
                registers += ", ...";
                break;
            }

            registers += fmt::format("{}b{} (packed bit {})", registers.empty() ? "" : ", ", boolAddress,
                getPackedBooleanIndex(boolAddress, isPixelShader));
        }

        warnings.push_back(fmt::format(
            "Shader conditionally executes instructions based on boolean constants ({}). These blocks are "
            "currently translated as unconditional blocks, which can produce incorrect results.",
            registers));
    }

    if (hasFunctionCalls)
    {
        warnings.push_back("Shader contains function calls (condcall/return), which are not translated. "
            "The resulting shader may be incomplete.");
    }

    if (simpleControlFlow)
    {
        out += '\n';
        indentation = 1;
    }
    else
    {
        out += "\n\tuint pc = 0;\n";
        out += "\twhile (true)\n";
        out += "\t{\n";
        out += "\t\tswitch (pc)\n";
        out += "\t\t{\n";
    }

    controlFlowCode = code;
    instrAddress = 0;
    uint32_t pc = 0;

    while (instrAddress < instrSize)
    {
        codes.code0 = controlFlowCode[0];
        codes.code1 = controlFlowCode[1] & 0xFFFF;
        codes.code2 = (controlFlowCode[1] >> 16) | (controlFlowCode[2] << 16);
        codes.code3 = controlFlowCode[2] >> 16;

        for (auto& cfInstr : controlFlow)
        {
            if (!simpleControlFlow)
            {
                indentation = 3;
                println("\t\tcase {}:", pc);
            }
            else
            {
                auto findResult = ifEndLabels.find(pc);
                if (findResult != ifEndLabels.end())
                {
                    for (uint32_t i = 0; i < findResult->second; i++)
                    {
                        --indentation;
                        indent();
                        out += "}\n";
                    }
                }
            }

            ++pc;

            uint32_t address = 0;
            uint32_t count = 0;
            uint32_t sequence = 0;
            bool shouldReturn = false;

            switch (cfInstr.opcode)
            {
            case ControlFlowOpcode::Exec:
            case ControlFlowOpcode::ExecEnd:
                address = cfInstr.exec.address;
                count = cfInstr.exec.count;
                sequence = cfInstr.exec.sequence;
                shouldReturn = (cfInstr.opcode == ControlFlowOpcode::ExecEnd);
                break;

            case ControlFlowOpcode::CondExec:
            case ControlFlowOpcode::CondExecEnd:
            case ControlFlowOpcode::CondExecPredClean:
            case ControlFlowOpcode::CondExecPredCleanEnd:
                address = cfInstr.condExec.address;
                count = cfInstr.condExec.count;
                sequence = cfInstr.condExec.sequence;
                shouldReturn = (cfInstr.opcode == ControlFlowOpcode::CondExecEnd || cfInstr.opcode == ControlFlowOpcode::CondExecEnd);
                break;

            case ControlFlowOpcode::CondExecPred:
            case ControlFlowOpcode::CondExecPredEnd:
                address = cfInstr.condExecPred.address;
                count = cfInstr.condExecPred.count;
                sequence = cfInstr.condExecPred.sequence;
                shouldReturn = (cfInstr.opcode == ControlFlowOpcode::CondExecPredEnd);
                break;

            case ControlFlowOpcode::LoopStart:
                if (simpleControlFlow)
                {
                    indent();
                #ifdef UNLEASHED_RECOMP
                    print("UNROLL ");
                #endif
                    println("for (aL = 0; aL < i{}.x; aL++)", uint32_t(cfInstr.loopStart.loopId));
                    indent();
                    out += "{\n";
                    ++indentation;
                }
                else 
                {
                    out += "\t\t\taL = 0;\n";
                }
                break;

            case ControlFlowOpcode::LoopEnd:
                if (simpleControlFlow)
                {
                    --indentation;
                    indent();
                    out += "}\n";
                }
                else
                {
                    out += "\t\t\t++aL;\n";
                    println("\t\t\tif (aL < i{}.x)", uint32_t(cfInstr.loopEnd.loopId));
                    out += "\t\t\t{\n";
                    println("\t\t\t\tpc = {};", uint32_t(cfInstr.loopEnd.address));
                    out += "\t\t\t\tcontinue;\n";
                    out += "\t\t\t}\n";
                }
                break;

            case ControlFlowOpcode::CondJmp:
            {
                if (cfInstr.condJmp.isUnconditional)
                {
                    // Unconditional jumps switch the shader over to the switch based control flow
                    // mode above, which is what makes the "continue" below valid.
                    println("\t\t\tpc = {};", uint32_t(cfInstr.condJmp.address));
                    out += "\t\t\tcontinue;\n";
                }
                else
                {
                    indent();
                    if (cfInstr.condJmp.isPredicated)
                    {
                        println("if ({}p0)", cfInstr.condJmp.condition ^ simpleControlFlow ? "" : "!");
                    }
                    else
                    {
                        // The condition is read straight from the packed boolean value instead of
                        // referencing the constant table name. Shaders may jump on boolean registers
                        // that are not present in the reflection data (or that are reflected with a
                        // different register index), which previously produced references to
                        // undeclared identifiers such as b128/b129/b130.
                        const uint32_t boolAddress = cfInstr.condJmp.boolAddress;
                        const uint32_t boolIndex = getPackedBooleanIndex(boolAddress, isPixelShader);
                        const char* comparison = (cfInstr.condJmp.condition ^ simpleControlFlow) ? "!=" : "==";

                        if (boolIndex < PACKED_BOOLEAN_BITS)
                        {
                            auto findResult = boolConstants.find(boolIndex);
                            if (findResult != boolConstants.end())
                                println("if ((g_Booleans & (1u << {})) {} 0) // {} (b{})", boolIndex, comparison, findResult->second, boolAddress);
                            else
                                println("if ((g_Booleans & (1u << {})) {} 0) // b{} (boolean constant not in reflection data)", boolIndex, comparison, boolAddress);
                        }
                        else
                        {
                            warnings.push_back(fmt::format(
                                "Conditional jump on boolean register b{} is outside of the {}-bit packed boolean range "
                                "supported by the shader common header and is always treated as false.",
                                boolAddress, PACKED_BOOLEAN_BITS));

                            println("if ({}) // b{} is outside of the supported packed boolean range", (comparison == "!=") ? "false" : "true", boolAddress);
                        }
                    }

                    if (simpleControlFlow)
                    {
                        indent();
                        out += "{\n";
                        ++indentation;
                    }
                    else
                    {
                        out += "\t\t\t{\n";
                        println("\t\t\t\tpc = {};", uint32_t(cfInstr.condJmp.address));
                        out += "\t\t\t\tcontinue;\n";
                        out += "\t\t\t}\n";
                    }
                }
                break;
            }
            }

            // Instruction addresses and counts come from the shader data as well.
            if (!isInBounds(size_t(address) * 12, size_t(count) * 12, size_t(shader->size.get())))
                throw ShaderRecompileError(fmt::format("Shader instruction block at {} is out of bounds.", address));

            auto instructionCode = code + address * 3;
            
            for (uint32_t i = 0; i < count; i++)
            {
                union
                {
                    VertexFetchInstruction vertexFetch;
                    TextureFetchInstruction textureFetch;
                    AluInstruction alu;
                    // The struct is named because GCC rejects an unnamed struct inside a union.
                    struct
                    {
                        uint32_t code0;
                        uint32_t code1;
                        uint32_t code2;
                    } codes;
                };
            
                codes.code0 = instructionCode[0];
                codes.code1 = instructionCode[1];
                codes.code2 = instructionCode[2];
            
                if ((sequence & 0x1) != 0)
                {
                    if (vertexFetch.opcode == FetchOpcode::VertexFetch)
                    {
                        recompile(vertexFetch, address + i);
                    }
                    else
                    {
                    #ifdef UNLEASHED_RECOMP
                        if (textureFetch.constIndex == 10) // g_GISampler
                        {
                            specConstantsMask |= SPEC_CONSTANT_BICUBIC_GI_FILTER;

                            indent();
                            out += "if (g_SpecConstants() & SPEC_CONSTANT_BICUBIC_GI_FILTER)\n";
                            indent();
                            out += "{\n";

                            ++indentation;
                            recompile(textureFetch, true);
                            --indentation;

                            indent();
                            out += "}\n";
                            indent();
                            out += "else\n";
                            indent();
                            out += "{\n";

                            ++indentation;
                            recompile(textureFetch, false);
                            --indentation;

                            indent();
                            out += "}\n";
                        }
                        else
                    #endif
                        {
                            recompile(textureFetch, false);
                        }
                    }
                }
                else
                {
                    recompile(alu);
                }
            
                sequence >>= 2;
                instructionCode += 3;
            }

            if (shouldReturn)
            {
                if (isPixelShader)
                {
                    specConstantsMask |= SPEC_CONSTANT_ALPHA_TEST;

                    indent();
                    out += "BRANCH if (g_SpecConstants() & SPEC_CONSTANT_ALPHA_TEST)\n";
                    indent();
                    out += "{\n";

                    indent();
                    out += "\tclip(output.oC0.w - g_AlphaThreshold);\n";

                    indent();
                    out += "}\n";

                #ifdef UNLEASHED_RECOMP
                    specConstantsMask |= SPEC_CONSTANT_ALPHA_TO_COVERAGE;

                    indent();
                    out += "else if (g_SpecConstants() & SPEC_CONSTANT_ALPHA_TO_COVERAGE)\n";
                    indent();
                    out += "{\n";

                    indent();
                    out += "\toutput.oC0.w *= 1.0 + computeMipLevel(pixelCoord) * 0.25;\n";
                    indent();
                    out += "\toutput.oC0.w = 0.5 + (output.oC0.w - g_AlphaThreshold) / max(fwidth(output.oC0.w), 1e-6);\n";

                    indent();
                    out += "}\n";
                #endif
                }
                else
                {
                    out += "\tif (g_ClipPlaneEnabled) output.clipDistance = dot(output.oPos, g_ClipPlane);\n";
                    out += "\toutput.oPos.xy += g_HalfPixelOffset * output.oPos.w;\n";
                }

                if (simpleControlFlow)
                {
                    indent();
                #ifdef UNLEASHED_RECOMP
                    if (hasMtxProjection)
                    {
                        out += "continue;\n";
                    }
                    else
                #endif
                    {
                        out += "return output;\n";
                    }
                }
                else
                {
                    out += "\t\t\tbreak;\n";
                }
            }
        }

        controlFlowCode += 3;
        instrAddress += 12;
    }

    if (!simpleControlFlow)
    {
        out += "\t\t\tbreak;\n";
        out += "\t\t}\n";
        out += "\t\tbreak;\n";
        out += "\t}\n";
    }

#ifdef UNLEASHED_RECOMP
    if (hasMtxProjection)
        out += "\t}\n";
#endif

    if (!simpleControlFlow)
        out += "\treturn output;\n";
#ifdef UNLEASHED_RECOMP
    else if (hasMtxProjection)
        out += "\treturn output;\n";
#endif

    out += "}";
}
