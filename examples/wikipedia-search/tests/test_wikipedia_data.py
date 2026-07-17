import importlib.util
import io
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest import mock
from urllib.parse import parse_qs, urlparse


MODULE_PATH = Path(__file__).resolve().parents[1] / "wikipedia_data.py"
SPEC = importlib.util.spec_from_file_location("wikipedia_data", MODULE_PATH)
wikipedia_data = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(wikipedia_data)
FIXTURES = Path(__file__).resolve().parent / "fixtures"


def read_ndjson(path):
    return [json.loads(line) for line in path.read_text(encoding="utf-8").splitlines()]


class WikipediaDataTest(unittest.TestCase):
    def test_checksum_resolves_dated_dump_filename(self):
        filename, checksum = wikipedia_data._find_dump_checksum(
            "a" * 40 + "  jawiki-20260701-pages-articles-multistream.xml.bz2\n"
        )
        self.assertEqual(filename, "jawiki-20260701-pages-articles-multistream.xml.bz2")
        self.assertEqual(checksum, "a" * 40)

    def test_fetch_api_writes_canonical_documents_and_skips_empty(self):
        response = json.loads((FIXTURES / "api-response.json").read_text(encoding="utf-8"))
        requests = []

        def fake_request(url, user_agent):
            requests.append((url, user_agent))
            query = parse_qs(urlparse(url).query)
            if "generator" in query:
                return response
            page_id = int(query["pageids"][0])
            page = next(page for page in response["query"]["pages"] if page["pageid"] == page_id)
            return {"batchcomplete": True, "query": {"pages": [page]}}

        with tempfile.TemporaryDirectory() as directory, mock.patch.object(
            wikipedia_data, "_request_json", side_effect=fake_request
        ):
            output = Path(directory) / "documents.ndjson"
            written, skipped = wikipedia_data.fetch_api_documents(
                "https://example.test/w/api.php", ["日本語"], 10, output, "fixture-agent/1.0"
            )
            self.assertEqual((written, skipped), (1, 1))
            self.assertEqual(requests[0][1], "fixture-agent/1.0")
            self.assertIn("gsrsearch=%E6%97%A5%E6%9C%AC%E8%AA%9E", requests[0][0])
            article_query = parse_qs(urlparse(requests[1][0]).query)
            self.assertEqual(article_query["pageids"], ["123"])
            self.assertEqual(article_query["explaintext"], ["1"])
            self.assertNotIn("exintro", article_query)
            documents = read_ndjson(output)
            self.assertEqual(documents[0]["id"], "jawiki:123")
            self.assertEqual(documents[0]["metadata"]["language"], "ja")
            self.assertEqual(documents[0]["metadata"]["wikipedia_revision_id"], 456)

    def test_fetch_api_paginates_at_extract_limit(self):
        def page(page_id):
            return {
                "pageid": page_id,
                "title": "記事{}".format(page_id),
                "extract": "本文{}".format(page_id),
                "fullurl": "https://example.test/wiki/{}".format(page_id),
            }

        search_responses = [
            {
                "continue": {"gsroffset": 50, "continue": "gsroffset||"},
                "query": {"pages": [page(page_id) for page_id in range(1, 51)]},
            },
            {"batchcomplete": True, "query": {"pages": [page(page_id) for page_id in range(51, 56)]}},
        ]
        search_requests = []
        article_requests = []

        def fake_request(url, user_agent):
            query = parse_qs(urlparse(url).query)
            if "generator" in query:
                search_requests.append(url)
                return search_responses[len(search_requests) - 1]
            article_requests.append(url)
            page_id = int(query["pageids"][0])
            return {"batchcomplete": True, "query": {"pages": [page(page_id)]}}

        with tempfile.TemporaryDirectory() as directory, mock.patch.object(
            wikipedia_data, "_request_json", side_effect=fake_request
        ):
            output = Path(directory) / "documents.ndjson"
            written, skipped = wikipedia_data.fetch_api_documents(
                "https://example.test/w/api.php", ["日本史"], 55, output, "fixture-agent/1.0"
            )
            documents = read_ndjson(output)

        self.assertEqual((written, skipped), (55, 0))
        self.assertEqual(len(search_requests), 2)
        self.assertEqual(len(article_requests), 55)
        first_query = parse_qs(urlparse(search_requests[0]).query)
        second_query = parse_qs(urlparse(search_requests[1]).query)
        self.assertEqual(first_query["gsrlimit"], ["50"])
        self.assertEqual(second_query["gsroffset"], ["50"])
        self.assertEqual(second_query["continue"], ["gsroffset||"])
        self.assertEqual(second_query["gsrlimit"], ["5"])
        article_query = parse_qs(urlparse(article_requests[0]).query)
        self.assertNotIn("exintro", article_query)
        self.assertEqual(len(documents), 55)

    def test_convert_dump_deduplicates_and_honors_schema(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "documents.ndjson"
            written, skipped = wikipedia_data.convert_wikiextractor(
                FIXTURES / "wikiextractor.jsonl", output
            )
            self.assertEqual((written, skipped), (1, 2))
            document = read_ndjson(output)[0]
            self.assertEqual(document["id"], "jawiki:100")
            self.assertEqual(document["title"], "検索技術")
            self.assertFalse(document["body"].endswith("\n"))

    def test_invalid_utf8_does_not_replace_existing_output(self):
        with tempfile.TemporaryDirectory() as directory:
            input_path = Path(directory) / "bad.jsonl"
            output = Path(directory) / "documents.ndjson"
            input_path.write_bytes(b"\xff\n")
            output.write_text("original\n", encoding="utf-8")
            with self.assertRaises(wikipedia_data.WikipediaDataError):
                wikipedia_data.convert_wikiextractor(input_path, output)
            self.assertEqual(output.read_text(encoding="utf-8"), "original\n")

    def test_empty_input_does_not_replace_existing_output(self):
        with tempfile.TemporaryDirectory() as directory:
            input_path = Path(directory) / "empty.jsonl"
            output = Path(directory) / "documents.ndjson"
            input_path.write_text("\n", encoding="utf-8")
            output.write_text("original\n", encoding="utf-8")
            with self.assertRaises(wikipedia_data.WikipediaDataError):
                wikipedia_data.convert_wikiextractor(input_path, output)
            self.assertEqual(output.read_text(encoding="utf-8"), "original\n")

    def test_limit_stops_conversion(self):
        with tempfile.TemporaryDirectory() as directory:
            input_path = Path(directory) / "articles.jsonl"
            output = Path(directory) / "documents.ndjson"
            records = [
                {"id": str(index), "url": "https://example.test/{}".format(index),
                 "title": "記事{}".format(index), "text": "本文{}".format(index)}
                for index in range(3)
            ]
            input_path.write_text(
                "".join(json.dumps(record, ensure_ascii=False) + "\n" for record in records),
                encoding="utf-8",
            )
            written, skipped = wikipedia_data.convert_wikiextractor(input_path, output, limit=2)
            self.assertEqual((written, skipped), (2, 0))
            self.assertEqual(len(read_ndjson(output)), 2)

    def test_load_embedding_settings_reads_index_and_shared_toml(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            index_config = root / "config.vector.toml"
            web_config = root / "config.toml"
            index_config.write_text("""
[vector]
enabled = true
model_id = "embed-index-v1"
dimensions = 3
metric = "cosine"
""", encoding="utf-8")
            web_config.write_text("""
[llm]
base_url = "http://localhost:1234/v1"
model = "answer-model"

[embedding]
provider = "lmstudio"
base_url = "http://localhost:1234/v1"
model = "embedding-model"
model_id = "embed-index-v1"
dimensions = 3
prompt_profile = "plain"
authorization_token_env = "YAPPOD_TEST_EMBEDDING_TOKEN"
timeout_ms = 45000
batch_size = 8

[usage_log]
path = "usage.jsonl"
""", encoding="utf-8")
            with mock.patch.dict(os.environ, {"YAPPOD_TEST_EMBEDDING_TOKEN": "secret"}):
                settings = wikipedia_data.load_embedding_settings(index_config, web_config)
            self.assertEqual(settings, wikipedia_data.EmbeddingSettings(
                "lmstudio", "http://localhost:1234/v1", "http://localhost:1234/v1/embeddings",
                "embedding-model", 3, 8, 45.0, "plain", "secret", (root / "usage.jsonl").resolve(),
            ))

    def test_load_embedding_settings_rejects_index_mismatch(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            index_config = root / "config.vector.toml"
            web_config = root / "config.toml"
            index_config.write_text(
                '[vector]\nenabled=true\nmodel_id="index-v1"\ndimensions=768\n', encoding="utf-8"
            )
            web_config.write_text("""
[embedding]
provider = "ollama"
base_url = "http://localhost:11434"
model = "embeddinggemma"
model_id = "different-index"
dimensions = 768
""", encoding="utf-8")
            with self.assertRaisesRegex(wikipedia_data.WikipediaDataError, "embedding.model_id"):
                wikipedia_data.load_embedding_settings(index_config, web_config)
            web_config.write_text("""
[embedding]
provider = "ollama"
base_url = "http://localhost:11434"
model = "embeddinggemma"
model_id = "index-v1"
dimensions = 384
""", encoding="utf-8")
            with self.assertRaisesRegex(wikipedia_data.WikipediaDataError, "dimensions"):
                wikipedia_data.load_embedding_settings(index_config, web_config)

    def test_load_embedding_settings_rejects_unknown_keys(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            index_config = root / "config.vector.toml"
            web_config = root / "config.toml"
            index_config.write_text(
                '[vector]\nenabled=true\nmodel_id="index-v1"\ndimensions=768\n', encoding="utf-8"
            )
            web_config.write_text("""
[embedding]
provider = "ollama"
base_url = "http://localhost:11434"
model = "embeddinggemma"
batch_szie = 8
""", encoding="utf-8")
            with self.assertRaisesRegex(wikipedia_data.WikipediaDataError, "batch_szie"):
                wikipedia_data.load_embedding_settings(index_config, web_config)

    def test_openai_endpoint_token_and_https_settings(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            index_config = root / "config.vector.toml"
            web_config = root / "config.toml"
            index_config.write_text(
                '[vector]\nenabled=true\nmodel_id="openai-768"\ndimensions=768\n', encoding="utf-8"
            )
            web_config.write_text("""
[embedding]
provider = "openai"
endpoint_url = "https://api.openai.com/v1/embeddings"
model = "text-embedding-3-small"
model_id = "openai-768"
dimensions = 768
prompt_profile = "plain"
authorization_token_env = "YAPPOD_TEST_OPENAI_TOKEN"
""", encoding="utf-8")
            with mock.patch.dict(os.environ, {"YAPPOD_TEST_OPENAI_TOKEN": "token"}):
                settings = wikipedia_data.load_embedding_settings(index_config, web_config)
            self.assertEqual(settings.provider, "openai")
            self.assertEqual(settings.endpoint_url, "https://api.openai.com/v1/embeddings")
            self.assertEqual(settings.authorization_token, "token")

            web_config.write_text(web_config.read_text(encoding="utf-8").replace(
                "https://api.openai.com", "http://api.openai.com"
            ), encoding="utf-8")
            with mock.patch.dict(os.environ, {"YAPPOD_TEST_OPENAI_TOKEN": "token"}):
                with self.assertRaisesRegex(wikipedia_data.WikipediaDataError, "must use https"):
                    wikipedia_data.load_embedding_settings(index_config, web_config)

            source = web_config.read_text(encoding="utf-8").replace(
                "http://api.openai.com", "https://api.openai.com"
            )
            web_config.write_text(source.replace(
                'authorization_token_env = "YAPPOD_TEST_OPENAI_TOKEN"',
                'authorization_token = "token"',
            ), encoding="utf-8")
            with self.assertRaisesRegex(wikipedia_data.WikipediaDataError, "not supported"):
                wikipedia_data.load_embedding_settings(index_config, web_config)

    def test_service_url_allows_only_explicit_http_private_ranges(self):
        for endpoint in (
            "http://localhost:1/v1",
            "http://127.255.0.1:1/v1",
            "http://10.2.3.4:1/v1",
            "http://172.31.0.1:1/v1",
            "http://192.168.1.2:1/v1",
            "http://[::1]:1/v1",
            "http://[fd12::1]:1/v1",
            "https://api.example.com/v1",
        ):
            with self.subTest(endpoint=endpoint):
                self.assertEqual(wikipedia_data._service_url(endpoint, "endpoint"), endpoint)
        for endpoint in (
            "http://example.com/v1",
            "http://8.8.8.8/v1",
            "http://169.254.1.1/v1",
            "http://[fe80::1]/v1",
        ):
            with self.subTest(endpoint=endpoint):
                with self.assertRaisesRegex(wikipedia_data.WikipediaDataError, "must use https"):
                    wikipedia_data._service_url(endpoint, "endpoint")

    def test_openai_batch_sends_dimensions_and_appends_usage(self):
        with tempfile.TemporaryDirectory() as directory:
            usage_log = Path(directory) / "usage.jsonl"
            response = {
                "data": [{"index": 0, "embedding": [1.0, 0.0]}],
                "usage": {"prompt_tokens": 4, "total_tokens": 4},
            }
            with mock.patch.object(wikipedia_data, "_post_json", return_value=response) as post:
                vectors = wikipedia_data._embedding_batch(
                    "openai", "https://api.openai.com/v1/embeddings", "text-embedding-3-small",
                    ["本文"], 2, 10.0, "token", usage_log,
                )
            self.assertEqual(vectors, [[1.0, 0.0]])
            self.assertEqual(post.call_args.args[1]["dimensions"], 2)
            event = read_ndjson(usage_log)[0]
            self.assertEqual(event["model"], "text-embedding-3-small")
            self.assertEqual(event["usage"]["total_tokens"], 4)

    def test_usage_log_failure_warns_without_raising(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "usage-directory"
            path.mkdir()
            stderr = io.StringIO()
            with mock.patch.object(wikipedia_data.sys, "stderr", stderr):
                wikipedia_data._append_usage_log(path, "openai", "text-embedding-3-small", None)
            self.assertIn("warning: cannot append usage log", stderr.getvalue())

    def test_embed_command_passes_toml_settings_to_adapter(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            index_config = root / "config.vector.toml"
            web_config = root / "config.toml"
            index_config.write_text(
                '[vector]\nenabled=true\nmodel_id="index-v1"\ndimensions=2\n', encoding="utf-8"
            )
            web_config.write_text("""
[embedding]
provider = "ollama"
base_url = "http://localhost:11434"
model = "embeddinggemma"
model_id = "index-v1"
dimensions = 2
""", encoding="utf-8")
            with mock.patch.object(wikipedia_data, "embed_documents", return_value=(1, 2)) as adapter:
                status = wikipedia_data.main([
                    "embed", "--documents", str(root / "documents.ndjson"),
                    "--passages", str(root / "passages.ndjson"),
                    "--output", str(root / "vector.ndjson"),
                    "--index-config", str(index_config), "--config", str(web_config),
                ])
            self.assertEqual(status, 0)
            self.assertEqual(adapter.call_args.args[3:], (
                "ollama", "http://localhost:11434/api/embed", "embeddinggemma", 2,
                16, 60.0, "plain", None, None,
            ))

    def test_embed_documents_uses_prepared_passage_order_and_lm_studio_indexes(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            documents = root / "documents.ndjson"
            passages = root / "passages.ndjson"
            output = root / "vector.ndjson"
            documents.write_text(
                json.dumps({"operation": "upsert", "id": "doc:1", "title": "記事", "body": "本文"}, ensure_ascii=False) + "\n",
                encoding="utf-8",
            )
            passages.write_text("".join([
                json.dumps({"operation": "upsert", "document_id": "doc:1", "passage_id": "p1", "ordinal": 0, "text": "第一"}, ensure_ascii=False) + "\n",
                json.dumps({"operation": "upsert", "document_id": "doc:1", "passage_id": "p2", "ordinal": 1, "text": "第二"}, ensure_ascii=False) + "\n",
            ]), encoding="utf-8")
            response = {"data": [
                {"index": 1, "embedding": [0, 1]},
                {"index": 0, "embedding": [1, 0]},
            ]}
            with mock.patch.object(wikipedia_data, "_post_json", return_value=response) as post:
                counts = wikipedia_data.embed_documents(
                    documents, passages, output, "lmstudio", "http://localhost:1234/v1/embeddings",
                    "embeddinggemma", 2, 16, 10.0, "embeddinggemma",
                )
            self.assertEqual(counts, (1, 2))
            self.assertEqual(read_ndjson(output)[0]["vectors"], [[1.0, 0.0], [0.0, 1.0]])
            self.assertEqual(post.call_args.args[0], "http://localhost:1234/v1/embeddings")
            self.assertEqual(post.call_args.args[1]["input"], [
                "title: 記事 | text: 第一",
                "title: 記事 | text: 第二",
            ])

    def test_embed_failure_does_not_replace_existing_output(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            documents = root / "documents.ndjson"
            passages = root / "passages.ndjson"
            output = root / "vector.ndjson"
            documents.write_text(
                '{"operation":"upsert","id":"doc:1","title":"記事","body":"本文"}\n', encoding="utf-8"
            )
            passages.write_text(
                '{"operation":"upsert","document_id":"doc:1","passage_id":"p1","ordinal":0,"text":"第一"}\n',
                encoding="utf-8",
            )
            output.write_text("original\n", encoding="utf-8")
            with mock.patch.object(
                wikipedia_data, "_post_json", return_value={"embeddings": [[float("nan"), 0.0]]}
            ), self.assertRaises(wikipedia_data.WikipediaDataError):
                wikipedia_data.embed_documents(
                    documents, passages, output, "ollama", "http://localhost:11434/api/embed",
                    "embeddinggemma", 2, 16, 10.0, "embeddinggemma",
                )
            self.assertEqual(output.read_text(encoding="utf-8"), "original\n")

    def test_embed_reports_whitespace_passage_location(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            documents = root / "documents.ndjson"
            passages = root / "passages.ndjson"
            output = root / "vector.ndjson"
            documents.write_text(
                '{"operation":"upsert","id":"jawiki:1","title":"記事","body":"本文"}\n',
                encoding="utf-8",
            )
            passages.write_text(
                '{"operation":"upsert","document_id":"jawiki:1","passage_id":"p1",'
                '"ordinal":0,"text":"\\n　"}\n',
                encoding="utf-8",
            )
            with self.assertRaisesRegex(
                wikipedia_data.WikipediaDataError, "jawiki:1 ordinal 0"
            ):
                wikipedia_data.embed_documents(
                    documents, passages, output, "ollama", "http://localhost:11434/api/embed",
                    "embeddinggemma", 2, 16, 10.0, "embeddinggemma",
                )


if __name__ == "__main__":
    unittest.main()
