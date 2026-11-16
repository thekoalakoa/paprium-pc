/* Copyright (C) 2026 Marsupial Solutions LLC
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * A port layer that does nothing, for tests that have no machine behind them:
 * device reads answer zero, device writes are dropped.
 */
#include "pt_port.h"

uint32_t pt_port_read(int size, uint32_t address) {
    (void)size;
    (void)address;
    return 0;
}

void pt_port_write(int size, uint32_t address, uint32_t value) {
    (void)size;
    (void)address;
    (void)value;
}

void pt_port_delay(int cycles) {
    (void)cycles;
}

void pt_port_irq_ack(int level) {
    (void)level;
}

int pt_port_tick(void) {
    pt_deadline = ~(uint64_t)0;
    return 0;
}

void pt_port_irq_return(int level) {
    (void)level;
}
