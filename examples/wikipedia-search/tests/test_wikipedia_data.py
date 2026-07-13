import importlib.util
import json
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
            return response

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

        responses = [
            {
                "continue": {"gsroffset": 20, "continue": "gsroffset||"},
                "query": {"pages": [page(page_id) for page_id in range(1, 21)]},
            },
            {
                "continue": {"gsroffset": 40, "continue": "gsroffset||"},
                "query": {"pages": [page(page_id) for page_id in range(21, 41)]},
            },
            {"batchcomplete": True, "query": {"pages": [page(page_id) for page_id in range(41, 56)]}},
        ]
        requests = []

        def fake_request(url, user_agent):
            requests.append((url, user_agent))
            return responses[len(requests) - 1]

        with tempfile.TemporaryDirectory() as directory, mock.patch.object(
            wikipedia_data, "_request_json", side_effect=fake_request
        ):
            output = Path(directory) / "documents.ndjson"
            written, skipped = wikipedia_data.fetch_api_documents(
                "https://example.test/w/api.php", ["日本史"], 55, output, "fixture-agent/1.0"
            )
            documents = read_ndjson(output)

        self.assertEqual((written, skipped), (55, 0))
        self.assertEqual(len(requests), 3)
        first_query = parse_qs(urlparse(requests[0][0]).query)
        second_query = parse_qs(urlparse(requests[1][0]).query)
        third_query = parse_qs(urlparse(requests[2][0]).query)
        self.assertEqual(first_query["gsrlimit"], ["20"])
        self.assertEqual(first_query["exlimit"], ["20"])
        self.assertEqual(second_query["gsroffset"], ["20"])
        self.assertEqual(second_query["continue"], ["gsroffset||"])
        self.assertEqual(second_query["gsrlimit"], ["20"])
        self.assertEqual(second_query["exlimit"], ["20"])
        self.assertEqual(third_query["gsroffset"], ["40"])
        self.assertEqual(third_query["gsrlimit"], ["15"])
        self.assertEqual(third_query["exlimit"], ["15"])
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
                    documents, passages, output, "lmstudio", "http://localhost:1234/v1",
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
                    documents, passages, output, "ollama", "http://localhost:11434",
                    "embeddinggemma", 2, 16, 10.0, "embeddinggemma",
                )
            self.assertEqual(output.read_text(encoding="utf-8"), "original\n")


if __name__ == "__main__":
    unittest.main()
