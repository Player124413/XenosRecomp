#include <atomic>
#include <chrono>
#include <filesystem>
#include <mutex>
#include <thread>

#include "shader.h"
#include "shader_recompiler.h"
#include "dxc_compiler.h"
#include "xcompress.h"

#ifdef XENOS_RECOMP_AIR
#include "air_compiler.h"
#endif

// Every message printed by the recompiler goes through the mutex below so that the
// output of the worker threads does not get interleaved with each other.
static std::mutex g_logMutex;

template<class... Args>
static void logLine(fmt::format_string<Args...> format, Args&&... args)
{
    std::lock_guard lock(g_logMutex);
    fmt::print(format, std::forward<Args>(args)...);
    fmt::print("\n");
}

static void logError(const std::string& message)
{
    std::lock_guard lock(g_logMutex);
    fmt::println(stderr, "error: {}", message);
}

static std::unique_ptr<uint8_t[]> readAllBytes(const char* filePath, size_t& fileSize)
{
    FILE* file = fopen(filePath, "rb");

    if (file == nullptr)
        throw std::runtime_error(fmt::format("Failed to open '{}' for reading.", filePath));

    if (fseek(file, 0, SEEK_END) != 0)
    {
        fclose(file);
        throw std::runtime_error(fmt::format("Failed to read '{}'.", filePath));
    }

    long length = ftell(file);
    if (length < 0 || fseek(file, 0, SEEK_SET) != 0)
    {
        fclose(file);
        throw std::runtime_error(fmt::format("'{}' is not a regular file.", filePath));
    }

    fileSize = size_t(length);

    auto data = std::make_unique<uint8_t[]>(fileSize);
    if (fileSize != 0 && fread(data.get(), 1, fileSize, file) != fileSize)
    {
        fclose(file);
        throw std::runtime_error(fmt::format("Failed to read '{}'.", filePath));
    }

    fclose(file);
    return data;
}

static void writeAllBytes(const char* filePath, const void* data, size_t dataSize)
{
    std::error_code errorCode;
    std::filesystem::path path(filePath);

    if (path.has_parent_path())
        std::filesystem::create_directories(path.parent_path(), errorCode);

    FILE* file = fopen(filePath, "wb");

    if (file == nullptr)
        throw std::runtime_error(fmt::format("Failed to open '{}' for writing.", filePath));

    if (dataSize != 0 && fwrite(data, 1, dataSize, file) != dataSize)
    {
        fclose(file);
        throw std::runtime_error(fmt::format("Failed to write '{}'.", filePath));
    }

    fclose(file);
}

static std::string_view trim(std::string_view value)
{
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t' || value.front() == '\r' || value.front() == '\n'))
        value.remove_prefix(1);

    while (!value.empty() && (value.back() == ' ' || value.back() == '\t' || value.back() == '\r' || value.back() == '\n'))
        value.remove_suffix(1);

    return value;
}

template<typename TCallback>
static void forEachLine(const std::string_view text, const TCallback& callback)
{
    if (text.empty())
        return;

    size_t lineStart = 0;

    while (true)
    {
        size_t lineEnd = text.find('\n', lineStart);
        if (lineEnd == std::string_view::npos)
            lineEnd = text.size();

        callback(text.substr(lineStart, lineEnd - lineStart));

        if (lineEnd == text.size())
            break;

        lineStart = lineEnd + 1;
    }
}

// Returns the diagnostics with each line indented, so that they can be embedded into
// reports and console output without losing track of which shader they belong to.
static std::string indentLines(const std::string_view text, const std::string_view indentation = "\t")
{
    std::string result;

    forEachLine(text, [&](const std::string_view line)
    {
        auto trimmedLine = trim(line);
        if (trimmedLine.empty())
            return;

        result.append(indentation);
        result.append(trimmedLine);
        result.push_back('\n');
    });

    return result;
}

// Calls the callback for every shader container found in a buffer. Games store their shaders
// inside archives that can be scanned this way, because a container starts with a magic value.
template<typename TCallback>
static void forEachShaderContainer(const uint8_t* data, size_t size, const TCallback& callback)
{
    for (size_t i = 0; size > sizeof(ShaderContainer) && i < size - sizeof(ShaderContainer) - 1;)
    {
        auto shaderContainer = reinterpret_cast<const ShaderContainer*>(data + i);
        const size_t dataSize = shaderContainer->virtualSize + shaderContainer->physicalSize;

        if ((shaderContainer->flags & 0xFFFFFF00) == 0x102A1100 &&
            dataSize <= (size - i) &&
            shaderContainer->field1C == 0 &&
            shaderContainer->field20 == 0)
        {
            callback(i, dataSize);
            i += dataSize;
        }
        else
        {
            i += sizeof(uint32_t);
        }
    }
}

// Archives that are too large for a single file are stored as "name.ar.00", "name.ar.01" and so on,
// with the part number at the end of the file name.
struct ArchiveName
{
    std::string key;      // name of the whole archive, without the part number
    uint32_t partIndex = 0;
    bool isPart = false;  // false when the file name does not have a part number
};

static ArchiveName splitArchiveName(const std::filesystem::path& path)
{
    const std::string name = path.filename().string();
    const size_t position = name.rfind(".ar");

    ArchiveName result;

    if (position == std::string::npos)
    {
        result.key = path.string();
        return result;
    }

    const std::string suffix = name.substr(position + 3);

    if (!suffix.empty() &&
        (suffix.size() != 3 || suffix[0] != '.' || !isdigit(uint8_t(suffix[1])) || !isdigit(uint8_t(suffix[2]))))
    {
        result.key = path.string();
        return result;
    }

    result.key = (path.parent_path() / name.substr(0, position + 3)).string();
    result.isPart = true;

    if (!suffix.empty())
        result.partIndex = uint32_t((suffix[1] - '0') * 10 + (suffix[2] - '0'));

    return result;
}

struct Options
{
    std::string input;
    std::string output;
    std::string include;
    std::string report;
    std::string dumpFailedShaders;
    bool allowFailures = false;
    uint32_t threadCount = 0; // 0 = use the number of available hardware threads
};

static void printUsage()
{
    fmt::println(
        "Usage: XenosRecomp [input path] [output path] [shader common header file path] [options]\n"
        "\n"
        "  Input paths may point to a directory or to a single shader container file.\n"
        "  When a directory is given, every shader container found inside it is recompiled\n"
        "  and a shader cache is written to the output path. When a file is given, the\n"
        "  generated HLSL of that shader is written to the output path instead.\n"
        "\n"
        "Options:\n"
        "  --report <path>          Write a JSON report of the recompilation to the given path.\n"
        "  --dump-failed <dir>      Write the generated HLSL of failed shaders into the given directory.\n"
        "  --allow-failures         Do not return an error code when shaders fail to compile.\n"
        "  --jobs <count>           Number of shaders to recompile in parallel. Defaults to the\n"
        "                           number of available hardware threads.\n"
        "  --help                   Show this message.");
}

static bool parseArgs(int argc, char** argv, Options& options)
{
    std::vector<std::string> positional;

    bool valid = true;

    auto readValue = [&](int& i, const char* option) -> std::string
    {
        if (i + 1 >= argc)
        {
            logError(fmt::format("'{}' expects a value.", option));
            valid = false;
            return {};
        }

        return argv[++i];
    };

    for (int i = 1; i < argc; i++)
    {
        std::string_view arg = argv[i];

        if (arg == "--help" || arg == "-h")
        {
            printUsage();
            exit(0);
        }
        else if (arg == "--report")
        {
            options.report = readValue(i, "--report");
        }
        else if (arg == "--dump-failed" || arg == "--dump-failed-shaders")
        {
            options.dumpFailedShaders = readValue(i, "--dump-failed");
        }
        else if (arg == "--allow-failures" || arg == "--continue-on-error")
        {
            options.allowFailures = true;
        }
        else if (arg == "--jobs" || arg == "-j")
        {
            std::string value = readValue(i, "--jobs");
            if (value.empty())
                return false;

            try
            {
                options.threadCount = std::max(1, std::stoi(value));
            }
            catch (const std::exception&)
            {
                logError(fmt::format("'{}' is not a valid job count.", value));
                return false;
            }
        }
        else if (arg.size() > 1 && arg[0] == '-' && !std::filesystem::exists(std::filesystem::path(arg)))
        {
            logError(fmt::format("Unknown option '{}'.", arg));
            return false;
        }
        else
        {
            positional.emplace_back(arg);
        }
    }

    if (!valid)
        return false;

    if (positional.size() > 3)
    {
        logError("Too many arguments were given.");
        return false;
    }

#ifndef XENOS_RECOMP_INPUT
    if (positional.size() > 0)
        options.input = positional[0];
#endif
#ifndef XENOS_RECOMP_OUTPUT
    if (positional.size() > 1)
        options.output = positional[1];
#endif
#ifndef XENOS_RECOMP_INCLUDE_INPUT
    if (positional.size() > 2)
        options.include = positional[2];
#endif

#ifdef XENOS_RECOMP_INPUT
    options.input = XENOS_RECOMP_INPUT;
#endif
#ifdef XENOS_RECOMP_OUTPUT
    options.output = XENOS_RECOMP_OUTPUT;
#endif
#ifdef XENOS_RECOMP_INCLUDE_INPUT
    options.include = XENOS_RECOMP_INCLUDE_INPUT;
#endif

    if (options.input.empty() || options.output.empty() || options.include.empty())
    {
        printUsage();
        return false;
    }

    return true;
}

struct RecompiledShader
{
    uint8_t* data = nullptr;
    size_t dataSize = 0;
    IDxcBlob* dxil = nullptr;
    std::vector<uint8_t> spirv;
    std::vector<uint8_t> air;
    uint32_t specConstantsMask = 0;
};

struct ShaderJob
{
    XXH64_hash_t hash = 0;
    uint8_t* data = nullptr;
    size_t dataSize = 0;
    std::string sourcePath;

    std::string displayName(bool isPixelShader) const
    {
        std::string name = sourcePath;
        for (char& c : name)
        {
            if (c == '\\')
                c = '/';
        }

        return fmt::format("{}#0x{:X} ({})", name, hash, isPixelShader ? "PS" : "VS");
    }
};

struct ShaderResult
{
    bool isPixelShader = false;
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
    // Only kept for shaders that failed, so that they can be written out for debugging.
    std::string hlsl;
    std::string hlslDumpPath;

    bool succeeded() const { return errors.empty(); }
};

static std::chrono::steady_clock::time_point g_startTime;

// Reported once for the whole run instead of once per shader.
static std::atomic<bool> g_unsignedDxil = false;

static void printProgress(std::atomic<uint32_t>& progress, uint32_t numShaders)
{
    const uint32_t currentProgress = ++progress;

    if ((currentProgress % 25) == 0 || currentProgress >= numShaders)
        logLine("Recompiling shaders... {}%", currentProgress / float(numShaders) * 100.0f);
}

static void recompileShader(const ShaderJob& job, ShaderResult& result, RecompiledShader& shader, const std::string_view include,
    std::atomic<uint32_t>& progress, uint32_t numShaders)
{
    thread_local ShaderRecompiler recompiler;
    recompiler = {};

    try
    {
        recompiler.recompile(job.data, job.dataSize, include);
    }
    catch (const ShaderRecompileError& error)
    {
        result.isPixelShader = recompiler.isPixelShader;
        result.errors.emplace_back(fmt::format("The shader could not be translated: {}", error.what()));
        result.warnings = std::move(recompiler.warnings);
        result.hlsl = std::move(recompiler.out);
        printProgress(progress, numShaders);
        return;
    }
    catch (const std::exception& error)
    {
        result.isPixelShader = recompiler.isPixelShader;
        result.errors.emplace_back(fmt::format("An unexpected error occurred while translating the shader: {}", error.what()));
        result.warnings = std::move(recompiler.warnings);
        result.hlsl = std::move(recompiler.out);
        printProgress(progress, numShaders);
        return;
    }

    result.isPixelShader = recompiler.isPixelShader;
    result.warnings = std::move(recompiler.warnings);

    thread_local DxcCompiler dxcCompiler;

    if (!dxcCompiler.isValid())
    {
        result.errors.emplace_back("The DirectX Shader Compiler is not available, so the shader could not be compiled.");
        result.hlsl = std::move(recompiler.out);
        printProgress(progress, numShaders);
        return;
    }

    shader.specConstantsMask = recompiler.specConstantsMask;

#ifdef XENOS_RECOMP_DXIL
    DxcCompileResult dxil = dxcCompiler.compile(recompiler.out, recompiler.isPixelShader, recompiler.specConstantsMask != 0, false,
        job.displayName(recompiler.isPixelShader) + " [DXIL]");

    if (!dxil.succeeded())
    {
        result.errors.emplace_back(fmt::format("Failed to compile the recompiled shader to DXIL:\n{}", indentLines(dxil.errors)));
        result.hlsl = std::move(recompiler.out);
        printProgress(progress, numShaders);
        return;
    }

    shader.dxil = dxil.object;

    if (*(reinterpret_cast<uint32_t*>(shader.dxil->GetBufferPointer()) + 1) == 0)
        g_unsignedDxil = true;
#endif

#ifdef XENOS_RECOMP_AIR
    try
    {
        shader.air = AirCompiler::compile(recompiler.out);
    }
    catch (const std::exception& error)
    {
        result.errors.emplace_back(fmt::format("Failed to compile the recompiled shader to AIR:\n{}", indentLines(error.what())));
        result.hlsl = std::move(recompiler.out);
        printProgress(progress, numShaders);
        return;
    }

    if (shader.air.empty())
        result.warnings.emplace_back("The generated Metal AIR blob is empty.");
#endif

    DxcCompileResult spirv = dxcCompiler.compile(recompiler.out, recompiler.isPixelShader, false, true,
        job.displayName(recompiler.isPixelShader) + " [SPIR-V]");

    if (!spirv.succeeded())
    {
        result.errors.emplace_back(fmt::format("Failed to compile the recompiled shader to SPIR-V:\n{}", indentLines(spirv.errors)));
        result.hlsl = std::move(recompiler.out);
        printProgress(progress, numShaders);
        return;
    }

    if (!smolv::Encode(spirv.object->GetBufferPointer(), spirv.object->GetBufferSize(), shader.spirv, smolv::kEncodeFlagStripDebugInfo))
    {
        result.errors.emplace_back("Failed to encode the compiled SPIR-V.");
        result.hlsl = std::move(recompiler.out);
    }

    spirv.object->Release();

    printProgress(progress, numShaders);
}

// Returns true if a valid UTF-8 sequence starts at the given offset, in which case its length is
// stored in 'length'. Diagnostics can contain bytes taken from shader data, which are not
// necessarily valid UTF-8, and the report has to stay readable by JSON parsers.
static bool decodeUtf8(const std::string_view value, size_t offset, size_t& length)
{
    const unsigned char first = static_cast<unsigned char>(value[offset]);

    if (first < 0x80)
    {
        length = 1;
        return true;
    }

    size_t expected = 0;
    uint32_t codePoint = 0;

    if ((first & 0xE0) == 0xC0)
    {
        expected = 1;
        codePoint = first & 0x1F;
    }
    else if ((first & 0xF0) == 0xE0)
    {
        expected = 2;
        codePoint = first & 0x0F;
    }
    else if ((first & 0xF8) == 0xF0)
    {
        expected = 3;
        codePoint = first & 0x07;
    }
    else
    {
        return false;
    }

    for (size_t i = 1; i <= expected; i++)
    {
        if (offset + i >= value.size())
            return false;

        const unsigned char next = static_cast<unsigned char>(value[offset + i]);

        if ((next & 0xC0) != 0x80)
            return false;

        codePoint = (codePoint << 6) | (next & 0x3F);
    }

    // Reject overlong encodings, surrogates and code points outside of Unicode.
    if ((expected == 1 && codePoint < 0x80) ||
        (expected == 2 && codePoint < 0x800) ||
        (expected == 3 && (codePoint < 0x10000 || codePoint > 0x10FFFF)) ||
        (codePoint >= 0xD800 && codePoint <= 0xDFFF))
    {
        return false;
    }

    length = expected + 1;
    return true;
}

static std::string jsonEscape(const std::string_view value)
{
    std::string result;
    result.reserve(value.size());

    for (size_t i = 0; i < value.size(); i++)
    {
        const char c = value[i];

        switch (c)
        {
        case '"': result += "\\\""; continue;
        case '\\': result += "\\\\"; continue;
        case '\n': result += "\\n"; continue;
        case '\r': result += "\\r"; continue;
        case '\t': result += "\\t"; continue;
        }

        const unsigned char byte = static_cast<unsigned char>(c);

        if (byte < 0x20)
        {
            result += fmt::format("\\u{:04X}", uint32_t(byte));
            continue;
        }

        if (byte < 0x80)
        {
            result.push_back(c);
            continue;
        }

        size_t length = 0;

        if (decodeUtf8(value, i, length))
        {
            result.append(value.substr(i, length));
            i += length - 1;
        }
        else
        {
            result += fmt::format("\\u{:04X}", uint32_t(byte));
        }
    }

    return result;
}

static void writeReport(const Options& options, const std::vector<ShaderJob>& jobs, const std::vector<ShaderResult>& results,
    uint32_t failedShaders, uint32_t shadersWithWarnings, const std::vector<std::string>& runWarnings,
    uint32_t decompressedArchives)
{
    if (options.report.empty())
        return;

    StringBuffer report;
    report.println("{{");
    report.println("  \"input\": \"{}\",", jsonEscape(options.input));
    report.println("  \"output\": \"{}\",", jsonEscape(options.output));
    report.println("  \"include\": \"{}\",", jsonEscape(options.include));
    report.println("  \"totalShaders\": {},", jobs.size());
    report.println("  \"successfulShaders\": {},", jobs.size() - failedShaders);
    report.println("  \"failedShaders\": {},", failedShaders);
    report.println("  \"shadersWithWarnings\": {},", shadersWithWarnings);
    report.println("  \"decompressedArchives\": {},", decompressedArchives);

    report.print("  \"warnings\": [");
    for (size_t i = 0; i < runWarnings.size(); i++)
        report.print("{}\n    \"{}\"", i == 0 ? "" : ",", jsonEscape(runWarnings[i]));
    report.println(runWarnings.empty() ? "]," : "\n  ],");

    report.println("  \"shaders\": [");

    bool firstShader = true;
    for (size_t i = 0; i < jobs.size(); i++)
    {
        const auto& result = results[i];
        if (result.succeeded() && result.warnings.empty())
            continue;

        if (!firstShader)
            report.println("    }},");

        firstShader = false;

        report.println("    {{");
        report.println("      \"hash\": \"0x{:X}\",", jobs[i].hash);
        report.println("      \"path\": \"{}\",", jsonEscape(jobs[i].sourcePath));
        report.println("      \"stage\": \"{}\",", result.isPixelShader ? "PS" : "VS");
        report.println("      \"status\": \"{}\",", result.succeeded() ? "warning" : "failed");

        report.print("      \"errors\": [");
        for (size_t j = 0; j < result.errors.size(); j++)
            report.print("{}\"{}\"", j == 0 ? "" : ", ", jsonEscape(indentLines(result.errors[j], "")));
        report.println("],");

        report.print("      \"warnings\": [");
        for (size_t j = 0; j < result.warnings.size(); j++)
            report.print("{}\"{}\"", j == 0 ? "" : ", ", jsonEscape(result.warnings[j]));
        report.println("],");

        report.println("      \"hlslDump\": \"{}\"", jsonEscape(result.hlslDumpPath));
    }

    if (!firstShader)
        report.println("    }}");

    report.println("  ]");
    report.println("}}");

    writeAllBytes(options.report.c_str(), report.out.data(), report.out.size());
}

static void dumpFailedShader(ShaderResult& result, const ShaderJob& job, const Options& options)
{
    if (options.dumpFailedShaders.empty() || result.succeeded() || result.hlsl.empty())
        return;

    std::string fileName = fmt::format("0x{:X}_{}.hlsl", job.hash, result.isPixelShader ? "ps" : "vs");
    std::string path = (std::filesystem::path(options.dumpFailedShaders) / fileName).string();

    StringBuffer dump;
    dump.println("// XenosRecomp failed to recompile this shader. The generated HLSL is kept here for debugging.");
    dump.println("// Source: {}", job.sourcePath);
    dump.println("// Hash: 0x{:X}", job.hash);
    dump.println("// Stage: {}", result.isPixelShader ? "pixel shader" : "vertex shader");
    dump.println("//");
    dump.println("// Errors:");

    for (const auto& error : result.errors)
    {
        forEachLine(error, [&](const std::string_view line) { dump.println("// {}", line); });
    }

    dump.println("");

    try
    {
        std::string contents = dump.out + result.hlsl;
        writeAllBytes(path.c_str(), contents.data(), contents.size());
        result.hlslDumpPath = path;
    }
    catch (const std::exception& error)
    {
        logError(error.what());
    }

    result.hlsl.clear();
}

static void printFailures(const std::vector<ShaderJob>& jobs, const std::vector<ShaderResult>& results, uint32_t maxEntries = 25)
{
    uint32_t totalFailures = 0;
    uint32_t printed = 0;

    for (const auto& result : results)
    {
        if (!result.succeeded())
            totalFailures++;
    }

    for (size_t i = 0; i < jobs.size(); i++)
    {
        const auto& result = results[i];
        if (result.succeeded())
            continue;

        if (printed == maxEntries)
        {
            logLine("...and {} more failed shaders, see the report for the full list.", totalFailures - printed);
            break;
        }

        printed++;

        logLine("");
        logLine("[{}] {}", jobs[i].sourcePath, result.isPixelShader ? "pixel shader" : "vertex shader");
        logLine("    hash: 0x{:X}", jobs[i].hash);

        for (const auto& error : result.errors)
        {
            forEachLine(error, [&](const std::string_view line) { logLine("    {}", line); });
        }

        if (!result.hlslDumpPath.empty())
            logLine("    generated HLSL: {}", result.hlslDumpPath);
    }
}

// Decodes the parts of a shader archive. Depending on the game, the parts of a split archive are
// either complete compressed files of their own or the pieces of a single compressed stream that
// was cut in half, so both layouts are tried before the data is handed over as it is.
static std::vector<uint8_t> decodeArchiveParts(const std::vector<std::vector<uint8_t>>& parts, std::string& status)
{
    std::vector<uint8_t> joined;

    for (const auto& part : parts)
        joined.insert(joined.end(), part.begin(), part.end());

    bool allContainers = !parts.empty();
    bool anyContainer = false;

    for (const auto& part : parts)
    {
        const bool isContainer = XCompressContainer::isContainer(part.data(), part.size());
        allContainers &= isContainer;
        anyContainer |= isContainer;
    }

    // Every part is a compressed file of its own.
    if (allContainers)
    {
        std::vector<uint8_t> result;
        bool complete = true;
        bool failed = false;

        for (size_t i = 0; i < parts.size() && !failed; i++)
        {
            try
            {
                auto decoded = XCompressContainer::decompress(parts[i].data(), parts[i].size());
                complete &= decoded.complete;
                result.insert(result.end(), decoded.data.begin(), decoded.data.end());
            }
            catch (const std::exception& error)
            {
                failed = true;
                status = error.what();
            }
        }

        if (!failed)
        {
            status = complete
                ? fmt::format("decompressed {} part(s) (Xbox 360 compression)", parts.size())
                : fmt::format("decompressed {} part(s), the last one is incomplete", parts.size());
            return result;
        }
    }

    // The parts may be a single compressed stream that was cut in half.
    if (XCompressContainer::isContainer(joined.data(), joined.size()))
    {
        try
        {
            auto decoded = XCompressContainer::decompress(joined.data(), joined.size());

            if (decoded.complete)
            {
                status = "decompressed 1 part (Xbox 360 compression)";
                return decoded.data;
            }
        }
        catch (const std::exception&)
        {
            // Fall through to the plain data below.
        }
    }

    // Nothing could be decoded. Hand the data over as it is, which covers both uncompressed
    // archives and archives that use a compression the recompiler does not know.
    if (status.empty() || status.find("decompressed") == 0)
        status = anyContainer ? "the Xbox 360 compressed data could not be decoded" : "uncompressed data";

    return joined;
}

static int recompileShaderCache(const Options& options, const std::string_view include)
{
    std::vector<std::vector<uint8_t>> files;
    std::map<XXH64_hash_t, RecompiledShader> shaders;
    std::vector<RecompiledShader*> shaderStorages;
    std::vector<ShaderJob> jobs;
    std::vector<ShaderResult> results;

    logLine("Scanning '{}' for shaders...", options.input);

    std::error_code errorCode;
    std::filesystem::recursive_directory_iterator iterator(options.input, std::filesystem::directory_options::skip_permission_denied, errorCode);

    if (errorCode)
        throw std::runtime_error(fmt::format("Failed to enumerate '{}': {}", options.input, errorCode.message()));

    std::map<XXH64_hash_t, std::string> shaderFilenames;
    std::filesystem::recursive_directory_iterator end;

    // The parts of a split archive are collected first, so that they can be decoded together.
    struct ArchivePart
    {
        std::filesystem::path path;
        uint32_t index = 0;
    };

    std::map<std::string, std::vector<ArchivePart>> archives;
    std::vector<std::string> archiveOrder;

    for (; iterator != end; iterator.increment(errorCode))
    {
        if (errorCode)
        {
            errorCode.clear();
            continue;
        }

        if (iterator->is_directory(errorCode))
            continue;

        const auto archive = splitArchiveName(iterator->path());

        if (archives.find(archive.key) == archives.end())
            archiveOrder.emplace_back(archive.key);

        archives[archive.key].push_back({ iterator->path(), archive.partIndex });
    }

    uint32_t decompressedArchives = 0;
    uint32_t failedArchives = 0;

    for (const auto& key : archiveOrder)
    {
        auto& parts = archives[key];

        std::sort(parts.begin(), parts.end(), [](const ArchivePart& left, const ArchivePart& right)
        {
            return left.index < right.index;
        });

        std::vector<std::vector<uint8_t>> partData;
        bool readable = true;

        for (const auto& part : parts)
        {
            try
            {
                size_t partSize = 0;
                auto data = readAllBytes(part.path.string().c_str(), partSize);
                partData.emplace_back(data.get(), data.get() + partSize);
            }
            catch (const std::exception& error)
            {
                logError(error.what());
                readable = false;
                break;
            }
        }

        if (!readable || partData.empty())
            continue;

        std::string status;
        std::vector<uint8_t> stream = decodeArchiveParts(partData, status);

        if (stream.empty())
            continue;

        bool foundAny = false;

        forEachShaderContainer(stream.data(), stream.size(), [&](size_t offset, size_t dataSize)
        {
            auto shaderContainer = reinterpret_cast<const ShaderContainer*>(stream.data() + offset);
            XXH64_hash_t hash = XXH3_64bits(shaderContainer, dataSize);
            auto shader = shaders.try_emplace(hash);

            if (shader.second)
            {
                shader.first->second.data = stream.data() + offset;
                shader.first->second.dataSize = dataSize;
                foundAny = true;
                shaderFilenames[hash] = parts.front().path.string();
            }
        });

        if (foundAny)
        {
            if (status.find("decompressed") == 0)
                decompressedArchives++;

            logLine("Found shaders in '{}' ({} bytes): {}", key, stream.size(), status);
            files.emplace_back(std::move(stream));
        }
        else if (status.find("decompressed") == 0)
        {
            decompressedArchives++;
        }
        else if (parts.size() > 1 || XCompressContainer::isContainer(partData.front().data(), partData.front().size()))
        {
            failedArchives++;
            logLine("No shaders found in '{}': {}", key, status);
        }
    }

    if (decompressedArchives != 0 || failedArchives != 0)
        logLine("Decoded {} Xbox 360 compressed file(s), {} could not be decoded.", decompressedArchives, failedArchives);

    jobs.reserve(shaders.size());
    shaderStorages.reserve(shaders.size());

    for (auto& [hash, shader] : shaders)
    {
        ShaderJob job{};
        job.hash = hash;
        job.data = shader.data;
        job.dataSize = shader.dataSize;
        job.sourcePath = shaderFilenames[hash];
        jobs.emplace_back(std::move(job));
        shaderStorages.emplace_back(&shader);
    }

    if (jobs.empty())
        throw std::runtime_error(fmt::format("No shader containers were found in '{}'.", options.input));

    logLine("Found {} unique shaders in '{}'", jobs.size(), options.input);

    results.resize(jobs.size());

    uint32_t numThreads = options.threadCount;
    if (numThreads == 0)
        numThreads = std::max(std::thread::hardware_concurrency(), 1u);

    numThreads = std::min<uint32_t>(numThreads, std::max<uint32_t>(uint32_t(jobs.size()), 1u));

    logLine("Recompiling shaders with {} threads", numThreads);

    std::atomic<uint32_t> progress = 0;
    std::atomic<size_t> nextJob = 0;

    auto worker = [&]
    {
        while (true)
        {
            const size_t index = nextJob++;
            if (index >= jobs.size())
                return;

            recompileShader(jobs[index], results[index], *shaderStorages[index], include, progress, uint32_t(jobs.size()));
        }
    };

    std::vector<std::thread> threads;
    threads.reserve(numThreads);

    for (uint32_t i = 0; i < numThreads; i++)
        threads.emplace_back(worker);

    for (auto& thread : threads)
        thread.join();

    uint32_t failedShaders = 0;
    uint32_t shadersWithWarnings = 0;
    uint32_t warningCount = 0;

    for (size_t i = 0; i < jobs.size(); i++)
    {
        auto& result = results[i];

        if (!result.succeeded())
            failedShaders++;

        if (!result.warnings.empty())
        {
            shadersWithWarnings++;
            warningCount += uint32_t(result.warnings.size());
        }

        dumpFailedShader(result, jobs[i], options);
    }

    logLine("");
    logLine("Recompiled {} shaders in {:.1f}s: {} succeeded, {} failed, {} warnings in {} shaders.",
        jobs.size(), std::chrono::duration<float>(std::chrono::steady_clock::now() - g_startTime).count(),
        jobs.size() - failedShaders, failedShaders, warningCount, shadersWithWarnings);

    if (failedShaders != 0)
    {
        logLine("");
        logLine("Failed shaders:");
        printFailures(jobs, results);
    }

    if (warningCount != 0)
    {
        std::map<std::string, uint32_t> warningCounts;

        for (const auto& result : results)
        {
            for (const auto& warning : result.warnings)
                warningCounts[warning]++;
        }

        logLine("");
        logLine("Warnings:");

        uint32_t printedWarnings = 0;
        for (const auto& [warning, count] : warningCounts)
        {
            if (printedWarnings++ == 10)
            {
                logLine("    ...and {} more, see the report for the full list.", warningCounts.size() - 10);
                break;
            }

            logLine("    [{} shaders] {}", count, warning);
        }
    }

    logLine("Creating shader cache...");

    StringBuffer f;
    f.println("#include \"shader_cache.h\"");
    f.println("ShaderCacheEntry g_shaderCacheEntries[] = {{");

    std::vector<uint8_t> dxil;
    std::vector<uint8_t> spirv;
    std::vector<uint8_t> air;

    for (size_t i = 0; i < jobs.size(); i++)
    {
        const XXH64_hash_t hash = jobs[i].hash;
        RecompiledShader& shader = *shaderStorages[i];

        const std::string& fullFilename = shaderFilenames[hash];
        std::string filename = fullFilename;
        size_t shaderPos = filename.find("shader");
        if (shaderPos != std::string::npos) {
            filename = filename.substr(shaderPos);
            // Prevent bad escape sequences in Windows shader path.
            std::replace(filename.begin(), filename.end(), '\\', '/');
        }
        f.println("\t{{ 0x{:X}, {}, {}, {}, {}, {}, {}, {}, \"{}\" }},",
            hash, dxil.size(), (shader.dxil != nullptr) ? shader.dxil->GetBufferSize() : 0,
            spirv.size(), shader.spirv.size(), air.size(), shader.air.size(), shader.specConstantsMask, filename);

        if (shader.dxil != nullptr)
        {
            dxil.insert(dxil.end(), reinterpret_cast<uint8_t *>(shader.dxil->GetBufferPointer()),
                reinterpret_cast<uint8_t *>(shader.dxil->GetBufferPointer()) + shader.dxil->GetBufferSize());
        }

    #ifdef XENOS_RECOMP_AIR
        air.insert(air.end(), shader.air.begin(), shader.air.end());
    #endif

        spirv.insert(spirv.end(), shader.spirv.begin(), shader.spirv.end());
    }

    f.println("}};");

    logLine("Compressing DXIL cache...");

    int level = ZSTD_maxCLevel();

#ifdef XENOS_RECOMP_DXIL
    std::vector<uint8_t> dxilCompressed(ZSTD_compressBound(dxil.size()));
    dxilCompressed.resize(ZSTD_compress(dxilCompressed.data(), dxilCompressed.size(), dxil.data(), dxil.size(), level));

    f.print("const uint8_t g_compressedDxilCache[] = {{");

    for (auto data : dxilCompressed)
        f.print("{},", data);

    f.println("}};");
    f.println("const size_t g_dxilCacheCompressedSize = {};", dxilCompressed.size());
    f.println("const size_t g_dxilCacheDecompressedSize = {};", dxil.size());
#endif

#ifdef XENOS_RECOMP_AIR
    logLine("Compressing AIR cache...");

    std::vector<uint8_t> airCompressed(ZSTD_compressBound(air.size()));
    airCompressed.resize(ZSTD_compress(airCompressed.data(), airCompressed.size(), air.data(), air.size(), level));

    f.print("const uint8_t g_compressedAirCache[] = {{");

    for (auto data : airCompressed)
        f.print("{},", data);

    f.println("}};");
    f.println("const size_t g_airCacheCompressedSize = {};", airCompressed.size());
    f.println("const size_t g_airCacheDecompressedSize = {};", air.size());
#endif

    logLine("Compressing SPIRV cache...");

    std::vector<uint8_t> spirvCompressed(ZSTD_compressBound(spirv.size()));
    spirvCompressed.resize(ZSTD_compress(spirvCompressed.data(), spirvCompressed.size(), spirv.data(), spirv.size(), level));

    f.print("const uint8_t g_compressedSpirvCache[] = {{");

    for (auto data : spirvCompressed)
        f.print("{},", data);

    f.println("}};");

    f.println("const size_t g_spirvCacheCompressedSize = {};", spirvCompressed.size());
    f.println("const size_t g_spirvCacheDecompressedSize = {};", spirv.size());
    f.println("const size_t g_shaderCacheEntryCount = {};", shaders.size());

    writeAllBytes(options.output.c_str(), f.out.data(), f.out.size());

    for (auto& [hash, shader] : shaders)
    {
        if (shader.dxil != nullptr)
            shader.dxil->Release();
    }

    std::vector<std::string> runWarnings;
    if (g_unsignedDxil)
    {
        runWarnings.emplace_back("The DXIL blobs were not signed. This happens when the recompiler is not built with "
            "the DXIL validation library, which is only available for Windows, and may cause the driver to reject "
            "the shaders at runtime.");
        logLine("");
        logLine("warning: {}", runWarnings.back());
    }

    writeReport(options, jobs, results, failedShaders, shadersWithWarnings, runWarnings, decompressedArchives);

    if (failedShaders != 0 && !options.allowFailures)
    {
        logError(fmt::format("{} of {} shaders failed to recompile. Re-run with --allow-failures to ignore this.",
            failedShaders, jobs.size()));
        return 1;
    }

    logLine("Done. Shader cache written to '{}'.", options.output);
    return 0;
}

int main(int argc, char** argv)
{
    g_startTime = std::chrono::steady_clock::now();

    // Line buffer stdout so that messages printed by the worker threads appear in the
    // logs in the same order as everything printed on stderr.
    setvbuf(stdout, nullptr, _IOLBF, 0);

    Options options;
    if (!parseArgs(argc, argv, options))
        return 2;

    std::string_view include;
    std::vector<uint8_t> includeStorage;

    try
    {
        size_t includeSize = 0;
        auto includeData = readAllBytes(options.include.c_str(), includeSize);
        includeStorage.assign(includeData.get(), includeData.get() + includeSize);
        include = std::string_view(reinterpret_cast<const char*>(includeStorage.data()), includeStorage.size());
    }
    catch (const std::exception& error)
    {
        logError(error.what());
        return 2;
    }

    try
    {
        if (std::filesystem::is_directory(options.input))
            return recompileShaderCache(options, include);

        ShaderRecompiler recompiler;
        size_t fileSize = 0;
        auto fileData = readAllBytes(options.input.c_str(), fileSize);

        // Directories are scanned for shaders because the files of a game are archives that hold
        // more than one of them, so a single file may contain several shaders as well.
        std::vector<uint8_t> decompressed;
        const uint8_t* data = fileData.get();
        size_t dataSize = fileSize;

        if (XCompressContainer::isContainer(fileData.get(), fileSize))
        {
            auto result = XCompressContainer::decompress(fileData.get(), fileSize);
            decompressed = std::move(result.data);
            data = decompressed.data();
            dataSize = decompressed.size();

            logLine("Decompressed '{}' from {} to {} bytes with Xbox 360 compression.",
                options.input, fileSize, dataSize);
        }

        size_t shaderOffset = 0;
        size_t shaderSize = 0;
        uint32_t shaderCount = 0;

        forEachShaderContainer(data, dataSize, [&](size_t offset, size_t size)
        {
            if (shaderCount++ == 0)
            {
                shaderOffset = offset;
                shaderSize = size;
            }
        });

        if (shaderCount == 0)
            throw std::runtime_error(fmt::format("No shader was found in '{}'.", options.input));

        if (shaderCount > 1)
            logLine("'{}' holds {} shaders, only the first one is recompiled. Use the directory input to recompile all of them.",
                options.input, shaderCount);

        recompiler.recompile(data + shaderOffset, shaderSize, include);
        writeAllBytes(options.output.c_str(), recompiler.out.data(), recompiler.out.size());
        logLine("Wrote the recompiled shader to '{}'.", options.output);

        for (const auto& warning : recompiler.warnings)
            logLine("warning: {}", warning);

        return 0;
    }
    catch (const ShaderRecompileError& error)
    {
        logError(error.what());
        return 1;
    }
    catch (const std::exception& error)
    {
        logError(error.what());
        return 2;
    }
}
