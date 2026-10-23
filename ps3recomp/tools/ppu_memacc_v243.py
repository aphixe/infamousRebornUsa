#!/usr/bin/env python3
"""v243A ppu_memacc_v243.py -- lean guest-memory accessor twins for lifted PPU functions (v243).

usage: python ppu_memacc_v243.py <in.cpp> <out.cpp> --list=<names.txt> --hdr=<ppu_memory_v243.h> [--mode=ab|all] [--stats]
       (in.cpp may equal out.cpp: the file is read completely before it is written)

Runs AFTER ppu_regcache(_v241).py on its output (CMake PORT_PPU_MEMLEAN=AB). For every function named in the list
(one base name func_XXXXXXXX per line, '#' comments; if the TU has a v241B twin func_X__n, that copy is the one the
shipping dispatcher runs and is the one twinned) it emits
    F__v243m  = the same body with the guest accessors bound to the lean helpers of ppu_memory_v243.h,
    F__v243l  = the unchanged body,
    F         = dispatcher: V243A_PICK() ? F__v243m : F__v243l   (g_v243a_sel, spu_coherency.c; it calls
                v243a_lean_try() while the decision is still pending).
Every other line of the TU is copied unchanged. Both twins are renamed, i.e. both lose their PGO profile (fair in-run
A/B). With --mode=all the selected function is instead rewritten in place: it keeps its original symbol and PGO
profile, has no twin and pays no dispatcher branch/call. The executable must call v243a_lean_require() before the
first guest instruction in that mode. The lean helpers are only valid in the runtime's standard fast mode.
"""
import sys, re

ACC = ['vm_read8', 'vm_read16', 'vm_read32', 'vm_read64', 'vm_write8', 'vm_write16', 'vm_write32', 'vm_write64', 'ppu_stvx16']
LEAN_ON = ['#pragma push_macro("%s")' % n for n in ACC] + ['#undef %s' % n for n in ACC] + [
    '#define vm_read8(a)  ppu__ld8L(PPU_VMB, (a))', '#define vm_read16(a) ppu__ld16L(PPU_VMB, (a))',
    '#define vm_read32(a) ppu__ld32L(PPU_VMB, (a))', '#define vm_read64(a) ppu__ld64L(PPU_VMB, (a))',
    '#define vm_write8(a, v)  ppu__st8L(PPU_VMB, (a), (v))', '#define vm_write16(a, v) ppu__st16L(PPU_VMB, (a), (v))',
    '#define vm_write32(a, v) ppu__st32L(PPU_VMB, (a), (v))', '#define vm_write64(a, v) ppu__st64L(PPU_VMB, (a), (v))',
    '#define ppu_stvx16(ea, src) ppu__stvx16L(PPU_VMB, (ea), (src))']
LEAN_OFF = ['#pragma pop_macro("%s")' % n for n in ACC]

def main():
    a = sys.argv[1:]
    src, dst = a[0], a[1]
    opts = dict(x.split('=', 1) if '=' in x else (x, '1') for x in a[2:])
    mode = opts.get('--mode', 'ab').lower()
    if mode not in ('ab', 'all'):
        raise SystemExit('ppu_memacc_v243: --mode must be ab or all (got %r)' % mode)
    names = set()
    for ln in open(opts['--list'], encoding='utf-8', errors='replace'):
        ln = ln.split('#', 1)[0].strip()
        if ln: names.add(ln.split()[0])
    hdr = opts['--hdr'].replace('\\', '/')
    L = open(src, encoding='utf-8', errors='replace').read().split('\n')
    sig = re.compile(r'^void (func_[0-9A-Fa-z_]+)\(ppu_context\* ctx\) \{')
    defs = {}
    for i, l in enumerate(L):
        m = sig.match(l)
        if m: defs[m.group(1)] = i
    targets = set()
    for n in names:
        if n + '__n' in defs: targets.add(n + '__n')
        elif n in defs and not L[defs[n]].rstrip().endswith('}'): targets.add(n)
    o = []; inc_done = False; nt = 0
    i = 0
    while i < len(L):
        l = L[i]
        if not inc_done and l.startswith('#include "ppu_recomp.h"'):
            o.append(l); o.append('#include "%s"   /* v243A ppu_memacc_v243.py */' % hdr); inc_done = True; i += 1; continue
        m = sig.match(l)
        if m and m.group(1) in targets:
            j = i
            while L[j] != '}': j += 1
            name = m.group(1); body = L[i + 1:j + 1]
            o.extend(LEAN_ON)
            if mode == 'all':
                o.append('void %s(ppu_context* ctx) {   /* v245 direct lean guest accessors */' % name)
                o.extend(body)
                o.extend(LEAN_OFF)
                nt += 1; i = j + 1; continue
            o.append('void %s__v243m(ppu_context* ctx) {' % name); o.extend(body)
            o.extend(LEAN_OFF)
            o.append('void %s__v243l(ppu_context* ctx) {' % name); o.extend(body)
            o.append('void %s(ppu_context* ctx) { if (V243A_PICK()) %s__v243m(ctx); else { if (g_v243a_sel < 0) v243a_lean_try(); %s__v243l(ctx); } }   /* v243A */'
                     % (name, name, name))
            nt += 1; i = j + 1; continue
        o.append(l); i += 1
    if nt and not inc_done:
        raise SystemExit('ppu_memacc_v243: no #include "ppu_recomp.h" line in ' + src)
    # forward declarations of the renamed twins are not needed: each is defined before its dispatcher
    open(dst, 'w', encoding='utf-8', newline='\n').write('\n'.join(o))
    if '--stats' in opts:
        print('%s: v243A memlean %s %d of %d listed' %
              (src, 'direct bodies' if mode == 'all' else 'twins', nt, len(names)))

if __name__ == '__main__':
    main()
