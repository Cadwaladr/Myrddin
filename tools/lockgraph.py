#!/usr/bin/env python3
"""Static lock graph of a C++ file: which mutex is taken while which other is held.

Usage: tools/lockgraph.py FILE [-v]
Functions are the blocks that start at column 0 and end with a '}' at column 0.
"""
import re
import sys
from collections import defaultdict

path = sys.argv[1]
verbose = '-v' in sys.argv
src = open(path, encoding='utf-8', errors='replace').read()


def strip(text):
    """Comments and string/char literals replaced by spaces, newlines kept."""
    out = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        two = text[i:i + 2]
        if two == '//':
            j = text.find('\n', i)
            j = n if j < 0 else j
            out.append(' ' * (j - i))
            i = j
        elif two == '/*':
            j = text.find('*/', i + 2)
            j = n if j < 0 else j + 2
            out.append(''.join(ch if ch == '\n' else ' ' for ch in text[i:j]))
            i = j
        elif c == "'" and i and (text[i - 1].isalnum()):  # 50'000'000: a digit separator
            out.append(' ')
            i += 1
        elif c == '"' or c == "'":
            j = i + 1
            while j < n and text[j] != c:
                j += 2 if text[j] == '\\' else 1
            out.append(c + ' ' * (j - i - 1) + c)
            i = j + 1
        else:
            out.append(c)
            i += 1
    return ''.join(out)


clean = strip(src)
lines = clean.split('\n')


def canon(expr):
    expr = expr.strip()
    last = re.split(r'\.|->', expr)[-1]
    if expr.startswith('g_') and '.' not in expr and '->' not in expr:
        return expr
    if last == 'mutex':
        if re.search(r'\b(in|inputs)\b', expr):
            return 'Inputs.mutex'
        if re.search(r'\b(l|g_limiter)\b', expr):
            return 'Limiter.mutex'
        return 'UNKNOWN(' + expr + ')'
    if last == 'gate_mutex':
        return 'Limiter.gate_mutex'
    if last == 'mark_mutex':
        return 'Device.mark_mutex'
    if last == 'fg_mutex':
        return 'Device.fg_mutex'
    return 'UNKNOWN(' + expr + ')'


# functions: header at column 0 ... line that is '}' at column 0
functions = {}  # name -> (first line index, last line index)
i = 0
head = re.compile(r'^[A-Za-z_][^;]*?\b([A-Za-z_]\w*)\s*\(')
while i < len(lines):
    line = lines[i]
    m = head.match(line)
    if m and not re.match(r'^(struct|class|namespace|enum|union|typedef|using|extern|#|template)\b', line):
        # find the '{' that opens the body: first line from here that ends with '{' before any ';' at depth 0
        j = i
        opened = False
        while j < len(lines) and j < i + 12:
            if lines[j].rstrip().endswith('{'):
                opened = True
                break
            if lines[j].rstrip().endswith(';'):
                break
            j += 1
        if opened:
            k = j + 1
            while k < len(lines) and not lines[k].startswith('}'):
                k += 1
            functions[m.group(1)] = (i, k)
            i = k + 1
            continue
    i += 1

lock_decl = re.compile(r'std::(lock_guard|unique_lock|scoped_lock)(?:<[^>]*>)?\s+(\w+)\s*\{\s*([^}]+?)\s*\}')
call = re.compile(r'\b([A-Za-z_]\w*)\s*\(')
blocking = re.compile(r'\b(?:d|device|input|other|a|b|dev)\s*(?:\.|->)\s*(WaitForFences|AcquireNextImageKHR|QueuePresentKHR|QueueSubmit|QueueWaitIdle|DeviceWaitIdle|GetFenceStatus)\s*\(|'
                      r'\b(sleep_for|sleep_until)\s*\(|\.\s*(wait|wait_for|wait_until)\s*\(|\b(ffx\w+|Ffx\w+)\s*\(')

direct = defaultdict(set)      # function -> mutexes it takes itself
calls = defaultdict(set)       # function -> functions it calls
scopes = []                    # (function, mutex, line, [inner lock (mutex, line)], [calls (name, line)], [blocking (text, line)])

for name, (a, b) in functions.items():
    depth = 0
    open_scopes = []  # (depth at declaration, record)
    for ln in range(a, min(b + 1, len(lines))):
        text = lines[ln]
        # process character by character for braces, but find tokens per line first
        events = []
        for m in lock_decl.finditer(text):
            events.append((m.start(), 'lock', canon(m.group(3))))
        for m in call.finditer(text):
            if m.group(1) in functions and m.group(1) != name:
                events.append((m.start(), 'call', m.group(1)))
        for m in blocking.finditer(text):
            events.append((m.start(), 'block', next(g for g in m.groups() if g)))
        for pos, ch in enumerate(text):
            if ch in '{}':
                events.append((pos, ch, None))
        events.sort(key=lambda e: e[0])
        skip_brace_until = -1
        for pos, kind, what in events:
            if kind == 'lock':
                direct[name].add(what)
                for _, rec in open_scopes:
                    rec[3].append((what, ln + 1))
                rec = (name, what, ln + 1, [], [], [])
                open_scopes.append((depth, rec))
                scopes.append(rec)
                # the braces of the declaration itself are not scopes
                skip_brace_until = text.find('}', pos)
            elif kind == 'call':
                calls[name].add(what)
                for _, rec in open_scopes:
                    rec[4].append((what, ln + 1))
            elif kind == 'block':
                for _, rec in open_scopes:
                    rec[5].append((what, ln + 1))
            elif pos <= skip_brace_until:
                continue
            elif kind == '{':
                depth += 1
            else:
                depth -= 1
                open_scopes = [(dd, rec) for dd, rec in open_scopes if dd <= depth]

# transitive closure of the mutexes a function may take
takes = {f: set(direct[f]) for f in functions}
changed = True
while changed:
    changed = False
    for f in functions:
        for g in calls[f]:
            new = takes[g] - takes[f]
            if new:
                takes[f] |= new
                changed = True

edges = defaultdict(list)  # (held, taken) -> [description]
for fn, held, line, inner, called, blocks in scopes:
    for taken, at in inner:
        edges[(held, taken)].append(f'{fn}:{line} -> :{at}')
    for g, at in called:
        for taken in takes[g]:
            edges[(held, taken)].append(f'{fn}:{line} -> {g}() at :{at}')

print(f'{len(functions)} functions, {len(scopes)} lock scopes')
print('\n== edges (held -> taken)')
for (held, taken), where in sorted(edges.items()):
    flag = '  <-- same mutex' if held == taken else ''
    print(f'  {held:22s} -> {taken:22s} {len(where):3d}x  e.g. {where[0]}{flag}')
    if verbose:
        for w in where[1:6]:
            print(f'      {w}')

# cycles
graph = defaultdict(set)
for (held, taken) in edges:
    graph[held].add(taken)
print('\n== cycles')
found = set()


def walk(start, node, trail):
    for nxt in graph[node]:
        if nxt == start:
            cyc = trail + [nxt]
            key = frozenset(cyc)
            if key not in found:
                found.add(key)
                print('  ' + ' -> '.join([start] + cyc))
        elif nxt not in trail and len(trail) < 6:
            walk(start, nxt, trail + [nxt])


for node in sorted(graph):
    walk(node, node, [])
if not found:
    print('  none')

print('\n== calls that can block, made while a mutex is held')
seen = set()
for fn, held, line, inner, called, blocks in scopes:
    for what, at in blocks:
        key = (fn, held, what)
        if key not in seen:
            seen.add(key)
            print(f'  {held:22s} held in {fn}:{line}: {what} at :{at}')
print('\n== functions taking locks that the regex could not classify')
for fn, held, line, *_ in scopes:
    if held.startswith('UNKNOWN'):
        print(f'  {fn}:{line} {held}')
