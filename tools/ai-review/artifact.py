#!/usr/bin/env python3
"""Find and safely download trusted main-branch RAG index artifacts."""

import argparse
import json
import os
import re
import stat
import sys
import urllib.error
import urllib.parse
import urllib.request
import zipfile
from io import BytesIO

API = "https://api.github.com"
REPO = "VaSeWS/iRon_fork"
WORKFLOW = ".github/workflows/rag-index.yml"
MAX_ARCHIVE = 16 * 1024 * 1024
MAX_INDEX = 32 * 1024 * 1024
HASH_RE = re.compile(r"[0-9a-f]{64}\Z")


class ArtifactError(Exception):
    """A sanitized error safe to print in CI logs."""


class SafeRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        redirected = super().redirect_request(req, fp, code, msg, headers, newurl)
        if redirected is not None and urllib.parse.urlsplit(req.full_url).netloc != urllib.parse.urlsplit(newurl).netloc:
            redirected.remove_header("Authorization")
            redirected.remove_header("Cookie")
        return redirected


def _opener():
    return urllib.request.build_opener(SafeRedirect())


def _token():
    token = os.environ.get("GH_TOKEN") or os.environ.get("GITHUB_TOKEN")
    if not token:
        raise ArtifactError("GH_TOKEN or GITHUB_TOKEN is required")
    return token


def _request(url, token, limit):
    req = urllib.request.Request(url, headers={
        "Accept": "application/vnd.github+json",
        "Authorization": "Bearer " + token,
        "X-GitHub-Api-Version": "2022-11-28",
        "User-Agent": "iron-ai-review-artifact-helper",
    })
    try:
        with _opener().open(req, timeout=30) as response:
            data = response.read(limit + 1)
    except (urllib.error.URLError, OSError, TimeoutError):
        raise ArtifactError("GitHub artifact API request failed") from None
    if len(data) > limit:
        raise ArtifactError("GitHub response exceeds the configured size limit")
    return data


def _api_json(url, token):
    try:
        value = json.loads(_request(url, token, 4 * 1024 * 1024))
    except (UnicodeDecodeError, json.JSONDecodeError):
        raise ArtifactError("GitHub API returned invalid JSON") from None
    if not isinstance(value, dict):
        raise ArtifactError("GitHub API returned an invalid response")
    return value


def _pages(url, token):
    page = 1
    while True:
        parsed = urllib.parse.urlsplit(url)
        query = urllib.parse.parse_qs(parsed.query)
        query.update({"per_page": ["100"], "page": [str(page)]})
        page_url = urllib.parse.urlunsplit((parsed.scheme, parsed.netloc, parsed.path, urllib.parse.urlencode(query, doseq=True), ""))
        data = _api_json(page_url, token)
        items = data.get("workflow_runs", data.get("artifacts"))
        if not isinstance(items, list):
            raise ArtifactError("GitHub API returned an invalid paginated response")
        yield from items
        if len(items) < 100:
            return
        page += 1


def _trusted_artifact(repo, digest, token):
    workflow_url = f"{API}/repos/{repo}/actions/workflows/rag-index.yml"
    workflow = _api_json(workflow_url, token)
    workflow_id = workflow.get("id")
    if not isinstance(workflow_id, int):
        raise ArtifactError("Trusted index workflow was not found")
    runs_url = f"{API}/repos/{repo}/actions/runs?branch=main"
    for run in _pages(runs_url, token):
        run_repo = run.get("repository") if isinstance(run, dict) else None
        if (not isinstance(run, dict) or run.get("workflow_id") != workflow_id or run.get("path") != WORKFLOW
                or run.get("head_branch") != "main" or run.get("event") not in ("push", "workflow_dispatch")
                or run.get("status") != "completed" or run.get("conclusion") != "success"
                or not isinstance(run_repo, dict) or not isinstance(run_repo.get("full_name"), str)
                or run_repo.get("full_name").lower() != repo.lower()):
            continue
        run_id = run.get("id")
        if not isinstance(run_id, int):
            continue
        for artifact in _pages(f"{API}/repos/{repo}/actions/runs/{run_id}/artifacts", token):
            artifact_run = artifact.get("workflow_run") if isinstance(artifact, dict) else None
            if (isinstance(artifact, dict) and artifact.get("name") == f"rag-index-v1-{digest}"
                    and artifact.get("expired") is False and isinstance(artifact.get("id"), int)
                    and isinstance(artifact_run, dict) and artifact_run.get("id") == run_id):
                return artifact
    return None


def find(repo, digest):
    artifact = _trusted_artifact(repo, digest, _token())
    if artifact is None:
        return {"found": False}
    return {"found": True, "id": artifact["id"], "run_id": artifact["workflow_run"]["id"]}


def _archive(repo, artifact_id, token):
    url = f"{API}/repos/{repo}/actions/artifacts/{artifact_id}/zip"
    return _request(url, token, MAX_ARCHIVE)


def _validated_index(archive):
    try:
        with zipfile.ZipFile(BytesIO(archive)) as bundle:
            entries = bundle.infolist()
            if len(entries) != 1 or entries[0].filename != "index.json":
                raise ArtifactError("Artifact archive must contain only index.json")
            info = entries[0]
            mode = info.external_attr >> 16
            file_type = stat.S_IFMT(mode)
            if info.is_dir() or (file_type not in (0, stat.S_IFREG)):
                raise ArtifactError("Artifact index.json must be a regular file")
            if info.file_size < 1 or info.file_size > MAX_INDEX:
                raise ArtifactError("Artifact index exceeds the configured size limit")
            with bundle.open(info) as stream:
                content = stream.read(MAX_INDEX + 1)
            if len(content) != info.file_size or len(content) > MAX_INDEX:
                raise ArtifactError("Artifact index has an invalid uncompressed size")
            return content
    except (zipfile.BadZipFile, RuntimeError, EOFError, OSError, ValueError):
        raise ArtifactError("Artifact archive is invalid or corrupt") from None


def download(repo, digest, out):
    token = _token()
    artifact = _trusted_artifact(repo, digest, token)
    if artifact is None:
        raise ArtifactError("No successful trusted main index artifact found. Build one with: gh workflow run rag-index.yml --repo VaSeWS/iRon_fork --ref main")
    content = _validated_index(_archive(repo, artifact["id"], token))
    destination = os.path.abspath(out)
    os.makedirs(os.path.dirname(destination), exist_ok=True)
    temp = destination + ".tmp"
    try:
        with open(temp, "wb") as stream:
            stream.write(content)
        os.replace(temp, destination)
    except OSError:
        try:
            os.unlink(temp)
        except OSError:
            pass
        raise ArtifactError("Unable to write downloaded index") from None
    return {"found": True, "id": artifact["id"], "run_id": artifact["workflow_run"]["id"], "size": len(content)}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)
    for command in ("find", "download"):
        sub = subparsers.add_parser(command)
        sub.add_argument("--repo", required=True)
        sub.add_argument("--hash", required=True)
        if command == "download":
            sub.add_argument("--out", required=True)
    args = parser.parse_args(argv)
    if args.repo != REPO:
        parser.error(f"--repo must be {REPO}")
    if not HASH_RE.fullmatch(args.hash):
        parser.error("--hash must be 64 lowercase hexadecimal characters")
    try:
        result = find(args.repo, args.hash) if args.command == "find" else download(args.repo, args.hash, args.out)
        print(json.dumps(result, separators=(",", ":")))
        return 0
    except ArtifactError as error:
        print(f"artifact: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
