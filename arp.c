#include "arp.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <rte_arp.h>
#include <rte_cycles.h>
#include <rte_eal.h>
#include <rte_ether.h>
#include <rte_ethdev.h>
#include <rte_ip.h>
#include <rte_mbuf.h>

static void print_mac(const struct rte_ether_addr *addr)
{
    printf("%02X:%02X:%02X:%02X:%02X:%02X",
        addr->addr_bytes[0], addr->addr_bytes[1], addr->addr_bytes[2],
        addr->addr_bytes[3], addr->addr_bytes[4], addr->addr_bytes[5]);
}

static void print_ipv4(uint32_t ip)
{
    printf("%u.%u.%u.%u",
        (uint8_t)(ip >> 24), (uint8_t)(ip >> 16), (uint8_t)(ip >> 8), (uint8_t)ip);
}

static uint32_t arp_target_ip(const struct app_config *cfg)
{
    return cfg->gw_ip_set ? cfg->gw_ip : cfg->dst_ip;
}

static void print_arp_request(uint16_t port_id, const struct app_config *cfg,
    const struct rte_ether_addr *dst_mac, const struct rte_ether_addr *tha)
{
    const uint32_t target_ip = arp_target_ip(cfg);

    printf("ARP TX port %u request eth ", port_id);
    print_mac(&cfg->src_mac);
    printf(" -> ");
    print_mac(dst_mac);
    printf(", who-has ");
    print_ipv4(target_ip);
    printf(" tell ");
    print_ipv4(cfg->src_ip);
    printf(", sha ");
    print_mac(&cfg->src_mac);
    printf(", tha ");
    print_mac(tha);
    printf("\n");
}

static void print_arp_reply(uint16_t port_id, const struct rte_ether_hdr *eth,
    const struct rte_arp_hdr *arp)
{
    printf("ARP RX port %u reply eth ", port_id);
    print_mac(&eth->src_addr);
    printf(" -> ");
    print_mac(&eth->dst_addr);
    printf(", ");
    print_ipv4(rte_be_to_cpu_32(arp->arp_data.arp_sip));
    printf(" is-at ");
    print_mac(&arp->arp_data.arp_sha);
    printf(", target ");
    print_ipv4(rte_be_to_cpu_32(arp->arp_data.arp_tip));
    printf(" ");
    print_mac(&arp->arp_data.arp_tha);
    printf("\n");
}

static void send_arp_packet(uint16_t port_id, struct rte_mempool *mbuf_pool,
    const struct app_config *cfg, const struct rte_ether_addr *dst_mac,
    const struct rte_ether_addr *tha, uint16_t opcode, uint32_t tip)
{
    const uint16_t arp_len = (uint16_t)(sizeof(struct rte_ether_hdr) + sizeof(struct rte_arp_hdr));
    const uint16_t pkt_len = RTE_ETHER_MIN_LEN - RTE_ETHER_CRC_LEN;
    struct rte_mbuf *mbuf;
    struct rte_ether_hdr *eth;
    struct rte_arp_hdr *arp;

    mbuf = rte_pktmbuf_alloc(mbuf_pool);
    if (mbuf == NULL) {
        rte_exit(EXIT_FAILURE, "Failed to allocate ARP mbuf\n");
    }

    mbuf->data_len = pkt_len;
    mbuf->pkt_len = pkt_len;

    eth = rte_pktmbuf_mtod(mbuf, struct rte_ether_hdr *);
    rte_ether_addr_copy(dst_mac, &eth->dst_addr);
    rte_ether_addr_copy(&cfg->src_mac, &eth->src_addr);
    eth->ether_type = rte_cpu_to_be_16(RTE_ETHER_TYPE_ARP);

    arp = (struct rte_arp_hdr *)(eth + 1);
    arp->arp_hardware = rte_cpu_to_be_16(RTE_ARP_HRD_ETHER);
    arp->arp_protocol = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4);
    arp->arp_hlen = RTE_ETHER_ADDR_LEN;
    arp->arp_plen = sizeof(uint32_t);
    arp->arp_opcode = rte_cpu_to_be_16(opcode);
    rte_ether_addr_copy(&cfg->src_mac, &arp->arp_data.arp_sha);
    arp->arp_data.arp_sip = rte_cpu_to_be_32(cfg->src_ip);
    rte_ether_addr_copy(tha, &arp->arp_data.arp_tha);
    arp->arp_data.arp_tip = rte_cpu_to_be_32(tip);
    memset((uint8_t *)eth + arp_len, 0, pkt_len - arp_len);

    if (rte_eth_tx_burst(port_id, 0, &mbuf, 1) != 1) {
        rte_pktmbuf_free(mbuf);
        rte_exit(EXIT_FAILURE, "Failed to send ARP packet\n");
    }
}

static void send_arp_request(uint16_t port_id, struct rte_mempool *mbuf_pool,
    const struct app_config *cfg)
{
    const struct rte_ether_addr broadcast = {
        .addr_bytes = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff }
    };
    const struct rte_ether_addr zero = { .addr_bytes = { 0, 0, 0, 0, 0, 0 } };

    print_arp_request(port_id, cfg, &broadcast, &zero);
    send_arp_packet(port_id, mbuf_pool, cfg, &broadcast, &zero,
        RTE_ARP_OP_REQUEST, arp_target_ip(cfg));
}

void send_gratuitous_arp(uint16_t port_id, struct rte_mempool *mbuf_pool,
    const struct app_config *cfg)
{
    const struct rte_ether_addr broadcast = {
        .addr_bytes = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff }
    };

    send_arp_packet(port_id, mbuf_pool, cfg, &broadcast, &cfg->src_mac,
        RTE_ARP_OP_REPLY, cfg->src_ip);
}

static int try_receive_arp_reply(uint16_t port_id, const struct app_config *cfg,
    struct rte_ether_addr *resolved_mac)
{
    struct rte_mbuf *rx_mbufs[BURST_DEFAULT];
    const uint32_t target_ip = arp_target_ip(cfg);
    uint16_t nb_rx;

    nb_rx = rte_eth_rx_burst(port_id, 0, rx_mbufs, BURST_DEFAULT);
    for (uint16_t i = 0; i < nb_rx; i++) {
        struct rte_mbuf *mbuf = rx_mbufs[i];
        struct rte_ether_hdr *eth;
        struct rte_arp_hdr *arp;
        bool match = false;

        if (rte_pktmbuf_data_len(mbuf) >= sizeof(*eth) + sizeof(*arp)) {
            eth = rte_pktmbuf_mtod(mbuf, struct rte_ether_hdr *);
            if (eth->ether_type == rte_cpu_to_be_16(RTE_ETHER_TYPE_ARP)) {
                arp = (struct rte_arp_hdr *)(eth + 1);
                match = arp->arp_opcode == rte_cpu_to_be_16(RTE_ARP_OP_REPLY) &&
                    arp->arp_data.arp_sip == rte_cpu_to_be_32(target_ip) &&
                    arp->arp_data.arp_tip == rte_cpu_to_be_32(cfg->src_ip);
                if (match) {
                    print_arp_reply(port_id, eth, arp);
                    rte_ether_addr_copy(&arp->arp_data.arp_sha, resolved_mac);
                }
            }
        }

        rte_pktmbuf_free(mbuf);
        if (match) {
            return 0;
        }
    }

    return -1;
}

int resolve_arp(uint16_t port_id, struct rte_mempool *mbuf_pool, struct app_config *cfg,
    volatile bool *keep_running)
{
    const uint64_t timeout_cycles = rte_get_timer_hz() * 3;
    const uint64_t retry_cycles = rte_get_timer_hz() / 2;
    uint64_t start = rte_get_timer_cycles();
    uint64_t next_retry = start;
    struct rte_ether_addr resolved_mac;

    while (*keep_running && (rte_get_timer_cycles() - start) < timeout_cycles) {
        uint64_t now = rte_get_timer_cycles();

        if (now >= next_retry) {
            send_arp_request(port_id, mbuf_pool, cfg);
            next_retry = now + retry_cycles;
        }

        if (try_receive_arp_reply(port_id, cfg, &resolved_mac) == 0) {
            cfg->dst_mac = resolved_mac;
            return 0;
        }

        rte_delay_us_sleep(1000);
    }

    return -1;
}
