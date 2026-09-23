#include "air_compiler.h"

#include <fstream>
#include <iterator>
#include <spawn.h>
#include <unistd.h>

#include <stdexcept>

struct TemporaryPath
{
    const std::string path;

    explicit TemporaryPath(std::string_view path) : path(path) {}

    ~TemporaryPath()
    {
        unlink(path.c_str());
    }
};

static int executeCommand(const char** argv)
{
    pid_t pid;
    if (posix_spawn(&pid, argv[0], nullptr, nullptr, const_cast<char**>(argv), nullptr) != 0)
        return -1;

    int status;
    if (waitpid(pid, &status, 0) == -1)
        return -1;

    return status;
}

std::vector<uint8_t> AirCompiler::compile(const std::string& shaderSource)
{
    // Save source to a location on disk for the compiler to read.
    char sourcePathTemplate[PATH_MAX] = "/tmp/xenos_metal_XXXXXX.metal";
    const int sourceFd = mkstemps(sourcePathTemplate, 6);
    if (sourceFd == -1)
        throw std::runtime_error(fmt::format("Failed to create temporary file for shader source: {}", strerror(errno)));

    const TemporaryPath sourcePath(sourcePathTemplate);
    const TemporaryPath irPath(sourcePath.path + ".ir");
    const TemporaryPath metalLibPath(sourcePath.path + ".metallib");

    const ssize_t sourceWritten = write(sourceFd, shaderSource.data(), shaderSource.size());
    close(sourceFd);
    if (sourceWritten < 0)
        throw std::runtime_error(fmt::format("Failed to write shader source to disk: {}", strerror(errno)));

    const char* compileCommand[] = {
        "/usr/bin/xcrun", "-sdk", "macosx", "metal", "-o", irPath.path.c_str(), "-c", sourcePath.path.c_str(), "-Wno-unused-variable", "-frecord-sources", "-gline-tables-only", "-fmetal-math-mode=relaxed", "-D__air__",
#ifdef UNLEASHED_RECOMP
        "-DUNLEASHED_RECOMP",
#endif
#ifdef MARATHON_RECOMP
        "-DMARATHON_RECOMP",
#endif
        nullptr
    };
    if (const int compileStatus = executeCommand(compileCommand); compileStatus != 0)
        throw std::runtime_error(fmt::format("The Metal compiler exited with status {} for the following source:\n{}", compileStatus, shaderSource));

    const char* linkCommand[] = { "/usr/bin/xcrun", "-sdk", "macosx", "metallib", "-o", metalLibPath.path.c_str(), irPath.path.c_str(), nullptr };
    if (const int linkStatus = executeCommand(linkCommand); linkStatus != 0)
        throw std::runtime_error(fmt::format("The Metal linker exited with status {} for the following source:\n{}", linkStatus, shaderSource));

    std::ifstream libStream(metalLibPath.path, std::ios::binary);
    std::vector<uint8_t> data((std::istreambuf_iterator(libStream)), std::istreambuf_iterator<char>());
    return data;
}
