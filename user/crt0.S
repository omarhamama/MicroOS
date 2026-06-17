/*
 * crt0.S — the C runtime, user edition ("C RunTime zero").
 *
 * Even in big OSes, main() is not where a program starts: a tiny shim
 * like this runs first (look up crt0/_start in any libc). The kernel's
 * program loader hands us argc in x0 and a pointer to the argv array
 * in x1 — exactly the AArch64 convention real kernels use. We pass
 * them straight to main(argc, argv), then turn main's return value
 * into an exit(code) system call so a program that just `return 0`s
 * still terminates cleanly with a status the shell can read.
 */

.section .text.start
.global _start
_start:
    /* x0 = argc, x1 = argv — already in the right registers for main */
    bl      main
    /* main returned; its value is in x0. exit(x0). */
    mov     x8, #0          /* SYS_EXIT */
    svc     #0
1:  b       1b              /* unreachable: exit does not return */
