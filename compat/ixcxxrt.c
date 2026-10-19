/* C++ runtime start for gcc16 objects linked with the ixemul crt0 (gcc 6.5 era).
 *
 * bebbo's ld collects static constructors, destructors and EH frame pointers into
 * named list sections (.list___CTOR_LIST__, .list___DTOR_LIST__,
 * .list___EH_FRAME_BEGINS__, .dlist___EH_FRAME_OBJECTS__; ldscript SORT_BY_NAME).
 * The libnix crt0 supplies a header and a 0 terminator for each list and walks them
 * (___initcpp, ___exitcpp, ___init_eh). The ixemul crt0 has none of this, so under
 * ixemul no global constructor ever ran and no EH frame was registered: Scintilla /
 * Lexilla / libstdc++ statics stayed zero and the first virtual call through one
 * jumped into garbage (ixemul "EMT trap").
 *
 * This object provides the same: header sections whose names sort just before each
 * list (a prefix of it) and terminators that sort just after it (name + "~"), plus a
 * main wrapper (link with -Wl,--wrap=main) that registers EH frames, runs ctors in
 * libnix order (last first) and queues dtors via atexit. */
#include <stdlib.h>
#ifdef IXRT_DIAG
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
/* crash-hunt markers: each one opens, appends, closes RAM:mk.txt so it survives the trap */
void ixrt_mark(const char *tag, long v)
{
	char b[160];
	int fd = open("RAM:mk.txt", O_WRONLY | O_CREAT | O_APPEND, 0666);
	if (fd < 0) return;
	write(fd, b, sprintf(b, "%s %ld (0x%lx)\n", tag, v, (unsigned long)v));
	close(fd);
}
/* ixemul handler frame (library/machdep.c sendsig): signum, code, addr, handler, sigcontext,
   then the gdb dummy frame {a5, faulting pc}; sig_trampoline passes &sf_sc as 4th arg */
static void ixrt_trap(int sig, int code, void *addr, struct sigcontext *scp)
{
	struct sigcontext sc = *scp;
	int *dummy = (int *)(scp + 1);
	unsigned long *sp = (unsigned long *)sc.sc_sp;
	unsigned short *pc = (unsigned short *)dummy[1];
	int i;
	ixrt_mark("TRAP sig", sig);
	ixrt_mark("TRAP code", code);
	ixrt_mark("TRAP addr", (long)addr);
	ixrt_mark("TRAP pc", dummy[1]);
	ixrt_mark("TRAP a5", dummy[0]);
	ixrt_mark("TRAP usp", sc.sc_sp);
	ixrt_mark("TRAP ixrt_mark", (long)&ixrt_mark);
	if (dummy[1] > 0x1000)
		for (i = 0; i < 4; i++) ixrt_mark("TRAP pcword", pc[i]);
	for (i = 0; i < 32; i++) ixrt_mark("TRAP stk", sp[i]);
	_exit(99);
}
#define MARK(t, v) ixrt_mark(t, v)
#else
#define MARK(t, v) ((void)0)
#endif

__asm__(
	"	.section .list___CTOR_LIST_,\"aw\"\n"
	"_ixrt_ctor_hdr:	.long 0\n"
	"	.section .list___CTOR_LIST__~,\"aw\"\n"
	"	.long 0\n"
	"	.section .list___DTOR_LIST_,\"aw\"\n"
	"_ixrt_dtor_hdr:	.long 0\n"
	"	.section .list___DTOR_LIST__~,\"aw\"\n"
	"	.long 0\n"
	"	.section .list___EH_FRAME_BEGIN,\"aw\"\n"
	"_ixrt_ehb_hdr:	.long 0\n"
	"	.section .list___EH_FRAME_BEGINS__~,\"aw\"\n"
	"	.long 0\n"
	"	.section .dlist___EH_FRAME_OBJECT,\"aw\"\n"
	"_ixrt_eho_hdr:	.long 0\n"
	"	.section .dlist___EH_FRAME_OBJECTS__~,\"aw\"\n"
	"	.long 0\n"
	"	.text\n");

typedef void (*fn)(void);
extern fn ixrt_ctor_hdr[], ixrt_dtor_hdr[];
extern void *ixrt_ehb_hdr[], *ixrt_eho_hdr[];
extern void __register_frame_info(const void *, void *);
extern int __real_main(int, char **, char **);

static void ixrt_dtors(void)
{
	fn *p;
	for (p = ixrt_dtor_hdr + 1; *p; p++)
		(*p)();
}

/* gcc 16.1.1b emits static-init functions that clobber callee-saved registers without
   saving them: libstdc++.a(wlocale-inst.o) __GLOBAL__sub_I__ZNSt12ctype_bynameIwEC2ERKSsm
   uses d2 for the 64-bit _ZGV guard increments and has no prologue/epilogue (objdump -d:
   clrl d0 ... movel d2,<guard> ... rts). Before this, argc (held in d2 here) was 0 by the
   time __real_main ran. Each ctor is called through a trampoline,
   so nothing live in this frame sits in a register across the call: the asm
   trampoline below saves d2-d7/a2-a6 around each one. */
void ixrt_call_ctor(fn f);
__asm__(
	"	.text\n"
	"	.even\n"
	"_ixrt_call_ctor:\n"
	"	moveml d2-d7/a2-a6,sp@-\n"
	"	movel sp@(48),a0\n"
	"	jsr a0@\n"
	"	moveml sp@+,d2-d7/a2-a6\n"
	"	rts\n");

int ixrt_ctor_count; /* sentinel: number of ctors run, checked by the demo */

int __wrap_main(int argc, char **argv, char **envp)
{
	void **b = ixrt_ehb_hdr + 1, **o = ixrt_eho_hdr + 1;
	fn *p, *first = ixrt_ctor_hdr + 1;
#ifdef IXRT_DIAG
	signal(SIGEMT, (void (*)(int))ixrt_trap);
	signal(SIGILL, (void (*)(int))ixrt_trap);
	signal(SIGBUS, (void (*)(int))ixrt_trap);
	signal(SIGSEGV, (void (*)(int))ixrt_trap);
	MARK("wrap_main argc", argc);
	MARK(argc > 1 ? argv[1] : "-", argc > 1 ? (long)argv[1] : 0);
	MARK("wrap_main self", (long)&__wrap_main);
#endif
	for (; *b; b++, o++)
		__register_frame_info(*b, *o);
	MARK("eh registered", b - (ixrt_ehb_hdr + 1));
	for (p = first; *p; p++) {}
	while (p > first) {
		--p;
		MARK("ctor", (long)*p);
		ixrt_call_ctor(*p);
		ixrt_ctor_count++;
		MARK(argc > 1 ? argv[1] : "-", ((unsigned char)argv[1][0] << 8) | (unsigned char)argv[1][1]);
	}
	atexit(ixrt_dtors);
	MARK("ctors done", ixrt_ctor_count);
	MARK(argc > 1 ? argv[1] : "-", 1);
	return __real_main(argc, argv, envp);
}
