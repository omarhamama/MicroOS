#pragma once

void uart_init(void);           /* output only — safe before the GIC is up */
void uart_irq_init(void);       /* switch input to interrupt-driven mode */
void uart_handle_irq(void);     /* called from handle_irq() on IRQ 33 */
void uart_putc(char c);
char uart_getc(void);           /* blocks (CPU asleep!) until a char arrives */
int  uart_haschar(void);        /* non-blocking: is input waiting? */
void uart_input_push(char c);   /* feed the console queue (UART or keyboard) */
void uart_puts(const char *s);
void uart_debug_state(uint32_t *fr, uint32_t *ris, uint32_t *mis,
                      uint32_t *head, uint32_t *tail);
