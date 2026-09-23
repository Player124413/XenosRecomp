#!/usr/bin/env python3
"""Runs XenosRecomp over generated test shaders and checks the results.

The test shaders are built by make_test_shaders.py and cover the container layouts and
control flow constructs that are easy to break, most notably conditional jumps on
boolean constant registers such as b128/b129, which used to be emitted as references to
identifiers that were never declared.

The generated Xbox 360 compressed archives are tested as well, because the shaders of a
game are stored in them as "shader.ar.00" and "shader.ar.01" files.

Usage:
    run_tests.py --tool <path to XenosRecomp> [--include <shader_common.h>] [--work-dir <dir>]

The script returns a non-zero exit code when a check fails.
"""

import argparse
import json
import os
import random
import re
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import make_test_shaders

# Matches an identifier that consists of a "b" followed by digits, which is how the
# recompiler used to reference boolean constant registers.
BARE_BOOLEAN_REGISTER = re.compile(r"(?<![A-Za-z0-9_])b\d+(?![A-Za-z0-9_])")

COMMENT = re.compile(r"//[^\n]*")


class TestFailure(Exception):
    pass


def strip_comments(source):
    source = COMMENT.sub("", source)
    # cbuffer declarations such as "register(b1, space4)" are not boolean registers, so
    # the digit is removed before the check below runs.
    return re.sub(r"\bregister\(b\d+", "register(b#", source)


def check(condition, message):
    if not condition:
        raise TestFailure(message)


def run_tool(arguments, expect_success=True):
    result = subprocess.run(arguments, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    output = result.stdout.decode("utf-8", "replace")

    if expect_success and result.returncode != 0:
        raise TestFailure("XenosRecomp exited with {}:\n{}".format(result.returncode, output))

    return result.returncode, output


def test_single_shaders(tool, include, work_directory, expected):
    """Recompiles every generated shader on its own and inspects the generated HLSL."""
    failures = 0
    hlslByFile = {}

    for entry in expected:
        name = entry["file"]
        path = os.path.join(work_directory, name)
        output_path = os.path.join(work_directory, name + ".hlsl")

        try:
            run_tool([tool, path, output_path, include])

            with open(output_path, "r", encoding="utf-8", errors="replace") as f:
                hlsl = f.read()

            code = strip_comments(hlsl)
            hlslByFile[name] = hlsl

            match = BARE_BOOLEAN_REGISTER.search(code)
            check(match is None, "generated HLSL references the undeclared boolean register '{}'".format(
                match.group(0) if match else ""))

            if "expected_bit" in entry:
                bit = entry["expected_bit"]
                check("(g_Booleans & (1u << {}))".format(bit) in code,
                      "expected a test of packed boolean bit {} in the generated HLSL".format(bit))

            alias = entry.get("cache_alias")

            if alias is not None:
                check(alias in hlslByFile, "the shader '{}' was not recompiled before '{}'".format(alias, name))
                check(hlsl == hlslByFile[alias],
                      "'{}' should recompile into the same shader as '{}', which holds the same shader in an "
                      "Xbox 360 compressed file".format(name, alias))

            if "out_of_range_register" in entry:
                check("outside of the supported packed boolean range" in hlsl,
                      "expected a note about boolean register b{} being out of range".format(
                          entry["out_of_range_register"]))
                check("if (true)" in code or "if (false)" in code,
                      "an out of range boolean register should be resolved to a constant condition")

            print("  ok   {}".format(name))
        except TestFailure as error:
            failures += 1
            print("  FAIL {}: {}".format(name, error))

    return failures


def test_shader_cache(tool, include, work_directory, expected):
    """Recompiles the whole directory and checks the cache and the report."""
    failures = 0
    cache_path = os.path.join(work_directory, "cache", "shader_cache.cpp")
    report_path = os.path.join(work_directory, "cache", "report.json")
    dump_path = os.path.join(work_directory, "cache", "failed")

    try:
        run_tool([tool, work_directory, cache_path, include, "--report", report_path,
                  "--dump-failed", dump_path])

        check(os.path.exists(cache_path), "the shader cache was not written")

        with open(cache_path, "r", encoding="utf-8", errors="replace") as f:
            cache = f.read()

        check("g_shaderCacheEntries" in cache, "the shader cache does not contain any entries")
        cacheEntries = len(set(entry.get("cache_alias", entry["file"]) for entry in expected))

        check("g_shaderCacheEntryCount = {}".format(cacheEntries) in cache,
              "the shader cache does not contain every test shader")
        check("g_compressedDxilCache" in cache or "g_compressedSpirvCache" in cache,
              "the shader cache does not contain any compiled shader data")

        check(os.path.exists(report_path), "the recompilation report was not written")

        with open(report_path, "r", encoding="utf-8") as f:
            report = json.load(f)

        check(report["totalShaders"] == cacheEntries,
              "the report counts {} shaders instead of {}".format(report["totalShaders"], cacheEntries))
        check(report["failedShaders"] == 0, "{} test shaders failed to recompile".format(report["failedShaders"]))

        warnings = [warning for shader in report["shaders"] for warning in shader["warnings"]]
        check(any("conditionally executes" in warning for warning in warnings),
              "the conditional exec warning is missing from the report")
        check(any("outside of the 32-bit packed boolean range" in warning for warning in warnings),
              "the out of range boolean warning is missing from the report")

        if os.path.isdir(dump_path):
            check(not os.listdir(dump_path), "the failed shader directory should be empty")

        print("  ok   shader cache and report")
    except TestFailure as error:
        failures += 1
        print("  FAIL shader cache: {}".format(error))

    return failures


def test_unresolvable_shaders(tool, include, work_directory):
    """Checks that a broken shader is reported instead of crashing the recompiler."""
    failures = 0
    broken_directory = os.path.join(work_directory, "broken")
    os.makedirs(broken_directory, exist_ok=True)

    # A shader container with the magic bytes of a container but a broken constant table
    # offset, which must be reported as a failed shader instead of invalid memory access.
    broken = bytearray(open(os.path.join(work_directory, "ps_bool129.bin"), "rb").read())
    broken[0x10:0x14] = (0x00FFFFFF).to_bytes(4, "big")

    broken_path = os.path.join(broken_directory, "broken.bin")
    with open(broken_path, "wb") as f:
        f.write(broken)

    try:
        code, output = run_tool([tool, broken_directory, os.path.join(broken_directory, "out.cpp"), include],
                                expect_success=False)

        check("Traceback" not in output and "SIGSEGV" not in output,
              "the recompiler crashed instead of reporting the broken shader")
        check(code != 0, "the recompiler should return a failure code for a broken shader")
        print("  ok   broken shader is reported")
    except TestFailure as error:
        failures += 1
        print("  FAIL broken shader: {}".format(error))

    return failures


def test_corrupted_shaders(tool, include, work_directory):
    """Checks that corrupted shader containers and archives are reported instead of crashing.

    Every offset stored in a container is read from untrusted data, so mutations of a valid
    shader are used to make sure that out of bounds offsets are rejected cleanly. Half of the
    files are mutations of an Xbox 360 compressed archive, which exercises the decompressor.
    """
    failures = 0
    corrupted_directory = os.path.join(work_directory, "corrupted")
    os.makedirs(corrupted_directory, exist_ok=True)

    random.seed(0x5EED)

    with open(os.path.join(work_directory, "ps_bool129.bin"), "rb") as f:
        sources = [f.read()]

    with open(os.path.join(work_directory, "xc_bool129.bin"), "rb") as f:
        sources.append(f.read())

    for index in range(32):
        source = sources[index % 2]
        data = bytearray(source)

        for _ in range(random.randint(1, 40)):
            # The magic value of a compressed file is left alone, so that the decompressor
            # is the code that has to deal with the corrupted data.
            data[random.randrange(4 if source is sources[1] else 0, len(data))] = random.randrange(256)

        # Container offsets, the shader offset and the constant table offset are hit as well.
        if index % 4 == 0:
            data[0x10:0x14] = random.randrange(1 << 32).to_bytes(4, "big")
        elif index % 4 == 1:
            data[0x14:0x18] = random.randrange(1 << 32).to_bytes(4, "big")
        elif index % 4 == 2:
            data[0x18:0x1C] = random.randrange(1 << 32).to_bytes(4, "big")
        else:
            del data[random.randrange(4, len(data)):]

        path = os.path.join(corrupted_directory, "corrupted_{:02d}.bin".format(index))
        with open(path, "wb") as f:
            f.write(bytes(data))

    report_path = os.path.join(corrupted_directory, "report.json")

    try:
        code, output = run_tool([tool, corrupted_directory, os.path.join(corrupted_directory, "cache.cpp"),
                                 include, "--report", report_path, "--allow-failures"], expect_success=False)

        check(code == 0, "corrupted shaders made the recompiler exit with {}:\n{}".format(code, output[-2000:]))

        with open(report_path, "r", encoding="utf-8") as f:
            report = json.load(f)

        check(report["totalShaders"] > 0, "no corrupted shader was picked up by the recompiler")
        check(report["failedShaders"] > 0, "corrupted shaders should be reported as failed")
        print("  ok   corrupted shaders are reported")
    except TestFailure as error:
        failures += 1
        print("  FAIL corrupted shaders: {}".format(error))

    return failures


def test_archives(tool, include, work_directory, archives):
    """Checks that the Xbox 360 compressed archives of a game are unpacked before recompiling.

    The shaders of a game are stored in "shader.ar.00" and "shader.ar.01" files, so the
    recompiler has to decompress them and join the parts of an archive that was split in the
    middle of a compression stream.
    """
    failures = 0

    cases = (
        ("split", "archive that was split into two compressed files", 2),
        ("joined", "archive that was split in the middle of the stream", 1),
        ("broken", "archive with a corrupted part", 2),
    )

    for key, description, expectedShaders in cases:
        directory = archives[key]
        cache_path = os.path.join(directory, "out", "shader_cache.cpp")
        report_path = os.path.join(directory, "out", "report.json")
        log_path = os.path.join(directory, "out", "log.txt")

        try:
            code, output = run_tool([tool, directory, cache_path, include, "--report", report_path])

            with open(log_path, "w", encoding="utf-8", errors="replace") as f:
                f.write(output)

            with open(report_path, "r", encoding="utf-8") as f:
                report = json.load(f)

            check(report["failedShaders"] == 0,
                  "{} shader(s) of the {} failed to recompile".format(report["failedShaders"], description))
            check(report["totalShaders"] == expectedShaders,
                  "the {} should hold {} shader(s), but {} were recompiled".format(
                      description, expectedShaders, report["totalShaders"]))
            check(report["decompressedArchives"] > 0,
                  "no Xbox 360 compressed archive was decoded for the {}".format(description))

            if key == "broken":
                check("could not be decoded" in output,
                      "the corrupted part of the {} was not reported".format(description))
                check(code == 0, "the {} made the recompiler exit with {}".format(description, code))
                check(any("cut off" in warning for warning in report["warnings"]),
                      "the archive that is cut off in the middle of a block should be reported as a warning")

            print("  ok   {}".format(description))
        except (TestFailure, OSError, ValueError) as error:
            failures += 1
            print("  FAIL {}: {}".format(description, error))

    return failures


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--tool", required=True, help="Path to the XenosRecomp executable")
    parser.add_argument("--include", default=None, help="Path to shader_common.h")
    parser.add_argument("--work-dir", default=None, help="Directory to generate test data into")
    args = parser.parse_args()

    repository_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    include = args.include or os.path.join(repository_root, "XenosRecomp", "shader_common.h")

    work_directory = args.work_dir or tempfile.mkdtemp(prefix="xenosrecomp-tests-")
    os.makedirs(work_directory, exist_ok=True)

    print("Generating test shaders in {}".format(work_directory))
    expected = make_test_shaders.make_test_shaders(work_directory)

    failures = 0
    failures += test_single_shaders(args.tool, include, work_directory, expected)
    failures += test_shader_cache(args.tool, include, work_directory, expected)
    failures += test_unresolvable_shaders(args.tool, include, work_directory)
    failures += test_corrupted_shaders(args.tool, include, work_directory)
    failures += test_archives(args.tool, include, work_directory, make_test_shaders.write_archives(
        work_directory, open(os.path.join(work_directory, "ps_bool129.bin"), "rb").read(),
        open(os.path.join(work_directory, "vs_bool5.bin"), "rb").read()))

    if args.work_dir is None:
        shutil.rmtree(work_directory, ignore_errors=True)

    if failures != 0:
        print("\n{} test(s) failed".format(failures))
        return 1

    print("\nAll tests passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
