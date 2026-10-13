import importlib.util
from pathlib import Path
import tempfile
import unittest

SPEC = importlib.util.spec_from_file_location("test_modules", Path(__file__).resolve().parents[1] / "test_modules.py")
runner = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(runner)


class ModuleRunnerTests(unittest.TestCase):
    def test_default_build_still_builds_all_libraries(self):
        self.assertEqual(runner.build_steps(None, None),
                         [([], "build.log"), (["--target", "libs"], "libs.log")])

    def test_focused_build_requires_explicit_modules_and_safe_targets(self):
        for targets, modules in ((["guest_audio_in_tests"], None), (["--clean-first"], ["libSceAudioIn"]),
                                 (["target;other"], ["libSceAudioIn"])):
            with self.subTest(targets=targets), self.assertRaises(ValueError):
                runner.build_steps(targets, modules)
        self.assertEqual(runner.build_steps(["audio_tests", "audio_tests", "audio_modes"], ["libSceAudioIn"]),
                         [(["--target", "audio_tests", "audio_modes"], "build.log")])

    def catalog(self, labels, timeout=30, disabled=False):
        return {"tests": [{"name": "decode", "properties": [
            {"name": "LABELS", "value": labels}, {"name": "TIMEOUT", "value": timeout},
            {"name": "DISABLED", "value": disabled}]}]}

    def result(self, body, expected=("decode",)):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "tests.xml"
            path.write_text(f"<testsuite>{body}</testsuite>", encoding="utf-8")
            return runner.results(path, expected)

    def test_named_functional_suite_is_discovered(self):
        self.assertEqual(runner.discover(self.catalog(["module-functional", "module:libSceOpusDec"])),
                         {"libSceOpusDec": ["decode"]})

    def test_empty_or_unlabelled_catalog_is_rejected(self):
        for catalog in ({"tests": []}, self.catalog(["module:libSceOpusDec"])):
            with self.subTest(catalog=catalog), self.assertRaises(ValueError):
                runner.discover(catalog)

    def test_invalid_registration_is_rejected(self):
        for labels, timeout, disabled in ((["module-functional"], 30, False),
                                         (["module-functional", "module:../unsafe"], 30, False),
                                         (["module-functional", "module:a", "module:b"], 30, False),
                                         (["module-functional", "module:a"], 0, False),
                                         (["module-functional", "module:a"], 31, False),
                                         (["module-functional", "module:a"], 30, True)):
            with self.subTest(labels=labels, timeout=timeout, disabled=disabled), self.assertRaises(ValueError):
                runner.discover(self.catalog(labels, timeout, disabled))

    def test_skipped_failed_or_not_run_is_not_a_pass(self):
        for status, child in (("run", "<skipped/>"), ("run", "<failure/>"), ("run", "<error/>"),
                              ("notrun", ""), ("disabled", ""), ("", "")):
            with self.subTest(status=status, child=child):
                self.assertFalse(self.result(f'<testcase name="decode" status="{status}">{child}</testcase>')[0]["passed"])

    def test_missing_extra_duplicate_results_are_rejected(self):
        for body in ("", '<testcase name="different" status="run"/>',
                     '<testcase name="decode" status="run"/>' * 2):
            with self.subTest(body=body), self.assertRaises(ValueError):
                self.result(body)

    def test_completed_case_passes(self):
        result = self.result('<testcase name="decode" status="run" time="0.25"/>')
        self.assertEqual(result, [{"name": "decode", "seconds": 0.25, "passed": True}])


if __name__ == "__main__":
    unittest.main()
