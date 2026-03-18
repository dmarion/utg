#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <rte_ether.h>

#define RX_DESC_DEFAULT 128
#define TX_DESC_DEFAULT 512
#define NUM_MBUFS 8191
#define MBUF_CACHE_SIZE 250
#define BURST_DEFAULT 64
#define BURST_MAX 256
#define PAYLOAD_PATTERN 0xAB

struct app_config {
    uint16_t port_id;
    uint16_t burst_size;
    uint16_t l2_pkt_len;
    uint16_t src_port;
    uint16_t src_port_rnd_mask;
    uint16_t dst_port;
    uint16_t dst_port_rnd_mask;
    uint64_t total_packets;
    double rate_mpps;
    bool dst_mac_override;
    struct rte_ether_addr src_mac;
    struct rte_ether_addr dst_mac;
    uint32_t src_ip;
    uint32_t src_ip_rnd_mask;
    uint32_t dst_ip;
    uint32_t dst_ip_rnd_mask;
};

struct io_counters {
    uint64_t tx_packets;
    uint64_t tx_bytes;
    uint64_t rx_packets;
    uint64_t rx_bytes;
};
