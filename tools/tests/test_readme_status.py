import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = "tools/update_readme_status.py"
SOURCE = "core/libs/prx/libSceExample/Export.cpp"
OPCODES = "core/shader/recompiler/RdnaDecoder/include/RdnaDecoder/RdnaOpcode.hpp"
DONE = "int APS5_VABI sceExample() { return 42; }\n"
TODO = "int APS5_VABI sceExample() { NotImplemented_nid_no_patch(__func__); return 0; }\n"
README = "# Example\n\n<!-- anyps5-status:start -->\n<!-- anyps5-status:end -->\n\nKeep this text.\n"


class ReadmeStatusTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="anyps5-status-test-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.env = os.environ.copy()
        for name in list(self.env):
            if name.startswith("GIT_"):
                del self.env[name]
        self.env["GIT_CONFIG_NOSYSTEM"] = "1"
        self.env["GIT_CONFIG_GLOBAL"] = os.devnull
        self.run_command("git", "init", "--quiet")
        self.run_command("git", "config", "user.name", "Status test")
        self.run_command("git", "config", "user.email", "status@example.invalid")
        self.run_command("git", "config", "core.autocrlf", "false")
        for name in [SCRIPT, "tools/progress.py", ".githooks/pre-commit"]:
            self.write(name, (ROOT / name).read_text(encoding="utf-8"))
        (self.root / ".githooks/pre-commit").chmod(0o755)
        self.write("tools/rdna_isa.txt", "S_ENDPGM SOPP\n")
        self.write(OPCODES, "enum class RdnaOpcode {\n    SEndpgm,\n};\n")
        self.write(SOURCE, DONE)
        self.write("README.md", README)
        self.run_command("git", "add", ".")

    def write(self, name, text):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8", newline="\n")

    def run_command(self, *args, success=True):
        result = subprocess.run(args, cwd=self.root, env=self.env, capture_output=True, text=True)
        if success:
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        else:
            self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        return result

    def update(self, *args, success=True):
        return self.run_command(sys.executable, SCRIPT, *args, success=success)

    def test_check_and_idempotence(self):
        self.update("--check", success=False)
        self.assertEqual((self.root / "README.md").read_text(), README)
        self.update()
        text = (self.root / "README.md").read_text()
        self.assertIn("| System library functions* | 1 | 1 | 100.00% |", text)
        self.assertIn("| Decoded shader instructions* | 1 | 1 | 100.00% |", text)
        self.assertIn("| libSceExample | 1 | 1 | 100.00% |", text)
        self.assertTrue(text.endswith("Keep this text.\n"))
        self.update("--check")
        self.update()
        self.assertEqual((self.root / "README.md").read_text(), text)

    def test_commit_uses_index_and_preserves_unstaged_source(self):
        self.update("--install-hook")
        self.write(SOURCE, TODO)
        self.run_command("git", "commit", "-m", "test: staged status")
        text = self.run_command("git", "show", "HEAD:README.md").stdout
        self.assertIn("| libSceExample | 1 | 1 | 100.00% |", text)
        self.assertEqual(self.run_command("git", "show", "HEAD:" + SOURCE).stdout, DONE)
        self.assertEqual((self.root / SOURCE).read_text(), TODO)
        self.update("--staged", "--check")
        self.update("--check", success=False)
        self.run_command("git", "add", SOURCE)
        self.run_command("git", "commit", "-m", "test: status regression")
        text = self.run_command("git", "show", "HEAD:README.md").stdout
        self.assertIn("| libSceExample | 0 | 1 | 0.00% |", text)
        self.update("--check")

    def test_unstaged_readme_is_not_overwritten(self):
        self.update("--install-hook")
        changed = README + "Unstaged prose.\n"
        self.write("README.md", changed)
        result = self.run_command("git", "commit", "-m", "test: preserve readme", success=False)
        self.assertIn("unstaged changes", result.stderr)
        self.assertEqual((self.root / "README.md").read_text(), changed)
        self.assertEqual(self.run_command("git", "show", ":README.md").stdout, README)

    def test_existing_hooks_and_bad_markers_are_preserved(self):
        self.run_command("git", "config", "core.hooksPath", "custom-hooks")
        self.update("--install-hook", success=False)
        self.assertEqual(self.run_command("git", "config", "--get", "core.hooksPath").stdout.strip(), "custom-hooks")
        for text in ["No markers", README + "<!-- anyps5-status:end -->"]:
            self.write("README.md", text)
            self.update(success=False)
            self.assertEqual((self.root / "README.md").read_text(), text)


if __name__ == "__main__":
    unittest.main()
