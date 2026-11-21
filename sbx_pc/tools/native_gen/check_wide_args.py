"""Register widths the native code can lose: lists the 64-bit and float
arguments the originals read that a src/game file's C++ may not hand on.

  python check_wide_args.py FILE...      (names under src/game, no .cpp)

Two kinds of argument are easy to lose in C++ written over registers:
- **64-bit values.** The game passes some structs by value in 64-bit
  registers, two words to a register (paints, TGA headers, sizes), and a
  callee that stores the register whole (std) or does 64-bit math on it
  reads both halves. A native that takes ctx.rN.u32, or hands on a
  uint32_t, gives the callee 0 in the high word (the black paint of
  2026-11-20: CarParts::SetPaint handed on the low words only).
- **Floats.** A callee reading f1 to f13 needs ctx.fN set before the call.

Listed, from the originals' code (a linear scan; the save/restore helpers,
memcpy and the printf family's varargs, which use only the low words, are
left out):
- A: each override whose original reads an argument register's high bits.
  Its C++ has to read that register's u64.
- B: each call in the file to a function that reads an argument's high
  bits. The C++ has to pass a uint64_t (or a signed type where the
  original's value is sign-extended).
- F: each call to a function reading float arguments where no ctx.fN is
  set in the six lines before it, and each override whose original reads
  float arguments its C++ doesn't.
Each line is something to read against the C++, not always a bug: an
original may store a register whole and read back only its low word
(std_list's 82099AE0), and an override may hand f1 on untouched from its
own caller."""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import paths

gen, _overrides, _grades = paths.load_index()
gen, ov, gr = paths.load_index()
HELPER_NAMES = {}
for _a, _s in gen.items():
    for _m in re.finditer(r'// bl (0x[0-9a-f]+)\n[^\n]*\n\s*(__\w+)\(ctx, base\)', _s):
        HELPER_NAMES[_m.group(1)] = _m.group(2)
VARARGS = {'82591FE0', '8258ED70', '8210A5C0', '8258FA50', '82586A00'}
ARGS = [f'r{i}' for i in range(3, 11)]
PROP = {'mr', 'or', 'and', 'xor', 'add', 'addi', 'subf', 'neg', 'nor', 'andc', 'orc', 'mulld',
        'addc', 'adde', 'subfc', 'subfe', 'addis', 'ori', 'oris', 'xori', 'xoris', 'andi.', 'andis.',
        'or.', 'and.', 'add.', 'subf.', 'addze', 'addme', 'subfic'}
SINK_CMP = {'cmpd', 'cmpld', 'cmpdi', 'cmpldi'}
SINK_SHIFT = {'srdi', 'sradi', 'srd', 'srad', 'rotldi', 'rldicl', 'rldicr', 'rldic', 'rldimi',
              'divd', 'divdu', 'mulhd', 'mulhdu', 'sld', 'cntlzd', 'rotld', 'rldcl', 'insrdi',
              'extrdi', 'sldi', 'mulld', 'std', 'stdu', 'stdx', 'stdux', 'clrldi'}
HELPERS = re.compile(r'^0x8258c[0-9a-f]{3}$')  # save/restore gpr helpers range (approx)

def instrs(a):
    s = gen.get(a)
    if s is None: return []
    out = []
    for l in s.splitlines():
        l = l.strip()
        if l.startswith('// '):
            out.append(l[3:])
    return out

_cache={}
def consumes(a, depth=0, why=None):
    """Set of arg regs whose high bits a reads. Conservative: linear scan."""
    if why is None and a in _cache: return _cache[a]
    if depth > 4 or a in VARARGS: return frozenset()
    taint = {r: {r} for r in ARGS}  # reg -> set of origin args
    hit = set()
    for ins in instrs(a):
        parts = ins.replace(',', ' ').split()
        if not parts: continue
        op, ops = parts[0], parts[1:]
        regs = [o for o in ops if re.fullmatch(r'r\d+', o)]
        def origins(rs):
            o = set()
            for r in rs: o |= taint.get(r, set())
            return o
        if op in ('bl',):
            t = ops[0] if ops else ''
            tgt = t[2:].upper() if t.startswith('0x') else ''
            if HELPER_NAMES.get('0x' + tgt.lower(), '').startswith(('__save', '__rest')):
                continue  # the register save/restore helpers
            if tgt:
                if tgt in gen:
                    c = consumes(tgt, depth + 1)
                    if why is not None and any(taint.get(r) for r in c): why.append((a, ins, {r: taint.get(r) for r in c if taint.get(r)}))
                    for r in c:
                        hit |= taint.get(r, set())
            for k in [0] + list(range(3, 13)): taint.pop(f'r{k}', None)
            continue
        if op in ('b', 'bctr', 'blr') or op.startswith('b') and op not in ('bl',):
            if op == 'b' and ops and ops[0].startswith('0x') and ops[0][2:].upper() in gen and ops[0][2:].upper() != a:
                tgt = ops[0][2:].upper()
                # tail call if target is a function entry
                for r in consumes(tgt, depth + 1):
                    hit |= taint.get(r, set())
            continue
        if op in ('std', 'stdu') and re.fullmatch(r'r(1[4-9]|2\d|3[01])', ops[0]) and ops[1].endswith('(r1)'):
            continue  # a non-volatile register's save
        if op in ('std', 'stdu'):
            if why is not None and taint.get(ops[0]): why.append((a, ins, taint.get(ops[0])))
            hit |= taint.get(ops[0], set()); continue
        if op in ('stdx', 'stdux'):
            hit |= taint.get(ops[0], set()); continue
        if op in SINK_CMP:
            if why is not None and origins(regs): why.append((a, ins, origins(regs)))
            hit |= origins(regs); continue
        base_op = op.rstrip('.')
        nums = [int(x) for x in ops if re.fullmatch(r'-?\d+', x)]
        def reads_high(sh, lo, hi):
            return any((i + sh) % 64 < 32 for i in range(lo, hi + 1))
        if base_op in ('clrldi', 'rldicl', 'rldicr', 'rldic', 'rldimi', 'rotldi', 'extsw', 'sld'):
            dst = regs[0] if regs else None
            src = regs[1] if len(regs) > 1 else None
            if base_op == 'extsw':
                consumes_hi = False
            elif base_op == 'clrldi':
                consumes_hi = nums[0] < 32
            elif base_op == 'rldicl':
                consumes_hi = reads_high(nums[0], nums[1], 63)
            elif base_op == 'rldicr':
                consumes_hi = reads_high(nums[0], 0, nums[1])
            elif base_op == 'rldic':
                consumes_hi = reads_high(nums[0], nums[1], 63 - nums[0])
            elif base_op == 'rldimi':
                consumes_hi = reads_high(nums[0], nums[1], 63 - nums[0])
            elif base_op == 'rotldi':
                consumes_hi = True
            else:  # sld: the high bits move up and out unless the shift is 0
                o = taint.get(src, set())
                if dst:
                    if o: taint[dst] = set(o)
                    else: taint.pop(dst, None)
                continue
            if consumes_hi and src:
                if why is not None and taint.get(src): why.append((a, ins, taint.get(src)))
                hit |= taint.get(src, set())
            if dst:
                if base_op == 'rldimi':
                    pass  # keeps its own bits outside the mask
                else:
                    taint.pop(dst, None)
            continue
        if op.rstrip('.') in SINK_SHIFT:
            dst = regs[0] if regs else None
            srcs = regs[1:]
            if why is not None and origins(srcs): why.append((a, ins, origins(srcs)))
            hit |= origins(srcs)
            if dst: taint.pop(dst, None)
            continue
        if op.startswith(('st', 'cmp', 'tw', 'td', 'mt', 'dcb')):
            continue
        # anything else writing a destination
        if regs:
            dst = regs[0]
            if op.rstrip('.') in PROP or op in PROP:
                o = origins(regs[1:])
                if o: taint[dst] = o
                else: taint.pop(dst, None)
            else:
                taint.pop(dst, None)
    r = frozenset(hit)
    if why is None: _cache[a] = r
    return r



# --- Float arguments ---

FARGS = [f'f{i}' for i in range(1, 14)]
_c = {}
def reads_f(a, depth=0):
    if a in _c: return _c[a]
    if depth > 4: return frozenset()
    live = set(FARGS); hit = set()
    for l in instrs(a):
        p = l.replace(',', ' ').split()
        if not p: continue
        op, ops = p[0], p[1:]
        if op == 'bl':
            t = ops[0][2:].upper()
            if HELPER_NAMES.get('0x' + t.lower(), '').startswith(('__save', '__rest')): continue
            if t in gen:
                hit |= set(reads_f(t, depth + 1)) & live
            live -= {f'f{i}' for i in range(0, 14)}
            continue
        if op == 'b' and ops and ops[0][2:].upper() in gen and ops[0][2:].upper() != a:
            hit |= set(reads_f(ops[0][2:].upper(), depth + 1)) & live
            continue
        fr = [o for o in ops if re.fullmatch(r'f\d+', o)]
        if not fr: continue
        if op.startswith(('stf', 'fcmp', 'mtfsf')):
            srcs = fr
            dst = None
        elif op.startswith(('lf',)):
            srcs = []; dst = fr[0]
        else:
            dst = fr[0]; srcs = fr[1:]
        for s in srcs:
            if s in live: hit.add(s)
        if dst: live.discard(dst)
    r = frozenset(hit); _c[a] = r; return r


def main():
    VARARGS.add('8258CB90')  # memcpy: keeps r3 (the destination) to return it
    for name in sys.argv[1:]:
        src = open(os.path.join(paths.SRC, 'game', name + '.cpp')).read()
        lines = src.split('\n')
        ovs = sorted(set(re.findall(r'[A-Z_]+\(sub_([0-9A-F]{8})\)', src)) & set(gen))
        for a in ovs:
            c = consumes(a)
            if c:
                print(name, 'A', a, 'reads the high words of', ' '.join(sorted(c)))
        seen = set()
        for i, l in enumerate(lines):
            for m in re.finditer(r'Call\w*\(\s*(?:g,\s*)?sub_([0-9A-F]{8})', l):
                a = m.group(1)
                if a not in gen:
                    continue
                c = consumes(a)
                if c and a not in seen:
                    seen.add(a)
                    print(f'{name}:{i + 1} B calls {a}, which reads the high words of', ' '.join(sorted(c)))
                fr = reads_f(a)
                near = '\n'.join(lines[max(0, i - 6):i + 1])
                miss = [x for x in sorted(fr) if f'ctx.{x}.' not in near]
                if miss:
                    print(f'{name}:{i + 1} F calls {a} without setting', ' '.join(miss))
        for m in re.finditer(r'extern "C" REX_FUNC\(sub_([0-9A-F]{8})\)\s*\{(.*?)\n\}', src, re.S):
            a, body = m.group(1), m.group(2)
            miss = [x for x in sorted(reads_f(a)) if f'ctx.{x}.' not in body]
            if miss:
                print(name, 'F', a, "doesn't read", ' '.join(miss))


if __name__ == '__main__':
    main()
