#!/usr/bin/env python3
"""Create the fixed SciFact-256/100 test fixture; stdlib only, no extraction."""
import csv
import hashlib
import io
import json
from pathlib import Path
import sys
import zipfile

SHA = '536e14446a0ba56ed1398ab1055f39fe852686ecad24a6306c80c490fa8e0165'


def prepare(source, output):
    if hashlib.sha256(source.read_bytes()).hexdigest() != SHA:
        raise ValueError('unsupported SciFact archive SHA-256')
    with zipfile.ZipFile(source) as archive:
        def records(name):
            return [json.loads(line) for line in archive.read('scifact/' + name).splitlines()]
        corpus = {r['_id']: r for r in records('corpus.jsonl')}
        queries = {r['_id']: r['text'] for r in records('queries.jsonl')}
        qrels = {}
        rows = csv.DictReader(io.StringIO(archive.read('scifact/qrels/test.tsv').decode()), delimiter='\t')
        for row in rows:
            if int(row['score']) > 0:
                qrels.setdefault(row['query-id'], set()).add(row['corpus-id'])
    # Exactly one relevant document: preserves the existing evaluator's contract.
    selected = sorted((q for q, rel in qrels.items() if len(rel) == 1), key=int)[:100]
    required = {next(iter(qrels[q])) for q in selected}
    # Deterministic distractors independent of text, model outputs and quality scores.
    distractors = sorted(set(corpus) - required,
                         key=lambda d: hashlib.sha256(('geist-scifact-v1:' + d).encode()).digest())
    ids = sorted(required | set(distractors[:256 - len(required)]), key=int)
    assert len(ids) == 256 and len(selected) == 100
    lines = ['/* Generated from SHA-pinned BEIR SciFact; see docs/MODEL_BENCHMARK.md. */',
             '#define RETRIEVAL_CORPUS "scifact-256-100-v1"',
             ]
    documents = []
    for i, d in enumerate(ids):
        text = corpus[d]['title'] + ' ' + corpus[d]['text']
        if len(text.encode()) > 4095:
            values = ','.join(str(b) for b in text.encode()) + ',0'
            lines.append(f'static const unsigned char long_doc_{i}[] = {{{values}}};')
            documents.append(f'(const char *)long_doc_{i},')
        else:
            documents.append(json.dumps(text, ensure_ascii=True) + ',')
    lines += ['static const char *const retrieval_docs[] = {'] + documents
    lines += ['};', 'static const struct { const char *language, *text; size_t relevant; } retrieval_queries[] = {']
    for q in selected:
        lines.append('{"en", ' + json.dumps(queries[q], ensure_ascii=True) + ', ' + str(ids.index(next(iter(qrels[q])))) + '},')
    lines.append('};')
    output.write_text('\n'.join(lines) + '\n')
    manifest = {'version': 'scifact-256-100-v1', 'archive_sha256': SHA,
                'header_sha256': hashlib.sha256(output.read_bytes()).hexdigest(),
                'document_ids': ids, 'query_ids': selected,
                'policy': 'first 100 numerically sorted test queries with exactly one positive; all positives plus SHA-ordered distractors'}
    output.with_suffix('.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print(json.dumps({k: v for k, v in manifest.items() if not k.endswith('_ids')}))


if __name__ == '__main__':
    if len(sys.argv) != 3:
        raise SystemExit('usage: prepare-quality.py SCIFACT_ZIP OUTPUT_HEADER')
    try:
        prepare(Path(sys.argv[1]), Path(sys.argv[2]))
    except (OSError, ValueError, KeyError, zipfile.BadZipFile) as error:
        raise SystemExit(str(error)) from error
