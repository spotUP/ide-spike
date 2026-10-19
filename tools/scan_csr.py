#!/usr/bin/env python3
"""scan_csr.py -- flag m68k functions that write callee-saved registers (d2-d7, a2-a6) without saving them.

Static scan over `m68k-amigaos-objdump -d` (MIT syntax) of .o files and .a archives.
Per function (each non-offset symbol label), code is followed by recursive descent from the entry
(conditional branches add targets; rts/rte/bra/jmp end a path), so data embedded in .text is not decoded.
written = registers that appear as a destination (last operand, exg both, dbcc counter, movem load list,
          mull/divl pair) excluding restores from sp@+.
saved   = registers pushed by moveml ...,sp@- / movel rN,sp@- / link aN anywhere on the reached paths.
Flag when written - saved is non-empty. a6 is also spelled fp in objdump output.

usage: scan_csr.py OUT.txt FILE.o|FILE.a ...   (prints one count line per input file)
"""
import re, subprocess, sys

OBJDUMP = '/Users/spot/opt/amiga16/bin/m68k-amigaos-objdump'
CSR = {'d2', 'd3', 'd4', 'd5', 'd6', 'd7', 'a2', 'a3', 'a4', 'a5', 'a6'}
SYM = re.compile(r'^([0-9a-f]+) <([^>]+)>:$')
INS = re.compile(r'^\s*([0-9a-f]+):\t[0-9a-f ]+\t(\S+)\s*(.*)$')
TGT = re.compile(r'^([0-9a-f]+) <')
NOWRITE = re.compile(r'^(cmp|tst|btst|pea|jsr|bsr|chk|trap|nop|illegal|stop|rts|rte|rtd|ftst|fcmp|fb|fdb|fmove|fs)')
END = re.compile(r'^(rts|rte|rtd|bra|jmp)')


def regs(text):
    """Registers named in an operand, expanding ranges like d2-d5/a2-a3."""
    text = text.replace('fp', 'a6')
    out = set()
    for part in text.split('/'):
        m = re.fullmatch(r'([ad])([0-7])-([ad])?([0-7])', part)
        if m:
            out |= {m.group(1) + str(i) for i in range(int(m.group(2)), int(m.group(4)) + 1)}
        elif re.fullmatch(r'[ad][0-7]', part):
            out.add(part)
    return out


def split_ops(s):
    ops, depth, cur = [], 0, ''
    for ch in s:
        if ch in '([':
            depth += 1
        elif ch in ')]':
            depth -= 1
        if ch == ',' and depth == 0:
            ops.append(cur); cur = ''
        else:
            cur += ch
    if cur:
        ops.append(cur)
    return [o.split(' <')[0].strip() for o in ops]


def effect(mn, ops):
    """(written, saved) register sets for one instruction."""
    w, s = set(), set()
    if not ops:
        return w, s
    dst = ops[-1].replace('fp', 'a6')
    if mn.startswith('movem'):
        if dst == 'sp@-':
            s |= regs(ops[0])
        elif ops[0] != 'sp@+' and not ops[0].startswith('sp@+'):
            w |= regs(dst)
        return w, s
    if mn.startswith('link'):
        s |= regs(ops[0]); return w, s
    if mn.startswith('unlk'):
        return w, s
    if mn.startswith('move') and dst == 'sp@-' and regs(ops[0]):
        s |= regs(ops[0]); return w, s
    if mn.startswith('move') and ops[0] == 'sp@+':
        return w, s  # restore
    if mn.startswith('exg'):
        return regs(ops[0]) | regs(dst), s
    if mn.startswith('db'):
        return regs(ops[0]), s
    if NOWRITE.match(mn) or (mn.startswith('b') and not mn.startswith(('bchg', 'bclr', 'bset', 'bfext', 'bfffo', 'bfins', 'bfchg', 'bfclr', 'bfset'))):
        return w, s
    if ':' in dst and re.fullmatch(r'd[0-7]:d[0-7]', dst):
        return set(dst.split(':')), s
    if re.fullmatch(r'[ad][0-7]', dst):
        w.add(dst)
    return w, s


def disasm(path):
    """yield (member, {addr: (mn, ops)}, [(addr, name)]) per object."""
    txt = subprocess.run([OBJDUMP, '-d', '-m', 'm68k:68040', path], capture_output=True, text=True).stdout
    member, code, syms, sec = None, {}, [], ''
    for line in txt.splitlines():
        if line.startswith('Disassembly of section '):
            sec = line[23:].rstrip(':')
            continue
        m = re.match(r'^(\S+):\s+file format', line)
        if m:
            if member is not None:
                yield member, code, syms
            member, code, syms = m.group(1), {}, []
            continue
        m = SYM.match(line)
        if m:
            if '+0x' not in m.group(2) and '-0x' not in m.group(2):
                syms.append(((sec, int(m.group(1), 16)), m.group(2)))
            continue
        m = INS.match(line)
        if m:
            code[(sec, int(m.group(1), 16))] = (m.group(2), split_ops(m.group(3)))
    if member is not None:
        yield member, code, syms


def scan_func(code, entry):
    addrs = sorted(code)
    nxt = {a: addrs[i + 1] for i, a in enumerate(addrs[:-1])}
    seen, work, w, s, insns = set(), [entry], set(), set(), []
    while work:
        a = work.pop()
        while a in code and a not in seen and len(seen) < 20000:
            seen.add(a)
            mn, ops = code[a]
            ww, ss = effect(mn, ops)
            if ww & CSR:
                insns.append('%x: %s %s' % (a[1], mn, ','.join(ops)))
            w |= ww; s |= ss
            if (mn.startswith('b') or mn.startswith('db') or mn.startswith('fb')) and ops:
                t = TGT.match(ops[-1] + ' <') if re.fullmatch(r'[0-9a-f]+', ops[-1]) else None
                if t and not mn.startswith('bsr'):
                    work.append((a[0], int(t.group(1), 16)))
            if END.match(mn) and not mn.startswith('bsr'):
                break
            a = nxt.get(a)
    return (w & CSR) - s, insns


def main():
    out, files = sys.argv[1], sys.argv[2:]
    lines = []
    for f in files:
        nfun = nflag = 0
        for member, code, syms in disasm(f):
            for addr, name in syms:
                if addr not in code or name.startswith('__EH_FRAME'):
                    continue
                nfun += 1
                bad, insns = scan_func(code, addr)
                if bad:
                    nflag += 1
                    lines.append('%s(%s) %s unsaved=%s first=%s' % (f.split('/')[-1], member.split('/')[-1], name,
                                 ','.join(sorted(bad)), ' | '.join(insns[:3])))
        print('%-40s functions=%d flagged=%d' % (f.split('/')[-1], nfun, nflag))
    open(out, 'w').write('\n'.join(lines) + '\n')


if __name__ == '__main__':
    main()
