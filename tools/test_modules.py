import argparse
from datetime import datetime, timezone
import json
from pathlib import Path
import re
import subprocess
import sys
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]


def discover(catalog):
    modules = {}
    for test in catalog["tests"]:
        properties = {item["name"]: item["value"] for item in test.get("properties", [])}
        labels = properties.get("LABELS", [])
        if "module-functional" not in labels:
            continue
        owners = [label.removeprefix("module:") for label in labels if label.startswith("module:")]
        if len(owners) != 1 or not re.fullmatch(r"[A-Za-z0-9_.-]+", owners[0]):
            raise ValueError(f"{test['name']}: exactly one valid module:<library> label is required")
        if properties.get("DISABLED") or not 0 < properties.get("TIMEOUT", 0) <= 30:
            raise ValueError(f"{test['name']}: an enabled test with TIMEOUT <= 30 is required")
        modules.setdefault(owners[0], []).append(test["name"])
    if not modules:
        raise ValueError("No module-functional tests registered; configure with BUILD_TESTING=ON")
    return modules


def results(path, expected):
    cases = list(ET.parse(path).getroot().iter("testcase"))
    names = [case.get("name") for case in cases]
    if len(names) != len(set(names)) or set(names) != set(expected):
        raise ValueError(f"Test result names do not match selection: expected {expected}, got {names}")
    return [{"name": case.get("name"), "seconds": float(case.get("time", "0")),
             "passed": case.get("status") == "run" and not any(
                 case.find(tag) is not None for tag in ("failure", "error", "skipped"))} for case in cases]


def changed_modules(base):
    completed = subprocess.run(["git", "diff", "--ignore-submodules=all", "--name-only", base,
                                "--", "core/libs/prx"], cwd=ROOT, capture_output=True, text=True, check=True)
    return {Path(path).parts[3] for path in completed.stdout.splitlines() if len(Path(path).parts) >= 5}


def command(arguments, output):
    print("Running: " + " ".join(map(str, arguments)), flush=True)
    with output.open("w", encoding="utf-8") as log:
        return subprocess.run(list(map(str, arguments)), cwd=ROOT, stdout=log, stderr=subprocess.STDOUT).returncode


def main():
    parser = argparse.ArgumentParser(description="Build and run functional system-library tests on the host PC")
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build")
    parser.add_argument("--module", action="append", help="library to test; repeat to select several; default: all registered")
    parser.add_argument("--base", help="also require suites for every library changed from this Git ref")
    parser.add_argument("--output-dir", type=Path, help="new directory for this run's logs and JUnit/JSON reports")
    args = parser.parse_args()
    build = args.build_dir.resolve()
    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S%fZ")
    output = (args.output_dir or build / "module-tests" / stamp).resolve()
    output.mkdir(parents=True, exist_ok=False)
    report = {"started_utc": stamp, "build_dir": str(build), "passed": False, "modules": {}}
    status = 1
    try:
        for target, log in (([], "build.log"), (["--target", "libs"], "libs.log")):
            if command(["cmake", "--build", build, *target], output / log):
                raise ValueError(f"Build failed; see {output / log}")
        catalog = subprocess.run(["ctest", "--test-dir", str(build), "--show-only=json-v1"],
                                 cwd=ROOT, capture_output=True, text=True, check=True)
        modules = discover(json.loads(catalog.stdout))
        selected = set(args.module or modules)
        required = changed_modules(args.base) if args.base else set()
        if not selected or selected - modules.keys() or required - selected:
            raise ValueError(f"Missing requested or changed module suites: {sorted((selected - modules.keys()) | (required - selected))}")
        report["commit"] = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip()
        report["dirty"] = bool(subprocess.check_output(["git", "status", "--porcelain", "--ignore-submodules=all"], cwd=ROOT, text=True).strip())
        for module in sorted(selected):
            expected = modules[module]
            expression = "^(" + "|".join(re.escape(name) for name in expected) + ")$"
            junit = output / f"{module}.xml"
            code = command(["ctest", "--test-dir", build, "--no-tests=error", "--output-on-failure",
                            "-R", expression, "--output-junit", junit], output / f"{module}.log")
            tests = results(junit, expected)
            report["modules"][module] = {"passed": code == 0 and all(test["passed"] for test in tests), "tests": tests}
            print(f"{module}: {'PASS' if report['modules'][module]['passed'] else 'FAIL'} ({len(tests)} tests)", flush=True)
        if command([sys.executable, ROOT / "tools/progress.py", output / "progress"], output / "progress.log"):
            raise ValueError("Progress generation failed")
        report["passed"] = all(module["passed"] for module in report["modules"].values())
        status = 0 if report["passed"] else 1
    except (OSError, ValueError, subprocess.SubprocessError, ET.ParseError) as error:
        report["error"] = str(error)
        print(str(error), file=sys.stderr)
    finally:
        (output / "report.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
        print(f"Report: {output / 'report.json'}", flush=True)
    return status


if __name__ == "__main__":
    raise SystemExit(main())
