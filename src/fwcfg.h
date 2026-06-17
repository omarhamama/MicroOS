#pragma once

#include <stdint.h>

/* Find a fw_cfg file by name (e.g. "etc/ramfb"). Returns its selector
 * key, or 0 if absent. */
uint16_t fwcfg_find(const char *name);

/* Write a buffer into a fw_cfg entry via DMA (how ramfb is configured). */
int fwcfg_dma_write(uint16_t selector, const void *buf, uint32_t len);
