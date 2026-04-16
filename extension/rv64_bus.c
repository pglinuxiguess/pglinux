/* SPDX-License-Identifier: GPL-2.0 */
/*
 * rv64_bus.c — Memory bus for the LinuxSQL RISC-V emulator
 *
 * Routes physical addresses to RAM or memory-mapped I/O devices.
 * Memory map follows the QEMU "virt" machine layout so we can
 * reuse standard device trees and firmware (OpenSBI).
 *
 * Address map:
 *   0x02000000  CLINT (timer + software interrupts)
 *   0x0C000000  PLIC (external interrupt controller)
 *   0x10000000  UART0 (16550-compatible)
 *   0x10001000  Virtio MMIO #0 (virtio-blk)
 *   0x80000000  RAM (256 MB default)
 */
#include "rv64_cpu.h"
#include <string.h>

extern uint8_t *get_guest_ram_ptr(void);

/* ---- UART 16550 ---- */

/*
 * Minimal 16550 UART — enough for Linux's 8250 driver.
 * TX writes go to the console callback.
 * RX reads come from the input ring buffer.
 */
static uint64_t uart_load(struct rv64_uart *u, uint64_t offset)
{
	/* DLAB (Divisor Latch Access Bit) changes register meaning */
	int dlab = (u->lcr >> 7) & 1;

	switch (offset) {
	case 0: /* RBR (read) or DLL (if DLAB) */
		if (dlab)
			return u->dll;
		/* Read from receive buffer */
		if (u->rx_head != u->rx_tail) {
			u->rbr = u->rx_buf[u->rx_tail];
			u->rx_tail = (u->rx_tail + 1) & 0xFF;
		}
		u->rx_ready = (u->rx_head != u->rx_tail);
		return u->rbr;

	case 1: /* IER or DLM */
		return dlab ? u->dlm : u->ier;

	case 2: /* IIR (read-only) */
		/* Check RX interrupt first (higher priority per 8250 spec) */
		if (u->rx_ready && (u->ier & 1))
			return 0x04;  /* Received data available */
		/* THRE interrupt: IER bit 1 set + transmitter always ready */
		if (u->ier & 2) {
			u->thre_pending = 0; /* Reading IIR acknowledges THRE */
			return 0x02;  /* THR empty */
		}
		return 0x01;      /* No interrupt pending */

	case 3: return u->lcr;
	case 4: return u->mcr;

	case 5: /* LSR — Line Status Register */
		/*
		 * Bit 0: Data Ready (rx buffer has data)
		 * Bit 5: THR Empty (transmitter can accept data)
		 * Bit 6: Transmitter Empty
		 */
		return (u->rx_ready ? 0x01 : 0) | 0x60;

	case 6: return u->msr;
	case 7: return u->scr;
	default: return 0;
	}
}

static void uart_store(struct rv64_uart *u, uint64_t offset, uint64_t val)
{
	int dlab = (u->lcr >> 7) & 1;

	switch (offset) {
	case 0: /* THR (write) or DLL (if DLAB) */
		if (dlab) {
			u->dll = val & 0xFF;
		} else {
			/* Transmit character */
			if (u->tx_callback)
				u->tx_callback((uint8_t)val, u->tx_opaque);
			/* TX is instant — immediately re-signal THRE if enabled.
			 * Without this, the 8250 driver gets stuck waiting for
			 * the next THRE interrupt after filling the FIFO. */
			if (u->ier & 2)
				u->thre_pending = 1;
		}
		break;

	case 1: /* IER or DLM */
		if (dlab) {
			u->dlm = val & 0xFF;
		} else {
			u->ier = val & 0x0F;
			/* If THRE interrupt enabled, immediately signal it.
			 * Our TX path is instant, so THRE is always pending. */
			u->thre_pending = (u->ier & 2) ? 1 : 0;
		}
		break;

	case 2: /* FCR (write-only, FIFO control — ignore) */
		break;
	case 3: u->lcr = val & 0xFF; break;
	case 4: u->mcr = val & 0x1F; break;
	case 7: u->scr = val & 0xFF; break;
	default: break;
	}
}

/* ---- CLINT (Core Local Interruptor) ---- */

static uint64_t clint_load(struct rv64_clint *c, uint64_t offset)
{
	switch (offset) {
	case 0x0000: /* msip — machine software interrupt pending */
		return 0;
	case CLINT_MTIMECMP:
	case CLINT_MTIMECMP + 4:
		if (offset == CLINT_MTIMECMP)
			return c->mtimecmp & 0xFFFFFFFF;
		return c->mtimecmp >> 32;
	case CLINT_MTIME:
	case CLINT_MTIME + 4:
		if (offset == CLINT_MTIME)
			return c->mtime & 0xFFFFFFFF;
		return c->mtime >> 32;
	default:
		return 0;
	}
}

static void clint_store(struct rv64_clint *c, uint64_t offset, uint64_t val,
                        int size)
{
	switch (offset) {
	case 0x0000: /* msip */
		break;
	case CLINT_MTIMECMP:
		if (size == 8)
			c->mtimecmp = val;
		else
			c->mtimecmp = (c->mtimecmp & 0xFFFFFFFF00000000ULL) | (val & 0xFFFFFFFF);
		break;
	case CLINT_MTIMECMP + 4:
		c->mtimecmp = (c->mtimecmp & 0xFFFFFFFF) | ((val & 0xFFFFFFFF) << 32);
		break;
	case CLINT_MTIME:
		if (size == 8)
			c->mtime = val;
		else
			c->mtime = (c->mtime & 0xFFFFFFFF00000000ULL) | (val & 0xFFFFFFFF);
		break;
	case CLINT_MTIME + 4:
		c->mtime = (c->mtime & 0xFFFFFFFF) | ((val & 0xFFFFFFFF) << 32);
		break;
	default:
		break;
	}
}

/* ---- PLIC (Platform-Level Interrupt Controller) ---- */

/*
 * Simplified PLIC for a single hart, single context.
 * Layout (per RISC-V spec):
 *   0x000000 - 0x000FFF  Priority registers (source 0-1023)
 *   0x001000 - 0x001FFF  Pending bits
 *   0x002000 - 0x1FFFFF  Enable bits (per context)
 *   0x200000 - 0x3FFFFFF  Priority threshold + claim/complete (per context)
 */
static uint64_t plic_load(struct rv64_plic *p, uint64_t offset)
{
	if (offset < 0x1000) {
		/* Priority registers */
		uint32_t src = offset / 4;
		return (src < 64) ? p->priority[src] : 0;
	}
	if (offset >= 0x1000 && offset < 0x1080) {
		/* Pending bits */
		uint32_t idx = (offset - 0x1000) / 4;
		return (idx < 2) ? p->pending[idx] : 0;
	}
	/* Enable bits: ctx 0 at 0x2000, ctx 1 at 0x2080 */
	if (offset >= 0x2000 && offset < 0x2100) {
		int ctx = (offset >= 0x2080) ? 1 : 0;
		uint32_t base = ctx ? 0x2080 : 0x2000;
		uint32_t idx = (offset - base) / 4;
		return (idx < 2) ? p->enable[ctx][idx] : 0;
	}
	/* Threshold + claim: ctx 0 at 0x200000, ctx 1 at 0x201000 */
	if (offset >= 0x200000 && offset < 0x202000) {
		int ctx = (offset >= 0x201000) ? 1 : 0;
		uint64_t off = offset - (ctx ? 0x201000 : 0x200000);
		if (off == 0)
			return p->threshold[ctx];
		if (off == 4) {
			/* Claim: return highest-priority pending+enabled IRQ */
			for (int i = 1; i < 64; i++) {
				int word = i / 32;
				int bit = i % 32;
				if ((p->pending[word] & p->enable[ctx][word] & (1U << bit))) {
					p->pending[word] &= ~(1U << bit);
					p->claim[ctx] = i;
					return i;
				}
			}
			return 0; /* no pending */
		}
	}
	return 0;
}

static void plic_store(struct rv64_plic *p, uint64_t offset, uint64_t val)
{
	if (offset < 0x1000) {
		uint32_t src = offset / 4;
		if (src < 64) p->priority[src] = val & 0x7;
		return;
	}
	/* Enable bits: ctx 0 at 0x2000, ctx 1 at 0x2080 */
	if (offset >= 0x2000 && offset < 0x2100) {
		int ctx = (offset >= 0x2080) ? 1 : 0;
		uint32_t base = ctx ? 0x2080 : 0x2000;
		uint32_t idx = (offset - base) / 4;
		if (idx < 2) p->enable[ctx][idx] = (uint32_t)val;
		return;
	}
	/* Threshold + claim: ctx 0 at 0x200000, ctx 1 at 0x201000 */
	if (offset >= 0x200000 && offset < 0x202000) {
		int ctx = (offset >= 0x201000) ? 1 : 0;
		uint64_t off = offset - (ctx ? 0x201000 : 0x200000);
		if (off == 0)
			p->threshold[ctx] = val & 0x7;
		if (off == 4) {
			/* Complete: acknowledge the IRQ */
			p->claim[ctx] = 0;
		}
		return;
	}
}

/* ---- Main bus load/store ---- */

int bus_load(struct rv64_cpu *cpu, uint64_t addr, int size, uint64_t *val)
{
	/* RAM */
	if (addr >= RAM_BASE && addr < RAM_BASE + cpu->ram_size) {
		uint8_t *ram = get_guest_ram_ptr();
		if (!ram) return -1;
		uint64_t offset = addr - RAM_BASE;
		switch (size) {
		case 1: *val = ram[offset]; break;
		case 2: *val = *(uint16_t *)(ram + offset); break;
		case 4: *val = *(uint32_t *)(ram + offset); break;
		case 8: *val = *(uint64_t *)(ram + offset); break;
		default: return -1;
		}
		return 0;
	}

	/* UART0 */
	if (addr >= UART0_BASE && addr < UART0_BASE + UART0_SIZE) {
		*val = uart_load(&cpu->uart, addr - UART0_BASE);
		return 0;
	}

	/* CLINT */
	if (addr >= CLINT_BASE && addr < CLINT_BASE + CLINT_SIZE) {
		*val = clint_load(&cpu->clint, addr - CLINT_BASE);
		return 0;
	}

	/* PLIC */
	if (addr >= PLIC_BASE && addr < PLIC_BASE + PLIC_SIZE) {
		*val = plic_load(&cpu->plic, addr - PLIC_BASE);
		return 0;
	}

	/* Virtio MMIO devices */
	{
		uint64_t vbase = VIRTIO_MMIO_BASE;
		for (int i = 0; i < VIRTIO_MMIO_COUNT; i++, vbase += VIRTIO_MMIO_SIZE) {
			if (addr >= vbase && addr < vbase + VIRTIO_MMIO_SIZE) {
				struct virtio_mmio_dev *dev = cpu->virtio_devs[i];
				if (dev) {
					uint64_t offset = addr - vbase;
					*val = virtio_mmio_load(dev, offset, size);
					return 0;
				}
			}
		}
	}

	/* Unmapped — return 0 (don't crash) */
	*val = 0;
	return 0;
}

int bus_store(struct rv64_cpu *cpu, uint64_t addr, int size, uint64_t val)
{
	/* RAM */
	if (addr >= RAM_BASE && addr < RAM_BASE + cpu->ram_size) {
		uint8_t *ram = get_guest_ram_ptr();
		if (!ram) return -1;
		uint64_t offset = addr - RAM_BASE;
		switch (size) {
		case 1: ram[offset] = (uint8_t)val; break;
		case 2: *(uint16_t *)(ram + offset) = (uint16_t)val; break;
		case 4: *(uint32_t *)(ram + offset) = (uint32_t)val; break;
		case 8: *(uint64_t *)(ram + offset) = val; break;
		default: return -1;
		}
		return 0;
	}

	/* UART0 */
	if (addr >= UART0_BASE && addr < UART0_BASE + UART0_SIZE) {
		uart_store(&cpu->uart, addr - UART0_BASE, val);
		/* Raise PLIC IRQ if UART has any pending interrupt condition */
		if ((cpu->uart.thre_pending) ||
		    (cpu->uart.rx_ready && (cpu->uart.ier & 1))) {
			cpu->plic.pending[UART0_IRQ / 32] |=
				(1U << (UART0_IRQ % 32));
		}
		return 0;
	}

	/* CLINT */
	if (addr >= CLINT_BASE && addr < CLINT_BASE + CLINT_SIZE) {
		clint_store(&cpu->clint, addr - CLINT_BASE, val, size);
		return 0;
	}

	/* PLIC */
	if (addr >= PLIC_BASE && addr < PLIC_BASE + PLIC_SIZE) {
		plic_store(&cpu->plic, addr - PLIC_BASE, val);
		return 0;
	}

	/* Virtio MMIO devices */
	{
		uint64_t vbase = VIRTIO_MMIO_BASE;
		for (int i = 0; i < VIRTIO_MMIO_COUNT; i++, vbase += VIRTIO_MMIO_SIZE) {
			if (addr >= vbase && addr < vbase + VIRTIO_MMIO_SIZE) {
				struct virtio_mmio_dev *dev = cpu->virtio_devs[i];
				if (dev) {
					uint8_t *ram = get_guest_ram_ptr();
					virtio_mmio_store(dev, addr - vbase, val, size,
									  ram, cpu->ram_size);
					/* Raise PLIC if device has pending interrupt */
					if (dev->int_status)
						cpu->plic.pending[dev->irq / 32] |=
							(1U << (dev->irq % 32));
					return 0;
				}
			}
		}
	}

	/* Unmapped — silently ignore */
	return 0;
}

/* ---- UART helper ---- */

void rv64_uart_rx(struct rv64_cpu *cpu, uint8_t ch)
{
	struct rv64_uart *u = &cpu->uart;
	int next = (u->rx_head + 1) & 0xFF;
	if (next != u->rx_tail) {
		u->rx_buf[u->rx_head] = ch;
		u->rx_head = next;
		u->rx_ready = 1;
	}
	/* Raise PLIC IRQ for UART if enabled */
	cpu->plic.pending[UART0_IRQ / 32] |= (1U << (UART0_IRQ % 32));
}
