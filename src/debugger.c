/*B-em v2.2 by Tom Walker
  Debugger*/

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "cpu_debug.h"
#include "debugger.h"
#include "b-em.h"
#include "main.h"
#include "mem.h"
#include "model.h"
#include "6502.h"
#include "debugger_symbols.h"

#include <allegro5/allegro_primitives.h>

#define NUM_BREAKPOINTS 8

int debug_core = 0;
int debug_tube = 0;
int debug_step = 0;
int indebug = 0;
extern int fcount;
static int vrefresh = 1;
static FILE *trace_fp = NULL;

struct file_stack;

struct filestack {
    struct filestack *next;
    FILE *fp;
} *exec_stack;

static void close_trace()
{
    if (trace_fp) {
        fputs("Trace finished due to emulator quit\n", trace_fp);
        fclose(trace_fp);
    }
}

static ALLEGRO_THREAD  *mem_thread;

static void *mem_thread_proc(ALLEGRO_THREAD *thread, void *data)
{
    ALLEGRO_DISPLAY *mem_disp;
    ALLEGRO_BITMAP *bitmap;
    ALLEGRO_LOCKED_REGION *region;
    int row, col, addr, cnt, red, grn, blu;

    log_debug("debugger: memory view thread started");
    al_set_new_window_title("B-Em Memory View");
    if ((mem_disp = al_create_display(256, 256))) {
        al_set_new_bitmap_flags(ALLEGRO_VIDEO_BITMAP);
        if ((bitmap = al_create_bitmap(256, 256))) {
            while (!quitting && !al_get_thread_should_stop(thread)) {
                al_rest(0.02);
                al_set_target_bitmap(bitmap);
                if ((region = al_lock_bitmap(bitmap, ALLEGRO_LOCK_WRITEONLY, ALLEGRO_PIXEL_FORMAT_ANY))) {
                    addr = 0;
                    for (row = 0; row < 256; row++) {
                        for (col = 0; col < 256; col++) {
                            red = grn = blu = 0;
                            if ((cnt = writec[addr])) {
                                red = cnt * 8;
                                writec[addr] = cnt - 1;
                            }
                            if ((cnt = readc[addr])) {
                                grn = cnt * 8;
                                readc[addr] = cnt - 1;
                            }
                            if ((cnt = fetchc[addr])) {
                                blu = cnt * 8;
                                fetchc[addr] = cnt - 1;
                            }
                            al_put_pixel(col, row, al_map_rgb(red, grn, blu));
                            addr++;
                        }
                    }
                    al_unlock_bitmap(bitmap);
                    al_set_target_backbuffer(mem_disp);
                    al_draw_bitmap(bitmap, 0.0, 0.0, 0);
                    al_flip_display();
                }
            }
            al_destroy_bitmap(bitmap);
        }
        al_destroy_display(mem_disp);
    }
    return NULL;
}

static void debug_memview_open(void)
{
    if (!mem_thread) {
        if ((mem_thread = al_create_thread(mem_thread_proc, NULL))) {
            log_debug("debugger: memory view thread created");
            al_start_thread(mem_thread);
        }
        else
            log_error("debugger: failed to create memory view thread");
    }
}

static void debug_memview_close(void)
{
    if (mem_thread) {
        al_join_thread(mem_thread, NULL);
        mem_thread = NULL;
    }
}

#ifdef WIN32
#include <windows.h>
#include <wingdi.h>
#include <process.h>

static int debug_cons = 0;
static HANDLE cinf, consf;

static inline bool debug_in(char *buf, size_t bufsize)
{
    DWORD len;

    if (ReadConsole(cinf, buf, bufsize, &len, NULL)) {
        buf[len] = 0;
        log_debug("debugger: read console, len=%d, s=%s", (int)len, buf);
        return true;
    } else {
        log_error("debugger: unable to read from console: %lu", GetLastError());
        return false;
    }
}

static void debug_out(const char *s, size_t len)
{
    WriteConsole(consf, s, len, NULL, NULL);
}

static void debug_outf(const char *fmt, ...)
{
    va_list ap;
    char s[256];
    size_t len;

    va_start(ap, fmt);
    len = vsnprintf(s, sizeof s, fmt, ap);
    va_end(ap);
    WriteConsole(consf, s, len, NULL, NULL);
}

LRESULT CALLBACK DebugWindowProcedure (HWND, UINT, WPARAM, LPARAM);
static HINSTANCE hinst;

BOOL CtrlHandler(DWORD fdwCtrlType)
{
    main_setquit();
    return TRUE;
}

static void debug_cons_open(void)
{
    int c;

    if (debug_cons++ == 0)
    {
        hinst = GetModuleHandle(NULL);
        if ((c = AllocConsole())) {
            SetConsoleCtrlHandler((PHANDLER_ROUTINE)CtrlHandler, TRUE);
            consf = GetStdHandle(STD_OUTPUT_HANDLE);
            cinf  = GetStdHandle(STD_INPUT_HANDLE);
        } else {
            log_fatal("debugger: unable to allocate console: %lu", GetLastError());
            exit(1);
        }
    }
}

static void debug_cons_close(void)
{
    if (--debug_cons == 0)
        FreeConsole();
}

#else

static inline bool debug_in(char *buf, size_t bufsize)
{
    return fgets(buf, bufsize, stdin);
}

static void debug_out(const char *s, size_t len)
{
    fwrite(s, len, 1, stdout);
    fflush(stdout);
}

static void debug_outf(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    fflush(stdout);
}

static inline void debug_cons_open(void) {}

static inline void debug_cons_close(void) {}

#endif

#include <errno.h>
#include "6502.h"
#include "via.h"
#include "sysvia.h"
#include "uservia.h"
#include "video.h"
#include "sn76489.h"
#include "model.h"

void debug_kill()
{
    close_trace();
    debug_memview_close();
    debug_cons_close();
}

static void enable_core_debug(void)
{
    debug_cons_open();
    debug_memview_open();
    debug_step = 1;
    debug_core = 1;
    log_info("debugger: debugging of core 6502 enabled");
    core6502_cpu_debug.debug_enable(1);
}

static void enable_tube_debug(void)
{
    if (curtube != -1)
    {
        debug_cons_open();
        debug_step = 1;
        debug_tube = 1;
        log_info("debugger: debugging of tube CPU enabled");
        tubes[curtube].debug->debug_enable(1);
    }
}

static void disable_core_debug(void)
{
    core6502_cpu_debug.debug_enable(0);
    log_info("debugger: debugging of core 6502 disabled");
    debug_core = 0;
    debug_memview_close();
    debug_cons_close();
}

static void disable_tube_debug(void)
{
    if (curtube != -1)
    {
        tubes[curtube].debug->debug_enable(0);
        log_info("debugger: debugging of tube CPU disabled");
        debug_tube = 0;
        debug_cons_close();
    }
}


static void push_exec(const char *fn)
{
    struct filestack *fs = malloc(sizeof(struct filestack));
    if (fs) {
        if ((fs->fp = fopen(fn, "r"))) {
            fs->next = exec_stack;
            exec_stack = fs;
        }
        else
            debug_outf("unable to open exec file '%s': %s\n", fn, strerror(errno));
    }
    else
        debug_outf("out of memory for exec file\n");
}

static void pop_exec(void)
{
    struct filestack *cur = exec_stack;
    fclose(cur->fp);
    exec_stack = cur->next;
    free(cur);
}

void debug_start(const char *exec_fn)
{
    if (debug_core)
        enable_core_debug();
    if (debug_tube)
        enable_tube_debug();
    if (exec_fn)
        push_exec(exec_fn);
}

void debug_end(void)
{
    if (debug_tube)
        disable_tube_debug();
    if (debug_core)
        disable_core_debug();
}

void debug_toggle_core(void)
{
    if (debug_core)
        disable_core_debug();
    else
        enable_core_debug();
}

void debug_toggle_tube(void)
{
    if (debug_tube)
        disable_tube_debug();
    else
        enable_tube_debug();
}

int readc[65536], writec[65536], fetchc[65536];

static uint32_t debug_memaddr=0;
static uint32_t debug_disaddr=0;
static uint8_t  debug_lastcommand=0;

static int tbreak = -1;
static int breakpoints[NUM_BREAKPOINTS] = {-1, -1, -1, -1, -1, -1, -1, -1};
static int breakr[NUM_BREAKPOINTS]      = {-1, -1, -1, -1, -1, -1, -1, -1};
static int breakw[NUM_BREAKPOINTS]      = {-1, -1, -1, -1, -1, -1, -1, -1};
static int breaki[NUM_BREAKPOINTS]      = {-1, -1, -1, -1, -1, -1, -1, -1};
static int breako[NUM_BREAKPOINTS]      = {-1, -1, -1, -1, -1, -1, -1, -1};
static int watchr[NUM_BREAKPOINTS]      = {-1, -1, -1, -1, -1, -1, -1, -1};
static int watchw[NUM_BREAKPOINTS]      = {-1, -1, -1, -1, -1, -1, -1, -1};
static int watchi[NUM_BREAKPOINTS]      = {-1, -1, -1, -1, -1, -1, -1, -1};
static int watcho[NUM_BREAKPOINTS]      = {-1, -1, -1, -1, -1, -1, -1, -1};
static int contcount = 0;

void debug_reset()
{
    if (trace_fp) {
        fputs("Processor reset!\n", trace_fp);
        fflush(trace_fp);
    }
}

static const char helptext[] =
    "\n    Debugger commands :\n\n"
    "    bclear n   - clear breakpoint n or breakpoint at n\n"
    "    bclearr n  - clear read breakpoint n or read breakpoint at n\n"
    "    bclearw n  - clear write breakpoint n or write breakpoint at n\n"
    "    bcleari n  - clear input breakpoint n or input breakpoint at n\n"
    "    bclearo n  - clear output breakpoint n or output breakpoint at n\n"
    "    blist      - list current breakpoints\n"
    "    break n    - set a breakpoint at n\n"
    "    breakr n   - break on reads from address n\n"
    "    breakw n   - break on writes to address n\n"
    "    breaki n   - break on input from I/O port\n"
    "    breako n   - break on output to I/O port\n"
    "    c          - continue running until breakpoint\n"
    "    c n        - continue until the nth breakpoint\n"
    "    d [n]      - disassemble from address n\n"
    "    exec f     - take commands from file f\n"
    "    n          - step, but treat a called subroutine as one step\n"
    "    m [n]      - memory dump from address n\n"
    "    paste s    - paste string s as keyboard input\n"
    "    q          - force emulator exit\n"
    "    r          - print 6502 registers\n"
    "    r sysvia   - print System VIA registers\n"
    "    r uservia  - print User VIA registers\n"
    "    r crtc     - print CRTC registers\n"
    "    r vidproc  - print VIDPROC registers\n"
    "    r sound    - print Sound registers\n"
    "    reset      - reset emulated machine\n"
    "    s [n]      - step n instructions (or 1 if no parameter)\n"
    "    symbol name=[rom:]addr\n"
    "               - add debugger symbol\n"
    "    symlist    - list all symbols\n"
    "    trace fn   - trace disassembly/registers to file, close file if no fn\n"
    "    vrefresh t - extra video refresh on entering debugger.  t=on or off\n"
    "    watchr n   - watch reads from address n\n"
    "    watchw n   - watch writes to address n\n"
    "    watchi n   - watch inputs from port n\n"
    "    watcho n   - watch outputs to port n\n"
    "    wclearr n  - clear read watchpoint n or read watchpoint at n\n"
    "    wclearw n  - clear write watchpoint n or write watchpoint at n\n"
    "    wcleari n  - clear input watchpoint n or input watchpoint at n\n"
    "    wclearo n  - clear output watchpoint n or output watchpoint at n\n"
    "    wlist      - list watchpoints\n"
    "    writem a v - write to memory, a = address, v = value\n";

static char xdigs[] = "0123456789ABCDEF";

size_t debug_print_8bit(uint32_t value, char *buf, size_t bufsize)
{
    if (bufsize >= 3) {
        buf[2] = 0;
        buf[0] = xdigs[(value >> 4) & 0x0f];
        buf[1] = xdigs[value & 0x0f];
    }
    return 3;
}

size_t debug_print_16bit(uint32_t value, char *buf, size_t bufsize)
{
    if (bufsize >= 5) {
        buf[4] = 0;
        for (int i = 3; i >= 0; i--) {
            buf[i] = xdigs[value & 0x0f];
            value >>= 4;
        }
    }
    return 5;
}

size_t debug_print_32bit(uint32_t value, char *buf, size_t bufsize)
{
    if (bufsize >= 9) {
        buf[8] = 0;
        for (int i = 7; i >= 0; i--) {
            buf[i] = xdigs[value & 0x0f];
            value >>= 4;
        }
    }
    return 9;
}

size_t debug_print_addr16(cpu_debug_t *cpu, uint32_t value, char *buf, size_t bufsize, bool include_symbol) {
    const char *sym = NULL;
    size_t ret;
    if (!include_symbol || !symbol_find_by_addr(cpu->symbols, value, &sym))
        sym = NULL;
    if (sym) {
        ret = snprintf(buf, bufsize, "%04X (%s)", value, sym);
    }
    else
        ret = snprintf(buf, bufsize, "%04X", value);

    if (ret > bufsize)
        return bufsize;
    else
        return ret;
}

size_t debug_print_addr32(cpu_debug_t *cpu, uint32_t value, char *buf, size_t bufsize, bool include_symbol) {
    const char *sym = NULL;
    size_t ret;
    if (!include_symbol || !symbol_find_by_addr(cpu->symbols, value, &sym))
        sym = NULL;
    if (sym) {
        ret = snprintf(buf, bufsize, "%08X (%s)", value, sym);
    }
    else
        ret = snprintf(buf, bufsize, "%08X", value);

    if (ret > bufsize)
        return bufsize;
    else
        return ret;
}

uint32_t debug_parse_addr(cpu_debug_t *cpu, const char *buf, const char **end)
{
    return strtoul(buf, (char **)end, 16);
}

static void print_registers(cpu_debug_t *cpu) {
    const char **np, *name;
    char buf[50];
    size_t len;
    int r;

    for (r = 0, np = cpu->reg_names; (name = *np++); ) {
        debug_outf(" %s=", name);
        len = cpu->reg_print(r++, buf, sizeof buf);
        debug_out(buf, len);
    }
    debug_out("\n", 1);
}

static uint32_t parse_address_or_symbol(cpu_debug_t *cpu, char *arg, const char **endret) {

    uint32_t a;
    //first see if there is a symbol
    if (symbol_find_by_name(cpu->symbols, arg, &a, endret))
        return a;
    return cpu->parse_addr(cpu, arg, endret);
}

static void set_sym(cpu_debug_t *cpu, const char *arg) {
    if (!cpu->symbols)
        cpu->symbols = symbol_new();
    if (cpu->symbols) {

        int n;
        char name[SYM_MAX + 1], rest[SYM_MAX + 1];

        n = sscanf(arg, "%" STRINGY(SYM_MAX) "[^= ] = %" STRINGY(SYM_MAX) "s", name, rest);
        const char *e;
        uint32_t addr;
        if (n == 2)
            addr = parse_address_or_symbol(cpu, rest, &e);


        if (n == 2 && e != rest && strlen(name)) {
            char abuf[17];
            cpu->print_addr(cpu, addr, abuf, 16, false);

            symbol_add(cpu->symbols, name, addr);

            debug_outf("SYMBOL %s set to %s\n", name, abuf);

        }
        else {
            debug_outf("Bad command\n");
        }
    }
    else {
        debug_outf("no symbol table\n");
    }

}

static void list_syms(cpu_debug_t *cpu, const char *arg) {
    symbol_list(cpu->symbols, cpu, &debug_outf);
}

static void set_point(cpu_debug_t *cpu, int *table, char *arg, const char *desc)
{
    int c;

    const char *end1;
    uint32_t a = parse_address_or_symbol(cpu, arg, &end1);

    char addrbuf[16 + SYM_MAX];
    cpu->print_addr(cpu, a, addrbuf, sizeof(addrbuf), true);

    if (end1 > arg) {

        // check to see if already set
        for (c = 0; c < NUM_BREAKPOINTS; c++) {
            if (table[c] == a)
            {
                debug_outf("    %s %i already set to %s\n", desc, c, addrbuf);
                return;
            }
        }

        for (c = 0; c < NUM_BREAKPOINTS; c++) {
            if (table[c] == -1) {
                table[c] = a;
                debug_outf("    %s %i set to %s\n", desc, c, addrbuf);
                return;
            }
        }
        debug_outf("    unable to set %s breakpoint, table full\n", desc);
    } else
        debug_out("    missing parameter or invalid address\n!", 24);
}

static void clear_point(cpu_debug_t *cpu, int *table, char *arg, const char *desc)
{
    int c, e;

    const char *p;
    e = parse_address_or_symbol(cpu, arg, &p);

    if (p != arg) {
        int ix = -1;
        //DB: changed this to search by address first then by index
        for (c = 0; c < NUM_BREAKPOINTS; c++) {
            if (table[c] == e) {
                ix = c;
            }
        }
        if (ix == -1 && e < NUM_BREAKPOINTS && table[e] != -1)
            ix = e;

        if (ix >= 0) {

            char addrbuf[16 + SYM_MAX];
            cpu->print_addr(cpu, table[ix], addrbuf, sizeof(addrbuf), true);

            debug_outf("    %s %i at %s cleared\n", desc, ix, addrbuf);
            table[ix] = -1;
        }
        else {
            debug_outf("    Can't find that breakpoint\n");
        }


    } else
        debug_out("    missing parameter\n!", 24);
}

static void list_points(cpu_debug_t *cpu, int *table, const char *desc)
{
    char addr_buf[17 + SYM_MAX];

    for (int c = 0; c < NUM_BREAKPOINTS; c++)
        if (table[c] != -1) {
            cpu->print_addr(cpu, table[c], addr_buf, sizeof(addr_buf), true);
            debug_outf("    %s %i : %-8s\n", desc, c, addr_buf);
        }
}

static void debug_paste(const char *iptr)
{
    int ch;
    char *str, *dptr;

    if ((ch = *iptr++)) {
        if ((str = al_malloc(strlen(iptr) + 1))) {
            dptr = str;
            do {
                if (ch == '|') {
                    if (!(ch = *iptr++))
                        break;
                    if (ch != '|')
                        ch &= 0x1f;
                }
                *dptr++ = ch;
                ch = *iptr++;
            } while (ch);
            *dptr = '\0';
            os_paste_start(str);
        }
    }
}

static void save_points(FILE *sfp, const char *cmd, int *points)
{
    for (int c = 0; c < NUM_BREAKPOINTS; c++) {
        int point = points[c];
        if (point != -1)
            fprintf(sfp, "%s %x\n", cmd, point);
    }
}

static void debugger_save(char *iptr)
{
    char *eptr = strchr(iptr, '\n');
    if (eptr)
        *eptr = '\0';
    FILE *sfp = fopen(iptr, "w");
    if (sfp) {
        save_points(sfp, "break", breakpoints);
        save_points(sfp, "breakr", breakr);
        save_points(sfp, "breakw", breakw);
        save_points(sfp, "breaki", breaki);
        save_points(sfp, "breako", breako);
        save_points(sfp, "watchr", watchr);
        save_points(sfp, "watchw", watchw);
        save_points(sfp, "watchi", watchi);
        save_points(sfp, "watcho", watcho);
        debug_outf("Breakpoints saved to %s\n", iptr);
        fclose(sfp);
    }
    else
        debug_outf("unable to open '%s' for writing: %s\n", strerror(errno));
}

void trimnl(char *buf) {
    int len = strlen(buf);

    while (len >= 1 && (buf[len - 1] == '\r' || buf[len - 1] == '\n'))
        buf[--len] = '\0';

}

typedef enum {
    SWS_GROUND,
    SWS_GOT_SQUARE,
    SWS_GOT_CURLY,
    SWS_IN_NAME,
    SWS_TOO_LONG,
    SWS_NAME_END,
    SWS_IN_VALUE,
    SWS_AWAIT_COMMA
} swstate;

static void swiftsym(cpu_debug_t *cpu, char *iptr)
{
    uint32_t romaddr = 0;
    char *sep = strpbrk(iptr, " \t");
    if (sep) {
        *sep++ = 0;
        romaddr = strtoul(sep, NULL, 16) << 28;
    }
    FILE *fp = fopen(iptr, "r");
    if (fp) {
        swstate state = SWS_GROUND;
        char name[80], *name_ptr, *name_end = name+sizeof(name);
        uint32_t addr;
        int ch, syms = 0;
        if (!cpu->symbols)
            cpu->symbols = symbol_new();
        while ((ch = getc(fp)) != EOF) {
            switch(state) {
                case SWS_GROUND:
                    if (ch == '[')
                        state = SWS_GOT_SQUARE;
                    break;
                case SWS_GOT_SQUARE:
                    if (ch == '{')
                        state = SWS_GOT_CURLY;
                    else if (ch != '[')
                        state = SWS_GROUND;
                    break;
                case SWS_GOT_CURLY:
                    if (ch == '\'') {
                        name_ptr = name;
                        state = SWS_IN_NAME;
                    }
                    else if (!strchr(" \t\r\n", ch))
                        state = SWS_GROUND;
                    break;
                case SWS_IN_NAME:
                    if (ch == '\'') {
                        *name_ptr = 0;
                        state = SWS_NAME_END;
                    }
                    else if (name_ptr >= name_end) {
                        debug_outf("swift import name too long");
                        state = SWS_TOO_LONG;
                    }
                    else
                        *name_ptr++ = ch;
                    break;
                case SWS_TOO_LONG:
                    if (ch == '\'') {
                        name_ptr = 0;
                        state = SWS_NAME_END;
                    }
                    break;
                case SWS_NAME_END:
                    if (ch == ':') {
                        addr = 0;
                        state = SWS_IN_VALUE;
                    }
                    else if (!strchr(" \t\r\n", ch))
                        state = SWS_GROUND;
                    break;
                case SWS_IN_VALUE:
                    if (ch >= '0' && ch <= '9')
                        addr = addr * 10 + ch - '0';
                    else if (ch == 'L') {
                        symbol_add(cpu->symbols, name, addr|romaddr);
                        state = SWS_AWAIT_COMMA;
                        syms++;
                    }
                    else if (ch == ',') {
                        symbol_add(cpu->symbols, name, addr|romaddr);
                        state = SWS_GOT_CURLY;
                        syms++;
                    }
                    else
                        state = SWS_GROUND;
                    break;
                case SWS_AWAIT_COMMA:
                    if (ch == ',')
                        state = SWS_GOT_CURLY;
                    else if (!strchr(" \t\r\n", ch))
                        state = SWS_GROUND;
            }
        }
        fclose(fp);
        debug_outf("%d symbols loaded from %s\n", syms, iptr);
    }
    else
        debug_outf("unable to open '%s': %s\n", iptr, strerror(errno));
}

void debugger_do(cpu_debug_t *cpu, uint32_t addr)
{
    uint32_t next_addr;
    char ins[256];

    main_pause();
    indebug = 1;
    const char *sym;
    if (symbol_find_by_addr(cpu->symbols, addr, &sym)) {
        debug_outf("%s:\n", sym);
    }
    log_debug("debugger: about to call disassembler, addr=%04X", addr);
    next_addr = cpu->disassemble(cpu, addr, ins, sizeof ins);
    debug_out(ins, strlen(ins));
    if (vrefresh)
        video_poll(CLOCKS_PER_FRAME, 0);

    for (;;) {
        char *iptr, *cmd;
        size_t cmdlen;
        int c;
        bool badcmd = false;

        if (exec_stack) {
            if (!fgets(ins, sizeof ins, exec_stack->fp)) {
                pop_exec();
                continue;
            }
        }
        else {
            debug_out(">", 1);
            if (!debug_in(ins, 255)) {
                static const char msg[] = "\nTreating EOF on console as 'continue'\n";
                debug_out(msg, sizeof(msg)-1);
                indebug = 0;
                main_resume();
                return;
            }
        }

        trimnl(ins);
        // Skip past any leading spaces.
        for (iptr = ins; (c = *iptr) && isspace(c); iptr++);
        if (c) {
            cmd = iptr;
            // Find the first space and terminate command name.
            while (c && !isspace(c)) {
                *iptr++ = tolower(c);
                c = *iptr;
            }
            *iptr = '\0';
            cmdlen = iptr - cmd;
            // Skip past any separating spaces.
            while (c && isspace(c))
                c = *++iptr;
            // iptr now points to the parameter.
        } else {
            cmd = iptr = ins;
            *iptr++ = debug_lastcommand;
            *iptr = '\0';
            cmdlen = 1;
        }
        switch (*cmd) {
            case 'b':
                if (!strncmp(cmd, "break", cmdlen))
                    set_point(cpu, breakpoints, iptr, "Breakpoint");
                else if (!strncmp(cmd, "breaki", cmdlen))
                    set_point(cpu, breaki, iptr, "Input breakpoint");
                else if (!strncmp(cmd, "breako", cmdlen))
                    set_point(cpu, breako, iptr, "Output breakpoint");
                else if (!strncmp(cmd, "breakr", cmdlen))
                    set_point(cpu, breakr, iptr, "Read breakpoint");
                else if (!strncmp(cmd, "breakw", cmdlen))
                    set_point(cpu, breakw, iptr, "Write breakpoint");
                else if (!strncmp(ins, "blist", cmdlen)) {
                    list_points(cpu, breakpoints, "Breakpoint");
                    list_points(cpu, breakr, "Read breakpoint");
                    list_points(cpu, breakw, "Write breakpoint");
                    list_points(cpu, breaki, "Input breakpoint");
                    list_points(cpu, breako, "Output breakpoint");
                }
                else if (!strncmp(ins, "bclear", cmdlen))
                    clear_point(cpu, breakpoints, iptr, "Breakpoint");
                else if (!strncmp(cmd, "bcleari", cmdlen))
                    clear_point(cpu, breaki, iptr, "Input breakpoint");
                else if (!strncmp(cmd, "bclearo", cmdlen))
                    clear_point(cpu, breako, iptr, "Output breakpoint");
                else if (!strncmp(cmd, "bclearr", cmdlen))
                    clear_point(cpu, breakr, iptr, "Read breakpoint");
                else if (!strncmp(cmd, "bclearw", cmdlen))
                    clear_point(cpu, breakw, iptr, "Write breakpoint");
                else
                    badcmd = true;
                break;

            case 'q':
                main_setquit();
                /* FALLTHOUGH */

            case 'c':
                if (*iptr)
                    sscanf(iptr, "%d", &contcount);
                debug_lastcommand = 'c';
                indebug = 0;
                main_resume();
                return;

            case 'd':
                if (*iptr) {
                    const char *e;
                    debug_disaddr = parse_address_or_symbol(cpu, iptr, &e);
                }
                for (c = 0; c < 12; c++) {
                    const char *sym;
                    if (symbol_find_by_addr(cpu->symbols, debug_disaddr, &sym)) {
                        debug_outf("%s:\n", sym);
                    }
                    debug_out("    ", 4);
                    debug_disaddr = cpu->disassemble(cpu, debug_disaddr, ins, sizeof ins);
                    debug_out(ins, strlen(ins));
                    debug_out("\n", 1);
                }
                debug_lastcommand = 'd';
                break;
            case 'e':
                if (!strncmp(cmd, "exec", cmdlen)) {
                    if (*iptr) {
                        char *eptr = strchr(iptr, '\n');
                        if (eptr)
                            *eptr = 0;
                        push_exec(iptr);
                    }
                }
                else
                    badcmd = true;
                break;

            case 'h':
            case '?':
                debug_out(helptext, sizeof helptext - 1);
                break;

            case 'm':
                if (*iptr) {
                    const char *e;
                    debug_memaddr = parse_address_or_symbol(cpu, iptr, &e);
                }
                for (c = 0; c < 16; c++) {
                    char dump[256], *dptr;
                    cpu->print_addr(cpu, debug_memaddr, dump, sizeof(dump), false);
                    debug_outf("%s : ", dump);
                    for (int d = 0; d < 16; d++)
                        debug_outf("%02X ", cpu->memread(debug_memaddr + d));
                    debug_out("  ", 2);
                    dptr = dump;
                    for (int d = 0; d < 16; d++) {
                        uint32_t temp = cpu->memread(debug_memaddr + d);
                        if (temp < ' ' || temp >= 0x7f)
                            *dptr++ = '.';
                        else
                            *dptr++ = temp;
                    }
                    *dptr++ = '\n';
                    debug_out(dump, dptr - dump);
                    debug_memaddr += 16;
                }
                debug_lastcommand = 'm';
                break;

            case 'n':
                tbreak = next_addr;
                debug_lastcommand = 'n';
                indebug = 0;
                main_resume();
                return;

            case 'p':
                if (!strncmp(cmd, "paste", cmdlen))
                    debug_paste(iptr);
                else
                    badcmd = true;
                break;

            case 'r':
                if (cmdlen >= 3 && !strncmp(cmd, "reset", cmdlen)) {
                    main_reset();
                    debug_outf("Emulator reset\n");
                } else if (*iptr) {
                    size_t arglen = strcspn(iptr, " \t\n");
                    iptr[arglen] = 0;
                    if (!strncasecmp(iptr, "sysvia", arglen)) {
                        debug_outf("    System VIA registers :\n");
                        debug_outf("    ORA  %02X ORB  %02X IRA %02X IRB %02X\n", sysvia.ora, sysvia.orb, sysvia.ira, sysvia.irb);
                        debug_outf("    DDRA %02X DDRB %02X ACR %02X PCR %02X\n", sysvia.ddra, sysvia.ddrb, sysvia.acr, sysvia.pcr);
                        debug_outf("    Timer 1 latch %04X   count %04X\n", sysvia.t1l / 2, (sysvia.t1c / 2) & 0xFFFF);
                        debug_outf("    Timer 2 latch %04X   count %04X\n", sysvia.t2l / 2, (sysvia.t2c / 2) & 0xFFFF);
                        debug_outf("    IER %02X IFR %02X\n", sysvia.ier, sysvia.ifr);
                    }
                    else if (!strncasecmp(iptr, "uservia", arglen)) {
                        debug_outf("    User VIA registers :\n");
                        debug_outf("    ORA  %02X ORB  %02X IRA %02X IRB %02X\n", uservia.ora, uservia.orb, uservia.ira, uservia.irb);
                        debug_outf("    DDRA %02X DDRB %02X ACR %02X PCR %02X\n", uservia.ddra, uservia.ddrb, uservia.acr, uservia.pcr);
                        debug_outf("    Timer 1 latch %04X   count %04X\n", uservia.t1l / 2, (uservia.t1c / 2) & 0xFFFF);
                        debug_outf("    Timer 2 latch %04X   count %04X\n", uservia.t2l / 2, (uservia.t2c / 2) & 0xFFFF);
                        debug_outf("    IER %02X IFR %02X\n", uservia.ier, uservia.ifr);
                    }
                    else if (!strncasecmp(iptr, "crtc", arglen)) {
                        debug_outf("    CRTC registers :\n");
                        debug_outf("    Index=%i\n", crtc_i);
                        debug_outf("    R0 =%02X  R1 =%02X  R2 =%02X  R3 =%02X  R4 =%02X  R5 =%02X  R6 =%02X  R7 =%02X  R8 =%02X\n", crtc[0], crtc[1], crtc[2], crtc[3], crtc[4], crtc[5], crtc[6], crtc[7], crtc[8]);
                        debug_outf("    R9 =%02X  R10=%02X  R11=%02X  R12=%02X  R13=%02X  R14=%02X  R15=%02X  R16=%02X  R17=%02X\n", crtc[9], crtc[10], crtc[11], crtc[12], crtc[13], crtc[14], crtc[15], crtc[16], crtc[17]);
                        debug_outf("    VC=%i SC=%i HC=%i MA=%04X\n", vc, sc, hc, ma);
                    }
                    else if (!strncasecmp(iptr, "vidproc", arglen)) {
                        debug_outf("    VIDPROC registers :\n");
                        debug_outf("    Control=%02X\n", ula_ctrl);
                        debug_outf("    Palette entries :\n");
                        debug_outf("     0=%01X   1=%01X   2=%01X   3=%01X   4=%01X   5=%01X   6=%01X   7=%01X\n", ula_palbak[0], ula_palbak[1], ula_palbak[2], ula_palbak[3], ula_palbak[4], ula_palbak[5], ula_palbak[6], ula_palbak[7]);
                        debug_outf("     8=%01X   9=%01X  10=%01X  11=%01X  12=%01X  13=%01X  14=%01X  15=%01X\n", ula_palbak[8], ula_palbak[9], ula_palbak[10], ula_palbak[11], ula_palbak[12], ula_palbak[13], ula_palbak[14], ula_palbak[15]);
                        debug_outf("    NULA palette :\n");
                        debug_outf("     0=%06X   1=%06X   2=%06X   3=%06X   4=%06X   5=%06X   6=%06X   7=%06X\n", nula_collook[0], nula_collook[1], nula_collook[2], nula_collook[3], nula_collook[4], nula_collook[5], nula_collook[6], nula_collook[7]);
                        debug_outf("     8=%06X   9=%06X  10=%06X  11=%06X  12=%06X  13=%06X  14=%06X  15=%06X\n", nula_collook[8], nula_collook[9], nula_collook[10], nula_collook[11], nula_collook[12], nula_collook[13], nula_collook[14], nula_collook[15]);
                        debug_outf("    NULA flash :\n");
                        debug_outf("     %01X%01X%01X%01X%01X%01X%01X%01X\n", nula_flash[0], nula_flash[1], nula_flash[2], nula_flash[3], nula_flash[4], nula_flash[5], nula_flash[6], nula_flash[7]);
                        debug_outf("    NULA registers :\n");
                        debug_outf("     Palette Mode=%01X  Horizontal Offset=%01X  Left Blank Size=%01X  Disable=%01X  Attribute Mode=%01X  Attribute Text=%01X\n", nula_palette_mode, nula_horizontal_offset, nula_left_blank, nula_disable, nula_attribute_mode, nula_attribute_text);
                    }
                    else if (!strncasecmp(iptr, "sound", arglen)) {
                        debug_outf("    Sound registers :\n");
                        debug_outf("    Voice 0 frequency = %04X   volume = %i  control = %02X\n", sn_latch[0] >> 6, sn_vol[0], sn_noise);
                        debug_outf("    Voice 1 frequency = %04X   volume = %i\n", sn_latch[1] >> 6, sn_vol[1]);
                        debug_outf("    Voice 2 frequency = %04X   volume = %i\n", sn_latch[2] >> 6, sn_vol[2]);
                        debug_outf("    Voice 3 frequency = %04X   volume = %i\n", sn_latch[3] >> 6, sn_vol[3]);
                    }
                    else if (!strncasecmp(iptr, "ram", arglen))
                       debug_outf("    System RAM registers :\n    ROMSEL=%02X ACCCON=%02X(%s%s%s%s %c%c%c%c)\n    ram1k=%02X ram4k=%02X ram8k=%02X vidbank=%02X\n", ram_fe30, ram_fe34, (ram_fe34 & 0x80) ? "IRR" : "---", (ram_fe34 & 0x40) ? "TST" : "---", (ram_fe34 & 0x20) ? "IFJ" : "---", (ram_fe34 & 0x10) ? "ITU" : "---", (ram_fe34 & 0x08) ? 'Y' : '-', (ram_fe34 & 0x04) ? 'X' : '-', (ram_fe34 & 0x02) ? 'E' : '-', (ram_fe34 & 0x01) ? 'D' : '-', ram1k, ram4k, ram8k, vidbank);
                    else
                        debug_outf("Register set %s not known\n", iptr);
                } else {
                    debug_outf("    registers for %s\n", cpu->cpu_name);
                    print_registers(cpu);
                }
                break;

            case 's':
                if (cmdlen > 1) {
                    if (!strncmp(cmd, "swiftsym", cmdlen)) {
                        if (iptr)
                            swiftsym(cpu, iptr);
                        else
                            debug_outf("Missing filename\n");
                    }
                    else if (!strncmp(cmd, "symbol", cmdlen)) {
                        if (*iptr)
                            set_sym(cpu, iptr);
                        else
                            debug_outf("Missing parameters\n");
                    }
                    else if (!strncmp(cmd, "symlist", cmdlen))
                        list_syms(cpu, iptr);
                    else if (!strncmp(cmd, "save", cmdlen)) {
                        if (*iptr)
                            debugger_save(iptr);
                        else
                            debug_outf("Missing filename\n");
                    }
                    else
                        badcmd = true;
                    break;
                }
                else {
                    if (*iptr)
                        sscanf(iptr, "%i", &debug_step);
                    else
                        debug_step = 1;
                    debug_lastcommand = 's';
                    indebug = 0;
                    main_resume();
                    return;
                }
            case 't':
                if (!strncmp(cmd, "trace", cmdlen)) {
                    if (trace_fp) {
                        fclose(trace_fp);
                        trace_fp = NULL;
                    }
                    if (*iptr) {
                        char *eptr = strchr(iptr, '\n');
                        if (eptr)
                            *eptr = '\0';
                        if ((trace_fp = fopen(iptr, "a")))
                            debug_outf("Tracing to %s\n", iptr);
                        else
                            debug_outf("Unable to open trace file '%s' for append: %s\n", iptr, strerror(errno));
                    } else
                        debug_outf("Trace file closed");
                }
                else
                    badcmd = true;
                break;

            case 'v':
                if (!strncmp(cmd, "vrefresh", cmdlen)) {
                    if (*iptr) {
                        if (!strncasecmp(iptr, "on", 2)) {
                            debug_outf("Extra video refresh enabled\n");
                            vrefresh = 1;
                            video_poll(CLOCKS_PER_FRAME, 0);
                        } else if (!strncasecmp(iptr, "off", 3)) {
                            debug_outf("Extra video refresh disabled\n");
                            vrefresh = 0;
                        }
                    }
                }
                else
                    badcmd = true;
                break;

            case 'w':
                if (!strncmp(cmd, "watchr", cmdlen))
                    set_point(cpu, watchr, iptr, "Read watchpoint");
                else if (!strncmp(cmd, "watchw", cmdlen))
                    set_point(cpu, watchw, iptr, "Write watchpoint");
                else if (!strncmp(cmd, "watchi", cmdlen))
                    set_point(cpu, watchi, iptr, "Input watchpoint");
                else if (!strncmp(cmd, "watcho", cmdlen))
                    set_point(cpu, watcho, iptr, "Output watchpoint");
                else if (!strncmp(cmd, "wlist", cmdlen)) {
                    list_points(cpu, watchr, "Read watchpoint");
                    list_points(cpu, watchw, "Write watchpoint");
                    list_points(cpu, watchi, "Input watchpoint");
                    list_points(cpu, watcho, "Output watchpoint");
                }
                else if (!strncmp(cmd, "wclearr", cmdlen))
                    clear_point(cpu, watchr, iptr, "Read watchpoint");
                else if (!strncmp(cmd, "wclearw", cmdlen))
                    clear_point(cpu, watchw, iptr, "Write watchpoint");
                else if (!strncmp(cmd, "wcleari", cmdlen))
                    clear_point(cpu, watchi, iptr, "Input watchpoint");
                else if (!strncmp(cmd, "wclearo", cmdlen))
                    clear_point(cpu, watcho, iptr, "Output watchpoint");
                else if (!strncmp(cmd, "writem", cmdlen)) {
                    if (*iptr) {
                        unsigned addr, value;
                        sscanf(iptr, "%X %X", &addr, &value);
                        log_debug("debugger: writem %04X %04X\n", addr, value);
                        cpu->memwrite(addr, value);
                        if (cpu == &core6502_cpu_debug && vrefresh)
                            video_poll(CLOCKS_PER_FRAME, 0);
                    }
                }
                else
                    badcmd = true;
                break;
            default:
                badcmd = true;
        }
        if (badcmd)
            debug_out("Bad command\n", 12);

    }
}

static inline void check_points(cpu_debug_t *cpu, uint32_t addr, uint32_t value, uint8_t size, int *break_tab, int *watch_tab, const char *desc)
{
    for (int c = 0; c < NUM_BREAKPOINTS; c++) {
        if (break_tab[c] == addr) {
            char addr_str[20 + SYM_MAX], iaddr_str[20 + SYM_MAX];
            uint32_t iaddr = cpu->get_instr_addr();
            cpu->print_addr(cpu, addr, addr_str, sizeof(addr_str), true);
            cpu->print_addr(cpu, iaddr, iaddr_str, sizeof(iaddr_str), true);
            debug_outf("cpu %s: %s: break on %s %s, value=%X\n", cpu->cpu_name, iaddr_str, desc, addr_str, value);
            debugger_do(cpu, iaddr);
        }
        if (watch_tab[c] == addr) {
            char addr_str[10], iaddr_str[10];
            uint32_t iaddr = cpu->get_instr_addr();
            cpu->print_addr(cpu, addr, addr_str, sizeof(addr_str), true);
            cpu->print_addr(cpu, iaddr, iaddr_str, sizeof(iaddr_str), true);
            debug_outf("cpu %s: %s: %s %s, value=%0*X\n", cpu->cpu_name, iaddr_str, desc, addr_str, size*2, value);
        }
    }
}

void debug_memread (cpu_debug_t *cpu, uint32_t addr, uint32_t value, uint8_t size) {
    check_points(cpu, addr, value, size, breakr, watchr, "read from");
}

void debug_memwrite(cpu_debug_t *cpu, uint32_t addr, uint32_t value, uint8_t size) {
    check_points(cpu, addr, value, size, breakw, watchw, "write to");
}

void debug_ioread (cpu_debug_t *cpu, uint32_t addr, uint32_t value, uint8_t size) {
    check_points(cpu, addr, value, size, breaki, watchi, "input from");
}

void debug_iowrite(cpu_debug_t *cpu, uint32_t addr, uint32_t value, uint8_t size) {
    check_points(cpu, addr, value, size, breako, watcho, "output to");
}

void debug_preexec (cpu_debug_t *cpu, uint32_t addr) {
    char buf[256];
    size_t len;
    int c, r, enter = 0;
    const char **np, *name;

    if ((addr & 0xF000) == 0x8000)
        addr = addr;

    if (trace_fp) {
        const char *symlbl;
        if (symbol_find_by_addr(cpu->symbols, addr, &symlbl)) {
            fputs(symlbl, trace_fp);
            fputs(":\n", trace_fp);
        }
        cpu->disassemble(cpu, addr, buf, sizeof buf);

        char *sym = strchr(buf, '\\');
        if (sym) {
            *(sym++) = '\0';
        }

        fputs("\t", trace_fp);
        fputs(buf, trace_fp);
        *buf = ' ';

        for (r = 0, np = cpu->reg_names; (name = *np++);) {
            len = cpu->reg_print(r++, buf + 1, sizeof buf - 1);
            fwrite(buf, len + 1, 1, trace_fp);
        }

        if (sym)
        {
            fputs(" \\", trace_fp);
            fputs(sym, trace_fp);
        }

        putc('\n', trace_fp);
    }

    if (addr == tbreak) {
        log_debug("debugger; enter for CPU %s on tbreak at %04X", cpu->cpu_name, addr);
        enter = 1;
    }
    else {
        for (c = 0; c < NUM_BREAKPOINTS; c++) {
            if (breakpoints[c] == addr) {
                char addr_str[16+SYM_MAX];
                cpu->print_addr(cpu, addr, addr_str, sizeof(addr_str), true);
                debug_outf("cpu %s: Break at %s\n", cpu->cpu_name, addr_str);
                if (contcount) {
                    contcount--;
                    return;
                }
                log_debug("debugger; enter for CPU %s on breakpoint at %s", cpu->cpu_name, addr_str);
                enter = 1;
            }
        }
        if (debug_step) {
            debug_step--;
            if (debug_step)
                return;
            log_debug("debugger; enter for CPU %s on single-step at %04X", cpu->cpu_name, addr);
            enter = 1;
        }
    }
    if (enter) {
        tbreak = -1;
        debugger_do(cpu, addr);
    }
}

void debug_trap(cpu_debug_t *cpu, uint32_t addr, int reason)
{
    const char *desc = cpu->trap_names[reason];
    char addr_str[20 + SYM_MAX];
    cpu->print_addr(cpu, addr, addr_str, sizeof(addr_str), true);
    debug_outf("cpu %s: %s at %s\n", cpu->cpu_name, desc, addr_str);
    debugger_do(cpu, addr);
}
