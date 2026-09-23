project(XenosRecomp)

# DXIL produced on other platforms cannot be signed by the DirectX Shader Compiler because
# DXIL.dll only ships for Windows, so only Windows builds sign their shaders and shaders
# built elsewhere may be rejected by the driver.
set(XENOS_RECOMP_DXIL_DEFAULT OFF)
if (WIN32)
    set(XENOS_RECOMP_DXIL_DEFAULT ON)
endif()

option(XENOS_RECOMP_DXIL "Generate DXIL shader cache" ${XENOS_RECOMP_DXIL_DEFAULT})

# Vulkan shader caches are generated on every platform, because the DirectX Shader Compiler ships
# its SPIR-V backend for Windows, Linux and macOS. Turning this off skips the SPIR-V compilation
# and removes the data from the generated shader cache, which halves its size for D3D12 games.
option(XENOS_RECOMP_SPIRV "Generate a SPIR-V shader cache (Vulkan)" ON)

if (APPLE)
    option(XENOS_RECOMP_AIR "Generate Metal AIR shader cache" ON)
endif()

# Compile definitions that describe the target game, for example UNLEASHED_RECOMP or
# MARATHON_RECOMP. Recompilers built by a game project usually set these through
# target_compile_definitions instead.
set(XENOS_RECOMP_EXTRA_DEFINES "" CACHE STRING
    "Additional compile definitions to pass to XenosRecomp (semicolon separated)")

set(SMOLV_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../thirdparty/smol-v/source")
set(LIBMSPACK_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/libmspack")

# The LZX decoder of libmspack, which is used to decode the Xbox 360 compressed files the shaders
# of a game are stored in. It is built as a separate target because the sources are C.
add_library(libmspack STATIC
    "${LIBMSPACK_SOURCE_DIR}/lzxd.c")

target_include_directories(libmspack PUBLIC "${LIBMSPACK_SOURCE_DIR}")

add_executable(XenosRecomp
    constant_table.h
    dxc_compiler.cpp
    dxc_compiler.h
    main.cpp
    pch.h
    shader.h
    shader_code.h
    shader_recompiler.cpp
    shader_recompiler.h
    xcompress.cpp
    xcompress.h
    "${SMOLV_SOURCE_DIR}/smolv.cpp")

target_link_libraries(XenosRecomp PRIVATE libmspack)

target_link_libraries(XenosRecomp PRIVATE
    Microsoft::DirectXShaderCompiler
    xxHash::xxhash
    libzstd_static
    fmt::fmt)

target_include_directories(XenosRecomp PRIVATE ${SMOLV_SOURCE_DIR})

target_precompile_headers(XenosRecomp PRIVATE pch.h)

# The shader structures are defined with anonymous structs inside unions, which is an
# extension supported by MSVC, Clang and GCC.
if (CMAKE_CXX_COMPILER_ID STREQUAL "Clang" OR CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang")
    target_compile_options(XenosRecomp PRIVATE -Wno-switch -Wno-unused-variable -Wno-null-arithmetic -fms-extensions)
elseif (CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_compile_options(XenosRecomp PRIVATE -Wno-switch -Wno-unused-variable -fms-extensions)
endif()

if (WIN32)
    target_compile_definitions(XenosRecomp PRIVATE _CRT_SECURE_NO_WARNINGS)
    add_custom_command(TARGET XenosRecomp POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_RUNTIME_DLLS:XenosRecomp> $<TARGET_FILE_DIR:XenosRecomp>
        COMMAND_EXPAND_LISTS
    )
endif()

if (XENOS_RECOMP_EXTRA_DEFINES)
    target_compile_definitions(XenosRecomp PRIVATE ${XENOS_RECOMP_EXTRA_DEFINES})
endif()

if (XENOS_RECOMP_DXIL)
    target_compile_definitions(XenosRecomp PRIVATE XENOS_RECOMP_DXIL)

    if (TARGET Microsoft::DXIL)
        target_link_libraries(XenosRecomp PRIVATE Microsoft::DXIL)
    else()
        message(WARNING "DXIL support was enabled, but the DXIL validation library is not available on this "
            "platform. The generated DXIL will not be signed and may be rejected by the driver.")
    endif()
endif()

if (XENOS_RECOMP_SPIRV)
    target_compile_definitions(XenosRecomp PRIVATE XENOS_RECOMP_SPIRV)
endif()

if (XENOS_RECOMP_AIR)
    target_compile_definitions(XenosRecomp PRIVATE XENOS_RECOMP_AIR)
    target_sources(XenosRecomp PRIVATE air_compiler.cpp air_compiler.h)
endif()
