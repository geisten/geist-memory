#!/usr/bin/env python3
"""Validate the fixed SciFact-256/100 report and conservative regression floors."""
from pathlib import Path
import sys


def check(text):
    lines = text.splitlines()
    if 'model_source=GGUF' not in lines or any('model_source=mock' in l for l in lines):
        raise ValueError('real GGUF evidence required')
    corpus = [dict(item.split('=', 1) for item in l.split()) for l in lines if l.startswith('corpus=')]
    if len(corpus) != 1 or any(corpus[0].get(k) != v for k, v in {
        'corpus': 'scifact-256-100-v1', 'documents': '256', 'dimension': '1024',
        'window': '256', 'omit_bos': '1', 'omit_eos': '0'}.items()):
        raise ValueError('wrong corpus or token policy')
    if 'model_sha256=4321e21b9da533f40386aa5ab968cced6196e21ecbf395ae04e5bce1ee88e767' not in lines:
        raise ValueError('wrong model hash')
    if 'query_prefix=query: ' not in lines:
        raise ValueError('wrong query prefix')
    rows = [dict(item.split('=', 1) for item in l.split()) for l in lines if l.startswith('query=')]
    if len(rows) != 100 or sorted(int(r['query']) for r in rows) != list(range(100)):
        raise ValueError('missing or duplicate queries')
    for r in rows:
        if r['language'] != 'en' or not 0 <= int(r['relevant']) < 256:
            raise ValueError('invalid query metadata')
        if any(not 1 <= int(r[k]) <= 256 for k in ('float_rank', 'binary_rank')):
            raise ValueError('invalid rank')
    floats = sum(int(r['float_rank']) <= 3 for r in rows)
    binary = sum(int(r['binary_rank']) <= 3 for r in rows)
    if floats < 50 or binary < 45 or floats - binary > 15:
        raise ValueError(f'quality floor failed: float@3={floats}/100 binary@3={binary}/100')
    return f'PASS: 100 queries; float@3={floats}/100 binary@3={binary}/100; fixed regression floors'


if __name__ == '__main__':
    if len(sys.argv) != 2:
        raise SystemExit('usage: check-model-quality.py QUALITY_LOG')
    try:
        print(check(Path(sys.argv[1]).read_text()))
    except (OSError, ValueError, KeyError) as error:
        raise SystemExit(str(error)) from error
