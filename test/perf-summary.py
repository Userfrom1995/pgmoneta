#!/usr/bin/env python3
"""Perf summary renderer (extracted from .github/workflows/ci.yml).

Byte-faithful extraction of the embedded python from the ci.yml
perf-summary job (same parsing, same two tables, same JSON shape)
PLUS: SYS_RESULT lines -> dataset['meta']['hosts'][pg] = {cpu, kernel}.
Summary markdown is unchanged (hosts only affect JSON meta).
"""
import glob
import json
import os
import re
import statistics
import sys

perf_re = re.compile(r'^PERF_RESULT\s+(.*)$')
cpu_re = re.compile(r'^CPU_RESULT\s+(.*)$')
lat_re = re.compile(r'^LAT_RESULT\s+(.*)$')
sys_re = re.compile(r'^SYS_RESULT\s+(.*)$')
kv_re = re.compile(r'(\w+)=(\S+)')
try:
    expected_reps = int(os.environ.get('PERF_REPS', '3'))
except ValueError:
    expected_reps = 3

try:
    with open('perf-meta/baseline-sha.txt') as f:
        base_sha = f.read().strip() or 'unknown'
except OSError:
    base_sha = 'unknown'

dropped = 0
malformed = 0
groups = {}
cpu_by_rep = {}
latencies = {}
hosts = {}
files = sorted(glob.glob('perf-results/*.txt'))
for path in files:
    with open(path) as f:
        for raw in f:
            s = raw.strip()
            if not s:
                continue
            m = perf_re.match(s)
            if m:
                kvs = dict(kv_re.findall(m.group(1)))
                try:
                    key = (kvs['build'], kvs['backend'], kvs['pg'], kvs.get('flags', 'base'))
                    rep = int(kvs['rep'])
                    ms = int(kvs['ms'])
                    nbytes = int(kvs['bytes'])
                except (KeyError, ValueError):
                    malformed += 1
                    continue
                groups.setdefault(key, []).append((rep, ms, nbytes))
                continue
            m = cpu_re.match(s)
            if m:
                kvs = dict(kv_re.findall(m.group(1)))
                try:
                    key = (kvs['build'], kvs['backend'], kvs['pg'], kvs.get('flags', 'base'), int(kvs['rep']))
                    cpu_by_rep[key] = float(kvs['cpu_s'])
                except (KeyError, ValueError):
                    malformed += 1
                continue
            m = lat_re.match(s)
            if m:
                kvs = dict(kv_re.findall(m.group(1)))
                try:
                    key = (kvs['build'], kvs['backend'], kvs['pg'], kvs.get('flags', 'base'), kvs['mode'])
                    latencies[key] = {
                        'n': int(kvs['n']),
                        'p50_ms': float(kvs['p50_ms']),
                        'p99_ms': float(kvs['p99_ms']),
                        'max_ms': float(kvs['max_ms']),
                    }
                except (KeyError, ValueError):
                    malformed += 1
                continue
            m = sys_re.match(s)
            if m:
                kvs = dict(kv_re.findall(m.group(1)))
                try:
                    hosts[kvs['pg']] = {'cpu': kvs['cpu'], 'kernel': kvs['kernel']}
                except KeyError:
                    malformed += 1
                continue
            dropped += 1

def mbps(ms, nbytes):
    if ms <= 0:
        return None
    return (nbytes / 1048576.0) / (ms / 1000.0)

rows = []
for key in sorted(groups, key=lambda k: (k[2], k[3], k[0], k[1])):
    build, backend, pg, flags = key
    vals = [v for v in (mbps(ms, b) for _, ms, b in groups[key]) if v is not None]
    if not vals:
        continue
    cpus = [cpu_by_rep.get((build, backend, pg, flags, rep)) for rep, _, _ in groups[key]]
    cpus = [c for c in cpus if c is not None]
    med_cpu = statistics.median(cpus) if cpus else None
    med_bytes = int(statistics.median([by for _, _, by in groups[key]]))
    rows.append((build, backend, pg, flags, len(vals), statistics.median(vals), med_cpu, med_bytes))

baselines = {pg: med for (build, backend, pg, flags, n, med, _, _) in rows
             if build == 'main' and backend == 'epoll' and flags == 'base'}

with open(os.environ['GITHUB_STEP_SUMMARY'], 'a') as out:
    out.write('## Perf summary (backup throughput)\n\n')
    out.write('Baseline `main`/epoll/base pinned to main HEAD: `{}`\n\n'.format(base_sha))
    out.write('Parsed {} file(s): {} dropped line(s), {} malformed result line(s).\n\n'.format(len(files), dropped, malformed))
    if not rows:
        out.write('ERROR: No PERF_RESULT lines found in perf-results artifacts ')
        out.write('(files seen: {}; dropped: {}; malformed: {}). '.format(len(files), dropped, malformed))
        out.write('Producer jobs may have failed/skipped before emitting perf — see the preamble above.\n')
        sys.exit(1)
    out.write('| Source | Backend | PG | Flags | Reps | Median backup | Median MB/s | Delta vs main/epoll/base |\n')
    out.write('|---|---|---|---|---|---|---|---|\n')
    for (build, backend, pg, flags, n, med, _, med_bytes) in rows:
        cells = [build, backend, pg, flags, str(n), '{:.1f}'.format(med_bytes / 1048576.0), '{:.1f}'.format(med)]
        if n < expected_reps:
            cells.append('WARNING: partial perf data ({}/{})'.format(n, expected_reps))
        else:
            base = baselines.get(pg)
            if base is None or base <= 0:
                cells.append('MISSING main/epoll/base baseline for pg {}'.format(pg))
            elif build == 'main' and backend == 'epoll' and flags == 'base':
                cells.append('baseline')
            else:
                cells.append('{:+.1f}%'.format((med - base) / base * 100.0))
        out.write('| {} |\n'.format(' | '.join(cells)))
    out.write('\nNote: `main` uses the libev backend spelling `iouring` (no underscore); `io_layer` uses `io_uring`.\n')
    out.write('\n## Efficiency (CPU + management latency)\n\n')
    out.write('| Source | Backend | PG | Flags | Median CPU s/rep | MB per CPU-s | ping p50/p99 seq (ms) | ping p50/p99 burst (ms) |\n')
    out.write('|---|---|---|---|---|---|---|---|\n')
    for (build, backend, pg, flags, n, med, med_cpu, _) in rows:
        med_mb = statistics.median([by for _, _, by in groups[(build, backend, pg, flags)]]) / 1048576.0
        if med_cpu is None or med_cpu <= 0:
            cpu_s, eff = 'n/a', 'n/a'
        else:
            cpu_s, eff = '{:.2f}'.format(med_cpu), '{:.1f}'.format(med_mb / med_cpu)
        seq = latencies.get((build, backend, pg, flags, 'sequential'))
        bst = latencies.get((build, backend, pg, flags, 'burst'))
        seq_s = '{:.2f}/{:.2f}'.format(seq['p50_ms'], seq['p99_ms']) if seq else 'n/a'
        bst_s = '{:.2f}/{:.2f}'.format(bst['p50_ms'], bst['p99_ms']) if bst else 'n/a'
        out.write('| {} |\n'.format(' | '.join([build, backend, pg, flags, cpu_s, eff, seq_s, bst_s])))
    out.write('\nFull machine-readable dataset: `perf-results.json` artifact below.\n')

dataset = {
    'meta': {'baseline_sha': base_sha, 'files': len(files),
             'dropped': dropped, 'malformed': malformed, 'hosts': hosts},
    'throughput': [
        {'build': b, 'backend': be, 'pg': pg, 'flags': fl, 'reps': n,
         'median_mbs': med, 'median_bytes': med_bytes,
         'samples': [{'rep': r, 'ms': ms, 'bytes': by,
                      'cpu_s': cpu_by_rep.get((b, be, pg, fl, r))}
                     for r, ms, by in sorted(groups[(b, be, pg, fl)])]}
        for (b, be, pg, fl, n, med, _, med_bytes) in rows
    ],
    'latency': [
        {'build': b, 'backend': be, 'pg': pg, 'flags': fl, 'mode': m, **v}
        for (b, be, pg, fl, m), v in sorted(latencies.items())
    ],
}
with open('perf-results.json', 'w') as f:
    json.dump(dataset, f, indent=2)
print('Wrote perf-results.json: {} throughput groups, {} latency entries'.format(len(rows), len(latencies)))
