#!/usr/bin/env python3
"""Check published Giscus configuration without posting any test comments."""
from html.parser import HTMLParser
from pathlib import Path
from urllib.parse import unquote
import json
import tomllib

ROOT = Path(__file__).resolve().parents[1]
PUBLIC = ROOT / 'public'
config = tomllib.loads((ROOT / 'config/_default/params.toml').read_text())

class Scripts(HTMLParser):
    def __init__(self):
        super().__init__()
        self.scripts = []
    def handle_starttag(self, tag, attrs):
        if tag == 'script':
            self.scripts.append(dict(attrs))

index = json.loads((PUBLIC / 'search/index.json').read_text())
assert index, 'Search index must include posts'
for post in index:
    html = (PUBLIC / unquote(post['permalink']).lstrip('/') / 'index.html').read_text()
    parser = Scripts()
    parser.feed(html)
    scripts = [s for s in parser.scripts if s.get('src') == 'https://giscus.app/client.js']
    assert len(scripts) == 1, f"Expected one Giscus client on {post['permalink']}"
    attrs = scripts[0]
    expected = {
        'data-repo': 'lixiaojun2914/lixiaojun2914.github.io',
        'data-repo-id': 'R_kgDOGwEMGg',
        'data-mapping': 'pathname',
        'data-strict': '1',
        'data-lang': 'zh-CN',
        'data-reactions-enabled': '1',
        'data-input-position': 'top',
        'data-loading': 'lazy',
    }
    for key, value in expected.items():
        assert attrs.get(key) == value, f"{post['permalink']}: wrong {key}"
    assert attrs.get('data-category-id', '').startswith('DIC_'), 'Missing discussion category ID'
    assert attrs.get('data-category') == config['comments']['giscus']['category']
    assert attrs['data-category-id'] == config['comments']['giscus']['categoryID']
    assert 'waline-container' not in html and 'Waline.init(' not in html, 'Waline still loaded'

for page in ['about', 'archives', 'search', 'links']:
    html = (PUBLIC / page / 'index.html').read_text()
    assert 'https://giscus.app/client.js' not in html, f'Comments should remain disabled on {page}'

print(f'PASS: Giscus on {len(index)} posts; stable pathname mapping; no Waline; utility pages unchanged')
