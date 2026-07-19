import dataclasses
import hashlib
import importlib.util
import io
import json
import os
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import subprocess
import sys
import tempfile
import textwrap
import threading
import types
import unittest
from unittest import mock


MODULE_PATH = Path(__file__).resolve().parents[1] / "local_files.py"
SPEC = importlib.util.spec_from_file_location("local_files", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
local_files = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = local_files
SPEC.loader.exec_module(local_files)


class EmbeddingHandler(BaseHTTPRequestHandler):
    calls = 0
    fail_on = None

    def do_POST(self):
        type(self).calls += 1
        length = int(self.headers.get("Content-Length", "0"))
        payload = json.loads(self.rfile.read(length).decode("utf-8"))
        if type(self).fail_on == type(self).calls:
            self.send_response(500)
            self.end_headers()
            return
        inputs = payload.get("input", [])
        body = json.dumps(
            {
                "data": [
                    {"index": index, "embedding": [1.0, float(index), 0.5]}
                    for index, _ in enumerate(inputs)
                ]
            }
        ).encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, format, *args):
        del format, args


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

    def start_embedding_server(self):
        EmbeddingHandler.calls = 0
        EmbeddingHandler.fail_on = None
        try:
            server = ThreadingHTTPServer(("127.0.0.1", 0), EmbeddingHandler)
        except PermissionError:
            self.skipTest("the test environment does not allow a loopback embedding server")
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        self.addCleanup(server.server_close)
        self.addCleanup(server.shutdown)
        return server

    def write_pipeline_config(self, directory, server, *, batch_size=2, shard_bytes=500,
                              extra="", vector_enabled=False):
        root = directory / "input"
        root.mkdir()
        (root / "alpha.txt").write_text("alpha searchable source", encoding="utf-8")
        (root / "beta.txt").write_text("beta searchable source", encoding="utf-8")
        makeindex = MODULE_PATH.parents[2] / "build" / "yappo_makeindex"
        config = directory / "local-files.toml"
        config.write_text(
            textwrap.dedent(
                f"""
                format_version = 2
                collection_id = "pipeline-fixture"
                [index]
                directory = {json.dumps(str(directory / "index"))}
                [tokenizer]
                id = "unicode_nfkc_casefold_v2"
                [chunking]
                max_chars = 32
                overlap_chars = 4
                [vector]
                enabled = {str(vector_enabled).lower()}
                {('model_id = "fixture-3d-v1"' if vector_enabled else '')}
                {('dimensions = 3' if vector_enabled else '')}
                {('metric = "cosine"' if vector_enabled else '')}
                [metadata]
                filterable_fields = ["collection_id", "source_path"]
                [input]
                root = {json.dumps(str(root))}
                include = ["*", "**/*"]
                exclude = []
                follow_symlinks = false
                [output]
                directory = {json.dumps(str(directory / "documents"))}
                shard_max_bytes = {shard_bytes}
                body_max_bytes = 1000000
                [prepare]
                directory = {json.dumps(str(directory / "passages"))}
                [embedding]
                directory = {json.dumps(str(directory / "vectors"))}
                provider = "openai"
                base_url = "http://127.0.0.1:{server.server_port}/v1"
                model = "fixture-model"
                model_id = "fixture-3d-v1"
                dimensions = 3
                batch_size = {batch_size}
                timeout_ms = 5000
                prompt_profile = "plain"
                {extra}
                [build]
                yappo_makeindex = {json.dumps(str(makeindex))}
                [daemon]
                run_directory = {json.dumps(str(directory / "run"))}
                core_host = "127.0.0.1"
                core_port = 18401
                front_host = "127.0.0.1"
                front_port = 18400
                [extract]
                max_extracted_bytes = 1048576
                enable_plugins = false
                tika_timeout_ms = 30000
                [formatters]
                enabled = false
                content_match_enabled = false
                content_scan_bytes = 1048576
                """
            ),
            encoding="utf-8",
        )
        return config

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

    def test_rejects_schema_version_in_application_toml(self):
        self.write_config()
        self.config.write_text(
            "schema_version = 1\n" + self.config.read_text(encoding="utf-8"),
            encoding="utf-8",
        )
        with self.assertRaisesRegex(local_files.LocalFilesError, "schema_version is not supported"):
            local_files.load_settings(self.config)

    def test_web_and_daemon_settings_do_not_change_pipeline_fingerprint(self):
        self.write_config(content_match=False)
        before = local_files.load_settings(self.config)
        with self.config.open("a", encoding="utf-8") as output:
            output.write("\n[daemon]\nfront_port = 19000\n[web]\nport = 5199\n")
        after = local_files.load_settings(self.config)
        self.assertEqual(before.config_fingerprint, after.config_fingerprint)

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

    def test_all_runs_only_the_stages_required_by_each_target(self):
        self.write_config()
        settings = local_files.load_settings(self.config)
        documents = {
            "total_records": 2,
            "total_bytes": 200,
            "failure_count": 0,
            "shards": [{"path": "documents-000001.ndjson"}],
        }
        passages = {
            "total_records": 3,
            "total_bytes": 300,
            "shards": [{"path": "passages-000001.ndjson"}],
        }
        vectors = {
            "total_records": 2,
            "total_bytes": 400,
            "passage_count": 3,
            "shards": [{"path": "documents-000001.ndjson"}],
        }
        expected = {
            "documents": (1, 0, 0, []),
            "lexical": (1, 0, 0, ["lexical"]),
            "rag": (1, 1, 0, ["rag"]),
            "hybrid": (1, 1, 1, ["hybrid"]),
        }
        for target, calls in expected.items():
            with self.subTest(target=target), \
                    mock.patch.object(local_files, "convert", return_value=documents) as convert, \
                    mock.patch.object(
                        local_files, "prepare_passages", return_value=passages
                    ) as prepare, \
                    mock.patch.object(
                        local_files, "embed_documents", return_value=vectors
                    ) as embed, \
                    mock.patch.object(
                        local_files,
                        "build_index",
                        side_effect=lambda _settings, selected: {
                            "target": selected,
                            "accepted": 2,
                        },
                    ) as build:
                result = local_files.run_all(settings, target)

            self.assertEqual(result["target"], target)
            self.assertEqual(convert.call_count, calls[0])
            self.assertEqual(prepare.call_count, calls[1])
            self.assertEqual(embed.call_count, calls[2])
            self.assertEqual([call.args[1] for call in build.call_args_list], calls[3])

    def test_all_targets_run_end_to_end_and_hybrid_index_is_searchable(self):
        makeindex = MODULE_PATH.parents[2] / "build" / "yappo_makeindex"
        search = MODULE_PATH.parents[2] / "build" / "search"
        if not makeindex.is_file() or not search.is_file():
            self.skipTest("build/yappo_makeindex and build/search are required")
        server = self.start_embedding_server()
        for target in ("documents", "lexical", "rag", "hybrid"):
            with self.subTest(target=target):
                directory = self.base / f"pipeline-{target}"
                directory.mkdir()
                config = self.write_pipeline_config(
                    directory, server, vector_enabled=target == "hybrid"
                )
                completed = subprocess.run(
                    [
                        sys.executable,
                        str(MODULE_PATH),
                        "all",
                        "--config",
                        str(config),
                        "--target",
                        target,
                    ],
                    check=False,
                    capture_output=True,
                    text=True,
                    timeout=30,
                )
                self.assertEqual(completed.returncode, 0, completed.stderr)
                summary = json.loads(completed.stdout)
                self.assertEqual(summary["target"], target)
                self.assertTrue((directory / "documents" / "manifest.json").is_file())
                self.assertEqual(
                    (directory / "passages").exists(), target in {"rag", "hybrid"}
                )
                self.assertEqual((directory / "vectors").exists(), target == "hybrid")
                self.assertEqual((directory / "index").exists(), target != "documents")

                calls_before_resume = EmbeddingHandler.calls
                resumed = subprocess.run(
                    [
                        sys.executable,
                        str(MODULE_PATH),
                        "all",
                        "--config",
                        str(config),
                        "--target",
                        target,
                    ],
                    check=False,
                    capture_output=True,
                    text=True,
                    timeout=30,
                )
                self.assertEqual(resumed.returncode, 0, resumed.stderr)
                self.assertEqual(json.loads(resumed.stdout)["target"], target)
                self.assertEqual(EmbeddingHandler.calls, calls_before_resume)

                if target == "hybrid":
                    vector_manifest = json.loads(
                        (directory / "vectors" / "manifest.json").read_text(encoding="utf-8")
                    )
                    vector_records = list(
                        local_files._iter_ndjson(
                            [directory / "vectors" / item["path"]
                             for item in vector_manifest["shards"]],
                            "vector document",
                        )
                    )
                    self.assertEqual(len(vector_records), 2)
                    self.assertTrue(all(record["vectors"] for record in vector_records))
                    for mode, arguments in (
                        ("lexical", ["--query", "alpha"]),
                        ("vector", ["--vector", "1,0,0.5"]),
                        (
                            "hybrid",
                            ["--query", "alpha", "--vector", "1,0,0.5"],
                        ),
                    ):
                        searched = subprocess.run(
                            [
                                str(search),
                                "--index",
                                str(directory / "index"),
                                "--mode",
                                mode,
                                "--scope",
                                "documents",
                                *arguments,
                            ],
                            check=False,
                            capture_output=True,
                            text=True,
                            timeout=10,
                        )
                        self.assertEqual(searched.returncode, 0, searched.stderr)
                        self.assertTrue(searched.stdout.strip())

    def test_source_identifier_queries_record_actual_tokenizer_behavior(self):
        search = MODULE_PATH.parents[2] / "build" / "search"
        if not search.is_file():
            self.skipTest("build/search is required")
        directory = self.base / "identifier-pipeline"
        directory.mkdir()
        config = self.write_pipeline_config(
            directory, types.SimpleNamespace(server_port=9)
        )
        (directory / "input" / "symbols.c").write_text(
            "snake_case_identifier camelCaseIdentifier "
            "Namespace::QualifiedName ++ ->\n",
            encoding="utf-8",
        )
        settings = local_files.load_settings(config)
        local_files.run_all(settings, "lexical")

        expected_totals = {
            "snake_case_identifier": 1,
            "camelCaseIdentifier": 1,
            "camel": 0,
            "QualifiedName": 1,
            "Namespace::QualifiedName": 1,
            "++": 0,
            "->": 0,
        }
        for query, expected_total in expected_totals.items():
            with self.subTest(query=query):
                completed = subprocess.run(
                    [
                        str(search),
                        "--index",
                        str(directory / "index"),
                        "--mode",
                        "lexical",
                        "--scope",
                        "documents",
                        "--query",
                        query,
                    ],
                    check=False,
                    capture_output=True,
                    text=True,
                    timeout=10,
                )
                self.assertEqual(completed.returncode, 0, completed.stderr)
                self.assertEqual(json.loads(completed.stdout)["total"], expected_total)

    def test_embedding_checkpoint_resumes_and_rejects_configuration_mismatch(self):
        makeindex = MODULE_PATH.parents[2] / "build" / "yappo_makeindex"
        if not makeindex.is_file():
            self.skipTest("build/yappo_makeindex is required")
        server = self.start_embedding_server()
        directory = self.base / "checkpoint-pipeline"
        directory.mkdir()
        config = self.write_pipeline_config(
            directory, server, batch_size=1, vector_enabled=True
        )
        settings = local_files.load_settings(config)
        local_files.convert(settings)
        local_files.prepare_passages(settings)

        EmbeddingHandler.fail_on = 2
        with self.assertRaisesRegex(local_files.LocalFilesError, "HTTP 500"):
            local_files.embed_documents(settings)
        work = directory / ".vectors.work"
        self.assertTrue((work / "input-000001" / "checkpoint.json").is_file())
        self.assertFalse((directory / "vectors").exists())

        assert settings.embedding is not None
        mismatched = dataclasses.replace(
            settings,
            embedding=dataclasses.replace(settings.embedding, model="different-model"),
        )
        previous_calls = EmbeddingHandler.calls
        with self.assertRaisesRegex(local_files.LocalFilesError, "configuration mismatch"):
            local_files.embed_documents(mismatched)
        self.assertEqual(EmbeddingHandler.calls, previous_calls)

        EmbeddingHandler.fail_on = None
        manifest = local_files.embed_documents(settings)
        self.assertEqual(manifest["total_records"], 2)
        self.assertEqual(EmbeddingHandler.calls, previous_calls + 1)
        self.assertTrue((directory / "vectors" / "manifest.json").is_file())
        self.assertFalse(work.exists())

    def test_embedding_resumes_after_last_durable_batch_inside_one_shard(self):
        makeindex = MODULE_PATH.parents[2] / "build" / "yappo_makeindex"
        if not makeindex.is_file():
            self.skipTest("build/yappo_makeindex is required")
        server = self.start_embedding_server()
        directory = self.base / "batch-checkpoint-pipeline"
        directory.mkdir()
        config = self.write_pipeline_config(
            directory, server, batch_size=1, shard_bytes=1_000_000,
            vector_enabled=True,
        )
        settings = local_files.load_settings(config)
        local_files.convert(settings)
        local_files.prepare_passages(settings)

        EmbeddingHandler.fail_on = 2
        with self.assertRaisesRegex(local_files.LocalFilesError, "HTTP 500"):
            local_files.embed_documents(settings)
        journal = directory / ".vectors.work" / ".input-000001.progress" / "vectors.ndjson"
        self.assertEqual(len(journal.read_text(encoding="utf-8").splitlines()), 1)
        previous_calls = EmbeddingHandler.calls

        EmbeddingHandler.fail_on = None
        manifest = local_files.embed_documents(settings)
        self.assertEqual(manifest["total_records"], 2)
        self.assertEqual(EmbeddingHandler.calls, previous_calls + 1)
        self.assertFalse((directory / ".vectors.work").exists())

    def test_embedding_journal_truncates_only_an_incomplete_tail(self):
        journal = self.base / "vectors.ndjson"
        entries = [
            ("doc-1", 0, "first", hashlib.sha256(b"first").hexdigest()),
            ("doc-1", 1, "second", hashlib.sha256(b"second").hexdigest()),
        ]
        local_files._append_embedding_journal_batch(
            journal, entries[:1], [[1.0, 0.0, 0.0]], 0
        )
        durable_size = journal.stat().st_size
        with journal.open("ab") as output:
            output.write(b'{"sequence":1,"document_id":"doc-1"')

        self.assertEqual(local_files._load_embedding_journal(journal, entries, 3), 1)
        self.assertEqual(journal.stat().st_size, durable_size)

    def test_embedding_journal_rejects_middle_corruption_and_input_hash_change(self):
        entries = [
            ("doc-1", 0, "first", hashlib.sha256(b"first").hexdigest()),
            ("doc-1", 1, "second", hashlib.sha256(b"second").hexdigest()),
        ]
        journal = self.base / "vectors.ndjson"
        local_files._append_embedding_journal_batch(
            journal, entries, [[1.0, 0.0, 0.0], [0.0, 1.0, 0.0]], 0
        )
        lines = journal.read_bytes().splitlines(keepends=True)
        journal.write_bytes(lines[0] + b"not-json\n")
        with self.assertRaisesRegex(local_files.LocalFilesError, "invalid JSON"):
            local_files._load_embedding_journal(journal, entries, 3)

        journal.unlink()
        local_files._append_embedding_journal_batch(
            journal, entries, [[1.0, 0.0, 0.0], [0.0, 1.0, 0.0]], 0
        )
        changed = list(entries)
        changed[0] = ("doc-1", 0, "changed", hashlib.sha256(b"changed").hexdigest())
        with self.assertRaisesRegex(local_files.LocalFilesError, "does not match"):
            local_files._load_embedding_journal(journal, changed, 3)

    def test_openai_payload_config_token_usage_log_and_secure_url(self):
        server = self.start_embedding_server()
        directory = self.base / "openai-settings"
        directory.mkdir()
        config = self.write_pipeline_config(
            directory,
            server,
            extra='authorization_token_env = "YAPPOD_TEST_OPENAI_TOKEN"\n[usage_log]\npath = "usage.jsonl"',
            vector_enabled=True,
        )
        config.write_text(config.read_text(encoding="utf-8").replace(
            f'base_url = "http://127.0.0.1:{server.server_port}/v1"',
            'endpoint_url = "https://api.openai.com/v1/embeddings"',
        ), encoding="utf-8")
        with mock.patch.dict(os.environ, {"YAPPOD_TEST_OPENAI_TOKEN": "secret"}):
            settings = local_files.load_settings(config)
        assert settings.embedding is not None
        self.assertEqual(settings.embedding.authorization_token, "secret")
        with mock.patch.object(
            local_files,
            "urlopen",
            return_value=mock.MagicMock(
                __enter__=lambda self: types.SimpleNamespace(
                    read=lambda: json.dumps({
                        "data": [{"index": 0, "embedding": [1, 0, 0]}],
                        "usage": {"prompt_tokens": 2, "total_tokens": 2},
                    }).encode("utf-8")
                ),
                __exit__=lambda *args: None,
            ),
        ) as opened:
            local_files._embedding_batch(settings.embedding, ["text"])
        request = opened.call_args.args[0]
        self.assertEqual(request.full_url, "https://api.openai.com/v1/embeddings")
        self.assertEqual(json.loads(request.data)["dimensions"], 3)
        event = json.loads((directory / "usage.jsonl").read_text(encoding="utf-8"))
        self.assertEqual(event["model"], "fixture-model")
        self.assertEqual(event["usage"]["total_tokens"], 2)

        source = config.read_text(encoding="utf-8").replace(
            "https://api.openai.com/v1/embeddings",
            "http://api.openai.com/v1/embeddings",
        )
        config.write_text(source, encoding="utf-8")
        with self.assertRaisesRegex(local_files.LocalFilesError, "must use https"):
            local_files.load_settings(config)

        with mock.patch.dict(os.environ, {"YAPPOD_TEST_OPENAI_TOKEN": "secret"}):
            self.assertEqual(
                local_files._authorization_token_from_env(
                    {"authorization_token_env": "YAPPOD_TEST_OPENAI_TOKEN"}, "embedding"
                ),
                "secret",
            )
        with self.assertRaisesRegex(local_files.LocalFilesError, "not supported"):
            local_files._authorization_token_from_env(
                {"authorization_token": "plain-text-secret"}, "embedding"
            )
        with self.assertRaisesRegex(local_files.LocalFilesError, "environment variable name"):
            local_files._authorization_token_from_env(
                {"authorization_token_env": "contains space"}, "embedding"
            )

    def test_service_url_allows_only_explicit_http_private_ranges(self):
        allowed = (
            "http://localhost:1/v1",
            "http://127.255.0.1:1/v1",
            "http://10.2.3.4:1/v1",
            "http://172.31.0.1:1/v1",
            "http://192.168.1.2:1/v1",
            "http://[::1]:1/v1",
            "http://[fd12::1]:1/v1",
            "https://api.example.com/v1",
        )
        for endpoint in allowed:
            with self.subTest(endpoint=endpoint):
                self.assertEqual(local_files._service_url(endpoint, "endpoint"), endpoint)
        for endpoint in (
            "http://example.com/v1",
            "http://8.8.8.8/v1",
            "http://169.254.1.1/v1",
            "http://[fe80::1]/v1",
        ):
            with self.subTest(endpoint=endpoint):
                with self.assertRaisesRegex(local_files.LocalFilesError, "must use https"):
                    local_files._service_url(endpoint, "endpoint")

    def test_usage_log_failure_warns_without_raising(self):
        path = self.base / "usage-directory"
        path.mkdir()
        stderr = io.StringIO()
        with mock.patch.object(local_files.sys, "stderr", stderr):
            local_files._append_usage_log(
                path,
                source="local-files",
                service="embedding",
                operation="batch_embed",
                provider="openai",
                model="text-embedding-3-small",
                usage=None,
            )
        self.assertIn("warning: cannot append usage log", stderr.getvalue())

    def test_build_rejects_corrupt_shard_without_publishing_index(self):
        directory = self.base / "corrupt-pipeline"
        directory.mkdir()
        config = self.write_pipeline_config(
            directory, types.SimpleNamespace(server_port=9)
        )
        settings = local_files.load_settings(config)
        manifest = local_files.convert(settings)
        shard = directory / "documents" / manifest["shards"][0]["path"]
        with shard.open("ab") as output:
            output.write(b"corrupt\n")

        with self.assertRaisesRegex(local_files.LocalFilesError, "verification failed"):
            local_files.build_index(settings, "lexical")

        self.assertFalse((directory / "index").exists())
        self.assertEqual(list(directory.glob(".index.tmp.*")), [])

    def test_all_rejects_corrupt_reusable_documents(self):
        directory = self.base / "corrupt-reuse-pipeline"
        directory.mkdir()
        config = self.write_pipeline_config(
            directory, types.SimpleNamespace(server_port=9)
        )
        settings = local_files.load_settings(config)
        manifest = local_files.run_all(settings, "documents")["documents"]
        shard = directory / "documents" / manifest["shards"][0]["path"]
        with shard.open("ab") as output:
            output.write(b"corrupt\n")

        with self.assertRaisesRegex(local_files.LocalFilesError, "verification failed"):
            local_files.run_all(settings, "documents")

    def test_all_rejects_changed_source_until_full_regeneration(self):
        directory = self.base / "changed-source-pipeline"
        directory.mkdir()
        config = self.write_pipeline_config(
            directory, types.SimpleNamespace(server_port=9)
        )
        settings = local_files.load_settings(config)
        local_files.run_all(settings, "documents")
        (directory / "input" / "alpha.txt").write_text(
            "changed source text", encoding="utf-8"
        )

        with self.assertRaisesRegex(local_files.LocalFilesError, "input snapshot"):
            local_files.run_all(settings, "documents")

    def test_all_reuses_index_sidecar_and_rejects_mismatch(self):
        makeindex = MODULE_PATH.parents[2] / "build" / "yappo_makeindex"
        if not makeindex.is_file():
            self.skipTest("build/yappo_makeindex is required")
        directory = self.base / "index-reuse-pipeline"
        directory.mkdir()
        config = self.write_pipeline_config(
            directory, types.SimpleNamespace(server_port=9)
        )
        settings = local_files.load_settings(config)
        first = local_files.run_all(settings, "lexical")
        second = local_files.run_all(settings, "lexical")
        self.assertEqual(second["index"], first["index"])

        state_path = directory / "index" / local_files.BUILD_STATE_FILENAME
        state = json.loads(state_path.read_text(encoding="utf-8"))
        state["target"] = "hybrid"
        state_path.write_text(json.dumps(state), encoding="utf-8")
        with self.assertRaisesRegex(local_files.LocalFilesError, "does not match"):
            local_files.run_all(settings, "lexical")

    def test_build_process_failure_does_not_publish_or_leave_staging(self):
        directory = self.base / "failed-build-pipeline"
        directory.mkdir()
        config = self.write_pipeline_config(
            directory, types.SimpleNamespace(server_port=9)
        )
        settings = local_files.load_settings(config)
        local_files.convert(settings)
        invalid_config = directory / "invalid-index.toml"
        invalid_config.write_text("[vector]\nenabled=false\n", encoding="utf-8")
        settings = dataclasses.replace(settings, config_path=invalid_config)

        with self.assertRaisesRegex(local_files.LocalFilesError, "exited with"):
            local_files.build_index(settings, "lexical")

        self.assertFalse((directory / "index").exists())
        self.assertEqual(list(directory.glob(".index.tmp.*")), [])

    def test_producer_failure_does_not_publish_or_leave_staging(self):
        directory = self.base / "failed-producer-pipeline"
        directory.mkdir()
        config = self.write_pipeline_config(
            directory, types.SimpleNamespace(server_port=9)
        )
        settings = local_files.load_settings(config)
        local_files.convert(settings)

        def fail_before_fifo(_fifo, _shards, errors):
            errors.append(RuntimeError("fixture producer failure"))

        with mock.patch.object(
            local_files, "_stream_document_shards", side_effect=fail_before_fifo
        ), self.assertRaisesRegex(local_files.LocalFilesError, "fixture producer failure"):
            local_files.build_index(settings, "lexical")

        self.assertFalse((directory / "index").exists())
        self.assertEqual(list(directory.glob(".index.tmp.*")), [])

    def test_ollama_embedding_response_uses_configured_dimensions(self):
        embedding = local_files.EmbeddingConfig(
            provider="ollama",
            base_url="http://127.0.0.1:11434",
            endpoint_url="http://127.0.0.1:11434/api/embed",
            model="fixture",
            model_id="fixture-2d-v1",
            dimensions=2,
            batch_size=4,
            timeout_ms=1000,
            prompt_profile="plain",
            authorization_token=None,
            usage_log_path=None,
        )
        with mock.patch.object(
            local_files,
            "_post_embedding_json",
            return_value={"embeddings": [[1, 2], [3.5, 4]]},
        ) as posted:
            vectors = local_files._embedding_batch(embedding, ["one", "two"])

        self.assertEqual(vectors, [[1.0, 2.0], [3.5, 4.0]])
        posted.assert_called_once_with(embedding, ["one", "two"])

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

    def test_main_reports_config_path_and_recovery_steps(self):
        stderr = io.StringIO()
        with mock.patch.object(local_files.sys, "stderr", stderr):
            status = local_files.main([
                "all", "--config", str(self.config), "--target", "hybrid",
            ])

        message = stderr.getvalue()
        self.assertEqual(status, 1)
        self.assertIn("local-files: error: 'all' command failed", message)
        self.assertIn("Reason: cannot load config", message)
        self.assertIn("Config: {}".format(self.config.resolve()), message)
        self.assertIn("How to fix:", message)
        self.assertIn("local-files.toml", message)


if __name__ == "__main__":
    unittest.main()
