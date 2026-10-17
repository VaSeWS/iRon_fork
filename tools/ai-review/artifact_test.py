import io
import os
import tempfile
import unittest
import urllib.error
import urllib.request
import zipfile
from unittest import mock
from urllib.parse import parse_qs, urlsplit

import artifact


HASH = "a" * 64
REPO = artifact.REPO


def run(**changes):
    value = {
        "id": 42,
        "workflow_id": 7,
        "path": artifact.WORKFLOW,
        "head_branch": "main",
        "event": "push",
        "status": "completed",
        "conclusion": "success",
        "repository": {"full_name": REPO},
    }
    value.update(changes)
    return value


def artifact_record(**changes):
    value = {
        "id": 9,
        "name": f"rag-index-v1-{HASH}",
        "expired": False,
        "workflow_run": {"id": 42},
    }
    value.update(changes)
    return value


def archive(entries):
    out = io.BytesIO()
    with zipfile.ZipFile(out, "w") as bundle:
        for name, data, mode in entries:
            info = zipfile.ZipInfo(name)
            if mode is not None:
                info.external_attr = mode << 16
            bundle.writestr(info, data)
    return out.getvalue()


class ProvenanceTests(unittest.TestCase):
    def test_filters_spoofed_runs_and_expired_or_wrong_run_artifacts(self):
        fake_runs = [
            run(path=".github/workflows/other.yml"),
            run(event="pull_request"),
            run(conclusion="failure"),
            run(head_branch="pr-branch"),
            run(repository={"full_name": "attacker/repo"}),
            run(id=41),
            run(),
        ]

        def pages(url, token):
            if "/actions/runs/" in url:
                return iter([
                    artifact_record(expired=True),
                    artifact_record(workflow_run={"id": 999}),
                    artifact_record(name="rag-index-v1-" + "b" * 64),
                    artifact_record(),
                ])
            return iter(fake_runs)

        with mock.patch.object(artifact, "_api_json", return_value={"id": 7}), mock.patch.object(artifact, "_pages", side_effect=pages):
            result = artifact._trusted_artifact(REPO, HASH, "credential")
        self.assertEqual(result["id"], 9)

    def test_excludes_expired_artifact(self):
        with mock.patch.object(artifact, "_api_json", return_value={"id": 7}), mock.patch.object(
            artifact, "_pages", side_effect=[iter([run()]), iter([artifact_record(expired=True)])]
        ):
            self.assertIsNone(artifact._trusted_artifact(REPO, HASH, "credential"))

    def test_paginates_full_api_pages(self):
        def json_response(url, token):
            if parse_qs(urlsplit(url).query).get("page") == ["1"]:
                return {"artifacts": [{"id": i} for i in range(100)]}
            return {"artifacts": [{"id": 100}]}

        with mock.patch.object(artifact, "_api_json", side_effect=json_response):
            values = list(artifact._pages("https://api.github.test/artifacts", "credential"))
        self.assertEqual([value["id"] for value in values], list(range(101)))


class ArchiveTests(unittest.TestCase):
    def test_accepts_only_one_regular_index(self):
        self.assertEqual(artifact._validated_index(archive([("index.json", b"{}", None)])), b"{}")

    def test_rejects_extra_duplicate_and_traversal_entries(self):
        cases = [
            [("index.json", b"{}", None), ("README.txt", b"x", None)],
            [("index.json", b"{}", None), ("index.json", b"{}", None)],
            [("../index.json", b"{}", None)],
            [("/index.json", b"{}", None)],
            [("dir/index.json", b"{}", None)],
        ]
        for entries in cases:
            with self.subTest(entries=[entry[0] for entry in entries]), self.assertRaises(artifact.ArtifactError):
                artifact._validated_index(archive(entries))

    def test_rejects_symlink_and_special_files(self):
        for mode in (0o120777, 0o060000):
            with self.subTest(mode=oct(mode)), self.assertRaises(artifact.ArtifactError):
                artifact._validated_index(archive([("index.json", b"target", mode)]))

    def test_rejects_uncompressed_size_over_limit(self):
        with mock.patch.object(artifact, "MAX_INDEX", 4), self.assertRaises(artifact.ArtifactError):
            artifact._validated_index(archive([("index.json", b"12345", None)]))

    def test_rejects_corrupt_zip_without_echoing_content(self):
        with self.assertRaises(artifact.ArtifactError) as caught:
            artifact._validated_index(b"private-token-and-not-a-zip")
        self.assertNotIn("private-token", str(caught.exception))

    def test_failed_download_never_writes_archive_entries(self):
        with tempfile.TemporaryDirectory() as folder:
            destination = os.path.join(folder, "index.json")
            with mock.patch.object(artifact, "_token", return_value="secret"), mock.patch.object(
                artifact, "_trusted_artifact", return_value=artifact_record()
            ), mock.patch.object(
                artifact, "_archive", return_value=archive([("../escape", b"bad", None)])
            ):
                with self.assertRaises(artifact.ArtifactError):
                    artifact.download(REPO, HASH, destination)
            self.assertFalse(os.path.exists(destination))
            self.assertFalse(os.path.exists(os.path.join(folder, "escape")))

    def test_cross_host_redirect_drops_authentication_headers(self):
        request = urllib.request.Request(
            "https://api.github.com/archive",
            headers={"Authorization": "Bearer secret", "Cookie": "session=secret"},
        )
        redirected = artifact.SafeRedirect().redirect_request(
            request, None, 302, "Found", {}, "https://objects.githubusercontent.com/archive"
        )
        self.assertIsNotNone(redirected)
        self.assertIsNone(redirected.get_header("Authorization"))
        self.assertIsNone(redirected.get_header("Cookie"))

    def test_request_errors_are_sanitized(self):
        secret = "never-print-this-token"
        opener = mock.Mock()
        opener.open.side_effect = urllib.error.URLError(secret)
        with mock.patch.object(artifact, "_opener", return_value=opener):
            with self.assertRaises(artifact.ArtifactError) as caught:
                artifact._request("https://api.github.com/private", secret, 100)
        self.assertNotIn(secret, str(caught.exception))


if __name__ == "__main__":
    unittest.main()
