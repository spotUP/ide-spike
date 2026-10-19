#!/usr/bin/env python3
"""bench_rig.py -- scidemo --bench on the rig (IDE spike S4/S5). Pattern from screen_rig/updemo_rig/userland_rig:
files into VTC:, a vsh script in an XCON: window started by NewShell, a done marker polled in RAM:.

  bench_rig.py --machine 060|a1200 --variants scidemo,scidemo-lite --runs N [--functional] [--timeout S] [--force]
Results: results/<variant>-<machine>[-func].txt (skipped when present: resumable). The rig must be up."""
import argparse, pathlib, re, shutil, struct, subprocess, sys, time
VT = pathlib.Path('/Users/spot/Code/UP-Term/vtcon/tools/rig')
sys.path.insert(0, str(VT))
import paths, ami, ixpty_rig

HERE = pathlib.Path(__file__).resolve().parent
VTC = paths.RIG / 'vtc'
TERMINFO = paths.UPTERM_ROOT / 'upterm-ports/build/tools/terminfo'
SRC = paths.UPTERM_ROOT / 'vtcon/shell/sh_expand.c'
ex = ixpty_rig.run


def stage(variants):
    d = VTC / 'ide'
    d.mkdir(exist_ok=True)
    shutil.copyfile(SRC, d / 'sh_expand.c')
    for v in variants:
        # a stale stripped copy once ran the previous build on the rig and recorded its numbers
        # the binary itself is not rebuilt here: its CPU flags (FPU or soft-float) are the caller's choice
        if subprocess.run(['make', '-q', '-C', str(HERE), 'out/' + v]).returncode != 0:
            sys.exit('out/%s is older than its sources: rebuild it (make CPU=... out/%s) first' % (v, v))
        subprocess.run(['make', '-C', str(HERE), 'out/%s.stripped' % v], check=True)
        shutil.copyfile(HERE / 'out' / (v + '.stripped'), d / v)
    shutil.rmtree(d / 'terminfo', ignore_errors=True)
    shutil.copytree(TERMINFO, d / 'terminfo')
    ixpty_rig.use_ixemul()


def window(script_lines, timeout):
    """run lines in a vsh script inside an XCON: window; returns (finished, rc text, err text, out text)."""
    sh = ['export TERM=vtcon TERMINFO=/VTC/ide/terminfo', 'unset LC_ALL LC_CTYPE'] + script_lines + ['echo done >RAM:b.done']
    (VTC / 'ide/run.sh').write_text('\n'.join(sh) + '\n', encoding='latin-1')
    (VTC / 'ide/win.script').write_text('Set HISTFILE RAM:rig.history\nVTC:vsh VTC:ide/run.sh\nEndCLI\n')
    ex('Delete RAM:b.done RAM:b.txt RAM:b.err RAM:b.rc QUIET')
    ex('Run >NIL: NewShell "XCON:0/0/660/400/IDE/CLOSE" FROM VTC:ide/win.script')
    t0 = time.time()
    while time.time() - t0 < timeout:
        time.sleep(3)
        if ex('List RAM:b.done QUICK')[0] == 0:
            break
    else:
        return False, '', '', ''
    g = lambda f: ex('Type RAM:' + f)[1].strip()
    return True, g('b.rc'), g('b.err'), g('b.txt')


def bench(v, timeout):
    return window(['VTC:ide/%s --bench VTC:ide/sh_expand.c RAM:b.txt 2>RAM:b.err' % v, 'echo rc=$? >RAM:b.rc'], timeout)


def functional(v):
    """start interactively on the file, let it draw, Ctrl-Q from the keyboard, require rc=0."""
    (VTC / 'ide/run.sh').write_text('')
    sh = ['export TERM=vtcon TERMINFO=/VTC/ide/terminfo', 'VTC:ide/%s VTC:ide/sh_expand.c 2>RAM:b.err' % v,
          'echo rc=$? >RAM:b.rc', 'echo done >RAM:b.done']
    (VTC / 'ide/run.sh').write_text('\n'.join(sh) + '\n')
    (VTC / 'ide/win.script').write_text('Set HISTFILE RAM:rig.history\nVTC:vsh VTC:ide/run.sh\nEndCLI\n')
    ex('Delete RAM:b.done RAM:b.err RAM:b.rc QUIET')
    ex('Run >NIL: NewShell "XCON:0/0/660/400/IDE/CLOSE" FROM VTC:ide/win.script')
    time.sleep(15)
    alive = ex('List RAM:b.done QUICK')[0] != 0
    ami.key(0x10, 0x08)  # Q with Ctrl
    t0 = time.time()
    while time.time() - t0 < 20 and ex('List RAM:b.done QUICK')[0] != 0:
        time.sleep(2)
    done = ex('List RAM:b.done QUICK')[0] == 0
    return 'still_running_after_15s=%s quit_on_ctrl_q=%s rc=%s err=%s' % (alive, done, ex('Type RAM:b.rc')[1].strip(), ex('Type RAM:b.err')[1].strip()[:400])


def open_styled_ok(txt):
    """the open-path sentinel: open styles only the first screen, never the whole document
    (SCI_COLOURISE 0,-1 on open cost 6.7 s on the 020). None when the line is missing."""
    m = re.search(r'BENCH open_styled_lines=(\d+) of (\d+) rows=(\d+)', txt)
    return None if not m else int(m.group(1)) <= int(m.group(3)) + 2


def one_phase_ok(txt):
    """scroll-paint sentinel: with one draw phase no text run goes through DrawTextTransparent (it reads
    every cell back from the screen: 165 ms of the 490 ms scroll paint on the 020). None when missing."""
    m = re.search(r'BENCH draw_transparent_runs=(\d+)', txt)
    return None if not m else int(m.group(1)) == 0


def tparm_cache_ok(txt):
    """tparm sentinel: the capability-format cache is looked up by string address; the strcmp tree walk
    (3.3 s of 020 scroll time) happens only for a first sight. None when the line is missing."""
    m = re.search(r'BENCH tparm_lookups=(\d+) tree_walks=(\d+) fast=(\d+)', txt)
    return None if not m else int(m.group(1)) > 0 and int(m.group(2)) * 20 <= int(m.group(1))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--machine', required=True)
    ap.add_argument('--variants', default='scidemo,scidemo-lite')
    ap.add_argument('--runs', type=int, default=3)
    ap.add_argument('--functional', action='store_true')
    ap.add_argument('--timeout', type=int, default=600)
    ap.add_argument('--force', action='store_true')
    a = ap.parse_args()
    out = HERE / 'results'
    out.mkdir(exist_ok=True)
    stage(a.variants.split(','))
    bad = []
    for v in a.variants.split(','):
        f = out / ('%s-%s%s.txt' % (v, a.machine, '-func' if a.functional else ''))
        if f.exists() and not a.force:
            print('skip', f.name)
            continue
        if a.functional:
            fin, rc, err, txt = bench(v, 120)
            res = ['bench-mode finished=%s rc=%s err=%s' % (fin, rc, err[:400]), txt, functional(v)]
        else:
            res = []
            for i in range(a.runs):
                fin, rc, err, txt = bench(v, a.timeout)
                res.append('run%d finished=%s %s err=%s\n%s' % (i + 1, fin, rc, err[:300], txt if fin else '> %d s' % a.timeout))
                ok = open_styled_ok(txt) if fin else True
                if not ok:
                    # a missing line counts too: a crash after open once dropped it silently
                    res[-1] += '\n[FAIL] open styled past the first screen' if ok is False else '\n[FAIL] no open_styled_lines line'
                    bad.append(v)
                ph = one_phase_ok(txt) if fin else True
                if not ph:
                    res[-1] += '\n[FAIL] text drawn in two phases (DrawTextTransparent)' if ph is False else '\n[FAIL] no draw_transparent_runs line'
                    bad.append(v)
                tc = tparm_cache_ok(txt) if fin else True
                if not tc:
                    res[-1] += '\n[FAIL] tparm format cache walked the tree per call' if tc is False else '\n[FAIL] no tparm_lookups line (link the private ncurses: NCDIR=build/ncurses/obj/lib)'
                    bad.append(v)
                print(res[-1])
                if not fin:
                    break
        f.write_text('\n'.join(res) + '\n')
        print(f.name, 'written')
        if not a.functional and not fin:
            # the timed-out bench is still running on the Amiga and holds RAM:b.*; a later variant would
            # fail on "object is in use" and record a bogus result, so stop here: restart the rig first
            sys.exit('timeout on %s: restart the rig before the next variant' % v)
    if bad:
        sys.exit('[FAIL] open-path sentinel: %s' % ' '.join(sorted(set(bad))))


if __name__ == '__main__':
    main()
