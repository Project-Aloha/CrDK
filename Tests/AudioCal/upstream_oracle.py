#!/usr/bin/env python3
"""Source-built AudioReach ACDB differential oracle (no query substitutions).

Calls the pinned upstream public C API using its GNU packed structures.
The source-build path adds only a Linux unused-argument macro and syslog
selection; all lookup, calibration, file, heap and mutex logic is upstream.
Copyright (c) 2026 Project Aloha Authors. SPDX-License-Identifier: BSD-3-Clause
"""
import ctypes as C
import hashlib
import json
import subprocess
from pathlib import Path

PIN = '3094583c726710b66eb0204fc5d6a7b336d9e575'
U32 = C.c_uint32
PTR = C.c_void_p

class Packed(C.Structure):
    _pack_ = 1

class File(Packed):
    _fields_ = [('length', U32), ('name', C.c_char * 256)]
class Files(Packed):
    _fields_ = [('count', U32), ('files', File * 2)]
class KV(Packed):
    _fields_ = [('key', U32), ('value', U32)]
class Vector(Packed):
    _fields_ = [('count', U32), ('pairs', PTR)]
class Blob(Packed):
    _fields_ = [('size', U32), ('data', PTR)]
class Graph(Packed):
    _fields_ = [('count', U32), ('size', U32), ('data', PTR)]
class Subgraphs(Packed):
    _fields_ = [('count', U32), ('ids', PTR)]
class GraphRequest(Packed):
    _fields_ = [('count', U32), ('ids', PTR), ('gkv', Vector)]
class DataReply(Packed):
    _fields_ = [('driver', Graph), ('spf', Blob)]
class CalRequest(Packed):
    _fields_ = [('count', U32), ('ids', PTR), ('prior', Vector), ('new', Vector)]
class TagRequest(Packed):
    _fields_ = [('count', U32), ('ids', PTR), ('tag', U32)]


def build(source, output):
    output.mkdir(parents=True, exist_ok=True)
    actual = subprocess.check_output(['git', '-C', str(source), 'rev-parse', 'HEAD'], text=True).strip()
    if actual != PIN:
        raise RuntimeError('Upstream ACDB pin differs: ' + actual)
    sources = sorted((source / 'acdb/src').glob('*.c'))
    sources += [source / ('ar_osal/src/linux/ar_osal_' + x + '.c')
                for x in ('file_io', 'heap', 'mutex', 'log', 'mem_op', 'string')]
    includes = [source / x for x in ('acdb/inc', 'acdb/api', 'ar_osal/api')]
    files = sources + [p for d in includes for p in d.glob('*.h')]
    hashes = {str(p.resolve()): hashlib.sha256(p.read_bytes()).hexdigest() for p in files}
    library = output / 'libacdb-oracle.so'
    cmd = ['clang', '-shared', '-fPIC', '-g', '-O1', '-DAR_OSAL_USE_SYSLOG',
           '-D__unused=__attribute__((unused))', '-Wno-incompatible-pointer-types',
           '-Wno-pointer-sign', '-Wno-ignored-attributes',
           *['-I' + str(p) for p in includes], *map(str, sources),
           '-pthread', '-Wl,--no-undefined', '-o', str(library)]
    result = subprocess.run(cmd, text=True, capture_output=True)
    (output / 'build.log').write_text(result.stdout + result.stderr)
    result.check_returncode()
    if hashes != {str(p.resolve()): hashlib.sha256(p.read_bytes()).hexdigest() for p in files}:
        raise RuntimeError('Upstream inputs changed during build')
    (output / 'build.json').write_text(json.dumps({
        'pin': PIN, 'command': cmd, 'inputs': hashes,
        'library_sha256': hashlib.sha256(library.read_bytes()).hexdigest(),
        'adaptation': 'Compile-only __unused macro and existing SYSLOG branch; no source patches',
    }, indent=2) + '\n')
    return library


class Oracle:
    def __init__(self, library):
        self.lib = C.CDLL(str(library.resolve()))
        self.lib.acdb_init.argtypes = [C.POINTER(Files), PTR]
        self.lib.acdb_ioctl.argtypes = [U32, PTR, U32, PTR, U32]
        self.lib.acdb_deinit.argtypes = []
        self.loaded = False

    def load(self, path):
        if self.loaded:
            self.lib.acdb_deinit()
        encoded = str(path.resolve()).encode()
        if len(encoded) >= 256:
            raise ValueError('Upstream API path limit')
        request = Files(1)
        request.files[0].length = len(encoded)
        request.files[0].name = encoded
        status = self.lib.acdb_init(C.byref(request), None)
        self.loaded = True
        return status

    def query(self, opcode, request, reply):
        return self.lib.acdb_ioctl(0xacdb0000 | opcode, C.byref(request),
                                  C.sizeof(request), C.byref(reply), C.sizeof(reply))

    @staticmethod
    def vector(pairs):
        backing = (KV * len(pairs))(*[KV(*p) for p in pairs])
        return backing, Vector(len(pairs), C.addressof(backing) if pairs else None)

    def blob(self, opcode, request):
        reply = Blob()
        status = self.query(opcode, request, reply)
        if status:
            return status, b''
        if reply.size > 16 * 1024 * 1024:
            raise ValueError('Unbounded oracle output')
        data = C.create_string_buffer(reply.size)
        reply.data = C.addressof(data)
        status = self.query(opcode, request, reply)
        return status, bytes(data)[:reply.size] if status == 0 else b''

    def select(self, gkv, ckv=(), tag=0xc0000004):
        import struct
        backing, vector = self.vector(gkv)
        graph = Graph()
        result = {'graph_status': self.query(1, vector, graph)}
        if result['graph_status']:
            return result
        data = C.create_string_buffer(graph.size)
        graph.data = C.addressof(data)
        result['graph_status'] = self.query(1, vector, graph)
        result['graph'] = bytes(data)
        ids = []
        offset = 0
        for _ in range(graph.count):
            sgid, count = struct.unpack_from('<II', data, offset)
            ids.append(sgid)
            offset += 8 + 4 * count
        sgids = (U32 * len(ids))(*ids)
        result['ids'] = ids
        req = GraphRequest(len(ids), C.addressof(sgids), vector)
        reply = DataReply()
        result['data_status'] = self.query(2, req, reply)
        if not result['data_status']:
            driver = C.create_string_buffer(reply.driver.size)
            spf = C.create_string_buffer(reply.spf.size)
            reply.driver.data = C.addressof(driver)
            reply.spf.data = C.addressof(spf)
            result['data_status'] = self.query(2, req, reply)
            result['driver'], result['spf'] = bytes(driver), bytes(spf)
        req = Subgraphs(graph.count, graph.data)
        result['connection_status'], result['connections'] = self.blob(3, req)
        cb, cv = self.vector(ckv)
        req = CalRequest(len(ids), C.addressof(sgids), Vector(), cv)
        result['calibration_status'], result['calibration'] = self.blob(4, req)
        # Calibration sorts caller IDs in place. Restore actual graph order.
        sgids[:] = ids
        req = TagRequest(len(ids), C.addressof(sgids), tag)
        reply = Subgraphs()
        result['tag_status'] = self.query(10, req, reply)
        if not result['tag_status']:
            tagged = C.create_string_buffer(reply.count * 8)
            reply.ids = C.addressof(tagged)
            result['tag_status'] = self.query(10, req, reply)
            result['tags'] = bytes(tagged)
        return result

    def close(self):
        if self.loaded:
            self.lib.acdb_deinit()
            self.loaded = False


if __name__ == '__main__':
    import argparse
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--fixture', type=Path, required=True)
    args = parser.parse_args()
    oracle = Oracle(build(args.source.resolve(), args.output.resolve()))
    status = oracle.load(args.fixture)
    print('load:', status)
    if status:
        raise SystemExit(1)
    selected = oracle.select([(0xa1000000, 0xa1000001), (0xa2000000, 0xa2000002),
                              (0xab000000, 1), (0xac000000, 0xac000002)],
                             [(0xa4000000, 0)])
    metadata = {}
    for key, value in selected.items():
        if isinstance(value, bytes):
            (args.output / (key + '.bin')).write_bytes(value)
            metadata[key] = {'bytes': len(value), 'sha256': hashlib.sha256(value).hexdigest()}
        else:
            metadata[key] = value
    oracle.close()
    (args.output / 'profile.json').write_text(json.dumps(metadata, indent=2) + '\n')
    print(json.dumps(metadata, indent=2))
