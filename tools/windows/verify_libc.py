import json
from pathlib import Path
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from test_modules import results


def verify(catalog, junit):
    tests = catalog["tests"]
    if not tests or any(any(prop["name"] == "DISABLED" and prop["value"]
                            for prop in test.get("properties", [])) for test in tests):
        raise ValueError("Full libc suite is empty or contains disabled tests")
    checked = results(junit, [test["name"] for test in tests])
    if not all(test["passed"] for test in checked):
        raise ValueError("Full libc suite contains failed or skipped tests")
    return len(checked)


def main():
    if len(sys.argv) != 3:
        raise ValueError("Usage: verify_libc.py libc-build-directory junit-file")
    catalog = subprocess.check_output(["ctest", "--test-dir", sys.argv[1], "--show-only=json-v1"], text=True)
    count = verify(json.loads(catalog), Path(sys.argv[2]))
    print(f"Full libc suite: {count} tests passed; none skipped, disabled or missing")


if __name__ == "__main__":
    main()
