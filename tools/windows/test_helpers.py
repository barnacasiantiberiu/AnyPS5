import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
import zipfile

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location("verify_libc", HERE / "verify_libc.py")
verify_libc = importlib.util.module_from_spec(spec)
spec.loader.exec_module(verify_libc)


def quoted(value):
    return "'" + str(value).replace("'", "''") + "'"


class LibcResultsTests(unittest.TestCase):
    def test_complete_and_rejected_results(self):
        with tempfile.TemporaryDirectory() as directory:
            junit = Path(directory) / "results.xml"
            catalog = {"tests": [{"name": "first"}, {"name": "second"}]}
            junit.write_text('<testsuite><testcase name="first" status="run"/>'
                             '<testcase name="second" status="run"/></testsuite>', encoding="utf-8")
            self.assertEqual(verify_libc.verify(catalog, junit), 2)
            for content in ('<testsuite/>', '<testsuite><testcase name="first" status="run"/></testsuite>',
                            '<testsuite><testcase name="first" status="run"/>'
                            '<testcase name="second" status="notrun"><skipped/></testcase></testsuite>'):
                junit.write_text(content, encoding="utf-8")
                with self.assertRaises(ValueError):
                    verify_libc.verify(catalog, junit)
            for invalid in ({"tests": []}, {"tests": [{"name": "first", "properties": [{"name": "DISABLED", "value": True}]}]}):
                with self.assertRaises(ValueError):
                    verify_libc.verify(invalid, junit)


@unittest.skipUnless(os.name == "nt", "Windows PowerShell 5.1 tests")
class WindowsHelpersTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="anyps5 helpers ")
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name)
        self.powershell = Path(os.environ["SystemRoot"]) / "System32/WindowsPowerShell/v1.0/powershell.exe"

    def run_ps(self, source, success=True):
        command = "$ErrorActionPreference='Stop'; . " + quoted(HERE / "Common.ps1") + "; " + source
        environment = {key: value for key, value in os.environ.items() if key.upper() != "PSMODULEPATH"}
        completed = subprocess.run([str(self.powershell), "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass",
                                    "-Command", command], capture_output=True, text=True, errors="replace", timeout=15,
                                   creationflags=subprocess.CREATE_NO_WINDOW, env=environment)
        if success:
            self.assertEqual(completed.returncode, 0, completed.stdout + completed.stderr)
        else:
            self.assertNotEqual(completed.returncode, 0, completed.stdout + completed.stderr)
        return completed.stdout + completed.stderr

    def test_all_scripts_parse_in_windows_powershell(self):
        self.run_ps("$files=Get-ChildItem -LiteralPath " + quoted(HERE) + " -Filter '*.ps1'; "
                    "foreach($file in $files) { $tokens=$null; $errors=$null; "
                    "$null=[Management.Automation.Language.Parser]::ParseFile($file.FullName,[ref]$tokens,[ref]$errors); "
                    "if($errors.Count) { throw ($errors | Out-String) } }")

    def test_relative_paths_follow_powershell_location(self):
        output = self.run_ps("Set-Location -LiteralPath " + quoted(self.directory) + "; Get-AnyPS5Path 'child folder' ''")
        self.assertEqual(Path(output.strip()), self.directory / "child folder")

    def test_bad_archive_hash_is_refused_without_extraction(self):
        archive = self.directory / "bad.zip"
        archive.write_bytes(b"not the pinned archive")
        destination = self.directory / "not installed"
        output = self.run_ps("& " + quoted(HERE / "Bootstrap.ps1") + " -Archive " + quoted(archive) +
                             " -ToolchainRoot " + quoted(destination) + " -Python " + quoted(sys.executable) + " -Offline", False)
        self.assertIn("SHA256 mismatch", output)
        self.assertFalse(destination.exists())
        self.assertEqual(archive.read_bytes(), b"not the pinned archive")

    def test_partial_install_is_refused_without_overwrite(self):
        destination = self.directory / "partial"
        destination.mkdir()
        marker = destination / "keep.txt"
        marker.write_text("keep", encoding="utf-8")
        output = self.run_ps("& " + quoted(HERE / "Bootstrap.ps1") + " -Archive " + quoted(self.directory / "absent.zip") +
                             " -ToolchainRoot " + quoted(destination) + " -Python " + quoted(sys.executable) + " -Offline", False)
        self.assertIn("partial or unsupported", output)
        self.assertEqual(list(destination.iterdir()), [marker])
        self.assertEqual(marker.read_text(encoding="utf-8"), "keep")

    def test_archive_paths_reject_traversal_links_and_case_collisions(self):
        cases = [["mingw64/bin/g++.exe"], ["../outside"], ["mingw64/../outside"], ["/mingw64/escape"],
                 ["mingw64/a", "mingw64/A"], ["mingw64/file:stream"], ["mingw64/NUL"], ["mingw64/link"]]
        for index, entries in enumerate(cases):
            with self.subTest(entries=entries):
                archive = self.directory / f"paths-{index}.zip"
                with zipfile.ZipFile(archive, "w") as target:
                    for name in entries:
                        entry = zipfile.ZipInfo(name)
                        if name.endswith("/link"):
                            entry.external_attr = 0o120777 << 16
                        target.writestr(entry, b"fixture")
                self.run_ps("Assert-AnyPS5ZipPaths " + quoted(archive), success=index == 0)

    def test_python_discovery_falls_back_after_native_stderr(self):
        output = self.run_ps("function Get-Command { @([pscustomobject]@{Name='broken.exe';Source=$env:ComSpec}, "
                             "[pscustomobject]@{Name='python.exe';Source=" + quoted(sys.executable) + "}) }; Get-AnyPS5Python")
        self.assertEqual(Path(output.strip()), Path(sys.executable))

    def test_multiple_git_candidates_select_one_executable(self):
        output = self.run_ps("function Get-Command { @([pscustomobject]@{Source=" + quoted(sys.executable) + "}, "
                             "[pscustomobject]@{Source='missing-second.exe'}) }; Get-AnyPS5Git")
        self.assertEqual(Path(output.strip()), Path(sys.executable))

    def test_environment_preserves_certificate_and_deduplicates_path(self):
        bundle = self.directory / "custom ca.crt"
        bundle.write_text("-----BEGIN CERTIFICATE-----\nfixture\n-----END CERTIFICATE-----", encoding="ascii")
        toolchain = self.directory / "tool chain"
        source = ("function Assert-AnyPS5Toolchain {}; function Get-AnyPS5Python { " + quoted(sys.executable) +
                  " }; function Get-AnyPS5Git { 'unused-git.exe' }; $script:AnyPS5Root=" + quoted(self.directory) +
                  "; $env:SSL_CERT_FILE=" + quoted(bundle) + "; $bin=" + quoted(toolchain / "bin") +
                  "; $env:PATH=$bin+';'+$bin+';C:\\Windows'; $first=Set-AnyPS5Environment " + quoted(toolchain) +
                  " ''; $second=Set-AnyPS5Environment " + quoted(toolchain) +
                  " ''; @{Path=$env:PATH;Certificate=$env:SSL_CERT_FILE;Temp=$env:TEMP;Build=$env:CMAKE_BUILD_PARALLEL_LEVEL;"
                  "Test=$env:CTEST_PARALLEL_LEVEL;Python=$second.Python} | ConvertTo-Json -Compress")
        result = json.loads(self.run_ps(source))
        self.assertEqual(result["Path"].split(";").count(str(toolchain / "bin")), 1)
        self.assertEqual(Path(result["Certificate"]), bundle)
        self.assertEqual(Path(result["Temp"]), self.directory / "build_tools/tmp")
        self.assertEqual((result["Build"], result["Test"]), ("4", "1"))
        self.assertEqual(Path(result["Python"]), Path(sys.executable))

    def test_git_certificate_discovery(self):
        bundle = self.directory / "Git/mingw64/etc/ssl/certs/ca-bundle.crt"
        bundle.parent.mkdir(parents=True)
        bundle.write_text("fixture", encoding="ascii")
        output = self.run_ps("$env:SSL_CERT_FILE=$null; Set-AnyPS5Certificate " +
                             quoted(self.directory / "Git/cmd/git.exe") + "; $env:SSL_CERT_FILE")
        self.assertEqual(Path(output.strip()), bundle)


if __name__ == "__main__":
    unittest.main()
