#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <rte_mempool.h>

#include "utg.h"

void send_gratuitous_arp(uint16_t port_id, struct rte_mempool *mbuf_pool,
    const struct app_config *cfg);
int resolve_arp(uint16_t port_id, struct rte_mempool *mbuf_pool, struct app_config *cfg,
    volatile bool *keep_running);
