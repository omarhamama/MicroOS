#pragma once

/*
 * power.h — turning the machine off and on again.
 *
 * Both go through PSCI (the "Power State Coordination Interface"), the
 * same ARM firmware doorway SMP uses to start core 1 — an `hvc` call with
 * a function id in x0. QEMU's virt board implements it: SYSTEM_OFF makes
 * the VM exit, SYSTEM_RESET reboots it. Neither call returns.
 */

void power_off(void)   __attribute__((noreturn));   /* PSCI SYSTEM_OFF  */
void power_reset(void) __attribute__((noreturn));    /* PSCI SYSTEM_RESET */
