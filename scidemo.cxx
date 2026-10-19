// Scinterm spike demo: open argv[1], cpp lexer, key loop. ctrl-q quits.
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <curses.h>
#include "Scintilla.h"
#include "SciLexer.h"
#include "ILexer.h"
#include "Lexilla.h"
#include "ScintillaCurses.h"

#define SSM(m, w, l) scintilla_send_message(sci, m, w, l)
typedef void SciView;
extern "C" int ixrt_ctor_count; // compat/ixcxxrt.c sentinel
#ifdef IXRT_DIAG
extern "C" void ixrt_mark(const char *, long);
#define MARK(t) ixrt_mark(t, __LINE__)
#else
#define MARK(t) ((void)0)
#endif
// ixemul crt0 (__stkext_startup) gives main a stack of at least __stack bytes. Without it the
// program runs on the caller's stack: ncurses initscr (terminfo read buffers on the stack) ran
// ~100 KB deep and overwrote the tail of the code hunk (libstdc++ codecvt code/vtables) when
// that hunk sat right below the stack; scidemo-lite then crashed in a later virtual call.
extern "C" { unsigned long __stack = 262144; }
extern "C" long g_sc_transparent_runs;
extern "C" long _nc_tparm_lookups, _nc_tparm_walks; extern "C" char *_nc_tiparm(int, const char *, ...);
extern "C" long _nc_vidmemo_calls, _nc_vidmemo_hits, _nc_vidmemo_misses, _nc_vidmemo_bad, _nc_vidmemo_flush, _nc_vidmemo_long; extern "C" int _nc_vidmemo_mode; // private ncurses (patches/ncurses-*.patch)
static void scnotification(SciView *, int, SCNotification *, void *) {}

// bench helpers: AvailMem(MEMF_ANY) via exec LVO -216 (no NDK headers, they clash with ixemul's)
static unsigned long availkb() {
	register void *a6 __asm__("a6") = *(void **)4;
	register unsigned long d1 __asm__("d1") = 0;
	register unsigned long d0 __asm__("d0");
	__asm__ volatile("jsr -216(%%a6)" : "=r"(d0), "+r"(d1), "+r"(a6) : : "a0", "a1", "cc", "memory");
	return d0 / 1024;
}
static long nowms() {
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return tv.tv_sec * 1000L + tv.tv_usec / 1000;
}

// write() probe (link: -Wl,--wrap=write): calls, bytes and time inside write() per bench phase,
// plus a copy of the bytes of one doupdate per phase (VTC:ide/cap_<phase>.bin) for offline analysis.
#include <unistd.h>
#include <fcntl.h>
extern "C" ssize_t __real_write(int, const void *, size_t);
static long wr_calls, wr_bytes, wr_ms; static long wr_each[8], wr_each_n[8];
static int wr_on;
static char *cap_buf; static long cap_len; static const long cap_max = 256 * 1024;
extern "C" ssize_t __wrap_write(int fd, const void *b, size_t n) {
	if (fd != 1 || !wr_on) return __real_write(fd, b, n);
	long t = nowms();
	ssize_t r = __real_write(fd, b, n);
	wr_ms += nowms() - t; if (wr_calls < 8) { wr_each[wr_calls] = nowms() - t; wr_each_n[wr_calls] = (long)n; } wr_calls++; wr_bytes += (long)n;
	if (cap_buf && cap_len + (long)n <= cap_max) { memcpy(cap_buf + cap_len, b, n); cap_len += (long)n; }
	return r;
}
static void cap_dump(const char *path) {
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd >= 0) { __real_write(fd, cap_buf, (size_t)cap_len); close(fd); }
	cap_len = 0;
}
#include <sys/select.h>
extern "C" int __real_select(int, fd_set *, fd_set *, fd_set *, struct timeval *);
static long sel_calls, sel_ms, sel_nz, sel_idx, sel_ms_first;
extern "C" int __wrap_select(int n, fd_set *r, fd_set *w, fd_set *x, struct timeval *t) {
	if (!wr_on) return __real_select(n, r, w, x, t);
	long t0 = nowms();
	int rc = __real_select(n, r, w, x, t);
	long d = nowms() - t0; sel_ms += d; sel_calls++; sel_nz += rc != 0;
	if (sel_idx++ == 0) sel_ms_first += d;
	return rc;
}
// Per-function timing probes inside ncurses doupdate (counts + ms, probe overhead included).
#define PROBE(NAME, RET, ARGS, CALL) \
	extern "C" RET __real_##NAME ARGS; static long p_##NAME##_n, p_##NAME##_ms; \
	extern "C" RET __wrap_##NAME ARGS { if (!wr_on) return __real_##NAME CALL; long t0 = nowms(); RET r = __real_##NAME CALL; p_##NAME##_ms += nowms() - t0; p_##NAME##_n++; return r; }
#define PROBEV(NAME, ARGS, CALL) \
	extern "C" void __real_##NAME ARGS; static long p_##NAME##_n, p_##NAME##_ms; \
	extern "C" void __wrap_##NAME ARGS { if (!wr_on) { __real_##NAME CALL; return; } long t0 = nowms(); __real_##NAME CALL; p_##NAME##_ms += nowms() - t0; p_##NAME##_n++; }
PROBE(_nc_flush_sp, int, (void *sp), (sp))
PROBE(_nc_mvcur_sp, int, (void *sp, int a, int b, int c, int d), (sp, a, b, c, d))
PROBE(vid_puts_sp, int, (void *sp, unsigned long at, short pair, void *op, int (*oc)(void *, int)), (sp, at, pair, op, oc))
PROBEV(_nc_scroll_optimize_sp, (void *sp), (sp))
PROBEV(_nc_hash_map_sp, (void *sp), (sp))
PROBE(wcrtomb, size_t, (char *s, wchar_t wc, mbstate_t *ps), (s, wc, ps))
#define PROBE_PRINT(o) fprintf(o, "BENCH probes flush=%ld/%ldms mvcur=%ld/%ldms vidputs=%ld/%ldms scrollopt=%ld/%ldms hashmap=%ld/%ldms wcrtomb=%ld/%ldms\n", \
	p__nc_flush_sp_n, p__nc_flush_sp_ms, p__nc_mvcur_sp_n, p__nc_mvcur_sp_ms, p_vid_puts_sp_n, p_vid_puts_sp_ms, \
	p__nc_scroll_optimize_sp_n, p__nc_scroll_optimize_sp_ms, p__nc_hash_map_sp_n, p__nc_hash_map_sp_ms, p_wcrtomb_n, p_wcrtomb_ms)
#define PROBE_RESET() (p__nc_flush_sp_n = p__nc_flush_sp_ms = p__nc_mvcur_sp_n = p__nc_mvcur_sp_ms = p_vid_puts_sp_n = p_vid_puts_sp_ms = \
	p__nc_scroll_optimize_sp_n = p__nc_scroll_optimize_sp_ms = p__nc_hash_map_sp_n = p__nc_hash_map_sp_ms = p_wcrtomb_n = p_wcrtomb_ms = 0)
// PC sampler (flag 'p'): a second exec task (priority 5) wakes every 2 ms on timer.device and reads the PC the
// scheduler saved for the benchmark task (tc_SPReg + 15 registers + SR); samples count only while wr_on
// (inside doupdate). Dumped as hex with the address of pc_task so the host can map it to symbols.
template <int LVO> static inline unsigned long xc(unsigned long d0v, unsigned long d1v, void *a0v, void *a1v, void *a2v = 0, void *a3v = 0) {
	register void *a6 __asm__("a6") = *(void **)4;
	register unsigned long d0 __asm__("d0") = d0v, d1 __asm__("d1") = d1v;
	register void *a0 __asm__("a0") = a0v, *a1 __asm__("a1") = a1v, *a2 __asm__("a2") = a2v, *a3 __asm__("a3") = a3v;
	__asm__ volatile("jsr %c[o](%%a6)" : "+r"(d0), "+r"(d1), "+r"(a0), "+r"(a1), "+r"(a2), "+r"(a3), "+r"(a6) : [o] "i"(LVO) : "cc", "memory");
	return d0;
}
static unsigned long pc_ring[16384]; static unsigned char pc_state[16384]; static char pc_who[16384][12]; static volatile int pc_on; static unsigned long pc_stk[8][96]; static long pc_n;
static volatile int pc_run, pc_done; static char *pc_target;
static void pc_task() {
	void *port = (void *)xc<-666>(0, 0, 0, 0);
	char *io = (char *)xc<-654>(40, 0, port, 0);
	static const char tname[] = "timer.device";
	xc<-444>(0, 0, (void *)tname, io);
	while (pc_run) {
		*(unsigned short *)(io + 28) = 9; *(long *)(io + 32) = 0; *(long *)(io + 36) = 2000;
		xc<-456>(0, 0, 0, io);
		if (pc_on && pc_n < 16384) {
			// who was interrupted: the sampler runs above every task, so the head of exec's TaskReady list (ExecBase+406)
			// is the task it preempted; an empty list means the CPU was idle. Name copied now (12 chars).
			{ char *sb = *(char **)4, *hd = *(char **)(sb + 406); const char *nm = "";
			  if (*(void **)hd) { nm = *(const char **)(hd + 10); if (!nm) nm = "?"; } else nm = "(idle)";
			  int k = 0; for (; k < 11 && nm[k]; k++) pc_who[pc_n][k] = nm[k]; pc_who[pc_n][k] = 0; }
			unsigned long sp = *(unsigned long *)(pc_target + 54);
			if (pc_n >= 100 && pc_n < 108) for (int i = 0; i < 96; i++) pc_stk[pc_n - 100][i] = ((unsigned long *)sp)[i];
			pc_ring[pc_n] = *(unsigned long *)(sp + 172); pc_state[pc_n++] = (unsigned char)(pc_target[15] | (wr_on ? 0x80 : 0));
		}
	}
	pc_done = 1;
}
static void pc_start() {
	char *t = (char *)xc<-198>(92 + 4096, 0x10001, 0, 0);
	t[8] = 1; t[9] = 30; *(char **)(t + 10) = (char *)"pcsampler";
	*(char **)(t + 58) = t + 92; *(char **)(t + 62) = t + 92 + 4096; *(char **)(t + 54) = t + 92 + 4096;
	pc_target = (char *)xc<-294>(0, 0, 0, 0); pc_run = 1;
	xc<-282>(0, 0, 0, t, (void *)&pc_task, 0);
}
static void pc_dump(const char *path) {
	pc_run = 0; for (long i = 0; i < 4000000 && !pc_done; i++) { } 
	FILE *f = fopen(path, "w"); if (!f) return;
	fprintf(f, "base %lx\n", (unsigned long)(void *)&__wrap_write);
	for (int k = 0; k < 8; k++) { fprintf(f, "stk"); for (int i = 0; i < 96; i++) fprintf(f, " %lx", pc_stk[k][i]); fprintf(f, "\n"); }
	for (long i = 0; i < pc_n; i++) fprintf(f, "%lx %d %s\n", pc_ring[i], pc_state[i], pc_who[i]);
	fclose(f);
}
struct wr_stat { long calls, bytes, ms; };
static wr_stat wr_take() { wr_stat w = { wr_calls, wr_bytes, wr_ms }; wr_calls = wr_bytes = wr_ms = 0; return w; }
#undef lines
#include <term.h>
#undef lines
#undef columns
// Per-call cost of the pieces of one colour change on this CPU (N calls each, 1 ms clock).
static long micro_sink;
static int micro_out(int ch) { micro_sink += ch; return ch; }
static void micro_print(FILE *o) {
	const int N = 1000;
	init_pair(60, COLOR_RED, COLOR_BLUE); init_pair(61, COLOR_GREEN, COLOR_BLACK);
	long t0 = nowms();
	for (int i = 0; i < N; i++) micro_sink += (long)tiparm(set_a_foreground, i & 7);
	long t_tiparm = nowms() - t0;
	t0 = nowms();
	for (int i = 0; i < N; i++) micro_sink += (long)_nc_tiparm(1, set_a_foreground, i & 7);
	long t_fast = nowms() - t0;
	t0 = nowms();
	for (int i = 0; i < N; i++) micro_sink += (long)_nc_tiparm(2, cursor_address, (i * 7) % 24, (i * 13) % 80);
	long t_cup = nowms() - t0;
	char *s = tiparm(set_a_foreground, 3);
	t0 = nowms();
	for (int i = 0; i < N; i++) tputs(s, 1, micro_out);
	long t_tputs = nowms() - t0;
	t0 = nowms();
	for (int i = 0; i < N; i++) vid_puts(A_NORMAL, 60 + (i & 1), NULL, micro_out);
	long t_vid = nowms() - t0;
	t0 = nowms();
	for (int i = 0; i < N; i++) vid_puts(A_BOLD * (i & 1), 60, NULL, micro_out);
	long t_vidattr = nowms() - t0;
	fprintf(o, "BENCH micro_us_per_call tiparm=%ld tiparm_internal_memo=%ld cup_spread=%ld tputs=%ld vid_puts_colour=%ld vid_puts_bold=%ld (s=%ld)\n", t_tiparm, t_fast, t_cup, t_tputs, t_vid, t_vidattr, micro_sink & 1);
}
static long clockres() { // smallest visible gettimeofday step in ms (spin up to 3 s)
	long t0 = nowms(), t;
	long end = t0 + 3000;
	while ((t = nowms()) == t0 && t < end) {}
	return t - t0;
}

int main(int argc, char **argv) {
	MARK("main");
	unsigned long avail_start = availkb();
	MARK("availkb");
	bool bench = argc > 2 && !strcmp(argv[1], "--bench");
	const char *path = bench ? argv[2] : (argc > 1 ? argv[1] : NULL);
	if (bench && argc > 4) { _nc_vidmemo_mode = !strcmp(argv[4], "off") ? 0 : !strcmp(argv[4], "verify") ? 2 : 1; }
	const char *flags = bench && argc > 5 ? argv[5] : NULL;
	setlocale(LC_CTYPE, "");
	MARK("setlocale");
	initscr(), raw(), cbreak(), noecho(), start_color();
	MARK("initscr");
	SciView *sci = scintilla_new(scnotification, NULL);
	MARK("scintilla_new");
	SSM(SCI_STYLESETFORE, STYLE_DEFAULT, 0xFFFFFF);
	SSM(SCI_STYLESETBACK, STYLE_DEFAULT, 0);
	/* Monospace fast path in PositionCache::MeasureWidths (fix A). */
	SSM(SCI_STYLESETCHECKMONOSPACED, STYLE_DEFAULT, 1);
	SSM(SCI_STYLECLEARALL, 0, 0);
	/* Keep every visible line laid out across paints (fix A). */
	SSM(SCI_SETLAYOUTCACHE, SC_CACHE_PAGE, 0);
	MARK("before CreateLexer");
	SSM(SCI_SETILEXER, 0, (sptr_t)CreateLexer("cpp"));
	MARK("CreateLexer");
	SSM(SCI_SETKEYWORDS, 0, (sptr_t) "int char void if else for while return static const struct");
	SSM(SCI_STYLESETFORE, SCE_C_COMMENT, 0x00FF00);
	SSM(SCI_STYLESETFORE, SCE_C_COMMENTLINE, 0x00FF00);
	SSM(SCI_STYLESETFORE, SCE_C_NUMBER, 0xFFFF00);
	SSM(SCI_STYLESETFORE, SCE_C_WORD, 0xFF0000);
	SSM(SCI_STYLESETFORE, SCE_C_STRING, 0xFF00FF);
	SSM(SCI_STYLESETBOLD, SCE_C_OPERATOR, 1);
	unsigned long avail_before = availkb(), avail_min = avail_before;
	long t0 = nowms(), t_read = 0, t_add = 0, t_colour = 0, t_setup = 0;
	if (path) {
		FILE *f = fopen(path, "rb");
		if (f) {
			fseek(f, 0, SEEK_END);
			long n = ftell(f);
			fseek(f, 0, SEEK_SET);
			char *buf = (char *)malloc(n + 1);
			size_t got = fread(buf, 1, n, f);
			buf[got] = 0;
			fclose(f);
			MARK("read file");
			t_read = nowms() - t0;
			if (flags && strchr(flags, 'u')) SSM(SCI_SETUNDOCOLLECTION, 0, 0);
			SSM(SCI_ADDTEXT, got, (sptr_t)buf);
			if (flags && strchr(flags, 'u')) { SSM(SCI_EMPTYUNDOBUFFER, 0, 0); SSM(SCI_SETUNDOCOLLECTION, 1, 0); }
			t_add = nowms() - t0 - t_read;
			MARK("ADDTEXT");
			SSM(SCI_GOTOPOS, 0, 0);
			free(buf);
		}
	}
	SSM(SCI_SETMARGINWIDTHN, 0, 5);
	SSM(SCI_SETFOCUS, 1, 0);
	if (bench) {
		/* No SCI_COLOURISE(0, -1) here: lexing the whole document before the first paint cost
		   6.7 s of the 9.5 s open on the 020 (results/open.md). Editor::Paint styles the visible
		   page on demand; the rest is lexed as it scrolls into view. */
		MARK("COLOURISE");
		if (!(flags && strchr(flags, 'n'))) { long tc = nowms(); SSM(SCI_COLOURISE, 0, SSM(SCI_POSITIONFROMLINE, LINES + 1, 0)); t_colour = nowms() - tc; }
		t_setup = nowms() - t0;
		scintilla_noutrefresh(sci);
		MARK("refresh");
		long open_paint_ms = nowms() - t0;
		cap_buf = (char *)malloc(cap_max); cap_len = 0; wr_on = 1; PROBE_RESET();
		if (flags && strchr(flags, 'p')) pc_start();
		long t_dou0 = nowms();
		doupdate();
		long t_dou = nowms() - t_dou0;
		char open_probes[512]; snprintf(open_probes, sizeof open_probes, "BENCH open_probes flush=%ld/%ldms mvcur=%ld/%ldms vidputs=%ld/%ldms scrollopt=%ld/%ldms hashmap=%ld/%ldms doupdate=%ldms write_each=%ld:%ld,%ld:%ld,%ld:%ld,%ld:%ld\n",
			p__nc_flush_sp_n, p__nc_flush_sp_ms, p__nc_mvcur_sp_n, p__nc_mvcur_sp_ms, p_vid_puts_sp_n, p_vid_puts_sp_ms, p__nc_scroll_optimize_sp_n, p__nc_scroll_optimize_sp_ms, p__nc_hash_map_sp_n, p__nc_hash_map_sp_ms, t_dou,
			wr_each_n[0], wr_each[0], wr_each_n[1], wr_each[1], wr_each_n[2], wr_each[2], wr_each_n[3], wr_each[3]);
		long open_ms = nowms() - t0;
		// Sentinel: open must not style past the first screen (whole-document lexing regression).
		long open_styled_lines = SSM(SCI_LINEFROMPOSITION, SSM(SCI_GETENDSTYLED, 0, 0), 0);
		long open_lines = (long)SSM(SCI_GETLINECOUNT, 0, 0); // read before scintilla_delete: sci is freed below
		wr_on = 0; cap_dump("VTC:ide/cap_open.bin"); wr_stat w_open = wr_take();
		unsigned long avail_after = availkb();
		if (avail_after < avail_min) avail_min = avail_after;
		// Split each step: Scintilla edit + paint into the curses buffer, then doupdate (terminal write).
		long tty = 0;
		t0 = nowms();
		long k_edit = 0, k_paint = 0, k_touched = 0, k_touched_max = 0; PROBE_RESET(); sel_calls = sel_ms = sel_nz = 0;
		for (int i = 0; i < 50; i++) {
			long te = nowms();
			scintilla_send_key(sci, 'x', SCMOD_NORM);
			long tp = nowms(); k_edit += tp - te;
			scintilla_noutrefresh(sci);
			k_paint += nowms() - tp;
			{ long n = 0; for (int r = 0; r < LINES; r++) n += is_linetouched(newscr, r) == TRUE; k_touched += n; if (n > k_touched_max) k_touched_max = n; }
			long t1 = nowms();
			sel_idx = 0;
			wr_on = 1; if (i != 10) cap_len = 0; doupdate(); wr_on = 0;
			if (i == 10) cap_dump("VTC:ide/cap_key.bin");
			tty += nowms() - t1;
		}
		wr_stat w_key = wr_take();
		char key_probes[400];
		snprintf(key_probes, sizeof key_probes, "BENCH key_probes keys=50 edit=%ldms paint=%ldms flush=%ld/%ldms mvcur=%ld/%ldms vidputs=%ld/%ldms scrollopt=%ld/%ldms hashmap=%ld/%ldms wcrtomb=%ld/%ldms selects=%ld/%ldms first_select=%ldms touched_rows_avg=%ld.%02ld max=%ld\n",
			k_edit, k_paint, p__nc_flush_sp_n, p__nc_flush_sp_ms, p__nc_mvcur_sp_n, p__nc_mvcur_sp_ms, p_vid_puts_sp_n, p_vid_puts_sp_ms,
			p__nc_scroll_optimize_sp_n, p__nc_scroll_optimize_sp_ms, p__nc_hash_map_sp_n, p__nc_hash_map_sp_ms, p_wcrtomb_n, p_wcrtomb_ms,
			sel_calls, sel_ms, sel_ms_first, k_touched / 50, (k_touched % 50) * 2, k_touched_max);
		PROBE_RESET(); sel_calls = sel_ms = sel_nz = 0;
		MARK("keys");
		long key_ms = (nowms() - t0) / 50, key_tty_ms = tty / 50;
		unsigned long a = availkb();
		if (a < avail_min) avail_min = a;
		tty = 0;
		long tp_look0 = _nc_tparm_lookups, tp_walk0 = _nc_tparm_walks;
		long vm_c0 = _nc_vidmemo_calls, vm_h0 = _nc_vidmemo_hits, vm_m0 = _nc_vidmemo_misses, vm_b0 = _nc_vidmemo_bad;
		t0 = nowms(); pc_on = 1;
		for (int i = 0; i < 20; i++) {
			scintilla_send_key(sci, SCK_NEXT, SCMOD_NORM);
			scintilla_noutrefresh(sci);
			long t1 = nowms();
			wr_on = 1; if (i != 2) cap_len = 0; doupdate(); wr_on = 0;
			if (i == 2) cap_dump("VTC:ide/cap_scroll.bin");
			tty += nowms() - t1;
		}
		long tp_look = _nc_tparm_lookups - tp_look0, tp_walk = _nc_tparm_walks - tp_walk0;
		long vm_c = _nc_vidmemo_calls - vm_c0, vm_h = _nc_vidmemo_hits - vm_h0, vm_m = _nc_vidmemo_misses - vm_m0, vm_b = _nc_vidmemo_bad - vm_b0;
		wr_stat w_scroll = wr_take(); long p_scroll_n = 20;
		pc_on = 0;
		if (flags && strchr(flags, 'p')) pc_dump("VTC:ide/pcs.txt");
		long scroll_ms = (nowms() - t0) / 20, scroll_tty_ms = tty / 20;
		a = availkb();
		if (a < avail_min) avail_min = a;
		long styled = 0, total = (long)SSM(SCI_GETLENGTH, 0, 0);
		for (long p = 0; p < total; p += 7) styled += SSM(SCI_GETSTYLEAT, p, 0) != 0;
		MARK("scroll");
		char add_parts[600] = "";
		if (flags && strchr(flags, 'm') && path) {
			FILE *f = fopen(path, "rb"); fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
			char *b = (char *)malloc(n + 1), *b2 = (char *)malloc(n + 1); n = (long)fread(b, 1, n, f); fclose(f);
			memset(b2, 'a', n);
			sptr_t d = SSM(SCI_CREATEDOCUMENT, 0, 0); SSM(SCI_SETDOCPOINTER, 0, d);
			long x = nowms(); SSM(SCI_ADDTEXT, n, (sptr_t)b); long a_norm = nowms() - x;
			SSM(SCI_CLEARALL, 0, 0); x = nowms(); SSM(SCI_ADDTEXT, n, (sptr_t)b); long a_norm2 = nowms() - x;
			SSM(SCI_CLEARALL, 0, 0); x = nowms(); SSM(SCI_ADDTEXT, n, (sptr_t)b2); long a_noeol = nowms() - x;
			SSM(SCI_CLEARALL, 0, 0); SSM(SCI_SETUNDOCOLLECTION, 0, 0); x = nowms(); SSM(SCI_ADDTEXT, n, (sptr_t)b); long a_noundo = nowms() - x; SSM(SCI_SETUNDOCOLLECTION, 1, 0);
			SSM(SCI_CLEARALL, 0, 0); x = nowms(); SSM(SCI_ADDTEXT, 1000, (sptr_t)b); long a_1k = nowms() - x;
			volatile long sink = 0; x = nowms(); for (int r = 0; r < 10; r++) memcpy(b2, b, n); long c_memcpy = nowms() - x;
			x = nowms(); for (int r = 0; r < 10; r++) { unsigned char *q = (unsigned char *)b2; for (long i = 0; i < n; i++) q[i] = 0; } long c_fill = nowms() - x;
			x = nowms(); for (int r = 0; r < 10; r++) { long c = 0; for (long i = 0; i < n; i++) c += b[i] == '\n'; sink += c; } long c_scan = nowms() - x;
			snprintf(add_parts, sizeof add_parts, "BENCH addtext_parts bytes=%ld normal=%ld normal2=%ld noeol=%ld noundo=%ld first1k=%ld | per100KB x10: memcpy=%ld bytefill=%ld bytescan=%ld ms\n", n, a_norm, a_norm2, a_noeol, a_noundo, a_1k, c_memcpy, c_fill, c_scan);
		}
		scintilla_delete(sci);
		endwin();
		MARK("endwin");
		FILE *o = argc > 3 ? fopen(argv[3], "w") : stdout;
		fprintf(o, "BENCH open_ms=%ld key_ms=%ld scroll_ms=%ld ram_kb=%ld\n", open_ms, key_ms, scroll_ms, (long)(avail_before - avail_min));
		fprintf(o, "BENCH avail_start_kb=%lu avail_before_kb=%lu avail_after_load_kb=%lu avail_min_kb=%lu drop_from_start_kb=%ld clock_res_ms=%ld styled_samples=%ld of %ld ctors_run=%d\n",
		       avail_start, avail_before, avail_after, avail_min, (long)(avail_start - avail_min), clockres(), styled, (total + 6) / 7, ixrt_ctor_count);
		fprintf(o, "BENCH split open_paint_ms=%ld open_tty_ms=%ld key_paint_ms=%ld key_tty_ms=%ld scroll_paint_ms=%ld scroll_tty_ms=%ld\n",
		       open_paint_ms, open_ms - open_paint_ms, key_ms - key_tty_ms, key_tty_ms, scroll_ms - scroll_tty_ms, scroll_tty_ms);
		fprintf(o, "BENCH open_parts read_ms=%ld addtext_ms=%ld colour_ms=%ld setup_ms=%ld refresh_ms=%ld\n", t_read, t_add, t_colour, t_setup - t_read - t_add - t_colour, open_paint_ms - t_setup);
		fputs(open_probes, o); fputs(add_parts, o);
		fprintf(o, "BENCH open_styled_lines=%ld of %ld rows=%d\n", open_styled_lines, open_lines, LINES);
		fprintf(o, "BENCH draw_transparent_runs=%ld\n", g_sc_transparent_runs);
		fprintf(o, "BENCH tparm_lookups=%ld tree_walks=%ld\n", tp_look, tp_walk);
		fprintf(o, "BENCH vidmemo mode=%d calls=%ld hits=%ld misses=%ld bad=%ld flush=%ld long=%ld\n", _nc_vidmemo_mode, vm_c, vm_h, vm_m, vm_b, _nc_vidmemo_flush, _nc_vidmemo_long);
		fputs(key_probes, o);
		PROBE_PRINT(o); (void)p_scroll_n;
		micro_print(o);
		fprintf(o, "BENCH selects total=%ld nonzero=%ld ms=%ld\n", sel_calls, sel_nz, sel_ms);
		fprintf(o, "BENCH writes open=%ld/%ldB/%ldms key_per=%ld/%ldB/%ldms scroll_per=%ld/%ldB/%ldms\n",
		       w_open.calls, w_open.bytes, w_open.ms, w_key.calls / 50, w_key.bytes / 50, w_key.ms / 50,
		       w_scroll.calls / 20, w_scroll.bytes / 20, w_scroll.ms / 20);
		fflush(o);
		if (o != stdout) fclose(o);
		return 0;
	}
	scintilla_refresh(sci);
	int c;
	WINDOW *win = scintilla_get_window(sci);
	keypad(win, TRUE);
	while ((c = wgetch(win)) != 17) { // ctrl-q
		if (c == KEY_UP) c = SCK_UP;
		else if (c == KEY_DOWN) c = SCK_DOWN;
		else if (c == KEY_LEFT) c = SCK_LEFT;
		else if (c == KEY_RIGHT) c = SCK_RIGHT;
		else if (c == KEY_NPAGE) c = SCK_NEXT;
		else if (c == KEY_PPAGE) c = SCK_PRIOR;
		scintilla_send_key(sci, c, SCMOD_NORM);
		scintilla_refresh(sci);
	}
	scintilla_delete(sci);
	endwin();
	return 0;
}
