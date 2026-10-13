import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import xml.etree.ElementTree as ET

TESTS = (
    "agc_shader_memory",
    "agc_shader_disk_cache",
    "agc_driver_bda_device",
    "agc_driver_prepared_shaders",
    "agc_driver_prepared_graphics",
    "agc_driver_buffer_descriptor_runtime",
)
VARIANTS = (
    ("default", "0", "0", None),
    ("bda", "1", "0", None),
    ("bounded", "0", "1", None),
    ("combined", "1", "1", "balanced"),
)


def run(build: Path, output: Path, build_jobs: int) -> None:
    output.mkdir(parents=True, exist_ok=True)
    listing = subprocess.run(
        ["ctest", "--test-dir", str(build), "--show-only=json-v1"],
        check=True, text=True, capture_output=True, timeout=60,
    )
    registered = {test["name"] for test in json.loads(listing.stdout)["tests"]}
    missing = set(TESTS) - registered
    if missing:
        raise RuntimeError(f"Required tests are not registered: {sorted(missing)}")
    command = ["cmake", "--build", str(build), "--parallel", str(build_jobs),
               "--target", *(test + "_tests" for test in TESTS)]
    subprocess.run(command, check=True, timeout=1800)
    expression = "^(" + "|".join(map(re.escape, TESTS)) + ")$"
    summary = []
    for name, bda, budget, optimizer in VARIANTS:
        env = os.environ.copy()
        for variable in ("APS5_BDA_BYTE_READS", "APS5_SPIRV_OPT", "APS5_PROFILE_SHADER_CACHE"):
            env.pop(variable, None)
        env.update(APS5_BDA_RANGE_CACHE=bda, APS5_SPECIALIZATION_CACHE_MIB=budget,
                   ANYPS5_REQUIRE_VULKAN="1", ANYPS5_NO_SHADER_CACHE="0",
                   ANYPS5_SHADER_CACHE_DIR=str(output / name / "shader-cache"))
        if optimizer is not None:
            env["APS5_SPIRV_OPT"] = optimizer
        junit = output / f"{name}.xml"
        junit.unlink(missing_ok=True)
        command = ["ctest", "--test-dir", str(build), "-R", expression,
                   "--output-on-failure", "--verbose", "--no-tests=error",
                   "--timeout", "180", "--output-junit", str(junit)]
        with (output / f"{name}.log").open("w", encoding="utf-8") as log:
            result = subprocess.run(command, env=env, stdout=log, stderr=subprocess.STDOUT,
                                    text=True, timeout=1500)
        if not junit.is_file():
            raise RuntimeError(f"{name}: CTest did not produce a JUnit report")
        cases = ET.parse(junit).getroot().findall(".//testcase")
        bad = [case.get("name") for case in cases
               if case.find("skipped") is not None or case.find("failure") is not None
               or case.find("error") is not None or case.get("status") in ("notrun", "disabled")]
        observed = [case.get("name") for case in cases]
        if result.returncode != 0 or bad or len(cases) != len(TESTS) or set(observed) != set(TESTS):
            raise RuntimeError(f"{name}: integration failed or skipped: exit={result.returncode}, cases={observed}, bad={bad}")
        log_text = (output / f"{name}.log").read_text(encoding="utf-8")
        partial_bda = "BDA read and multi-dword write execution not tested" in log_text
        summary.append(dict(variant=name, tests=len(cases), passed=True,
                            bda_read_and_wide_write_executed=not partial_bda))
        (output / "summary.json").write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
        print(f"{name}: {len(cases)} passed; full BDA execution={not partial_bda}", flush=True)


def main() -> int:
    parser = argparse.ArgumentParser(description="Build and run actual shader regressions in four isolated processes/configurations")
    parser.add_argument("--build-dir", type=Path, default=Path("build"))
    parser.add_argument("--output-dir", type=Path)
    parser.add_argument("--jobs", type=int, default=2)
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error("--jobs must be positive")
    build = args.build_dir.resolve()
    output = (args.output_dir or build / "shader-resource-reports").resolve()
    try:
        run(build, output, args.jobs)
    except (OSError, ValueError, KeyError, RuntimeError, subprocess.SubprocessError, ET.ParseError) as error:
        print(str(error), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
