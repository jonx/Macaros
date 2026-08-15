// AROS AArch64 bring-up — PL011 UART console + a tiny kprintf.
//
// The console every milestone prints through (and reads from, for the M8 shell).
// PL011 is QEMU virt's UART0 at 0x0900_0000; while the MMU is off the device
// range is identity-mapped Device memory (see mmu.c). Foreshadows a real AROS
// serial/console driver.

#include <stdint.h>
#include <stdarg.h>
#include "kern.h"

#define UART0_BASE   0x09000000UL
#define UART_DR      0x00
#define UART_FR      0x18
#define UART_LCRH    0x2c
#define UART_CR      0x30
#define UART_FR_TXFF (1u << 5)   // transmit FIFO full
#define UART_FR_RXFE (1u << 4)   // receive FIFO empty

static inline void mmio_w32(unsigned long base, unsigned off, uint32_t v) {
    *(volatile uint32_t *)(base + off) = v;
}
static inline uint32_t mmio_r32(unsigned long base, unsigned off) {
    return *(volatile uint32_t *)(base + off);
}

void uart_init(void) {
    mmio_w32(UART0_BASE, UART_LCRH, 0x70);    // 8-bit word, FIFO enabled
    mmio_w32(UART0_BASE, UART_CR,   0x301);   // UARTEN | TXE | RXE
}

void uart_putc(char c) {
    while (mmio_r32(UART0_BASE, UART_FR) & UART_FR_TXFF) { /* wait for space */ }
    mmio_w32(UART0_BASE, UART_DR, (uint8_t)c);
}

int uart_getc(void) {
    while (mmio_r32(UART0_BASE, UART_FR) & UART_FR_RXFE) { /* wait for input */ }
    return (int)(mmio_r32(UART0_BASE, UART_DR) & 0xff);
}

// ---- tiny kprintf: %c %s %d %u %x %p, plus %l{d,u,x} ----
static void put_uint(unsigned long v, unsigned base, int width, char pad) {
    char buf[32];
    const char *digits = "0123456789abcdef";
    int i = 0;
    if (v == 0) buf[i++] = '0';
    while (v) { buf[i++] = digits[v % base]; v /= base; }
    while (i < width) buf[i++] = pad;
    while (i--) uart_putc(buf[i]);
}

static void put_str(const char *s) {
    for (; *s; ++s) {
        if (*s == '\n') uart_putc('\r');   // cook \n -> \r\n for the terminal
        uart_putc(*s);
    }
}

void kprintf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    for (; *fmt; ++fmt) {
        if (*fmt != '%') {
            if (*fmt == '\n') uart_putc('\r');
            uart_putc(*fmt);
            continue;
        }
        switch (*++fmt) {
        case 's': put_str(va_arg(ap, const char *)); break;
        case 'c': uart_putc((char)va_arg(ap, int)); break;
        case 'u': put_uint(va_arg(ap, unsigned), 10, 0, ' '); break;
        case 'x': put_uint(va_arg(ap, unsigned), 16, 0, ' '); break;
        case 'p': put_str("0x"); put_uint((unsigned long)va_arg(ap, void *), 16, 16, '0'); break;
        case 'd': {
            long v = va_arg(ap, int);
            if (v < 0) { uart_putc('-'); v = -v; }
            put_uint((unsigned long)v, 10, 0, ' ');
            break;
        }
        case 'l':
            switch (*++fmt) {
            case 'u': put_uint(va_arg(ap, unsigned long), 10, 0, ' '); break;
            case 'x': put_uint(va_arg(ap, unsigned long), 16, 0, ' '); break;
            case 'd': {
                long v = va_arg(ap, long);
                if (v < 0) { uart_putc('-'); v = -v; }
                put_uint((unsigned long)v, 10, 0, ' ');
                break;
            }
            default: uart_putc('%'); uart_putc('l'); uart_putc(*fmt); break;
            }
            break;
        case '%': uart_putc('%'); break;
        default: uart_putc('%'); uart_putc(*fmt); break;
        }
    }
    va_end(ap);
}
