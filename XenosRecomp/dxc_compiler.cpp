#include "dxc_compiler.h"

DxcCompiler::DxcCompiler()
{
    DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&dxcCompiler));
    // dxcCompiler stays null when the DirectX Shader Compiler is unavailable, which is
    // reported by the caller instead of terminating the process here.
}

DxcCompiler::~DxcCompiler()
{
    if (dxcCompiler != nullptr)
        dxcCompiler->Release();
}

// Makes DXC report diagnostics with the name of the shader that is being compiled
// instead of the generic "hlsl.hlsl" name it uses for in-memory sources.
static std::string makeSourceDirective(const std::string& sourceName)
{
    if (sourceName.empty())
        return {};

    std::string name = sourceName;
    for (char& c : name)
    {
        if (c == '\\' || c == '"' || c == '\n' || c == '\r')
            c = '/';
    }

    return fmt::format("#line 2 \"{}\"\n", name);
}

DxcCompileResult DxcCompiler::compile(const std::string& shaderSource, bool compilePixelShader, bool compileLibrary, bool compileSpirv,
    const std::string& sourceName)
{
    DxcCompileResult compileResult {};

    if (dxcCompiler == nullptr)
    {
        compileResult.errorCode = E_FAIL;
        compileResult.errors = "The DirectX Shader Compiler is not available.";
        return compileResult;
    }

    std::string source = makeSourceDirective(sourceName) + shaderSource;

    DxcBuffer buffer{};
    buffer.Ptr = source.c_str();
    buffer.Size = source.size();

    const wchar_t* args[32]{};
    uint32_t argCount = 0;

    const wchar_t* target = nullptr;
    if (compileLibrary)
    {
        // Libraries are only supported by DXIL; SPIR-V has no library profile.
        target = L"-T lib_6_3";
    }
    else
    {
        if (compilePixelShader)
            target = L"-T ps_6_0";
        else
            target = L"-T vs_6_0";
    }

    if (!compileLibrary)
    {
        args[argCount++] = L"-E shaderMain";
    }

    args[argCount++] = target;
    args[argCount++] = L"-HV 2021";
    args[argCount++] = L"-all-resources-bound";

    if (compileSpirv)
    {
        args[argCount++] = L"-spirv";
        args[argCount++] = L"-fvk-use-dx-layout";

        if (!compilePixelShader)
            args[argCount++] = L"-fvk-invert-y";
    }
    else
    {
        args[argCount++] = L"-Wno-ignored-attributes";
        args[argCount++] = L"-Qstrip_reflect";
    }

    args[argCount++] = L"-Qstrip_debug";

#ifdef UNLEASHED_RECOMP
    args[argCount++] = L"-DUNLEASHED_RECOMP";
#endif
#ifdef MARATHON_RECOMP
    args[argCount++] = L"-DMARATHON_RECOMP";
#endif

    IDxcResult* result = nullptr;
    HRESULT hr = dxcCompiler->Compile(&buffer, args, argCount, nullptr, IID_PPV_ARGS(&result));

    if (FAILED(hr) || result == nullptr)
    {
        compileResult.errorCode = hr;
        compileResult.errors = fmt::format("The DirectX Shader Compiler failed to run (0x{:08X}).", uint32_t(hr));

        if (result != nullptr)
            result->Release();

        return compileResult;
    }

    HRESULT status = E_FAIL;
    hr = result->GetStatus(&status);

    if (SUCCEEDED(hr) && SUCCEEDED(status))
    {
        hr = result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&compileResult.object), nullptr);

        if (FAILED(hr) || compileResult.object == nullptr)
        {
            compileResult.object = nullptr;
            compileResult.errorCode = hr;
            compileResult.errors = "The DirectX Shader Compiler did not return a shader object.";
        }
    }
    else
    {
        compileResult.errorCode = FAILED(hr) ? hr : status;
    }

    if (result->HasOutput(DXC_OUT_ERRORS))
    {
        IDxcBlobUtf8* errors = nullptr;
        if (SUCCEEDED(result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errors), nullptr)) && errors != nullptr)
        {
            const char* text = errors->GetStringPointer();
            size_t textSize = errors->GetStringLength();

            if (text != nullptr && textSize != 0)
            {
                compileResult.errors.assign(text, textSize);

                // DXC terminates diagnostics with a newline, which would otherwise produce
                // empty lines when the message is embedded into reports.
                while (!compileResult.errors.empty() &&
                    (compileResult.errors.back() == '\n' || compileResult.errors.back() == '\r'))
                {
                    compileResult.errors.pop_back();
                }
            }

            errors->Release();
        }
    }

    result->Release();

    if (compileResult.object == nullptr && compileResult.errors.empty())
        compileResult.errors = "The DirectX Shader Compiler failed without producing any diagnostics.";

    return compileResult;
}
