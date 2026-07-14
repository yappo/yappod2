import dataclasses
import importlib.util
import json
from pathlib import Path
import sys
import tempfile
import textwrap
import types
import unittest
from unittest import mock


MODULE_PATH = Path(__file__).resolve().parents[1] / "local_files.py"
SPEC = importlib.util.spec_from_file_location("local_files", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
local_files = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = local_files
SPEC.loader.exec_module(local_files)


class LocalFilesTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.base = Path(self.temporary.name)
        self.root = self.base / "input"
        self.root.mkdir()
        self.output = self.base / "output"
        self.config = self.base / "local-files.toml"

    def tearDown(self):
        self.temporary.cleanup()

    def write_config(self, extra="", *, collection="fixtures", shard_bytes=67108864,
                     body_bytes=1000000, content_match=False):
        self.config.write_text(
            textwrap.dedent(
                f"""
                schema_version = 1
                collection_id = {json.dumps(collection)}

                [input]
                root = {json.dumps(str(self.root))}
                include = ["*", "**/*"]
                exclude = []
                follow_symlinks = false

                [output]
                directory = {json.dumps(str(self.output))}
                shard_max_bytes = {shard_bytes}
                body_max_bytes = {body_bytes}

                [extract]
                max_extracted_bytes = 67108864
                enable_plugins = false
                tika_timeout_ms = 30000

                [formatters]
                enabled = true
                content_match_enabled = {str(content_match).lower()}
                content_scan_bytes = 1048576
                {extra}
                """
            ),
            encoding="utf-8",
        )

    def convert(self, content_override=None):
        settings = local_files.load_settings(self.config, content_override)
        return local_files.convert(settings)

    def records(self, manifest, key="shards"):
        records = []
        for descriptor in manifest[key]:
            path = self.output / descriptor["path"]
            records.extend(json.loads(line) for line in path.read_text(encoding="utf-8").splitlines())
        return records

    def test_document_id_is_path_based_stable_and_bounded(self):
        relative = local_files.normalize_relative_path(
            self.root / ("資料/" + "長い名前" * 80 + ".md"), self.root
        )
        self.assertEqual(relative, local_files.unicodedata.normalize("NFC", relative))
        first = local_files.document_id("one", relative, 0)
        self.assertEqual(first, local_files.document_id("one", relative, 0))
        self.assertNotEqual(first, local_files.document_id("two", relative, 0))
        self.assertNotEqual(first, local_files.document_id("one", relative + ".renamed", 0))
        self.assertNotEqual(first, local_files.document_id("one", relative, 1))
        self.assertLessEqual(len(first.encode("utf-8")), 255)
        self.assertTrue(first.endswith(":p0000000000"))
        with self.assertRaises(local_files.LocalFilesError):
            local_files.document_id("one", relative, -1)

    def test_content_override_changes_config_fingerprint(self):
        self.write_config(content_match=False)
        configured = local_files.load_settings(self.config)
        enabled = local_files.load_settings(self.config, True)
        disabled = local_files.load_settings(self.config, False)
        self.assertNotEqual(configured.config_fingerprint, enabled.config_fingerprint)
        self.assertNotEqual(configured.config_fingerprint, disabled.config_fingerprint)
        self.assertNotEqual(enabled.config_fingerprint, disabled.config_fingerprint)

    def test_body_split_preserves_text_and_utf8_boundaries(self):
        text = ("あいうえお\n\n" * 90) + ("abcdef\n" * 20)
        parts = local_files.split_body(text, 101)
        self.assertGreater(len(parts), 1)
        self.assertEqual("".join(parts), text)
        self.assertTrue(all(len(part.encode("utf-8")) <= 101 for part in parts))

    def test_excluded_directory_is_not_scanned(self):
        hidden = self.root / ".git"
        hidden.mkdir()
        (hidden / "secret").write_text("secret", encoding="utf-8")
        visible = self.root / "visible.txt"
        visible.write_text("visible", encoding="utf-8")
        real_scandir = local_files.os.scandir

        def guarded_scandir(path):
            if Path(path) == hidden:
                raise AssertionError("excluded directory was scanned")
            return real_scandir(path)

        with mock.patch.object(local_files.os, "scandir", side_effect=guarded_scandir):
            paths = list(local_files._walk_sorted(self.root, (), (".git/**",), False))

        self.assertEqual(paths, [visible])

    def test_convert_extracts_formats_rotates_shards_and_hides_absolute_path(self):
        (self.root / "plain.md").write_text("# Heading\nsource_body\n", encoding="utf-8")
        (self.root / "value.json").write_text('{"z": 1, "a": "日本語"}', encoding="utf-8")
        (self.root / "page.html").write_text(
            "<h1>Visible</h1><script>secret()</script><p>Body</p>", encoding="utf-8"
        )
        (self.root / "unknown.bin").write_bytes(b"\x00\x01\x02")
        self.write_config(shard_bytes=350)

        manifest = self.convert()
        records = self.records(manifest)

        self.assertEqual(manifest["successful_files"], 3)
        self.assertEqual(manifest["failure_count"], 1)
        self.assertGreater(len(manifest["shards"]), 1)
        self.assertEqual([record["title"] for record in records], ["page.html", "plain.md", "value.json"])
        html = records[0]
        self.assertIn("Visible", html["body"])
        self.assertIn("Body", html["body"])
        self.assertNotIn("secret", html["body"])
        absolute_root = str(self.root.resolve())
        self.assertNotIn(absolute_root, json.dumps(records, ensure_ascii=False))
        self.assertEqual(html["metadata"]["source_path"], "page.html")
        self.assertEqual(len(html["metadata"]["source_sha256"]), 64)
        failure = self.records(manifest, "failure_shards")[0]
        self.assertEqual(failure["path"], "unknown.bin")
        self.assertEqual(failure["code"], "unsupported_format")

        disk_manifest = json.loads((self.output / "manifest.json").read_text(encoding="utf-8"))
        self.assertEqual(disk_manifest, manifest)
        for descriptor in manifest["shards"] + manifest["failure_shards"]:
            shard = self.output / descriptor["path"]
            self.assertEqual(shard.stat().st_size, descriptor["file_bytes"])
            self.assertEqual(local_files.sha256_file(shard), descriptor["sha256"])

    def test_formatter_matches_path_and_content_and_receives_absolute_argv(self):
        source = self.root / "special name;not-a-shell.src"
        source.write_text("SPECIAL_FORMAT=yes\noriginal", encoding="utf-8")
        script = self.base / "formatter.py"
        script.write_text(
            "import pathlib, sys\nprint('FORMATTED:' + str(pathlib.Path(sys.argv[1]).resolve()))\n",
            encoding="utf-8",
        )
        extra = textwrap.dedent(
            f"""

            [[formatters.rules]]
            name = "special"
            path_glob = ["*.src"]
            content_regex = ["(?m)^SPECIAL_FORMAT=yes$"]
            command = [{json.dumps(sys.executable)}, {json.dumps(str(script))}, "{{path}}"]
            timeout_ms = 30000
            max_stdout_bytes = 4096
            """
        )
        self.write_config(extra, content_match=True)

        manifest = self.convert()
        record = self.records(manifest)[0]

        self.assertEqual(record["metadata"]["extractor"], "formatter:special")
        self.assertEqual(record["body"].strip(), "FORMATTED:" + str(source.resolve()))
        self.assertFalse((self.base / "not-a-shell.src").exists())

    def test_formatter_rule_order_and_same_selector_or_semantics(self):
        (self.root / "Dockerfile").write_text("FROM scratch\n", encoding="utf-8")
        first_script = self.base / "first.py"
        second_script = self.base / "second.py"
        first_script.write_text("print('FIRST')\n", encoding="utf-8")
        second_script.write_text("print('SECOND')\n", encoding="utf-8")
        extra = textwrap.dedent(
            f"""

            [[formatters.rules]]
            name = "first"
            basename_glob = ["Makefile", "Dockerfile"]
            command = [{json.dumps(sys.executable)}, {json.dumps(str(first_script))}, "{{path}}"]

            [[formatters.rules]]
            name = "second"
            path_glob = ["*"]
            command = [{json.dumps(sys.executable)}, {json.dumps(str(second_script))}, "{{path}}"]
            """
        )
        self.write_config(extra)

        record = self.records(self.convert())[0]

        self.assertEqual(record["body"].strip(), "FIRST")
        self.assertEqual(record["metadata"]["extractor"], "formatter:first")

    def test_content_rule_is_skipped_when_content_matching_is_disabled(self):
        (self.root / "plain.txt").write_text("MATCH_ME\noriginal", encoding="utf-8")
        script = self.base / "formatter.py"
        script.write_text("print('FORMATTED')\n", encoding="utf-8")
        extra = textwrap.dedent(
            f"""

            [[formatters.rules]]
            name = "content-only"
            content_regex = ["MATCH_ME"]
            command = [{json.dumps(sys.executable)}, {json.dumps(str(script))}, "{{path}}"]
            """
        )
        self.write_config(extra, content_match=True)

        record = self.records(self.convert(content_override=False))[0]

        self.assertEqual(record["body"], "MATCH_ME\noriginal")
        self.assertEqual(record["metadata"]["extractor"], "text")

    def test_formatter_failure_is_recorded_while_other_files_continue(self):
        (self.root / "bad.fail").write_text("bad", encoding="utf-8")
        (self.root / "good.txt").write_text("good", encoding="utf-8")
        script = self.base / "fail.py"
        script.write_text("import sys\nprint('reason', file=sys.stderr)\nsys.exit(7)\n", encoding="utf-8")
        extra = textwrap.dedent(
            f"""

            [[formatters.rules]]
            name = "failure"
            path_glob = ["*.fail"]
            command = [{json.dumps(sys.executable)}, {json.dumps(str(script))}, "{{path}}"]
            """
        )
        self.write_config(extra)

        manifest = self.convert()

        self.assertEqual([record["title"] for record in self.records(manifest)], ["good.txt"])
        failure = self.records(manifest, "failure_shards")[0]
        self.assertEqual(failure["code"], "extractor_failed")
        self.assertEqual(failure["extractor"], "formatter:failure")
        self.assertIn("exited with 7", failure["message"])

    def test_formatter_timeout_empty_invalid_utf8_and_output_limit(self):
        source = self.root / "source.txt"
        source.write_text("source", encoding="utf-8")
        script = self.base / "formatter.py"
        cases = (
            ("import time\ntime.sleep(1)\nprint('late')\n", 10, 4096, "timeout"),
            ("pass\n", 30000, 4096, "no_text"),
            ("import sys\nsys.stdout.buffer.write(b'\\xff')\n", 30000, 4096, "invalid_utf8"),
            ("print('x' * 100)\n", 30000, 10, "output_limit"),
        )
        for program, timeout_ms, max_bytes, expected in cases:
            with self.subTest(expected=expected):
                script.write_text(program, encoding="utf-8")
                with self.assertRaises(local_files.FileFailure) as caught:
                    local_files._command_text(
                        (sys.executable, str(script), "{path}"),
                        source,
                        self.base,
                        timeout_ms,
                        max_bytes,
                        "formatter:test",
                    )
                self.assertEqual(caught.exception.code, expected)

    def test_markitdown_formats_are_dispatched_with_plugin_setting(self):
        calls = []

        class FakeMarkItDown:
            def __init__(self, *, enable_plugins):
                calls.append(("init", enable_plugins))

            def convert(self, path):
                calls.append(("convert", Path(path).suffix))
                return types.SimpleNamespace(markdown="converted text")

        self.write_config()
        settings = local_files.load_settings(self.config)
        settings = dataclasses.replace(settings, enable_plugins=True)
        fake_module = types.SimpleNamespace(MarkItDown=FakeMarkItDown)
        with mock.patch.dict(sys.modules, {"markitdown": fake_module}):
            for suffix in (".pdf", ".docx", ".xls", ".xlsx", ".pptx"):
                path = self.root / f"fixture{suffix}"
                path.write_bytes(b"fixture")
                text, extractor = local_files.extract_file(path, path.name, settings)
                self.assertEqual(text, "converted text")
                self.assertEqual(extractor, "markitdown")
        self.assertEqual([call for call in calls if call[0] == "init"], [("init", True)] * 5)

    def test_unknown_extension_uses_markitdown_only_when_plugins_are_enabled(self):
        source = self.root / "fixture.mydoc"
        source.write_bytes(b"MYDOC\nbody")

        class FakeMarkItDown:
            def __init__(self, *, enable_plugins):
                self.enable_plugins = enable_plugins

            def convert(self, path):
                self.path = path
                return types.SimpleNamespace(markdown="plugin text")

        self.write_config()
        settings = dataclasses.replace(local_files.load_settings(self.config), enable_plugins=True)
        with mock.patch.dict(sys.modules, {"markitdown": types.SimpleNamespace(MarkItDown=FakeMarkItDown)}):
            text, extractor = local_files.extract_file(source, source.name, settings)

        self.assertEqual(text, "plugin text")
        self.assertEqual(extractor, "markitdown-plugin")

    def test_tika_fallback_is_explicitly_configured(self):
        source = self.root / "slides.key"
        source.write_bytes(b"fixture")
        script = self.base / "tika.py"
        script.write_text("print('keynote text')\n", encoding="utf-8")
        self.write_config()
        settings = local_files.load_settings(self.config)
        settings = dataclasses.replace(
            settings, tika_command=(sys.executable, str(script), "{path}")
        )

        text, extractor = local_files.extract_file(source, source.name, settings)

        self.assertEqual(text.strip(), "keynote text")
        self.assertEqual(extractor, "tika")

    def test_existing_output_is_not_overwritten(self):
        (self.root / "good.txt").write_text("good", encoding="utf-8")
        self.output.mkdir()
        marker = self.output / "keep"
        marker.write_text("unchanged", encoding="utf-8")
        self.write_config()

        with self.assertRaises(local_files.LocalFilesError):
            self.convert()

        self.assertEqual(marker.read_text(encoding="utf-8"), "unchanged")

    def test_all_failures_leave_no_published_output(self):
        (self.root / "unknown.bin").write_bytes(b"\x00\x01")
        self.write_config()

        with self.assertRaises(local_files.LocalFilesError):
            self.convert()

        self.assertFalse(self.output.exists())
        self.assertEqual(list(self.base.glob(".output.tmp.*")), [])


if __name__ == "__main__":
    unittest.main()
