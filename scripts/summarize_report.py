#!/usr/bin/env python3
"""Renders the XenosRecomp JSON report as Markdown for the GitHub job summary.

Usage: summarize_report.py <report.json> [--log <log file>] > summary.md
"""

import argparse
import json
import os
import sys


def format_failures(report, limit=10):
    lines = []

    failed = [shader for shader in report.get("shaders", []) if shader.get("status") == "failed"]
    for shader in failed[:limit]:
        lines.append("- `{}` ({}, hash `{}`)".format(shader.get("path", "?"), shader.get("stage", "?"),
                                                     shader.get("hash", "?")))

        for error in shader.get("errors", []):
            for line in error.strip().splitlines()[:6]:
                lines.append("  ```\n  {}\n  ```".format(line.strip()))

        if shader.get("hlslDump"):
            lines.append("  - generated HLSL: `{}`".format(shader["hlslDump"]))

    if len(failed) > limit:
        lines.append("- ...and {} more, see the report artifact.".format(len(failed) - limit))

    return lines


def format_warnings(report, limit=10):
    lines = []
    seen = {}
    order = []

    for warning in report.get("warnings", []):
        if warning not in seen:
            seen[warning] = 0
            order.append(warning)
        seen[warning] += 1

    for shader in report.get("shaders", []):
        for warning in shader.get("warnings", []):
            if warning not in seen:
                seen[warning] = 0
                order.append(warning)
            seen[warning] += 1

    for warning in order[:limit]:
        count = seen[warning]
        suffix = " ({} times)".format(count) if count > 1 else ""
        lines.append("- {}{}".format(warning, suffix))

    if len(order) > limit:
        lines.append("- ...and {} more warnings.".format(len(order) - limit))

    if not order:
        lines.append("- None.")

    return lines


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("report", help="Path to the report written by XenosRecomp")
    parser.add_argument("--log", default=None, help="Path to the full recompiler log")
    args = parser.parse_args()

    if not os.path.exists(args.report):
        print("## Shader recompilation")
        print()
        print("The recompiler did not write a report, which means it failed before finishing.")
        print("Check the step log for the error that was printed above.")
        return 0

    with open(args.report, "r", encoding="utf-8") as f:
        report = json.load(f)

    total = report.get("totalShaders", 0)
    failed = report.get("failedShaders", 0)
    succeeded = report.get("successfulShaders", total - failed)

    print("## Shader recompilation")
    print()
    print("| Shaders | Count |")
    print("| --- | --- |")
    print("| Total | {} |".format(total))
    print("| Recompiled | {} |".format(succeeded))
    print("| Failed | {} |".format(failed))
    print("| With warnings | {} |".format(report.get("shadersWithWarnings", 0)))
    print()

    if failed == 0:
        print("All shaders were recompiled successfully.")
    else:
        print("### Failed shaders")
        print()
        for line in format_failures(report):
            print(line)
        print()
        print("These shaders are missing from the generated shader cache. Download the "
              "`failed-shaders` artifact, which contains the generated HLSL for every failed "
              "shader together with the compiler errors, and fix the shaders or the recompiler.")

    print()
    print("### Warnings")
    print()
    for line in format_warnings(report):
        print(line)

    if args.log:
        print()
        print("Full recompiler log: `{}`".format(os.path.basename(args.log)))

    return 0


if __name__ == "__main__":
    sys.exit(main())
