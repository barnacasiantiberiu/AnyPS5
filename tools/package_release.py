import argparse
import re
import shutil
import tarfile
import tempfile
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def windows_runtime_directory(build, runtime_dir=None):
    if runtime_dir is not None:
        return Path(runtime_dir)
    cache = build / "CMakeCache.txt"
    try:
        contents = cache.read_text(encoding="utf-8")
    except (OSError, UnicodeError) as error:
        raise RuntimeError(f"Cannot read {cache}; configure the build or pass --runtime-dir") from error
    match = re.search(r"^CMAKE_CXX_COMPILER:[^=\r\n]+=(.*)$", contents, re.MULTILINE)
    compiler = Path(match.group(1).strip()) if match else None
    if compiler is None or not compiler.is_absolute():
        raise RuntimeError(f"Missing absolute CMAKE_CXX_COMPILER path in {cache}; configure the build or pass --runtime-dir")
    return compiler.parent


def package(platform, build, output, version, runtime_dir=None):
    if not re.fullmatch(r"v[0-9A-Za-z][0-9A-Za-z._-]*", version) or version.endswith("."):
        raise ValueError(f"Invalid release tag for asset filenames: {version}")
    if runtime_dir is not None and platform != "windows":
        raise ValueError("--runtime-dir requires --platform windows")
    libraries = sorted((build / "core/libs/libs").glob("*.prx"))
    expected = {f"{directory.name}.prx" for directory in (ROOT / "core/libs/prx").iterdir() if directory.is_dir() and not directory.name.startswith(".")}
    missing = expected - {library.name for library in libraries}
    if missing:
        raise RuntimeError(f"Missing patched libraries: {', '.join(sorted(missing))}")
    executable = "relinker.exe" if platform == "windows" else "relinker"
    files = [library for library in libraries if library.name in expected]
    if platform == "windows":
        runtime = windows_runtime_directory(build, runtime_dir)
        files.extend(runtime / name for name in ("libgcc_s_seh-1.dll", "libstdc++-6.dll", "libwinpthread-1.dll"))
    binary = build / "core/relinker" / executable
    for file in [*files, binary]:
        if not file.is_file() or file.stat().st_size == 0:
            raise RuntimeError(f"Missing or empty release file: {file}")
    output.mkdir(parents=True, exist_ok=True)
    asset = f"relinker-{version}.exe" if platform == "windows" else f"relinker-{version}"
    with tempfile.TemporaryDirectory(prefix=".release-", dir=output) as temporary:
        staging = Path(temporary)
        with zipfile.ZipFile(staging / f"prx-{platform}-{version}.zip", "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
            for file in files:
                archive.write(file, arcname=f"libs/{file.name}")
        with tarfile.open(staging / f"prx-{platform}-{version}.tar.gz", "w:gz", compresslevel=9, dereference=True) as archive:
            for file in files:
                archive.add(file, arcname=f"libs/{file.name}")
        shutil.copy2(binary, staging / asset)
        for file in sorted(staging.iterdir()):
            file.replace(output / file.name)


def collect_docs(source, output):
    documents = sorted(document for document in source.rglob("*.md") if document.is_file())
    if not documents:
        raise RuntimeError(f"No Markdown documents found in {source}")
    names = {file.name.casefold() for file in output.iterdir()} if output.exists() else set()
    for document in documents:
        if document.name.casefold() in names:
            raise RuntimeError(f"Duplicate release asset name: {document.name}")
        names.add(document.name.casefold())
    output.mkdir(parents=True, exist_ok=True)
    for document in documents:
        shutil.copy2(document, output / document.name)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--platform", choices=("linux", "windows"))
    parser.add_argument("--build", type=Path)
    parser.add_argument("--version")
    parser.add_argument("--runtime-dir", type=Path, help="Windows runtime DLL directory (defaults to the configured CMake compiler directory)")
    parser.add_argument("--docs", type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    if args.docs is not None:
        if args.platform is not None or args.build is not None or args.version is not None or args.runtime_dir is not None:
            parser.error("--docs cannot be combined with --platform, --build, --version or --runtime-dir")
        collect_docs(args.docs, args.output)
    else:
        if args.platform is None or args.build is None or args.version is None:
            parser.error("--platform, --build and --version are required when --docs is not specified")
        if args.runtime_dir is not None and args.platform != "windows":
            parser.error("--runtime-dir requires --platform windows")
        package(args.platform, args.build, args.output, args.version, runtime_dir=args.runtime_dir)


if __name__ == "__main__":
    main()
