#!/usr/bin/env python3
"""Threads of a Windows minidump: name, where it is (module+offset) and the return addresses on its stack.

Usage: tools/minidump-threads.py DUMP [-m MODULE_SUBSTRING]... [-a]
  -m  only threads with an address in a module whose name contains this (several allowed)
  -a  all threads
"""
import struct
import sys

args = sys.argv[1:]
path = args[0]
wanted = [args[i + 1].lower() for i, a in enumerate(args) if a == '-m']
show_all = '-a' in args
data = open(path, 'rb').read()

sig, ver, n_streams, dir_rva = struct.unpack_from('<IIII', data, 0)
assert sig == 0x504D444D, 'not a minidump'
streams = {}
for i in range(n_streams):
    kind, size, rva = struct.unpack_from('<III', data, dir_rva + 12 * i)
    streams[kind] = (rva, size)


def string(rva):
    n, = struct.unpack_from('<I', data, rva)
    return data[rva + 4:rva + 4 + n].decode('utf-16-le', 'replace')


modules = []
if 4 in streams:
    rva, _ = streams[4]
    n, = struct.unpack_from('<I', data, rva)
    for i in range(n):
        base, size, _, _, name_rva = struct.unpack_from('<QIIII', data, rva + 4 + 108 * i)
        modules.append((base, size, string(name_rva).replace('\\', '/').split('/')[-1]))
modules.sort()


def where(addr):
    for base, size, name in modules:
        if base <= addr < base + size:
            return f'{name}+0x{addr - base:x}'
    return None


names = {}
if 24 in streams:
    rva, _ = streams[24]
    n, = struct.unpack_from('<I', data, rva)
    for i in range(n):
        tid, name_rva = struct.unpack_from('<IQ', data, rva + 4 + 12 * i)
        names[tid] = string(name_rva)

if 6 in streams:
    rva, _ = streams[6]
    tid, = struct.unpack_from('<I', data, rva)
    code, flags, _, addr = struct.unpack_from('<IIQQ', data, rva + 8)
    print(f'exception: thread 0x{tid:x} code 0x{code:x} at 0x{addr:x} ({where(addr)})')

rva, _ = streams[3]
n, = struct.unpack_from('<I', data, rva)
print(f'{n} threads, {len(modules)} modules')
for i in range(n):
    tid, _, _, _, teb, stack_start, stack_size, stack_rva, ctx_size, ctx_rva = struct.unpack_from(
        '<IIIIQQIIII', data, rva + 4 + 48 * i)
    rip, = struct.unpack_from('<Q', data, ctx_rva + 0xF8)
    rsp, = struct.unpack_from('<Q', data, ctx_rva + 0x98)
    here = where(rip) or f'0x{rip:x}'
    chain = []
    for off in range(0, stack_size - 7, 8):
        value, = struct.unpack_from('<Q', data, stack_rva + off)
        w = where(value)
        if w:
            chain.append(w)
    text = ' '.join([here] + chain).lower()
    if not show_all and wanted and not any(m in text for m in wanted):
        continue
    if not show_all and not wanted:
        continue
    print(f'\nthread 0x{tid:x} "{names.get(tid, "")}"  at {here}  (stack {stack_size} bytes)')
    shown = 0
    last = None
    for c in chain:
        if c == last:
            continue
        last = c
        print(f'    {c}')
        shown += 1
        if shown >= 14:
            break
