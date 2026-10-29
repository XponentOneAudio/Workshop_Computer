#!/usr/bin/env python3
"""extract_netlist.py - flat netlist from the KiCad 8 schematic of the
MFOS Noise Toaster (github.com/XponentOneAudio/MFOS-NoiseToaster, PCB/).

No KiCad needed: parses the .kicad_sch S-expressions, places every symbol's
pins (rotation + mirror), joins wires / junction points / labels / power
symbols / hierarchical sheet pins, and prints one line per net.

    python3 extract_netlist.py path/to/MFOS-NoiseToaster/PCB > ../docs/netlist.txt
"""
import collections
import math
import os
import re
import sys


def tokens(text):
    for m in re.finditer(r'\(|\)|"(?:[^"\\]|\\.)*"|[^\s()]+', text):
        yield m.group(0)


def parse(text):
    stack = [[]]
    for t in tokens(text):
        if t == '(':
            stack.append([])
        elif t == ')':
            node = stack.pop()
            stack[-1].append(node)
        else:
            stack[-1].append(t[1:-1] if t.startswith('"') else t)
    return stack[0][0]


def children(node, name):
    return [c for c in node if isinstance(c, list) and c and c[0] == name]


def child(node, name):
    found = children(node, name)
    return found[0] if found else None


def prop(node, name):
    for p in children(node, 'property'):
        if p[1] == name:
            return p[2]
    return None


def pt(x, y):
    return (round(float(x), 2), round(float(y), 2))


def lib_pins(lib):
    """{lib_id: {unit: [(number, name, x, y)]}} in symbol coordinates (y up)."""
    out = {}
    for sym in children(lib, 'symbol'):
        units = collections.defaultdict(list)
        for sub in children(sym, 'symbol'):
            unit = int(re.match(r'.*_(\d+)_(\d+)$', sub[1]).group(1))
            for p in children(sub, 'pin'):
                at = child(p, 'at')
                units[unit].append((child(p, 'number')[1], child(p, 'name')[1], float(at[1]), float(at[2])))
        out[sym[1]] = units
    return out


def on_segment(p, a, b):
    (x, y), (x1, y1), (x2, y2) = p, a, b
    if abs((x2 - x1) * (y - y1) - (y2 - y1) * (x - x1)) > 1e-3:
        return False
    return min(x1, x2) - 1e-3 <= x <= max(x1, x2) + 1e-3 and min(y1, y2) - 1e-3 <= y <= max(y1, y2) + 1e-3


class Nets:
    def __init__(self):
        self.parent = {}

    def find(self, a):
        self.parent.setdefault(a, a)
        while self.parent[a] != a:
            self.parent[a] = self.parent[self.parent[a]]
            a = self.parent[a]
        return a

    def union(self, a, b):
        self.parent[self.find(a)] = self.find(b)


def load_sheet(nets, pins_out, path, prefix, directory):
    tree = parse(open(path).read())
    lp = lib_pins(child(tree, 'lib_symbols'))
    P = lambda pos: ('P', prefix, pos)

    points = set()
    for s in children(tree, 'symbol'):
        lib = child(s, 'lib_id')[1]
        at = child(s, 'at')
        unit = int(child(s, 'unit')[1]) if child(s, 'unit') else 1
        mirror = child(s, 'mirror')
        ref, val = prop(s, 'Reference'), prop(s, 'Value')
        x0, y0 = float(at[1]), float(at[2])
        rot = math.radians(float(at[3]) if len(at) > 3 else 0.0)
        for num, name, px, py in lp[lib].get(unit, []) + lp[lib].get(0, []):
            py = -py  # symbol y is up, sheet y is down
            if mirror:
                if mirror[1] == 'x':
                    py = -py
                else:
                    px = -px
            qx = px * math.cos(rot) + py * math.sin(rot)
            qy = -px * math.sin(rot) + py * math.cos(rot)
            pos = pt(x0 + qx, y0 + qy)
            points.add(pos)
            if ref.startswith('#PWR'):
                nets.union(P(pos), ('G', val))           # power symbols are global
            elif not ref.startswith('#'):
                pins_out.append((ref, num, name, val, P(pos)))

    wires = []
    for w in children(tree, 'wire'):
        xy = children(child(w, 'pts'), 'xy')
        a, b = pt(xy[0][1], xy[0][2]), pt(xy[1][1], xy[1][2])
        wires.append((a, b))
        points |= {a, b}
    for kind in ('label', 'global_label', 'hierarchical_label'):
        for l in children(tree, kind):
            at = child(l, 'at')
            pos = pt(at[1], at[2])
            points.add(pos)
            key = ('G', l[1]) if kind == 'global_label' else ('L', prefix, l[1])
            nets.union(P(pos), key)
    sheets = []
    for sh in children(tree, 'sheet'):
        name = prop(sh, 'Sheetname') or prop(sh, 'Sheet name')
        fname = prop(sh, 'Sheetfile') or prop(sh, 'Sheet file')
        sub = prefix + '/' + name
        for p in children(sh, 'pin'):
            at = child(p, 'at')
            pos = pt(at[1], at[2])
            points.add(pos)
            nets.union(P(pos), ('L', sub, p[1]))       # sheet pin <-> hierarchical label
        sheets.append((sub, fname))
    for a, b in wires:
        nets.union(P(a), P(b))
        for p in points:
            if on_segment(p, a, b):
                nets.union(P(p), P(a))
    for sub, fname in sheets:
        load_sheet(nets, pins_out, os.path.join(directory, fname), sub, directory)


def main():
    directory = sys.argv[1]
    nets, pins = Nets(), []
    load_sheet(nets, pins, os.path.join(directory, 'Noise Toaster PCB.kicad_sch'), '', directory)
    groups = collections.defaultdict(list)
    for ref, num, name, val, key in pins:
        label = f"{ref}.{num}" + (f"({name})" if name not in ('~', '') else '') + f"[{val}]"
        groups[nets.find(key)].append(label)
    names = collections.defaultdict(set)
    for k in list(nets.parent):
        if k[0] == 'G':
            names[nets.find(k)].add(k[1])
        elif k[0] == 'L':
            names[nets.find(k)].add(k[1].split('/')[-1] + ':' + k[2] if k[1] else k[2])
    rows = []
    for root, members in groups.items():
        rows.append((', '.join(sorted(names.get(root, []))) or '-', ' '.join(sorted(members))))
    for n, m in sorted(rows, key=lambda r: r[1]):
        print(f"{n}\n    {m}")


if __name__ == '__main__':
    main()
