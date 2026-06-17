/*
 * dtb.c — parsing the Device Tree: how a kernel learns what machine
 * it woke up on.
 *
 * x86 PCs enumerate hardware through ACPI tables and PCI probing. The
 * embedded/ARM world instead receives a DEVICE TREE BLOB: firmware
 * hands the kernel (in x0, remember boot.S) a binary description of
 * everything — how much RAM, where the UART lives, which interrupt
 * each device uses. Until now MicroOS ignored it and hard-coded every
 * address; this file is the honest alternative.
 *
 * The format ("flattened device tree") is two regions + a header:
 *
 *   STRUCTURE block — a token stream describing a tree:
 *       BEGIN_NODE "uart@9000000"     (then that node's contents)
 *       PROP len nameoff <data>       (a key=value on the current node)
 *       END_NODE
 *   STRINGS block — property NAMES, deduplicated; props point in by
 *       offset (the same trick as a compiler's string table).
 *
 * Everything is BIG-endian (DTB predates ARM64 and kept its PowerPC
 * manners), so every read goes through be32()/be64().
 *
 * The properties that matter most in practice:
 *   compatible = "arm,pl011"   what driver does this node need?
 *   reg        = <addr size>   where are its registers?
 *   interrupts = <...>         which interrupt line?
 */

#include "dtb.h"
#include "lib.h"
#include "kprintf.h"

#define FDT_BEGIN_NODE  1u
#define FDT_END_NODE    2u
#define FDT_PROP        3u
#define FDT_NOP         4u
#define FDT_END         9u

static const uint8_t *blob;         /* the DTB itself */
static const uint8_t *structs;      /* token stream */
static const char    *strings;     /* property-name table */
static uint32_t       struct_size;

static uint64_t ram_base, ram_size, uart_base;
static int      virtio_slots;

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}

static uint64_t be64(const uint8_t *p)
{
    return ((uint64_t)be32(p) << 32) | be32(p + 4);
}

uint64_t dtb_ram_base(void)  { return ram_base; }
uint64_t dtb_ram_size(void)  { return ram_size; }
uint64_t dtb_uart_base(void) { return uart_base; }
int dtb_virtio_slots(void)   { return virtio_slots; }

/* Does a `compatible` property (a list of NUL-separated strings)
 * contain the given string? */
static int compat_has(const uint8_t *data, uint32_t len, const char *want)
{
    uint32_t i = 0;
    while (i < len) {
        if (strcmp((const char *)data + i, want) == 0)
            return 1;
        i += (uint32_t)strlen((const char *)data + i) + 1;
    }
    return 0;
}

/*
 * One walk serves both jobs: with print=1 it pretty-prints the tree
 * (the `dtb` command); the scan for hardware facts happens every time.
 * The token stream is processed strictly left to right — the only
 * state needed is the current depth and the current node's name.
 */
static void walk(int print)
{
    const uint8_t *p = structs;
    const char *node_name = "";
    int depth = 0;

    virtio_slots = 0;

    while (p < structs + struct_size) {
        uint32_t token = be32(p);
        p += 4;

        if (token == FDT_END)
            break;

        if (token == FDT_NOP)
            continue;

        if (token == FDT_END_NODE) {
            depth--;
            continue;
        }

        if (token == FDT_BEGIN_NODE) {
            node_name = (const char *)p;
            uint32_t n = (uint32_t)strlen(node_name) + 1;
            p += (n + 3) & ~3u;             /* names pad to 4 bytes */
            if (print) {
                for (int i = 0; i < depth; i++)
                    kprintf("  ");
                kprintf("%s/\n", depth == 0 ? "" : node_name);
            }
            depth++;
            continue;
        }

        if (token == FDT_PROP) {
            uint32_t len     = be32(p);
            uint32_t nameoff = be32(p + 4);
            const uint8_t *data = p + 8;
            p += 8 + ((len + 3) & ~3u);     /* data pads to 4 bytes too */

            const char *pname = strings + nameoff;

            /* ---- the scan: collect the facts we care about -------- */
            if (strcmp(pname, "compatible") == 0 &&
                compat_has(data, len, "virtio,mmio"))
                virtio_slots++;
            if (strcmp(pname, "reg") == 0 && len >= 16) {
                if (strncmp(node_name, "memory", 6) == 0) {
                    ram_base = be64(data);
                    ram_size = be64(data + 8);
                }
                if (strncmp(node_name, "pl011", 5) == 0 ||
                    strncmp(node_name, "uart", 4) == 0)
                    uart_base = be64(data);
            }

            /* ---- the pretty print --------------------------------- */
            if (print) {
                for (int i = 0; i < depth; i++)
                    kprintf("  ");
                if (strcmp(pname, "compatible") == 0)
                    kprintf("%s = \"%s\"\n", pname, (const char *)data);
                else if (strcmp(pname, "reg") == 0 && len >= 16)
                    kprintf("%s = <0x%lx 0x%lx>\n", pname,
                            be64(data), be64(data + 8));
                else if (len == 4)
                    kprintf("%s = <0x%x>\n", pname, be32(data));
                else
                    kprintf("%s (%u bytes)\n", pname, len);
            }
            continue;
        }

        return;                             /* unknown token: stop safely */
    }
}

int dtb_init(uint64_t addr)
{
    blob = (const uint8_t *)addr;
    if (!blob || be32(blob) != 0xd00dfeed)
        return -1;

    structs     = blob + be32(blob + 8);    /* off_dt_struct */
    strings     = (const char *)blob + be32(blob + 12); /* off_dt_strings */
    struct_size = be32(blob + 36);          /* size_dt_struct */

    walk(0);                                /* scan only, no printing */
    return 0;
}

void dtb_dump(void)
{
    if (!blob) {
        kprintf("no valid device tree\n");
        return;
    }
    kprintf("Device Tree Blob at %p (%u bytes) — what firmware told us:\n\n",
            blob, be32(blob + 4));
    walk(1);
    kprintf("\nThe facts MicroOS extracted (instead of hard-coding!):\n");
    kprintf("  RAM:    %lu MiB at 0x%lx\n", ram_size >> 20, ram_base);
    kprintf("  UART:   0x%lx\n", uart_base);
    kprintf("  virtio: %d MMIO slots\n", virtio_slots);
}

/*
 * Find the bootloader's framebuffer (a "simple-framebuffer" node) in a
 * DTB, WITHOUT needing dtb_init — used on the R36S (RK3326), where U-Boot
 * has already lit the panel and describes the framebuffer it left running.
 * Returns 0 and fills the addr/width/height/stride outputs, else -1.
 * (Assumes 64-bit address cells in `reg`, the arm64 norm; UNVERIFIED.)
 */
int dtb_find_simplefb(const void *dtb, uint64_t *addr, uint32_t *w,
                      uint32_t *h, uint32_t *stride)
{
    const uint8_t *b = dtb;
    if (!b || be32(b) != 0xD00DFEED)
        return -1;
    const uint8_t *p     = b + be32(b + 8);     /* off_dt_struct  */
    const char    *strs  = (const char *)(b + be32(b + 12)); /* off_dt_strings */

    uint64_t cur_a = 0; uint32_t cur_w = 0, cur_h = 0, cur_s = 0; int cur_fb = 0;
    for (int guard = 0; guard < (1 << 20); guard++) {
        uint32_t tok = be32(p); p += 4;
        if (tok == FDT_END) break;
        if (tok == FDT_NOP) continue;
        if (tok == FDT_BEGIN_NODE) {
            const char *nm = (const char *)p;
            p += ((uint32_t)strlen(nm) + 1 + 3) & ~3u;
            cur_a = 0; cur_w = cur_h = cur_s = 0; cur_fb = 0;   /* new node */
            continue;
        }
        if (tok == FDT_END_NODE) {
            if (cur_fb && cur_a && cur_w && cur_h) {
                *addr = cur_a; *w = cur_w; *h = cur_h;
                *stride = cur_s ? cur_s : cur_w * 4;
                return 0;
            }
            continue;
        }
        if (tok == FDT_PROP) {
            uint32_t len = be32(p), nameoff = be32(p + 4);
            const uint8_t *data = p + 8;
            const char *pname = strs + nameoff;
            p += 8 + ((len + 3) & ~3u);
            if (strcmp(pname, "compatible") == 0 &&
                compat_has(data, len, "simple-framebuffer")) cur_fb = 1;
            else if (strcmp(pname, "reg") == 0 && len >= 8)   cur_a = be64(data);
            else if (strcmp(pname, "width") == 0 && len >= 4) cur_w = be32(data);
            else if (strcmp(pname, "height") == 0 && len >= 4) cur_h = be32(data);
            else if (strcmp(pname, "stride") == 0 && len >= 4) cur_s = be32(data);
        }
    }
    return -1;
}
