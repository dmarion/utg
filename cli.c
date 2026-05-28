#include "cli.h"

#include <arpa/inet.h>
#include <getopt.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <rte_byteorder.h>
#include <rte_ether.h>
#include <rte_ip.h>
#include <rte_udp.h>

static int parse_mac(const char *text, struct rte_ether_addr *addr)
{
    struct rte_ether_addr tmp;

    if (rte_ether_unformat_addr(text, &tmp) != 0) {
        return -1;
    }

    *addr = tmp;
    return 0;
}

static int parse_ipv4(const char *text, uint32_t *out)
{
    struct in_addr addr;

    if (inet_pton(AF_INET, text, &addr) != 1) {
        return -1;
    }

    *out = rte_be_to_cpu_32(addr.s_addr);
    return 0;
}

static int parse_ipv4_mask(const char *text, uint32_t *out)
{
    return parse_ipv4(text, out);
}

static int parse_u16(const char *text, uint16_t *out)
{
    char *end = NULL;
    unsigned long value = strtoul(text, &end, 10);

    if (text[0] == '\0' || *end != '\0' || value > UINT16_MAX) {
        return -1;
    }

    *out = (uint16_t)value;
    return 0;
}

static int parse_u64(const char *text, uint64_t *out)
{
    char *end = NULL;
    unsigned long long value = strtoull(text, &end, 10);

    if (text[0] == '\0' || *end != '\0') {
        return -1;
    }

    *out = (uint64_t)value;
    return 0;
}

static int parse_double(const char *text, double *out)
{
    char *end = NULL;
    double value = strtod(text, &end);

    if (text[0] == '\0' || *end != '\0' || value < 0.0) {
        return -1;
    }

    *out = value;
    return 0;
}

void usage(const char *prog)
{
    printf(
        "Usage:\n"
        "  %s [EAL args] -- [options]\n"
        "\n"
        "Options:\n"
        "  --port-id N         DPDK port to use (default: 0)\n"
        "  --src-ip IPv4       Source IPv4 address (default: 10.0.0.2)\n"
        "  --src-ip-rnd-mask IPv4\n"
        "                     Randomize masked source-IP bits per packet (default: 0.0.0.0)\n"
        "  --dst-ip IPv4       Destination IPv4 address (default: 10.0.0.1)\n"
        "  --dst-ip-rnd-mask IPv4\n"
        "                     Randomize masked destination-IP bits per packet (default: 0.0.0.0)\n"
        "  --gw-ip IPv4        Next-hop IPv4 address to resolve with ARP (default: dst-ip)\n"
        "  --dst-mac MAC       Destination MAC address (default: ff:ff:ff:ff:ff:ff)\n"
        "  --src-port N        UDP source port (default: 1234)\n"
        "  --src-port-rnd-mask N\n"
        "                     Randomize masked source-port bits per packet (default: 0)\n"
        "  --dst-port N        UDP destination port (default: 1234)\n"
        "  --dst-port-rnd-mask N\n"
        "                     Randomize masked destination-port bits per packet (default: 0)\n"
        "  --l2-pkt-len N      Total L2 packet bytes (default: 64)\n"
        "  --burst N           Packets per TX burst (default: 64)\n"
        "  --count N           Total packets to send, 0 means infinite (default: 0)\n"
        "  --rate F            Target TX rate in Mpps, 0 means line-rate (default: 0)\n"
        "\n"
        "Example:\n"
        "  %s -l 0-1 -n 4 -- --src-ip 10.0.0.2 --dst-ip 10.0.0.1 \\\n"
        "      --dst-port 20000 --rate 0.3 --count 1000000\n",
        prog, prog);
}

int parse_app_args(int argc, char **argv, struct app_config *cfg)
{
    static const struct option long_opts[] = {
        { "port-id", required_argument, NULL, 'p' },
        { "dst-mac", required_argument, NULL, 'm' },
        { "src-ip", required_argument, NULL, 's' },
        { "src-ip-rnd-mask", required_argument, NULL, 'S' },
        { "dst-ip", required_argument, NULL, 'd' },
        { "dst-ip-rnd-mask", required_argument, NULL, 'D' },
        { "gw-ip", required_argument, NULL, 'g' },
        { "src-port", required_argument, NULL, 'q' },
        { "src-port-rnd-mask", required_argument, NULL, 'x' },
        { "dst-port", required_argument, NULL, 'r' },
        { "dst-port-rnd-mask", required_argument, NULL, 'y' },
        { "l2-pkt-len", required_argument, NULL, 'l' },
        { "burst", required_argument, NULL, 'b' },
        { "count", required_argument, NULL, 'c' },
        { "rate", required_argument, NULL, 't' },
        { NULL, 0, NULL, 0 }
    };
    int opt;

    optind = 1;
    while ((opt = getopt_long(argc, argv, "", long_opts, NULL)) != -1) {
        switch (opt) {
        case 'p':
            if (parse_u16(optarg, &cfg->port_id) != 0) {
                return -1;
            }
            break;
        case 'm':
            if (parse_mac(optarg, &cfg->dst_mac) != 0) {
                return -1;
            }
            cfg->dst_mac_override = true;
            break;
        case 's':
            if (parse_ipv4(optarg, &cfg->src_ip) != 0) {
                return -1;
            }
            break;
        case 'S':
            if (parse_ipv4_mask(optarg, &cfg->src_ip_rnd_mask) != 0) {
                return -1;
            }
            break;
        case 'd':
            if (parse_ipv4(optarg, &cfg->dst_ip) != 0) {
                return -1;
            }
            break;
        case 'D':
            if (parse_ipv4_mask(optarg, &cfg->dst_ip_rnd_mask) != 0) {
                return -1;
            }
            break;
        case 'g':
            if (parse_ipv4(optarg, &cfg->gw_ip) != 0) {
                return -1;
            }
            cfg->gw_ip_set = true;
            break;
        case 'q':
            if (parse_u16(optarg, &cfg->src_port) != 0) {
                return -1;
            }
            break;
        case 'x':
            if (parse_u16(optarg, &cfg->src_port_rnd_mask) != 0) {
                return -1;
            }
            break;
        case 'r':
            if (parse_u16(optarg, &cfg->dst_port) != 0) {
                return -1;
            }
            break;
        case 'y':
            if (parse_u16(optarg, &cfg->dst_port_rnd_mask) != 0) {
                return -1;
            }
            break;
        case 'l':
            if (parse_u16(optarg, &cfg->l2_pkt_len) != 0) {
                return -1;
            }
            break;
        case 'b':
            if (parse_u16(optarg, &cfg->burst_size) != 0 ||
                cfg->burst_size == 0 || cfg->burst_size > BURST_MAX) {
                return -1;
            }
            break;
        case 'c':
            if (parse_u64(optarg, &cfg->total_packets) != 0) {
                return -1;
            }
            break;
        case 't':
            if (parse_double(optarg, &cfg->rate_mpps) != 0) {
                return -1;
            }
            break;
        default:
            return -1;
        }
    }

    if (cfg->l2_pkt_len < (sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv4_hdr) +
            sizeof(struct rte_udp_hdr))) {
        return -1;
    }

    return 0;
}
