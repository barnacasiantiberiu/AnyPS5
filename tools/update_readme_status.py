import argparse
import importlib.util
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
START = "<!-- anyps5-status:start -->"
END = "<!-- anyps5-status:end -->"
OPCODES = "core/shader/recompiler/RdnaDecoder/include/RdnaDecoder/RdnaOpcode.hpp"
INPUTS = ["tools/progress.py", "tools/rdna_isa.txt", OPCODES, "core/libs/prx"]


def git(*args, data=None):
    return subprocess.run(["git", *args], cwd=ROOT, input=data, capture_output=True, check=True).stdout


def render(root):
    spec = importlib.util.spec_from_file_location("readme_progress", root / "tools/progress.py")
    progress = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(progress)
    libraries, shaders = progress.collect_libraries(), progress.collect_shaders()
    lines = [START, "", "Status of this fork, generated from the code in this commit.", "",
             "| Component | Counted | Known total | Progress |", "| --- | ---: | ---: | ---: |"]
    for label, data in [("System library functions*", libraries), ("Decoded shader instructions*", shaders)]:
        lines.append(f'| {label} | {data["done"]} | {data["total"]} | {data["percent"]:.2f}% |')
    remaining = libraries["total"] - libraries["done"]
    lines += ["", f"**{remaining} known library functions remain marked unimplemented.**", "",
              "*These are static source counts, not test coverage or a guarantee of game compatibility. "
              "The library total includes only declarations known to this project. Shader progress counts "
              "recognized instructions, not verified GPU execution. See the [counting rules](docs/dev/PROGRESS.md).", "",
              "<details>", "<summary>System libraries: status by module</summary>", "",
              "| Library | Counted | Known total | Progress |", "| --- | ---: | ---: | ---: |"]
    for group in libraries["groups"]:
        name = group["name"]
        total = group["done"] + group["todo"]
        if "shared_sources" in group:
            lines.append(f'| {name} | Shared with {group["shared_sources"]} | — | — |')
        elif total:
            lines.append(f'| {name} | {group["done"]} | {total} | {100 * group["done"] / total:.2f}% |')
        else:
            lines.append(f"| {name} | No declared exports | — | — |")
    lines += ["", "</details>", "", END]
    return "\n".join(lines)


def replace(text, block):
    if text.count(START) != 1 or text.count(END) != 1 or text.index(START) >= text.index(END):
        raise ValueError("README.md must contain exactly one ordered pair of status markers")
    begin, end = text.index(START), text.index(END) + len(END)
    return text[:begin] + block + text[end:]


def staged_block():
    names = git("ls-files", "-z", "--", *INPUTS).split(b"\0")
    names = [name for name in names if name and (
        name.decode("utf-8") in INPUTS or name.endswith((b".cpp", b"/CMakeLists.txt")))]
    with tempfile.TemporaryDirectory(prefix="anyps5-readme-") as directory:
        root = Path(directory)
        git("checkout-index", "--stdin", "-z", f"--prefix={root.as_posix()}/", data=b"\0".join(names) + b"\0")
        return render(root)


def update(staged=False, check=False):
    readme = ROOT / "README.md"
    text = git("show", ":README.md").decode("utf-8") if staged else readme.read_text(encoding="utf-8")
    block = staged_block() if staged else render(ROOT)
    updated = replace(text, block)
    if updated == text:
        print("README status is current.")
        return 0
    if check:
        print("README status is stale. Run python tools/update_readme_status.py.", file=sys.stderr)
        return 1
    if staged and readme.read_text(encoding="utf-8") != text:
        raise ValueError("README.md has unstaged changes; stage or stash them before committing. Nothing was overwritten.")
    readme.write_text(updated, encoding="utf-8", newline="\n")
    if staged:
        git("add", "--", "README.md")
    print("Updated README status" + (" in this commit." if staged else "."))
    return 0


def install_hook():
    result = subprocess.run(["git", "config", "--get", "core.hooksPath"], cwd=ROOT, capture_output=True, text=True)
    if result.returncode not in (0, 1):
        raise ValueError(result.stderr)
    if result.stdout.strip() not in ("", ".githooks"):
        raise ValueError("An existing hooksPath is configured; integrate .githooks/pre-commit with it manually.")
    previous = Path(git("rev-parse", "--git-path", "hooks/pre-commit").decode().strip())
    if not previous.is_absolute():
        previous = ROOT / previous
    if not result.stdout.strip() and previous.exists():
        raise ValueError("An existing pre-commit hook is installed; integrate the README hook with it manually.")
    git("config", "--local", "anyps5.python", sys.executable)
    git("config", "--local", "core.hooksPath", ".githooks")
    print("Installed local README status hook using " + sys.executable)


def main():
    parser = argparse.ArgumentParser(description="Keep this fork's README status in sync with its source code")
    parser.add_argument("--staged", action="store_true", help="measure the Git index and stage only the updated README")
    parser.add_argument("--check", action="store_true", help="fail if the status differs without writing files")
    parser.add_argument("--install-hook", action="store_true", help="enable automatic updates for commits in this checkout")
    args = parser.parse_args()
    try:
        if args.install_hook:
            install_hook()
            return 0
        return update(args.staged, args.check)
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print(str(error), file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
