#!/usr/bin/env python3
"""Verify preserved source, legacy URLs, local assets and search after a Hugo build."""
from pathlib import Path
from html.parser import HTMLParser
from urllib.parse import urlsplit, unquote, urljoin
import hashlib
import json
import sys

ROOT = Path(__file__).resolve().parents[1]
PUBLIC = ROOT / "public"
manifest = json.loads((ROOT / "docs/migration-manifest.json").read_text())
errors = []

def check(condition, message):
    if not condition:
        errors.append(message)

for name, digest in manifest["files"].items():
    p = ROOT / name
    check(p.is_file(), "Missing source: " + name)
    if p.is_file():
        check(hashlib.sha256(p.read_bytes()).hexdigest() == digest,
              "Tracked content changed: " + name + " (update manifest after intentional edits)")

for path in manifest["legacy_routes"]:
    check((PUBLIC / path.lstrip("/")).is_file(), "Missing legacy URL: " + path)

class Links(HTMLParser):
    def __init__(self):
        super().__init__()
        self.urls = []
    def handle_starttag(self, tag, attrs):
        data = dict(attrs)
        for key in ("href", "src"):
            if data.get(key):
                self.urls.append(data[key])
        if data.get("srcset"):
            self.urls.extend(part.strip().split()[0] for part in data["srcset"].split(",") if part.strip())

pages = list(PUBLIC.rglob("*.html"))
check(bool(pages), "No generated pages; run hugo first")
for p in pages:
    parser = Links()
    parser.feed(p.read_text())
    page_url = "/" + p.relative_to(PUBLIC).as_posix()
    if page_url.endswith("index.html"):
        page_url = page_url[:-10]
    for url in parser.urls:
        parsed = urlsplit(url)
        if parsed.scheme and parsed.scheme not in ("http", "https"):
            continue
        if parsed.netloc and parsed.netloc != "lixiaojun2914.github.io":
            continue
        if not parsed.path:
            continue
        path = unquote(urlsplit(urljoin(page_url, parsed.path)).path)
        target = PUBLIC / path.lstrip("/")
        check(target.is_file() or (target / "index.html").is_file(),
              f"Broken local URL in {p.relative_to(PUBLIC)}: {url}")

index = PUBLIC / "search/index.json"
check(index.is_file(), "Missing search JSON")
if index.is_file():
    data = json.loads(index.read_text())
    expected = {"/p/c-lang/", "/p/test-post/", "/p/markdown/", "/p/numpy/",
                "/p/python/", "/p/pytorch/", "/p/rich-content/", "/p/哈哈哈哈/"}
    check(expected.issubset({unquote(item["permalink"]) for item in data}), "Search must include all 8 original posts")
    check(any("列表生成式" in item["content"] for item in data), "Chinese full-text content missing from search")

if errors:
    print("FAILED:", *sorted(set(errors)), sep="\n- ")
    sys.exit(1)
print(f"PASS: {len(manifest['files'])} tracked source snapshots match manifest; "
      f"{len(manifest['legacy_routes'])} legacy URLs; {len(pages)} HTML pages/local assets; all 8 original posts searchable")
