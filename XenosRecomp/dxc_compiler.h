#pragma once

#include <string>

struct DxcCompileResult
{
    // Owned by the caller, release with Release(). Null when the compilation failed.
    IDxcBlob* object = nullptr;
    // Diagnostics produced by DXC. Empty when the compilation succeeded.
    std::string errors;
    // The result of the compilation call itself. Compilation failures are usually
    // reported through errors instead of this value.
    HRESULT errorCode = S_OK;

    bool succeeded() const { return object != nullptr; }
};

struct DxcCompiler
{
    IDxcCompiler3* dxcCompiler = nullptr;

    DxcCompiler();
    ~DxcCompiler();

    bool isValid() const { return dxcCompiler != nullptr; }

    DxcCompileResult compile(const std::string& shaderSource, bool compilePixelShader, bool compileLibrary, bool compileSpirv,
        const std::string& sourceName = {});
};
