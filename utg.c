#include <errno.h>
#include <inttypes.h>
#include <signal.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <time.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "arp.h"
#include "cli.h"
#include "stats.h"

#include <rte_byteorder.h>
#include <rte_cycles.h>
#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_ip.h>
#include <rte_lcore.h>
#include <rte_mbuf.h>
#include <rte_random.h>
#include <rte_udp.h>

static volatile bool keep_running = true;

static uint16_t cksum_update_u16(uint16_t old_cksum_be, uint16_t old_value_be,
    uint16_t new_value_be)
{
    uint32_t sum;

    sum = (uint32_t)(~rte_be_to_cpu_16(old_cksum_be) & 0xffff);
    sum += (uint32_t)(~rte_be_to_cpu_16(old_value_be) & 0xffff);
    sum += rte_be_to_cpu_16(new_value_be);
    sum = (sum & 0xffff) + (sum >> 16);
    sum = (sum & 0xffff) + (sum >> 16);

    return rte_cpu_to_be_16((uint16_t)~sum);
}

static uint16_t cksum_update_u32(uint16_t old_cksum_be, uint32_t old_value_be,
    uint32_t new_value_be)
{
    uint16_t old_hi = (uint16_t)(old_value_be >> 16);
    uint16_t old_lo = (uint16_t)(old_value_be & 0xffff);
    uint16_t new_hi = (uint16_t)(new_value_be >> 16);
    uint16_t new_lo = (uint16_t)(new_value_be & 0xffff);
    uint16_t cksum;

    cksum = cksum_update_u16(old_cksum_be, old_hi, new_hi);
    cksum = cksum_update_u16(cksum, old_lo, new_lo);
    return cksum;
}

struct shared_counters {
    atomic_uint_fast64_t tx_packets;
    atomic_uint_fast64_t tx_bytes;
    atomic_uint_fast64_t rx_packets;
    atomic_uint_fast64_t rx_bytes;
};

struct packet_template {
    uint16_t l2_pkt_len;
    uint16_t hdr_len;
    uint8_t bytes[RTE_MBUF_DEFAULT_BUF_SIZE];
};

struct worker_args {
    const struct app_config *cfg;
    const struct packet_template *template;
    struct rte_mempool *mbuf_pool;
    struct shared_counters *counters;
    uint16_t queue_id;
    uint16_t num_workers;
};

static void handle_signal(int signo)
{
    (void)signo;
    keep_running = false;
}


static void init_packet_template(struct packet_template *template, const struct app_config *cfg)
{
    const uint16_t hdr_len = (uint16_t)(sizeof(struct rte_ether_hdr) +
        sizeof(struct rte_ipv4_hdr) + sizeof(struct rte_udp_hdr));
    const uint16_t payload_len = (uint16_t)(cfg->l2_pkt_len - hdr_len);
    struct rte_ether_hdr *eth = (struct rte_ether_hdr *)template->bytes;
    struct rte_ipv4_hdr *ip = (struct rte_ipv4_hdr *)(eth + 1);
    struct rte_udp_hdr *udp = (struct rte_udp_hdr *)(ip + 1);
    uint8_t *payload = (uint8_t *)(udp + 1);

    template->l2_pkt_len = cfg->l2_pkt_len;
    template->hdr_len = hdr_len;

    rte_ether_addr_copy(&cfg->dst_mac, &eth->dst_addr);
    rte_ether_addr_copy(&cfg->src_mac, &eth->src_addr);
    eth->ether_type = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4);

    ip->version_ihl = RTE_IPV4_VHL_DEF;
    ip->type_of_service = 0;
    ip->total_length = rte_cpu_to_be_16((uint16_t)(sizeof(*ip) + sizeof(*udp) + payload_len));
    ip->packet_id = 0;
    ip->fragment_offset = rte_cpu_to_be_16(RTE_IPV4_HDR_DF_FLAG);
    ip->time_to_live = 64;
    ip->next_proto_id = IPPROTO_UDP;
    ip->hdr_checksum = 0;
    ip->src_addr = rte_cpu_to_be_32(cfg->src_ip);
    ip->dst_addr = rte_cpu_to_be_32(cfg->dst_ip);

    udp->src_port = rte_cpu_to_be_16(cfg->src_port);
    udp->dst_port = rte_cpu_to_be_16(cfg->dst_port);
    udp->dgram_len = rte_cpu_to_be_16((uint16_t)(sizeof(*udp) + payload_len));
    udp->dgram_cksum = 0;

    memset(payload, PAYLOAD_PATTERN, payload_len);

    ip->hdr_checksum = rte_ipv4_cksum(ip);
    udp->dgram_cksum = rte_ipv4_udptcp_cksum(ip, udp);
}

static void fill_packet(struct rte_mbuf *mbuf, const struct packet_template *template,
    const struct app_config *cfg)
{
    const uint32_t base_src_ip_be = rte_cpu_to_be_32(cfg->src_ip);
    const uint32_t base_dst_ip_be = rte_cpu_to_be_32(cfg->dst_ip);
    const uint16_t base_src_port_be = rte_cpu_to_be_16(cfg->src_port);
    const uint16_t base_dst_port_be = rte_cpu_to_be_16(cfg->dst_port);
    struct rte_ether_hdr *eth;
    struct rte_ipv4_hdr *ip;
    struct rte_udp_hdr *udp;
    uint32_t new_src_ip_be = base_src_ip_be;
    uint32_t new_dst_ip_be = base_dst_ip_be;
    uint16_t new_src_port_be = base_src_port_be;
    uint16_t new_dst_port_be = base_dst_port_be;

    mbuf->data_len = template->l2_pkt_len;
    mbuf->pkt_len = template->l2_pkt_len;

    rte_memcpy(rte_pktmbuf_mtod(mbuf, void *), template->bytes, template->l2_pkt_len);

    eth = rte_pktmbuf_mtod(mbuf, struct rte_ether_hdr *);
    ip = (struct rte_ipv4_hdr *)(eth + 1);
    udp = (struct rte_udp_hdr *)(ip + 1);

    if (cfg->src_ip_rnd_mask != 0 || cfg->src_port_rnd_mask != 0) {
        uint32_t rand32 = rte_rand();

        if (cfg->src_ip_rnd_mask != 0) {
            uint32_t src_ip = (cfg->src_ip & ~cfg->src_ip_rnd_mask) |
                (rand32 & cfg->src_ip_rnd_mask);
            new_src_ip_be = rte_cpu_to_be_32(src_ip);
            ip->src_addr = new_src_ip_be;
        }
        if (cfg->src_port_rnd_mask != 0) {
            uint16_t src_port = (uint16_t)((cfg->src_port & ~cfg->src_port_rnd_mask) |
                ((uint16_t)(rand32 >> 16) & cfg->src_port_rnd_mask));
            new_src_port_be = rte_cpu_to_be_16(src_port);
            udp->src_port = new_src_port_be;
        }
    }

    if (cfg->dst_ip_rnd_mask != 0 || cfg->dst_port_rnd_mask != 0) {
        uint32_t rand32 = rte_rand();

        if (cfg->dst_ip_rnd_mask != 0) {
            uint32_t dst_ip = (cfg->dst_ip & ~cfg->dst_ip_rnd_mask) |
                (rand32 & cfg->dst_ip_rnd_mask);
            new_dst_ip_be = rte_cpu_to_be_32(dst_ip);
            ip->dst_addr = new_dst_ip_be;
        }
        if (cfg->dst_port_rnd_mask != 0) {
            uint16_t dst_port = (uint16_t)((cfg->dst_port & ~cfg->dst_port_rnd_mask) |
                ((uint16_t)(rand32 >> 16) & cfg->dst_port_rnd_mask));
            new_dst_port_be = rte_cpu_to_be_16(dst_port);
            udp->dst_port = new_dst_port_be;
        }
    }

    if (cfg->src_ip_rnd_mask != 0 || cfg->dst_ip_rnd_mask != 0 ||
        cfg->src_port_rnd_mask != 0 || cfg->dst_port_rnd_mask != 0) {
        uint16_t ip_cksum = ip->hdr_checksum;
        uint16_t udp_cksum = udp->dgram_cksum;

        if (new_src_ip_be != base_src_ip_be) {
            ip_cksum = cksum_update_u32(ip_cksum, base_src_ip_be, new_src_ip_be);
            udp_cksum = cksum_update_u32(udp_cksum, base_src_ip_be, new_src_ip_be);
        }
        if (new_dst_ip_be != base_dst_ip_be) {
            ip_cksum = cksum_update_u32(ip_cksum, base_dst_ip_be, new_dst_ip_be);
            udp_cksum = cksum_update_u32(udp_cksum, base_dst_ip_be, new_dst_ip_be);
        }
        if (new_src_port_be != base_src_port_be) {
            udp_cksum = cksum_update_u16(udp_cksum, base_src_port_be, new_src_port_be);
        }
        if (new_dst_port_be != base_dst_port_be) {
            udp_cksum = cksum_update_u16(udp_cksum, base_dst_port_be, new_dst_port_be);
        }

        ip->hdr_checksum = ip_cksum;
        udp->dgram_cksum = udp_cksum;
    }
}

static int port_init(uint16_t port_id, struct rte_mempool *mbuf_pool, uint16_t num_queues)
{
    struct rte_eth_conf port_conf = { 0 };
    struct rte_eth_dev_info dev_info;
    struct rte_eth_fc_conf fc_conf = {
        .mode = RTE_ETH_FC_NONE,
    };
    struct rte_eth_rxconf rxq_conf;
    struct rte_eth_txconf txq_conf;
    int ret;

    ret = rte_eth_dev_info_get(port_id, &dev_info);
    if (ret < 0) {
        return ret;
    }

    port_conf.rxmode.mq_mode = RTE_ETH_MQ_RX_RSS;
    port_conf.rx_adv_conf.rss_conf.rss_hf =
        RTE_ETH_RSS_IPV4 | RTE_ETH_RSS_NONFRAG_IPV4_UDP;
    port_conf.rx_adv_conf.rss_conf.rss_hf &= dev_info.flow_type_rss_offloads;
    port_conf.txmode.mq_mode = RTE_ETH_MQ_TX_NONE;

    ret = rte_eth_dev_configure(port_id, num_queues, num_queues, &port_conf);
    if (ret < 0) {
        return ret;
    }

    rxq_conf = dev_info.default_rxconf;
    rxq_conf.offloads = port_conf.rxmode.offloads;
    txq_conf = dev_info.default_txconf;
    txq_conf.offloads = port_conf.txmode.offloads;
    for (uint16_t queue_id = 0; queue_id < num_queues; queue_id++) {
        ret = rte_eth_rx_queue_setup(port_id, queue_id, RX_DESC_DEFAULT,
            rte_eth_dev_socket_id(port_id), &rxq_conf, mbuf_pool);
        if (ret < 0) {
            return ret;
        }

        ret = rte_eth_tx_queue_setup(port_id, queue_id, TX_DESC_DEFAULT,
            rte_eth_dev_socket_id(port_id), &txq_conf);
        if (ret < 0) {
            return ret;
        }
    }

    ret = rte_eth_dev_start(port_id);
    if (ret < 0) {
        return ret;
    }

    ret = rte_eth_dev_set_link_up(port_id);
    if (ret == -ENOTSUP || ret == -EOPNOTSUPP) {
        fprintf(stderr, "Warning: explicit link-up not supported on port %" PRIu16 "\n",
            port_id);
    } else if (ret < 0) {
        return ret;
    }

    ret = rte_eth_dev_flow_ctrl_set(port_id, &fc_conf);
    if (ret < 0) {
        fprintf(stderr, "Warning: could not disable flow control on port %" PRIu16 ": %s\n",
            port_id, rte_strerror(-ret));
    }

    rte_eth_promiscuous_enable(port_id);
    (void)mbuf_pool;
    return 0;
}

static int wait_for_link_up(uint16_t port_id)
{
    const unsigned int max_checks = 50;
    struct rte_eth_link link;

    printf("Waiting for port %" PRIu16 " link up...\n", port_id);
    for (unsigned int i = 0; i < max_checks; i++) {
        memset(&link, 0, sizeof(link));
        if (rte_eth_link_get_nowait(port_id, &link) == 0 &&
            link.link_status == RTE_ETH_LINK_UP) {
            printf("Port %" PRIu16 " link up: %u Mbps %s-duplex\n", port_id,
                link.link_speed,
                link.link_duplex == RTE_ETH_LINK_FULL_DUPLEX ? "full" : "half");
            return 0;
        }

        rte_delay_us_sleep(100000);
    }

    return -ETIMEDOUT;
}

static double monotonic_seconds(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static struct io_counters snapshot_counters(const struct shared_counters *counters)
{
    struct io_counters snapshot;

    snapshot.tx_packets = atomic_load_explicit(&counters->tx_packets, memory_order_relaxed);
    snapshot.tx_bytes = atomic_load_explicit(&counters->tx_bytes, memory_order_relaxed);
    snapshot.rx_packets = atomic_load_explicit(&counters->rx_packets, memory_order_relaxed);
    snapshot.rx_bytes = atomic_load_explicit(&counters->rx_bytes, memory_order_relaxed);
    return snapshot;
}

static void worker_drain_rx(struct worker_args *worker)
{
    struct rte_mbuf *rx_mbufs[BURST_MAX];
    uint16_t nb_rx;

    do {
        nb_rx = rte_eth_rx_burst(worker->cfg->port_id, worker->queue_id, rx_mbufs, BURST_MAX);
        if (nb_rx == 0) {
            break;
        }

        atomic_fetch_add_explicit(&worker->counters->rx_packets, nb_rx, memory_order_relaxed);
        for (uint16_t i = 0; i < nb_rx; i++) {
            atomic_fetch_add_explicit(&worker->counters->rx_bytes,
                rte_pktmbuf_pkt_len(rx_mbufs[i]), memory_order_relaxed);
            rte_pktmbuf_free(rx_mbufs[i]);
        }
    } while (nb_rx == BURST_MAX);
}

static int io_worker_main(void *arg)
{
    struct worker_args *worker = arg;
    const struct app_config *cfg = worker->cfg;
    struct rte_mbuf *tx_mbufs[BURST_MAX];
    const uint64_t pkt_len = cfg->l2_pkt_len;
    const uint64_t hz = rte_get_timer_hz();
    const double worker_rate_mpps = (cfg->rate_mpps > 0.0) ?
        (cfg->rate_mpps / worker->num_workers) : 0.0;
    const double cycles_per_packet = (worker_rate_mpps > 0.0) ?
        ((double)hz / (worker_rate_mpps * 1000000.0)) : 0.0;
    double next_tx_cycle = (double)rte_get_timer_cycles();
    uint64_t local_sent = 0;

    printf("lcore %u started\n", rte_lcore_id());

    while (keep_running) {
        uint16_t burst = cfg->burst_size;
        uint16_t i;
        uint16_t nb_tx;

        if (cycles_per_packet > 0.0) {
            double now = (double)rte_get_timer_cycles();

            if (next_tx_cycle > now) {
                worker_drain_rx(worker);
                rte_pause();
                continue;
            }

            burst = (uint16_t)((now - next_tx_cycle) / cycles_per_packet) + 1;
            if (burst == 0) {
                burst = 1;
            }
            if (burst > cfg->burst_size) {
                burst = cfg->burst_size;
            }
        }

        if (cfg->total_packets != 0) {
            uint64_t global_sent = atomic_load_explicit(&worker->counters->tx_packets,
                memory_order_relaxed);

            if (global_sent >= cfg->total_packets) {
                keep_running = false;
                break;
            }

            if ((cfg->total_packets - global_sent) < burst) {
                burst = (uint16_t)(cfg->total_packets - global_sent);
            }
        }

        for (i = 0; i < burst; i++) {
            tx_mbufs[i] = rte_pktmbuf_alloc(worker->mbuf_pool);
            if (tx_mbufs[i] == NULL) {
                break;
            }
            fill_packet(tx_mbufs[i], worker->template, cfg);
        }

        if (i == 0) {
            worker_drain_rx(worker);
            rte_pause();
            continue;
        }

        nb_tx = rte_eth_tx_burst(cfg->port_id, worker->queue_id, tx_mbufs, i);
        local_sent += nb_tx;
        atomic_fetch_add_explicit(&worker->counters->tx_packets, nb_tx, memory_order_relaxed);
        atomic_fetch_add_explicit(&worker->counters->tx_bytes,
            (uint64_t)nb_tx * pkt_len, memory_order_relaxed);
        if (cycles_per_packet > 0.0) {
            double now = (double)rte_get_timer_cycles();

            next_tx_cycle += (double)nb_tx * cycles_per_packet;
            if (next_tx_cycle < now - (cycles_per_packet * BURST_MAX)) {
                next_tx_cycle = now;
            }
        }

        for (uint16_t j = nb_tx; j < i; j++) {
            rte_pktmbuf_free(tx_mbufs[j]);
        }

        worker_drain_rx(worker);
    }

    if (cfg->total_packets != 0 && local_sent >= cfg->total_packets) {
        keep_running = false;
    }

    printf("lcore %u stopped\n", rte_lcore_id());
    return 0;
}

int main(int argc, char **argv)
{
    struct rte_mempool *mbuf_pool;
    struct app_config cfg = {
        .port_id = 0,
        .burst_size = BURST_DEFAULT,
        .l2_pkt_len = 64,
        .src_port = 1234,
        .src_port_rnd_mask = 0,
        .dst_port = 1234,
        .dst_port_rnd_mask = 0,
        .total_packets = 0,
        .src_ip = RTE_IPV4(10, 0, 0, 2),
        .src_ip_rnd_mask = 0,
        .dst_ip = RTE_IPV4(10, 0, 0, 1),
        .dst_ip_rnd_mask = 0,
        .dst_mac = {
            .addr_bytes = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff }
        },
    };
    struct packet_template packet_template;
    double last_report_sec;
    struct shared_counters shared = { 0 };
    struct io_counters prev_totals = { 0 };
    struct io_counters totals;
    struct rte_eth_stats final_stats;
    struct worker_args worker_args[RTE_MAX_LCORE];
    unsigned int worker_lcores[RTE_MAX_LCORE];
    uint16_t num_workers = 0;
    int ret;

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    ret = rte_eal_init(argc, argv);
    if (ret < 0) {
        rte_exit(EXIT_FAILURE, "Failed to initialize EAL\n");
    }

    argc -= ret;
    argv += ret;

    if (parse_app_args(argc, argv, &cfg) != 0) {
        usage("utg");
        rte_exit(EXIT_FAILURE, "Invalid application arguments\n");
    }
    if (!cfg.gw_ip_set) {
        cfg.gw_ip = cfg.dst_ip;
    }

    if (!rte_eth_dev_is_valid_port(cfg.port_id)) {
        rte_exit(EXIT_FAILURE, "Port %u is not available\n", cfg.port_id);
    }

    RTE_LCORE_FOREACH_WORKER(ret) {
        worker_lcores[num_workers++] = (unsigned int)ret;
    }

    if (num_workers == 0) {
        rte_exit(EXIT_FAILURE, "Need at least one worker lcore in addition to the main lcore\n");
    }

    mbuf_pool = rte_pktmbuf_pool_create("MBUF_POOL", NUM_MBUFS,
        MBUF_CACHE_SIZE, 0, RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id());
    if (mbuf_pool == NULL) {
        rte_exit(EXIT_FAILURE, "Cannot create mbuf pool: %s\n", rte_strerror(rte_errno));
    }

    ret = port_init(cfg.port_id, mbuf_pool, num_workers);
    if (ret < 0) {
        rte_exit(EXIT_FAILURE, "Cannot initialize port %" PRIu16 ": %s\n",
            cfg.port_id, rte_strerror(-ret));
    }

    ret = wait_for_link_up(cfg.port_id);
    if (ret < 0) {
        rte_exit(EXIT_FAILURE, "Port %" PRIu16 " link did not come up: %s\n",
            cfg.port_id, rte_strerror(-ret));
    }

    rte_eth_macaddr_get(cfg.port_id, &cfg.src_mac);

    if (!cfg.dst_mac_override) {
        printf("Resolving ARP for %s IP...\n", cfg.gw_ip_set ? "gateway" : "destination");
        if (resolve_arp(cfg.port_id, mbuf_pool, &cfg, &keep_running) != 0) {
            rte_exit(EXIT_FAILURE, "Failed to resolve ARP for next-hop IP\n");
        }
    }

    printf("Sending gratuitous ARP...\n");
    send_gratuitous_arp(cfg.port_id, mbuf_pool, &cfg);
    init_packet_template(&packet_template, &cfg);

    printf("Starting UDP generator on port %" PRIu16 "\n", cfg.port_id);
    printf("  %-10s %02X:%02X:%02X:%02X:%02X:%02X\n", "src_mac",
        cfg.src_mac.addr_bytes[0], cfg.src_mac.addr_bytes[1], cfg.src_mac.addr_bytes[2],
        cfg.src_mac.addr_bytes[3], cfg.src_mac.addr_bytes[4], cfg.src_mac.addr_bytes[5]);
    printf("  %-10s %02X:%02X:%02X:%02X:%02X:%02X\n", "dst_mac",
        cfg.dst_mac.addr_bytes[0], cfg.dst_mac.addr_bytes[1], cfg.dst_mac.addr_bytes[2],
        cfg.dst_mac.addr_bytes[3], cfg.dst_mac.addr_bytes[4], cfg.dst_mac.addr_bytes[5]);
    printf("  %-10s %" PRIu8 ".%" PRIu8 ".%" PRIu8 ".%" PRIu8 "\n", "src_ip",
        (uint8_t)(cfg.src_ip >> 24), (uint8_t)(cfg.src_ip >> 16),
        (uint8_t)(cfg.src_ip >> 8), (uint8_t)cfg.src_ip);
    printf("  %-10s %" PRIu8 ".%" PRIu8 ".%" PRIu8 ".%" PRIu8 "\n", "src_ip_mask",
        (uint8_t)(cfg.src_ip_rnd_mask >> 24), (uint8_t)(cfg.src_ip_rnd_mask >> 16),
        (uint8_t)(cfg.src_ip_rnd_mask >> 8), (uint8_t)cfg.src_ip_rnd_mask);
    printf("  %-10s %" PRIu8 ".%" PRIu8 ".%" PRIu8 ".%" PRIu8 "\n", "dst_ip",
        (uint8_t)(cfg.dst_ip >> 24), (uint8_t)(cfg.dst_ip >> 16),
        (uint8_t)(cfg.dst_ip >> 8), (uint8_t)cfg.dst_ip);
    printf("  %-10s %" PRIu8 ".%" PRIu8 ".%" PRIu8 ".%" PRIu8 "\n", "dst_ip_mask",
        (uint8_t)(cfg.dst_ip_rnd_mask >> 24), (uint8_t)(cfg.dst_ip_rnd_mask >> 16),
        (uint8_t)(cfg.dst_ip_rnd_mask >> 8), (uint8_t)cfg.dst_ip_rnd_mask);
    printf("  %-10s %" PRIu8 ".%" PRIu8 ".%" PRIu8 ".%" PRIu8 "%s\n", "gw_ip",
        (uint8_t)(cfg.gw_ip >> 24), (uint8_t)(cfg.gw_ip >> 16),
        (uint8_t)(cfg.gw_ip >> 8), (uint8_t)cfg.gw_ip,
        cfg.gw_ip_set ? "" : " (dst_ip)");
    printf("  %-10s %" PRIu16 "\n", "src_port", cfg.src_port);
    printf("  %-10s 0x%04" PRIx16 "\n", "src_port_mask", cfg.src_port_rnd_mask);
    printf("  %-10s %" PRIu16 "\n", "dst_port", cfg.dst_port);
    printf("  %-10s 0x%04" PRIx16 "\n", "dst_port_mask", cfg.dst_port_rnd_mask);
    printf("  %-10s %" PRIu16 "\n", "l2_pkt_len", cfg.l2_pkt_len);
    printf("  %-10s %" PRIu16 "\n", "burst", cfg.burst_size);
    printf("  %-10s %.3f Mpps\n", "rate", cfg.rate_mpps);
    printf("  %-10s %" PRIu16 "\n", "workers", num_workers);
    printf("  %-10s %" PRIu64 "\n", "count", cfg.total_packets);
    printf("  %-10s main=%u\n", "threads", rte_lcore_id());
    for (uint16_t i = 0; i < num_workers; i++) {
        printf("  %-10s worker=%u queue=%" PRIu16 "\n", "",
            worker_lcores[i], i);
    }

    for (uint16_t i = 0; i < num_workers; i++) {
        worker_args[i].cfg = &cfg;
        worker_args[i].template = &packet_template;
        worker_args[i].mbuf_pool = mbuf_pool;
        worker_args[i].counters = &shared;
        worker_args[i].queue_id = i;
        worker_args[i].num_workers = num_workers;

        ret = rte_eal_remote_launch(io_worker_main, &worker_args[i], worker_lcores[i]);
        if (ret != 0) {
            keep_running = false;
            for (uint16_t j = 0; j < i; j++) {
                rte_eal_wait_lcore(worker_lcores[j]);
            }
            rte_exit(EXIT_FAILURE, "Failed to launch worker on lcore %u\n", worker_lcores[i]);
        }
    }

    last_report_sec = monotonic_seconds();
    print_stats_header();

    while (keep_running) {
        double now_sec = monotonic_seconds();
        double interval_sec = now_sec - last_report_sec;

        if (interval_sec >= 1.0) {
            totals = snapshot_counters(&shared);
            print_rate_line(&prev_totals, &totals, interval_sec);
            prev_totals = totals;
            last_report_sec = now_sec;
        }

        rte_delay_us_sleep(10000);
    }

    for (uint16_t i = 0; i < num_workers; i++) {
        rte_eal_wait_lcore(worker_lcores[i]);
    }
    totals = snapshot_counters(&shared);
    if (rte_eth_stats_get(cfg.port_id, &final_stats) == 0) {
        double interval_sec = monotonic_seconds() - last_report_sec;
        if (interval_sec > 0.0) {
            print_rate_line(&prev_totals, &totals, interval_sec);
        }
        print_final_stats(cfg.port_id, num_workers, &totals, &final_stats);
    }
    rte_eth_dev_stop(cfg.port_id);
    rte_eth_dev_close(cfg.port_id);
    rte_eal_cleanup();
    return 0;
}
