#!/usr/bin/env python3
"""Generate real-fixture vectors by invoking the unmodified upstream ACDB API."""
import argparse
import hashlib
import json
import struct
from pathlib import Path
from upstream_oracle import Oracle, build

U = struct.Struct('<I')

def word(data, offset):
    return U.unpack_from(data, offset)[0]

def chunks(data):
    result = {}
    offset = 12
    while offset < len(data):
        name, size = struct.unpack_from('<4sI', data, offset)
        assert name not in result
        result[name] = memoryview(data)[offset + 8:offset + 8 + size]
        offset += 8 + size
    assert offset == len(data)
    return result

def graphs(data):
    parts = chunks(data)
    kt, lut = parts[b'GKVT'], parts[b'GKVL']
    offset = 4
    for _ in range(word(kt, 0)):
        count, rows = struct.unpack_from('<II', kt, offset)
        offset += 8
        for _ in range(rows):
            keys = struct.unpack_from('<' + 'I' * count, kt, offset)
            table = word(kt, offset + count * 4)
            offset += (count + 1) * 4
            values, entries = struct.unpack_from('<II', lut, table)
            assert values == count
            for row in range(entries):
                at = table + 8 + row * (count + 2) * 4
                yield list(zip(keys, struct.unpack_from('<' + 'I' * count, lut, at)))
    assert offset == len(kt)

def request(gkv, ckv):
    values = [len(gkv), len(ckv)]
    for pairs in (gkv, ckv):
        values.extend(x for pair in pairs for x in pair)
        values.extend([0] * (64 - len(pairs) * 2))
    values.extend([0xc0000004, 48000, 16, 2, 1, 0, 3, 0x100, 1, 2, 3])
    return struct.pack('<141I', *values)

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', required=True, type=Path)
    parser.add_argument('--fixtures', required=True, type=Path)
    parser.add_argument('--inventory', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    library = build(args.source.resolve(), args.output / 'upstream')
    oracle = Oracle(library)
    summary = []
    total = 0
    for file_index, fixture in enumerate(json.loads(args.inventory.read_text())['fixtures']):
        path = (args.fixtures / fixture['path']).resolve()
        data = path.read_bytes()
        assert hashlib.sha256(data).hexdigest() == fixture['sha256']
        assert len(data) == fixture['size']
        status = oracle.load(path)
        assert status == 0, (path, status)
        cases = [(gkv, []) for gkv in graphs(data)]
        if fixture['path'] == 'qcom/qli/qcm6490/acdbdata/acdb_cal.acdb':
            gkv = [(0xa1000000, 0xa1000001), (0xa2000000, 0xa2000002),
                   (0xab000000, 1), (0xac000000, 0xac000002)]
            cases.extend((gkv, [(0xa4000000, level)]) for level in range(16))
        output = args.output / ('fixture-%02d.vectors' % file_index)
        records = []
        with output.open('wb') as stream:
            name = str(path).encode()
            stream.write(b'CAL1' + U.pack(len(name)) + name + U.pack(len(cases)))
            for index, (gkv, ckv) in enumerate(cases):
                selected = oracle.select(gkv, ckv) if gkv else {'graph_status': 19}
                stream.write(U.pack(index) + request(gkv, ckv))
                metadata = {'gkv': gkv, 'ckv': ckv}
                for name, status_name in [('graph', 'graph_status'), ('driver', 'data_status'),
                                          ('spf', 'data_status'), ('connections', 'connection_status'),
                                          ('calibration', 'calibration_status'), ('tags', 'tag_status')]:
                    result = selected.get(status_name, 19)
                    payload = selected.get(name, b'')
                    stream.write(struct.pack('<II', result, len(payload)) + payload)
                    metadata[name] = {'status': result, 'bytes': len(payload),
                                      'sha256': hashlib.sha256(payload).hexdigest()}
                records.append(metadata)
        summary.append({'fixture': fixture['path'], 'fixture_sha256': fixture['sha256'],
                        'vectors': str(output), 'vectors_sha256': hashlib.sha256(output.read_bytes()).hexdigest(),
                        'cases': records})
        total += len(cases)
        print(f"{fixture['path']}: {len(cases)} source-oracle cases", flush=True)
    oracle.close()
    (args.output / 'oracle-results.json').write_text(json.dumps(summary, indent=2) + '\n')
    print(f'{total} cases from {len(summary)} unchanged real fixtures')

if __name__ == '__main__':
    main()
