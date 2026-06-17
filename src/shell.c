/*
 * shell.c — an interactive command interpreter.
 *
 * This is the moment the project becomes an *operating system you can
 * talk to*: a loop that reads a line from the serial port, parses a
 * command name, and dispatches to a handler. With the RAM filesystem
 * underneath it, it's also our file manager: the shell keeps a
 * "current working directory" pointer (that's all a cwd is!) and the
 * file commands resolve paths relative to it.
 *
 * It also hosts the power commands. Interesting fact: a kernel cannot
 * turn off the machine by itself — power is firmware's job. ARM systems
 * expose it via PSCI (Power State Coordination Interface): you put a
 * function ID in x0 and execute `hvc #0` (hypervisor call), which traps
 * UP one privilege level to QEMU itself, which then acts on it. The
 * same mechanism (a controlled jump to a more-privileged layer) is
 * exactly how user programs will one day call into our kernel.
 */

#include <stdint.h>
#include "shell.h"
#include "uart.h"
#include "kprintf.h"
#include "timer.h"
#include "mem.h"
#include "task.h"
#include "proc.h"
#include "file.h"
#include "pipe.h"
#include "fs.h"
#include "virtio_blk.h"
#include "mfs.h"
#include "dtb.h"
#include "rtc.h"
#include "gui.h"
#include "fb.h"
#include "virtio_input.h"
#include "net.h"
#include "dns.h"
#include "tcp.h"
#include "http.h"
#include "browser.h"
#include "fonts.h"
#include "fb.h"
#include "virtio_snd.h"
#include "smp.h"
#include "power.h"
#include "lib.h"


/* The shell's current directory — `cd` just repoints this. */
static struct fs_node *cwd;

/* ------------------------------------------------------------------ */
/* File commands                                                       */
/* ------------------------------------------------------------------ */

/* ls, cat, and echo are no longer builtins — they're real ELF
 * programs in /bin now, launched by the shell like any other. The
 * shell keeps only builtins that must run IN the shell (cd changes
 * the shell's own cwd) or that mutate the in-RAM fs tree directly. */

static void tree_walk(struct fs_node *n, int depth)
{
    for (int i = 0; i < depth; i++)
        uart_puts("  ");
    if (!n->parent)
        kprintf("/\n");
    else
        kprintf("%s%s\n", n->name, n->type == FS_DIR ? "/" : "");
    if (n->type == FS_DIR)
        for (struct fs_node *c = n->children; c; c = c->next)
            tree_walk(c, depth + 1);
}

static void cmd_tree(char *arg)
{
    struct fs_node *n = *arg ? fs_resolve(cwd, arg) : cwd;
    if (!n)
        kprintf("tree: no such path '%s'\n", arg);
    else
        tree_walk(n, 0);
}

static void cmd_cd(char *arg)
{
    struct fs_node *n = *arg ? fs_resolve(cwd, arg) : fs_root;
    if (!n)
        kprintf("cd: no such path '%s'\n", arg);
    else if (n->type != FS_DIR)
        kprintf("cd: '%s' is a file\n", arg);
    else
        cwd = n;
}

static void cmd_pwd(char *arg)
{
    (void)arg;
    fs_print_path(cwd);
    kprintf("\n");
}

static void cmd_mkdir(char *arg)
{
    char leaf[FS_NAME_MAX];
    struct fs_node *dir;

    if (!*arg) {
        kprintf("usage: mkdir <path>\n");
        return;
    }
    dir = fs_resolve_parent(cwd, arg, leaf);
    if (!dir || !fs_create(dir, leaf, FS_DIR))
        kprintf("mkdir: cannot create '%s' (bad path or name taken?)\n", arg);
}

static void cmd_write(char *arg)
{
    /* Split "write <path> <text...>" at the first space; everything
     * after it (spaces included) becomes the file's content. */
    char *text = arg;
    while (*text && *text != ' ')
        text++;
    if (*text == ' ')
        *text++ = '\0';

    if (!*arg) {
        kprintf("usage: write <path> <text>\n");
        return;
    }

    struct fs_node *f = fs_resolve(cwd, arg);
    if (!f) {                            /* doesn't exist yet: create it */
        char leaf[FS_NAME_MAX];
        struct fs_node *dir = fs_resolve_parent(cwd, arg, leaf);
        if (dir)
            f = fs_create(dir, leaf, FS_FILE);
    }
    if (!f || f->type != FS_FILE) {
        kprintf("write: cannot write to '%s'\n", arg);
        return;
    }

    char content[130];                   /* line is 128 max, +\n +NUL */
    uint64_t len = strlen(text);
    memcpy(content, text, len);
    if (len)
        content[len++] = '\n';
    if (fs_set_content(f, content, len) < 0)
        kprintf("write: out of memory\n");
}

static void cmd_rm(char *arg)
{
    struct fs_node *n = *arg ? fs_resolve(cwd, arg) : 0;
    if (!n) {
        kprintf("rm: no such path '%s'\n", arg);
        return;
    }
    /* Removing the directory we're standing in would leave cwd
     * dangling — a use-after-free. Real shells have this rule too. */
    for (struct fs_node *p = cwd; p; p = p->parent) {
        if (p == n) {
            kprintf("rm: cannot remove '%s': it contains your cwd\n", arg);
            return;
        }
    }
    switch (fs_unlink(n)) {
    case 0:                                                       break;
    case FS_ERR_ROOT:      kprintf("rm: cannot remove /\n");      break;
    case FS_ERR_NOT_EMPTY: kprintf("rm: '%s' not empty\n", arg);  break;
    }
}

/* ------------------------------------------------------------------ */
/* Disk commands                                                       */
/* ------------------------------------------------------------------ */

static void cmd_sync(char *arg)
{
    (void)arg;
    /* This command mattered in MicroFS v1, which only persisted when
     * you asked. v2 writes through on every operation — kept as a
     * little museum piece. */
    if (mfs_mounted())
        kprintf("nothing to do: MicroFS v2 is write-through — every "
                "change is already on disk\n");
    else
        kprintf("sync: no filesystem mounted (RAM-only mode)\n");
}

/* First run: create a 40 KB patterned file (79 blocks — way past the
 * 24 direct pointers, so the inode must grow an indirect block).
 * After a reboot: verify every byte survived the round trip. */
static void cmd_bigtest(char *arg)
{
    (void)arg;
    struct fs_node *f = fs_resolve(cwd, "/big.dat");
    if (f) {
        uint64_t bad = 0;
        for (uint64_t i = 0; i < f->size; i++)
            if (f->data[i] != (char)('A' + (i % 26)))
                bad++;
        kprintf("big.dat: %lu bytes read back, %lu wrong — %s\n",
                f->size, bad,
                bad ? "CORRUPT!" : "indirect blocks verified");
        return;
    }

    char *buf = kmalloc(40000);
    if (!buf) {
        kprintf("bigtest: out of memory\n");
        return;
    }
    for (uint64_t i = 0; i < 40000; i++)
        buf[i] = (char)('A' + (i % 26));
    f = fs_create(fs_root, "big.dat", FS_FILE);
    if (!f || fs_set_content(f, buf, 40000) < 0)
        kprintf("bigtest: write failed\n");
    else
        kprintf("created big.dat: 40000 bytes across 79 blocks (indirect!)\n"
                "reboot and run bigtest again to verify it from disk\n");
    kfree(buf);
}

static void cmd_crashfs(char *arg)
{
    (void)arg;
    kprintf("Staging a change to welcome.txt, committing it to the\n");
    kprintf("JOURNAL, then 'crashing' before the real write...\n");
    if (mfs_crashtest() < 0) {
        kprintf("crashfs: failed (no filesystem?)\n");
        return;
    }
    kprintf("Rebooting mid-operation. Watch the boot log for the replay,\n");
    kprintf("then: cat welcome.txt\n");
    power_reset();
}

static void cmd_disk(char *arg)
{
    (void)arg;
    if (!vblk_present()) {
        kprintf("no disk attached (run via 'make run' to get one)\n");
        return;
    }
    kprintf("virtio-blk disk at MMIO %p, IRQ %u\n",
            (void *)vblk_mmio_base(), vblk_irq());
    kprintf("  capacity: %lu sectors = %lu KiB (disk.img on your Mac)\n",
            vblk_capacity(), vblk_capacity() / 2);
    mfs_stat();
}

/* ------------------------------------------------------------------ */
/* System commands                                                     */
/* ------------------------------------------------------------------ */

static void cmd_help(char *arg);

static void cmd_about(char *arg)
{
    (void)arg;
    uint64_t current_el;
    asm volatile("mrs %0, CurrentEL" : "=r"(current_el));

    kprintf("MicroOS 1.1 — a small but complete educational OS for ARM64\n");
    kprintf("Running at EL%lu, MMU on, W^X, per-process address spaces\n",
            (current_el >> 2) & 3);
    kprintf("Console: PL011 UART, IRQ-driven, tasks block on wait channels\n");
    kprintf("Tasks:   preemptive scheduler; fork/exec/wait, real /bin programs\n");
    kprintf("Shell:   launches ELF programs by name; pipes (cat x | wc -l)\n");
    if (rtc_present()) {
        struct datetime dt;
        rtc_datetime(&dt);
        kprintf("Clock:   PL031 RTC — %d-%02d-%02d %02d:%02d:%02d\n",
                dt.year, dt.month, dt.day, dt.hour, dt.min, dt.sec);
    }
    kprintf("Files:   MicroFS v3 — journaled, block-cached, write-through\n");
    kprintf("Network: DHCP-configured; ARP/ICMP/UDP/DNS/TCP — try wget\n");
    kprintf("Cores:   %s\n", smp_core1_online()
            ? "2 online (core 1 counting in parallel — see 'cores')"
            : "1 online");
    if (gui_active())
        kprintf("Display: %dx%d ramfb desktop, double-buffered, ~33 fps\n",
                FB_WIDTH, FB_HEIGHT);
    else
        kprintf("Display: none (start with -device ramfb)\n");
}

static void cmd_date(char *arg)
{
    (void)arg;
    if (!rtc_present()) {
        kprintf("no real-time clock found\n");
        return;
    }
    static const char *wd[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
    static const char *mo[] = { "", "January", "February", "March", "April",
                                "May", "June", "July", "August", "September",
                                "October", "November", "December" };
    struct datetime dt;
    rtc_datetime(&dt);
    kprintf("%s, %s %d, %d  %02d:%02d:%02d\n",
            wd[dt.wday], mo[dt.month], dt.day, dt.year,
            dt.hour, dt.min, dt.sec);
    kprintf("(%lu seconds since the Unix epoch, from the PL031 RTC)\n",
            rtc_epoch());
}

static void cmd_uptime(char *arg)
{
    (void)arg;
    uint64_t ms = timer_uptime_ms();
    kprintf("up %lu.%lu%lu s (%lu timer ticks)\n",
            ms / 1000, (ms / 100) % 10, (ms / 10) % 10, timer_ticks());
}

static uint64_t parse_dec(const char *p)
{
    uint64_t v = 0;
    while (*p >= '0' && *p <= '9')
        v = v * 10 + (uint64_t)(*p++ - '0');
    return v;
}

static void cmd_sleep(char *arg)
{
    uint64_t secs = parse_dec(arg);
    if (!secs) {
        kprintf("usage: sleep <seconds>\n");
        return;
    }
    /* A REAL sleep now: the shell task leaves the run queue entirely
     * and other tasks get the CPU (run `spawn`, then `sleep 3`, then
     * `ps` — the spawned counters kept working while we slept).
     * Typing during the nap still isn't lost: the UART interrupt
     * buffers it, and the shell replays it afterward. */
    task_sleep(secs * TIMER_HZ);
}

/* ------------------------------------------------------------------ */
/* Task commands                                                       */
/* ------------------------------------------------------------------ */

/* What spawned demo tasks run: tick a counter the user can watch in
 * `ps`, forever. Lives entirely in its own task with its own stack. */
static void counter_task(uint64_t arg)
{
    (void)arg;
    for (;;) {
        current->scratch++;
        task_sleep(TIMER_HZ / 10);      /* +1 every 100 ms */
    }
}

static void cmd_spawn(char *arg)
{
    (void)arg;
    int id = task_create("counter", counter_task, 0);
    if (id < 0)
        kprintf("spawn: task table is full\n");
    else
        kprintf("spawned task %d — watch its SCRATCH column in 'ps'\n", id);
}

static void cmd_ps(char *arg)
{
    (void)arg;
    task_dump();
}

static void cmd_gui(char *arg)
{
    if (strcmp(arg, "mouse") == 0) {    /* ring-state dump for debugging */
        vinput_debug();
        return;
    }
    if (!gui_active()) {
        kprintf("no display — start QEMU with -device ramfb (make run)\n");
        return;
    }
    kprintf("display:  %dx%d, XRGB8888, double-buffered\n",
            FB_WIDTH, FB_HEIGHT);
    kprintf("painter:  kernel task 'gui', %lu frames painted; repaints on\n",
            gui_frames());
    kprintf("          change (mouse/console/stats), up to ~30 fps dragging\n");
    kprintf("pipeline: fw_cfg DMA -> ramfb scanout of kernel RAM\n");
    kprintf("this very text is in the Console window too — it mirrors\n");
    kprintf("every byte that crosses the serial port.\n");
}

static void cmd_net(char *arg)
{
    (void)arg;
    net_info();
}

static void cmd_ping(char *arg)
{
    uint64_t n = parse_dec(arg);
    net_ping(n ? (int)n : 4);
}

static void cmd_nslookup(char *arg)
{
    uint8_t ip[4];
    if (!*arg) {
        kprintf("usage: nslookup <hostname>\n");
        return;
    }
    if (dns_resolve(arg, ip) < 0)
        kprintf("nslookup: cannot resolve '%s'\n", arg);
    else
        kprintf("%s is %d.%d.%d.%d\n", arg, ip[0], ip[1], ip[2], ip[3]);
}

/* wget <host>[/path] — HTTP/1.0 GET over our own TCP, saved to our
 * own filesystem. The whole stack, vertically: shell -> HTTP -> TCP
 * -> IP -> Ethernet -> virtio -> QEMU -> your Mac -> the internet. */
static void cmd_wget(char *arg)
{
    if (!*arg) {
        kprintf("usage: wget <host>[/path]   e.g. wget example.com\n");
        return;
    }
    kprintf("fetching http://%s ...\n", arg);

    char *body = kmalloc(MFS_MAX_FILE);
    if (!body) {
        kprintf("out of memory\n");
        return;
    }
    int n = http_get(arg, body, MFS_MAX_FILE);
    if (n < 0) {
        kprintf("wget: fetch failed (host unreachable, or HTTPS-only?)\n");
        kfree(body);
        return;
    }

    struct fs_node *f = fs_resolve(fs_root, "/download.html");
    if (!f)
        f = fs_create(fs_root, "download.html", FS_FILE);
    if (f && fs_set_content(f, body, (uint64_t)n) == 0)
        kprintf("received %d bytes -> /download.html (try: cat download.html, "
                "or: browse %s)\n", n, arg);
    else
        kprintf("received %d bytes (couldn't save to disk)\n", n);
    kfree(body);
}

static void cmd_browse(char *arg)
{
    if (!*arg) {
        kprintf("usage: browse <url>   e.g. browse example.com\n");
        return;
    }
    browser_navigate(arg);
    kprintf("loading %s — see the Browser window (its dock icon)\n", arg);
}

/* https <url> — fetch over our from-scratch TLS 1.3 stack and report
 * the verified certificate. The whole crypto tower runs here: X25519,
 * ChaCha20-Poly1305, SHA-256, RSA/ECDSA cert validation. */
static void cmd_https(char *arg)
{
    if (!*arg) {
        kprintf("usage: https <host>[/path]   e.g. https community.letsencrypt.org\n");
        return;
    }
    char url[256];
    if (strncmp(arg, "https://", 8) == 0)
        ksprintf(url, sizeof url, "%s", arg);
    else
        ksprintf(url, sizeof url, "https://%s", arg);

    kprintf("TLS 1.3 handshake with %s (this does real crypto - a moment)...\n", arg);
    char *body = kmalloc(MFS_MAX_FILE);
    if (!body) { kprintf("out of memory\n"); return; }

    struct tls_info info;
    int n = http_get_info(url, body, MFS_MAX_FILE, &info);
    kprintf("  cipher:   %s\n", info.cipher ? info.cipher : "?");
    kprintf("  keyexch:  %s\n", info.group ? info.group : "?");
    if (info.sigscheme)
        kprintf("  cert sig: %s\n", info.sigscheme);
    if (n < 0) {
        kprintf("  RESULT:   FAILED - %s\n",
                info.err[0] ? info.err : "connection failed");
        kfree(body);
        return;
    }
    kprintf("  subject:  %s\n", info.subject);
    kprintf("  issuer:   %s\n", info.issuer);
    kprintf("  VERIFIED: chain trusted to root '%s'\n", info.anchor);

    struct fs_node *f = fs_resolve(fs_root, "/secure.html");
    if (!f) f = fs_create(fs_root, "secure.html", FS_FILE);
    if (f && fs_set_content(f, body, (uint64_t)n) == 0)
        kprintf("  received %d bytes over TLS -> /secure.html\n", n);
    else
        kprintf("  received %d bytes over TLS\n", n);
    kfree(body);
}

/* ------------------------------------------------------------------ */
/* Font commands — the system typeface is a swappable pointer.         */
/* ------------------------------------------------------------------ */

/* "fonts" lists the installed faces; "fonts <n>" makes one active.
 * Switching is instantaneous: fonts_set() repoints fb.c's renderer, so
 * the very next desktop repaint (and every kprintf after) uses it. */
static void cmd_fonts(char *arg)
{
    if (*arg) {
        int n = (int)parse_dec(arg);
        if (n < 0 || n >= fonts_count()) {
            kprintf("fonts: no font #%d (have 0..%d)\n", n, fonts_count() - 1);
            return;
        }
        fonts_set(n);
        kprintf("system font is now '%s' (#%d) — watch the desktop redraw\n",
                fonts_name(n), n);
        return;
    }
    kprintf("installed system fonts:\n");
    for (int i = 0; i < fonts_count(); i++)
        kprintf("  %s %d  %s\n", i == fonts_active() ? "*" : " ",
                i, fonts_name(i));
    kprintf("use 'fonts <n>' to switch (or click one in the Fonts window)\n");
}

/* "fontget <url>" downloads a .mf8 bitmap font over plain HTTP and
 * installs it. Real Google Fonts are HTTPS-only vector TTFs, which
 * would need TLS + a TrueType rasterizer we don't have — so we serve
 * honest little bitmaps in the same 8x8 format as the built-ins. */
static void cmd_fontget(char *arg)
{
    if (!*arg) {
        kprintf("usage: fontget <host>[/path]   (a .mf8 bitmap font)\n");
        kprintf("note: Google Fonts can't be used (HTTPS + vector TTF);\n");
        kprintf("      fontget fetches plain-HTTP .mf8 files instead.\n");
        return;
    }
    kprintf("fetching http://%s ...\n", arg);

    char *body = kmalloc(16384);
    if (!body) {
        kprintf("out of memory\n");
        return;
    }
    int n = http_get(arg, body, 16384);
    if (n < 0) {
        kprintf("fontget: fetch failed (unreachable, or HTTPS-only?)\n");
        kfree(body);
        return;
    }
    int idx = fonts_load_mf8("Web", body, n);
    if (idx < 0)
        kprintf("fontget: got %d bytes but it isn't a valid .mf8 font\n", n);
    else
        kprintf("installed font '%s' (#%d) — type 'fonts %d' to use it\n",
                fonts_name(idx), idx, idx);
    kfree(body);
}

/* "fontload <path>" installs a .mf8 already on our filesystem (e.g. one
 * saved by fontsave, or written with the editor). Closes the loop:
 * serialize -> store -> parse -> install, no network needed. */
static void cmd_fontload(char *arg)
{
    struct fs_node *f = *arg ? fs_resolve(cwd, arg) : 0;
    if (!f || f->type != FS_FILE) {
        kprintf("fontload: no such file '%s'\n", arg);
        return;
    }
    int idx = fonts_load_mf8("Loaded", f->data, (int)f->size);
    if (idx < 0)
        kprintf("fontload: '%s' isn't a valid .mf8 font\n", arg);
    else
        kprintf("installed font '%s' (#%d) — type 'fonts %d' to use it\n",
                fonts_name(idx), idx, idx);
}

/* "fontsave <path>" exports the active font as a .mf8 file you can cat,
 * edit, or fontget from another machine — shows the format is just text. */
static void cmd_fontsave(char *arg)
{
    if (!*arg) {
        kprintf("usage: fontsave <path>   (writes the active font as .mf8)\n");
        return;
    }
    char *buf = kmalloc(8192);
    if (!buf) {
        kprintf("out of memory\n");
        return;
    }
    int n = fonts_save_mf8(fonts_active(), buf, 8192);

    struct fs_node *f = fs_resolve(cwd, arg);
    if (!f) {
        char leaf[FS_NAME_MAX];
        struct fs_node *dir = fs_resolve_parent(cwd, arg, leaf);
        if (dir)
            f = fs_create(dir, leaf, FS_FILE);
    }
    if (f && f->type == FS_FILE && fs_set_content(f, buf, (uint64_t)n) == 0)
        kprintf("saved font '%s' to %s (%d bytes of .mf8 hex)\n",
                fonts_name(fonts_active()), arg, n);
    else
        kprintf("fontsave: cannot write '%s'\n", arg);
    kfree(buf);
}

static void cmd_beep(char *arg)
{
    /* "beep [hz] [ms]" — split two optional decimal args */
    char *ms_str = arg;
    while (*ms_str && *ms_str != ' ')
        ms_str++;
    if (*ms_str == ' ')
        *ms_str++ = '\0';

    uint64_t hz = parse_dec(arg);
    uint64_t ms = parse_dec(ms_str);
    if (!hz) hz = 440;                  /* concert A, naturally */
    if (!ms) ms = 400;

    if (!vsnd_present()) {
        kprintf("beep: no sound card (needs -device virtio-sound-device)\n");
        return;
    }
    kprintf("%lu Hz square wave for %lu ms...\n", hz, ms);
    vsnd_beep((uint32_t)hz, (uint32_t)ms);
}

/* ------------------------------------------------------------------ */
/* Launching programs — the shell's core job on any Unix.              */
/* ------------------------------------------------------------------ */

/* Chop a command string (in place) into an argv array. A bare name
 * like "ls" is rewritten to "/bin/ls" in pathbuf so the shell finds
 * programs there — the "PATH" of this tiny system, hard-coded to /bin. */
static int build_argv(char *s, char **argv, int max,
                      char *pathbuf, int pblen)
{
    int argc = 0;
    while (*s && argc < max - 1) {
        while (*s == ' ')
            *s++ = '\0';
        if (!*s)
            break;
        argv[argc++] = s;
        while (*s && *s != ' ')
            s++;
    }
    argv[argc] = 0;
    if (argc == 0)
        return 0;

    int has_slash = 0;
    for (char *p = argv[0]; *p; p++)
        if (*p == '/')
            has_slash = 1;
    if (!has_slash) {
        ksprintf(pathbuf, (unsigned long)pblen, "/bin/%s", argv[0]);
        argv[0] = pathbuf;
    }
    return argc;
}

/* Launch one command with the given stdin/stdout (NULL = console).
 * Returns the pid, or -1. proc_spawn_io consumes in/out either way. */
static int launch(char *cmd, struct file *in, struct file *out)
{
    char *argv[16];
    char pathbuf[64];
    if (build_argv(cmd, argv, 16, pathbuf, sizeof(pathbuf)) == 0) {
        file_close(in);
        file_close(out);
        return -1;
    }
    return proc_spawn_io(argv[0], argv, in, out);
}

/* Run a command in the FOREGROUND: launch it and wait for it to
 * finish, exactly like a shell running `ls`. */
static void run_foreground(char *cmd)
{
    int pid = launch(cmd, 0, 0);
    if (pid < 0) {
        kprintf("%s: command not found\n", cmd);
        return;
    }
    task_waitpid(pid, 0);
}

/* `exec <prog> [args]` — kept as a friendly alias, though you can just
 * type the program name (or path) directly, like any shell. */
static void cmd_exec(char *arg)
{
    if (!*arg)
        kprintf("usage: exec <program> [args]  (or just type its name)\n");
    else
        run_foreground(arg);
}

/* Run `left | right`: a pipe joins left's stdout to right's stdin, the
 * two run concurrently, and the shell waits for both. THE canonical
 * Unix construction — and now MicroOS does it for real. */
static void run_pipeline(char *left, char *right)
{
    struct file *rf, *wf;
    if (pipe_new(&rf, &wf) < 0) {
        kprintf("pipe: out of memory\n");
        return;
    }
    int p1 = launch(left, 0, wf);       /* left writes into the pipe   */
    int p2 = launch(right, rf, 0);      /* right reads from the pipe    */
    if (p1 >= 0)
        task_waitpid(p1, 0);
    if (p2 >= 0)
        task_waitpid(p2, 0);
    if (p1 < 0 || p2 < 0)
        kprintf("pipeline: a command was not found\n");
}

static void cmd_cores(char *arg)
{
    (void)arg;
    uint64_t mpidr;
    asm volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
    kprintf("core 0: mpidr %lu — running the kernel, the shell, "
            "everything you see\n", mpidr & 0xFF);
    if (!smp_core1_online()) {
        kprintf("core 1: offline (QEMU needs -smp 2)\n");
        return;
    }
    uint64_t a = smp_core1_count();
    task_sleep(TIMER_HZ / 4);
    uint64_t b = smp_core1_count();
    kprintf("core 1: ONLINE — counter %lu (+%lu in the last 250 ms, "
            "counted in parallel)\n", b, b - a);
}

static void cmd_kill(char *arg)
{
    int id = (int)parse_dec(arg);
    if (!*arg || task_kill(id) < 0)
        kprintf("kill: can't kill '%s' (bad id, the shell, or yourself)\n", arg);
    else
        kprintf("task %d terminated\n", id);
}

static void cmd_mem(char *arg)
{
    (void)arg;
    mem_print_stats();
}

static void cmd_alloc(char *arg)
{
    uint64_t size = 64;
    if (*arg && parse_hex(arg, &size) < 0) {
        kprintf("usage: alloc <hex-bytes>   (e.g. 'alloc 100' = 256 bytes)\n");
        return;
    }
    void *p = kmalloc(size);
    if (p)
        kprintf("allocated %lu bytes at %p\n", size, p);
    else
        kprintf("out of heap!\n");
}

static void cmd_memtest(char *arg)
{
    (void)arg;
    kprintf("free blocks before: %lu\n", mem_free_block_count());
    kprintf("allocating three adjacent 2 KiB blocks: a, b, c\n");
    void *a = kmalloc(2048), *b = kmalloc(2048), *c = kmalloc(2048);
    kprintf("  a=%p  b=%p  c=%p\n", a, b, c);

    kfree(a);
    kfree(c);
    kprintf("freed a and c (not touching):  free blocks = %lu  (+2)\n",
            mem_free_block_count());

    kfree(b);
    kprintf("freed b (touches both):        free blocks = %lu  (3 merged into 1!)\n",
            mem_free_block_count());
}

static void cmd_peek(char *arg)
{
    uint64_t addr;
    if (parse_hex(arg, &addr) < 0) {
        kprintf("usage: peek <hex-address>   (e.g. 'peek 40080000')\n");
        return;
    }
    /* No MMU means no protection: we can read any mapped address.
     * (Peeking an address where no device/RAM lives will fault —
     * try it, then read the crash report!) */
    uint64_t value = *(volatile uint64_t *)addr;
    kprintf("[0x%lx] = 0x%lx\n", addr, value);
}

static void cmd_dtb(char *arg);     /* defined below, needs dtb_addr */

static void cmd_clear(char *arg)
{
    (void)arg;
    /* ANSI escape codes: clear screen, cursor to top-left. */
    uart_puts("\x1b[2J\x1b[H");
}

/* Four ways to crash, one per MMU protection. Each raises a
 * synchronous exception: watch vectors.S route it to handle_sync(),
 * which prints a crash report naming the exact cause and address. */
static void cmd_crash(char *arg)
{
    if (strcmp(arg, "null") == 0) {
        kprintf("Dereferencing a NULL pointer (page 0 is unmapped)...\n");
        kprintf("got: %d\n", *(volatile int *)0);
    } else if (strcmp(arg, "wro") == 0) {
        kprintf("Writing to kernel CODE (mapped read-only)...\n");
        *(volatile uint64_t *)cmd_crash = 0;
    } else if (strcmp(arg, "exec") == 0) {
        kprintf("Jumping to the HEAP (mapped execute-never)...\n");
        void *p = kmalloc(16);
        ((void (*)(void))p)();
    } else if (*arg == '\0') {
        kprintf("Executing a brk #0 instruction on purpose...\n");
        asm volatile("brk #0");
    } else {
        kprintf("usage: crash [null|wro|exec]   (no arg = brk trap)\n");
    }
}

static void cmd_reboot(char *arg)
{
    (void)arg;
    kprintf("Rebooting... (no flush needed: the disk is always current)\n");
    power_reset();
}

static void cmd_shutdown(char *arg)
{
    (void)arg;
    kprintf("It is now safe to turn off your virtual machine.\n");
    power_off();
}

/* ------------------------------------------------------------------ */
/* Command table and dispatch                                          */
/* ------------------------------------------------------------------ */

struct command {
    const char *name;
    const char *help;
    void (*fn)(char *arg);
};

static const struct command commands[] = {
    { "help",     "list available commands",                  cmd_help },
    { "about",    "what is this OS?",                         cmd_about },

    { "tree",     "tree [path] — show the file hierarchy",    cmd_tree },
    { "cd",       "cd <path> — change directory ('..' = up)", cmd_cd },
    { "pwd",      "print the current directory",              cmd_pwd },
    { "mkdir",    "mkdir <path> — create a directory",        cmd_mkdir },
    { "write",    "write <path> <text> — create/overwrite",   cmd_write },
    { "rm",       "rm <path> — delete file or empty dir",     cmd_rm },
    { "sync",     "(v1 relic — MicroFS is write-through)",    cmd_sync },
    { "disk",     "filesystem stats: blocks, journal, cache", cmd_disk },
    { "crashfs",  "crash mid-write; journal heals on boot",   cmd_crashfs },
    { "bigtest",  "create/verify a 40 KB file (indirect)",    cmd_bigtest },

    { "mem",      "memory layout and heap statistics",        cmd_mem },
    { "alloc",    "alloc <hex-bytes> — exercise kmalloc()",   cmd_alloc },
    { "memtest",  "watch freed blocks coalesce",              cmd_memtest },
    { "peek",     "peek <hex-addr> — read 8 bytes of memory", cmd_peek },
    { "dtb",      "dump the parsed device tree",              cmd_dtb },
    { "clear",    "clear the screen",                         cmd_clear },
    { "exec",     "exec <prog> [args] — run a program",       cmd_exec },
    { "spawn",    "start a background counter task",          cmd_spawn },
    { "gui",      "info about the graphical display",         cmd_gui },
    { "net",      "network card and address info",            cmd_net },
    { "ping",     "ping [n] — ICMP echo the gateway",         cmd_ping },
    { "nslookup", "nslookup <host> — DNS resolve",            cmd_nslookup },
    { "wget",     "wget <host>[/path] — HTTP GET to disk",    cmd_wget },
    { "browse",   "browse <url> — open it in the Browser",    cmd_browse },
    { "https",    "https <host> — TLS 1.3 fetch + verify cert", cmd_https },
    { "fonts",    "fonts [n] — list/switch the system font",  cmd_fonts },
    { "fontget",  "fontget <url> — install a .mf8 font (HTTP)",cmd_fontget },
    { "fontload", "fontload <path> — install a .mf8 from disk",cmd_fontload },
    { "fontsave", "fontsave <path> — export active font .mf8", cmd_fontsave },
    { "beep",     "beep [hz] [ms] — play a tone",             cmd_beep },
    { "ps",       "list tasks and their CPU time",            cmd_ps },
    { "kill",     "kill <id> — terminate a task",             cmd_kill },
    { "cores",    "what is CPU core 1 up to?",                cmd_cores },

    { "date",     "wall-clock date and time (PL031 RTC)",     cmd_date },
    { "uptime",   "time since boot (counted by timer IRQs)",  cmd_uptime },
    { "sleep",    "sleep <secs> — nap; typing isn't lost",    cmd_sleep },
    { "crash",    "crash [null|wro|exec] — fault on purpose", cmd_crash },
    { "reboot",   "reset the machine (PSCI)",                 cmd_reboot },
    { "shutdown", "power off the machine (PSCI)",             cmd_shutdown },
};

#define NUM_COMMANDS (sizeof(commands) / sizeof(commands[0]))

static void cmd_help(char *arg)
{
    (void)arg;
    for (unsigned i = 0; i < NUM_COMMANDS; i++)
        kprintf("  %s%s%s\n", commands[i].name,
                strlen(commands[i].name) < 6 ? "\t\t" : "\t",
                commands[i].help);
}

/*
 * Read one line, handling the quirks of raw serial input ourselves:
 * the terminal sends '\r' for Enter and 0x7F for Backspace, and echoes
 * nothing — every character you SEE while typing is us sending it back.
 * (Normally a kernel's tty layer does this for every program; here we
 * are the tty layer.)
 */
static void read_line(char *buf, int max)
{
    int len = 0;

    for (;;) {
        char c = uart_getc();

        if (c == '\r' || c == '\n') {
            uart_puts("\n");
            buf[len] = '\0';
            return;
        }

        if (c == 0x7F || c == '\b') {               /* backspace */
            if (len > 0) {
                len--;
                uart_puts("\b \b");     /* back, erase with space, back */
            }
            continue;
        }

        /* printable ASCII OR a UTF-8 byte (>=0x80) — so you can type
         * Arabic/accented filenames; the GUI renders them shaped. */
        if (((unsigned char)c >= ' ') && (unsigned char)c != 0x7F && len < max - 1) {
            buf[len++] = c;
            uart_putc(c);                             /* echo */
        }
    }
}

void shell_run(void)
{
    char line[128];

    cwd = fs_root;
    kprintf("Type 'help' to see what I can do.\n\n");

    for (;;) {
        uart_puts("microos:");
        fs_print_path(cwd);
        uart_puts("> ");
        read_line(line, sizeof(line));

        if (line[0] == '\0')
            continue;

        /* A pipe? Split "left | right" and wire them together. (One
         * pipe; chaining N stages is a nice loop-shaped exercise.) */
        char *bar = 0;
        for (char *p = line; *p; p++)
            if (*p == '|') {
                bar = p;
                break;
            }
        if (bar) {
            *bar = '\0';
            run_pipeline(line, bar + 1);
            continue;
        }

        /* Split "name arg arg..." at the first space — but keep `full`
         * intact, since a program needs its whole command line. */
        char full[128];
        memcpy(full, line, sizeof(full));
        char *arg = line;
        while (*arg && *arg != ' ')
            arg++;
        if (*arg == ' ')
            *arg++ = '\0';
        while (*arg == ' ')
            arg++;

        const struct command *found = 0;
        for (unsigned i = 0; i < NUM_COMMANDS; i++) {
            if (strcmp(line, commands[i].name) == 0) {
                found = &commands[i];
                break;
            }
        }

        if (found)
            found->fn(arg);         /* a shell builtin */
        else
            run_foreground(full);   /* otherwise: a program in /bin */
    }
}

/* ------------------------------------------------------------------ */
/* DTB inspection                                                      */
/* ------------------------------------------------------------------ */

static void cmd_dtb(char *arg)
{
    (void)arg;
    dtb_dump();         /* the full parsed tree — see dtb.c */
}
