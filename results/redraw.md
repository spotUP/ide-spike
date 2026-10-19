# Redraw drawn line by line (scidemo-lite, A1200 020+68882)

Date: 2026-10-19. Rig 3 (A1200 mode), FPU build, ncursesw 6.6 from upterm-ports.

## Measured (probed build, 20 page scrolls, per scroll unless noted)

- `BENCH open_ms=9579 key_ms=83 scroll_ms=1627`; split `scroll_paint 436 / scroll_tty 1191` (tty = inside doupdate).
- write(): 30 calls, 4476 B, 185 ms per scroll (one write per screen line, ~150 B each).
- select() (check_pending every 5 lines, zero timeout, goes to the vtcon handler as ACTION_WAIT_CHAR which renders a frame): 220 calls over 71 doupdates, 905 ms total, ~45 ms per scroll.
- Probes inside doupdate (ms totals over 20 scrolls): `vid_puts_sp 4771 calls / 12918 ms` (238 calls, 646 ms per scroll, 2.7 ms per call), `_nc_mvcur_sp 580 / 6470 ms` (29 per scroll, 323 ms; includes the flush), `_nc_flush_sp 600 / 3985 ms` (30 per scroll, 200 ms), `_nc_scroll_optimize_sp 20 / 111 ms`, `_nc_hash_map_sp 20 / 105 ms`, `wcrtomb 0`.
- Captured stream (two scrolls, 8845 B, 1607 escapes): `\E[#m` 1090, `\E(B` 292, `\E[#;#m` 148, `\E[#;#H` 47, `\E[#X` 12. ncurses emits fg AND bg SGR on every colour-pair change and `\E(B\E[0;1m` / `\E(B\E[0m` (the `sgr` capability) on every bold toggle; every one of those goes through tparm.

## Violated invariant

One doupdate() is one frame: ncurses collects the whole screen update in its output buffer (out_limit ~2.3 KB per 25x80) and hands it to the terminal in one write at the end. Instead each cursor move inside doupdate flushes the buffer, so the terminal receives the frame as ~30 partial writes, each rendered by the handler at frame pace: the screen draws line by line.

## Root cause (level: ncurses library state machine, not Scintilla, not vtcon, not ixemul)

1. `base/lib_newterm.c:303` sets `SP_PARM->_endwin = ewInitial`. `tty/tty_update.c:821-831` (doupdate) moves the state to `ewRunning` only when coming back from `ewSuspend` (after endwin). A program that never calls endwin stays in `ewInitial` for its whole life. `tty/lib_mvcur.c:1082` (`_nc_mvcur`, the entry doupdate uses) flushes the output buffer after every cursor move while `_endwin == ewInitial` ("screen has not been initialized, e.g. low-level terminfo programs"). Result: 29 mvcur = 29 flushes = 30 writes per screen, 200 ms of write() plus 29 handler round trips per page scroll. This is upstream ncurses 6.6 behaviour; on a fast pipe it is invisible, on the Amiga handler path (ACTION_WRITE per write) it is the line-by-line drawing.
2. Attribute emission cost: 238 vid_puts per screen at 2.7 ms each = 646 ms per scroll, i.e. 54% of doupdate. Each colour-pair change runs tparm for `setaf` and `setab`, each bold toggle runs tparm for the 9-parameter `sgr`; tparm formats every number through ixemul `snprintf("%d")` (`tinfo/lib_tparm.c` save_number). That is ~620 tparm calls per screen on a 14 MHz 68020.

## Fix (root level: ncurses port patch, pkgs/ncurses/patches in upterm-ports)

1. doupdate marks the screen `ewRunning` on its first run when it finds `ewInitial` (the screen is initialised once doupdate runs). `_nc_mvcur` then stops flushing; the frame goes out as one write at the end of doupdate (`tty_update.c:1084`).
2. tparm `save_number`: format plain `%d` with a local integer-to-ASCII routine instead of snprintf.

Not changed: Scinterm/PlatCurses (the style runs are real), vtcon handler (it only renders what it receives), scidemo (no `typeahead(-1)`; the per-5-line select costs ~45 ms per scroll and is ncurses' documented behaviour; revisit if still over budget).

## Results

(see bottom, appended after the rebench)

## Results (rig 3, A1200, FPU lite build, probes still linked in both runs)

| | before | after |
|---|---|---|
| open_ms | 9579 | 9151 |
| key_ms | 83 | 81 |
| scroll_ms (per page) | 1627 | 1462 |
| scroll_tty (inside doupdate) | 1191 | 963 |
| writes per page scroll | 30 / 4476 B / 185 ms | 2 / 4476 B / 137 ms |
| _nc_flush calls (20 scrolls) | 600 / 3985 ms | 20 / 580 ms |
| _nc_mvcur (20 scrolls) | 580 / 6470 ms | 580 / 1854 ms |
| vid_puts (20 scrolls) | 4771 / 12918 ms | 4771 / 12485 ms |

The frame now leaves doupdate as one write (line-by-line drawing gone), but the page scroll is still 1462 ms. The %d fast path changed nothing measurable: the tparm cost is not snprintf. What remains per page scroll: vid_puts 238 calls x 2.6 ms = 624 ms (tparm for setaf, setab, sgr per style run; ncurses 6.4+ re-analyses the capability string in tparm_setup on every call), mvcur 29 x 3.2 ms = 93 ms (tparm for cup), vtcon handler write 137 ms for 4.4 KB (about 30 us per byte in vt_feed on the 020), check_pending selects about 50 ms, Scintilla paint 499 ms (outside ncurses). Next root-level candidates: cache the tparm analysis per capability string (or cache formatted setaf/setab strings per colour in _nc_do_color), stop emitting both fg and bg when only one changed, then the handler's per-byte parse cost. Patch: upterm-ports/pkgs/ncurses/patches/0001-doupdate-one-write-per-frame-fast-tparm-d-formatting.patch. Probes: scidemo.cxx (__wrap_write/select/_nc_flush_sp/_nc_mvcur_sp/vid_puts_sp/...), Makefile --wrap flags; pristine copy scidemo.cxx.pre-writeprobe.

## 2026-10-19 tparm interprets the format on every call (rig 3, probed FPU build)

Probes added to scidemo-lite (`-Wl,--wrap=` on tputs_sp, _nc_do_color_sp, tfind, _nc_tparm_analyze, then
_nc_tiparm, pair_content_sp); 20 page scrolls, probe overhead included:

| probe | calls | ms | per call |
|---|---|---|---|
| vid_puts_sp | 4771 | 19206 | 4.0 ms |
| _nc_do_color_sp | 4771 | 12138 | 2.5 ms |
| _nc_tiparm | 16531 | 10998 | 0.67 ms |
| tputs_sp | 13414 | 4809 | 0.36 ms |
| tfind | 16531 | 3314 | 0.20 ms |
| _nc_tparm_analyze | 0 | 0 | analysis cache works (HAVE_TSEARCH=1, tfind from libixcompat) |
| _nc_mvcur_sp | 580 | 2768 | 4.8 ms |

Result files: `scidemo-lite-a1200.probed2a.txt` (tfind/analyze probes), `scidemo-lite-a1200.probed2b.txt`
(tiparm/pair_content probes). Both: open 9.5 s, key 84-88 ms, scroll 1823-1834 ms (1462 ms with the
earlier, lighter probe set).

The earlier hypothesis (format re-analysed per call) is wrong: analyze=0. The cost is `tparam_internal`
itself: every call interprets the whole `%?%p1%{8}%<%t3%p1%d%e...` program for setaf/setab, and the 9-flag
`sgr` program, byte by byte, 0.67 ms per call on the 020, 3.5 calls per vid_puts (setaf, setab, sgr, plus
cup inside mvcur). Nothing in ncurses remembers the result for a parameter tuple it has formatted before.

Invariant: formatting a capability for a parameter tuple is a pure function of (format, params) unless the
format reads dynamic or static variables (`%g`, `%P`); so a repeated tuple costs a table lookup, not an
interpretation.

Level: ncurses `ncurses/tinfo/lib_tparm.c`, in the per-format cache entry (`TPARM_DATA`) that `tparm_setup`
already keeps per format string. Add a small direct-mapped memo (16 entries: numeric params + formatted
text) filled by `tparam_internal` on a miss and returned on a hit; formats with string parameters or `%g`/`%P`
are never memoised. This fixes setaf, setab, sgr and cup together at the one place they share. The
alternative (emit only the changed colour in `_nc_do_color`) is a byte saving on top, not the root, and
touches reverse-emulation semantics; not done.

### Result: tparm memo (upterm-ports patch 0002, commit 7a0aba5)

Same light probe set as the 1462 ms baseline (`scidemo-lite-a1200.ewrunning-probed.txt` vs
`scidemo-lite-a1200.txt`), rig 3, FPU build:

| | before | after |
|---|---|---|
| open_ms | 9151 | 8856 |
| key_ms | 81 | 76 |
| scroll_ms | 1462 | 1249 |
| scroll_tty_ms | 963 | 760 |
| vid_puts_sp (20 scrolls) | 4771 / 12485 ms | 4771 / 7716 ms |

With the heavy probe set (`scidemo-lite-a1200.probed2b.txt` vs `scidemo-lite-a1200.memo-probed2.txt`):
_nc_tiparm 16531 calls 10998 ms -> 6966 ms (0.67 -> 0.42 ms per call), docolor 12138 -> 9332 ms,
scroll 1834 -> 1624 ms.

Still above the 300 ms target. Per page scroll (light probes): vid_puts 386 ms, mvcur 95 ms, flush 50 ms,
paint 489 ms. Next biggest costs, measured, not fixed:

1. `tparm_setup` per call: `tfind` strcmp walk over the format tree, 0.20 ms x 16531 = 3.3 s per 20
   scrolls, plus strlen/memset. A pointer-keyed one-entry (or small) cache in front of `tfind` (the format
   pointers are the stable terminfo string table) would remove most of what is left of tiparm.
2. `tputs_sp`: 13414 calls, 5.0 s (0.37 ms per call): pad parsing and per-byte `outc` function-pointer
   calls into `_nc_outch`.
3. `_nc_mvcur_sp`: 580 calls, 1.9 s (3.3 ms per cursor move, 29 moves per scroll).
4. Scintilla paint: 489 ms per scroll, unchanged by all tty work.
