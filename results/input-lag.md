# IDE spike: keystroke cost on 68020+68882 (rig 4 --a1200)

Goal: `key_ms` under 100 (stretch) from the 645 ms baseline, behaviour identical,
`styled_samples=9088 of 14366` unchanged.

Baseline (`results/scidemo-lite-a1200.txt` before this work, 020+882 FPU build):

    BENCH open_ms=9446 key_ms=645 scroll_ms=1731 ram_kb=667
    BENCH split open_paint_ms=7959 open_tty_ms=1487 key_paint_ms=612 key_tty_ms=33 ...

So 612 of 645 ms per key is Scintilla paint work, only 33 ms is terminal output.

## Method

No profiler on the Amiga, so the same sources (Scintilla 5 + Scinterm 6.0 +
LexillaLite + scidemo.cxx) were built natively with clang -O2 -g in an 80x25 pty and
sampled with macOS `sample` (1 ms, 8 s windows) while the bench key loop ran.
Native scratch build: scratchpad `native/` (Makefile, gen_native.py, prof.sh, sample_*.txt).

The native key loop had to be made representative: the bench types 50 `x` at
position 0, so the native copy deletes those 50 bytes every 50 keys
(`SCI_DELETERANGE 0,50`); without that the caret line grows to 30000 chars and
the profile is dominated by laying out and lexing that one line. With the reset
the native run reports exactly the Amiga `styled_samples=9088 of 14366`.

The lexer (`Document::EnsureStyledTo`, ~670 bytes restyled per key, constant)
serves as the clock to compare profiles: its share of samples rises as the rest
gets cheaper.

## Profile before any change (per key, native sample1, 6001 samples)

    NoutRefresh 77%
      PaintText 40%: LayoutLine of all ~24 visible lines 24% (MeasureWidths ->
        grapheme_width/mbtowc/wcwidth per byte 11%, BreakFinder 3%,
        CalculatePositions 3%, CharAt/StyleAt copies 3%)
        DrawLine 15%: DrawEOL -> FillRectangle (mvwaddch per cell) 6%,
        DrawForeground 5%, BreakFinder 3%
      StyleAreaBounded (cpp lexer, edited line to end of view) 15%
      UpdateCursor -> LocationFromPosition -> LayoutLine (caret line again) 18%
    send_key 19%: InsertCharacter -> XYScrollToMakeVisible -> LayoutLine (caret line) 19%
    doupdate 3%

Root causes:

1. Default layout cache level is `Caret` (size 1). `PaintText` retrieves every
   visible line through that one slot, so each line evicts the previous one and
   every line is fully laid out on every paint; the caret line is laid out three
   more times (scroll check, paint, cursor update). Counted natively with a
   probe: 29.6 LayoutLine calls per key, 27 of them full layouts.
2. `Style::checkMonospaced` is off by default, so `PositionCache::MeasureWidths`
   never takes the `monospaceASCII` path and every ASCII run is measured through
   `SurfaceImpl::MeasureWidths`, which calls `mbtowc` + `wcwidth` per byte.
3. `SurfaceImpl::FillRectangle` draws one `mvwaddch` per cell (cursor move,
   control-char and width checks each time); called for every line background
   and for the line remainder after the text, ~1500 cells per paint.
4. `DrawTextNoClip` measures every byte with `mbtowc` + `wcwidth` twice (clip
   loops) before `mvwaddnstr`.

## Changes

### A. scidemo.cxx (editor configuration, no library change)

    SSM(SCI_STYLESETCHECKMONOSPACED, STYLE_DEFAULT, 1);   // before SCI_STYLECLEARALL
    SSM(SCI_SETLAYOUTCACHE, SC_CACHE_PAGE, 0);

Effect (native probe counters per key): full layouts 27 -> 2.5, cache checks
25 (cheap char/style compare). MeasureWidths no longer calls into curses for
ASCII. Both are IDE configuration the real editor must set too. Backup:
`scidemo.cxx.orig`.

### B. Scinterm PlatCurses.cxx (patch `patches/scinterm-PlatCurses-fast-fill-and-ascii-width.patch`)

- `grapheme_width`: byte < 0x80 returns 1 without `mbtowc`/`wcwidth` (same
  answer: printable ASCII is width 1, control chars map -1 -> 1).
- `FillRectangle`: one `mvwhline` per row instead of `mvwaddch` per cell. Same
  cells, same attrs (both go through `_nc_render`), same clipping (`ceil(rc.right)`
  reproduces the `x < rc.right` loop bound).
- `DrawTextNoClip`: skip the right-edge clip loop when the run has no more bytes
  than cells left (a character never covers more cells than bytes).

Native effect (lexer-as-clock): A then B together make the key loop about 1.5x
faster than A alone; A alone removed the layout work (LayoutLine 42% -> 3.5%).

## Profile after A+B (native sample_fixC, 6205 samples)

    NoutRefresh 82%
      DrawForeground 31% (waddnstr 11%, Colors::Pair map lookups 2%,
        DrawTextTransparent mvwin_wch/getcchar 3%)
      FillRectangle (line background + remainder) 13%
      lexer (EnsureStyledTo) 16%
      PaintSelMargin 3.5%, LayoutLine 4%, wnoutrefresh 4.5%
    doupdate (terminal output) 15%
    send_key 2%

What remains is curses cell writing, the cpp lexer restyling to the end of the
view (Scintilla design: `StyleToPositionInView`), and ncurses output.

## Amiga measurements (rig 4, --a1200)

(filled in below as runs complete)

Baseline (020+882): `key_ms=645 key_paint_ms=612 key_tty_ms=33`.

After A+B (`results/scidemo-lite-a1200.probed.txt` has the probed run, numbers below
from the unprobed one): `key_ms=389 key_paint_ms=355 key_tty_ms=34`,
`styled_samples=9088 of 14366` unchanged.

Timers on the Amiga (gettimeofday around the hot blocks, one key = one Refresh;
each timer pair costs 212 us on the 020, 606 pairs per key, so the per-call
numbers are inflated; `probed` column raw, `real` column after subtracting it):

    block                               calls/key   probed us   real us
    StyleAreaBounded (LexCPP to view end)   1          44800     ~44600
    PaintSelMargin                           1          22029     ~21800
    EditView::PaintText                      1         342925    ~215000
      FillRectangle (mvwhline rows)        226          50078      ~2000
      DrawTextNoClip (mvwaddnstr)          202          76679     ~34000
      DrawTextTransparent (mvwin_wch..)    178          31007      ~0-5000
    wnoutrefresh                             1          45816     ~45600
    UpdateCursor                             1            616       ~400

So after A+B the curses calls themselves are ~40 ms of the 355; the rest is
Scintilla drawing all ~24 visible lines (DrawLine, BreakFinder, segment loops,
Colors::Pair map lookups, doubles on the 882), plus the lexer restyling to the
end of the view and ncurses diffing the whole window in wnoutrefresh. All of it
scales with the number of lines painted per key, which is where change C goes.

### C. Scinterm partial repaint (patch `patches/scinterm-partial-repaint.patch`)

Scinterm ignored `Window::InvalidateAll()` / `InvalidateRectangle()` and painted
every visible line on every `NoutRefresh()`. The GUI platforms paint only the
exposed rectangle; Scintilla invalidates just the edited/caret rows on a
keystroke (`InvalidateRange` -> `RedrawRect`), the restyled rows (ChangeStyle
notification), and everything on scroll (`Redraw`).

- `PlatCurses`: the two `Invalidate*` calls now record a per-window pending
  rectangle; `TakeInvalidatedRectangle()` hands it to Scinterm and clears it.
- `ScintillaCurses::NoutRefresh()`: paints the invalidated rows (whole rows, so
  no horizontal clipping differences), styles the area first so a style spill
  (new multi-line comment) is included, repaints everything when Scintilla
  abandons the paint (`PaintState::abandoned`, handled like `ScintillaWin::
  FullPaint`), when the window was resized, or when an autocompletion list /
  calltip that was drawn over the buffer has gone. Nothing invalidated: an
  empty area is painted so `SCN_UPDATEUI` still fires.
- Styling per key now goes to the line after the edited one (Scintilla's
  `PositionAfterArea`), as on GTK/Win32, instead of to the end of the view;
  the style bytes end up identical (verified natively: `styled_samples=9088 of
  14366`), the lexer just runs when the lines are painted.

Native check: builds, same `styled_samples`, key loop too fast to sample.

Amiga result after A+B+C (`results/scidemo-lite-a1200.txt`, copy
`results/scidemo-lite-a1200.fixABC.txt`):

    BENCH open_ms=9610 key_ms=74 scroll_ms=1536 ram_kb=666
    BENCH ... styled_samples=9088 of 14366 ctors_run=19
    BENCH split open_paint_ms=7981 open_tty_ms=1629 key_paint_ms=52 key_tty_ms=22 ...

Per key: 645 -> 389 (A+B) -> 74 ms (C). `ctors_run` 18 -> 19 is the new
file-scope `std::map` in PlatCurses.cxx. Open and scroll are unchanged (they
paint everything, as before); scroll is the next target if it matters
(`scroll_tty_ms=1088` is terminal output, `scroll_paint_ms=448` is paint).

## Summary

| change | key_ms (020+882) |
|---|---|
| baseline | 645 |
| A (layout cache Page + checkMonospaced, scidemo.cxx) + B (PlatCurses fill/width) | 389 |
| C (Scinterm partial repaint) | 74 |

Still hot per key (~52 ms paint): painting the 1-2 invalidated rows (lexer for
those lines, DrawLine, mvwaddnstr), `wnoutrefresh` of the whole window
(ncurses compares only touched rows, so cheaper now), `PaintSelMargin` for the
rows, `UpdateCursor`. Not measured again after C (budget); the next step
would be the chtype `mvwaddchnstr` fast path in `DrawTextNoClip` and caching
`Colors::Pair`.

## Files

Work dir `~/Code/UP-Term/ide-spike/` (not a git repo), backups next to each file:

- `scidemo.cxx` (fix A) - `scidemo.cxx.orig`
- `src/scinterm/scinterm-scinterm_6.0/PlatCurses.cxx` (B + C) - `PlatCurses.cxx.orig`
  (pristine), `PlatCurses.cxx.fixB` (after B only)
- `src/scinterm/scinterm-scinterm_6.0/PlatCurses.h` (C) - `PlatCurses.h.orig`
- `src/scinterm/scinterm-scinterm_6.0/ScintillaCurses.cxx` (C) - `ScintillaCurses.cxx.orig`
- patches: `patches/scinterm-PlatCurses-fast-fill-and-ascii-width.patch` (B, from
  pristine), `patches/scinterm-partial-repaint.patch` (C, on top of B)
- Scintilla and Lexilla sources: unchanged (probes removed).
- results: `scidemo-lite-a1200.baseline.txt`, `scidemo-lite-a1200.probed.txt`
  (A+B with timers), `scidemo-lite-a1200.fixABC.txt`.
- The probe timers (`benchprobe.h`, `g_probe`) are removed from every source.
