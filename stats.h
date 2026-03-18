#pragma once

#include <stdint.h>

#include <rte_ethdev.h>

#include "utg.h"

void print_stats_header(void);
void print_rate_line(const struct io_counters *prev, const struct io_counters *curr,
    double interval_sec);
void print_final_stats(uint16_t port_id, uint16_t num_queues,
    const struct io_counters *totals, const struct rte_eth_stats *stats);
