#include "stats.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

void print_stats_header(void)
{
    printf("\n");
    printf("       TX              RX\n");
    printf("  [Mpps]  [Gb/s]  [Mpps]  [Gb/s]\n");
}

void print_rate_line(const struct io_counters *prev, const struct io_counters *curr,
    double interval_sec)
{
    const double tx_mpps = (double)(curr->tx_packets - prev->tx_packets) / interval_sec / 1e6;
    const double rx_mpps = (double)(curr->rx_packets - prev->rx_packets) / interval_sec / 1e6;
    const double tx_gbps = (double)(curr->tx_bytes - prev->tx_bytes) * 8.0 / interval_sec / 1e9;
    const double rx_gbps = (double)(curr->rx_bytes - prev->rx_bytes) * 8.0 / interval_sec / 1e9;

    printf("%8.2f %7.2f %8.2f %7.2f\n", tx_mpps, tx_gbps, rx_mpps, rx_gbps);
}

static bool xstat_matches_queue(const char *name, uint16_t queue_id)
{
    char pattern[32];
    const char *patterns[] = {
        "q%u",
        "q_%u",
        "queue%u",
        "queue_%u",
        "rx_q%u",
        "tx_q%u",
        "rxq%u",
        "txq%u",
    };

    for (size_t i = 0; i < sizeof(patterns) / sizeof(patterns[0]); i++) {
        snprintf(pattern, sizeof(pattern), patterns[i], queue_id);
        if (strstr(name, pattern) != NULL) {
            return true;
        }
    }

    return false;
}

static void print_per_queue_xstats(uint16_t num_queues,
    const struct rte_eth_xstat_name *names, const struct rte_eth_xstat *values, int nb_xstats)
{
    bool printed_any = false;

    for (uint16_t queue_id = 0; queue_id < num_queues; queue_id++) {
        bool printed_queue = false;

        for (int i = 0; i < nb_xstats; i++) {
            if (values[i].value == 0 || !xstat_matches_queue(names[i].name, queue_id)) {
                continue;
            }

            if (!printed_any) {
                printf("\nPer-Queue Xstats\n");
                printf("%-8s %-40s %18s\n", "queue", "name", "value");
                printed_any = true;
            }
            printed_queue = true;
            printf("%-8" PRIu16 " %-40s %18" PRIu64 "\n",
                queue_id, names[i].name, values[i].value);
        }

        if (printed_any && printed_queue) {
            printf("\n");
        }
    }
}

static void print_other_xstats(const struct rte_eth_xstat_name *names,
    const struct rte_eth_xstat *values, int nb_xstats, uint16_t num_queues)
{
    bool printed = false;

    for (int i = 0; i < nb_xstats; i++) {
        bool matched_queue = false;

        if (values[i].value == 0) {
            continue;
        }

        for (uint16_t queue_id = 0; queue_id < num_queues; queue_id++) {
            if (xstat_matches_queue(names[i].name, queue_id)) {
                matched_queue = true;
                break;
            }
        }

        if (matched_queue) {
            continue;
        }

        if (!printed) {
            printf("Other Xstats\n");
            printf("%-40s %18s\n", "name", "value");
            printed = true;
        }
        printf("%-40s %18" PRIu64 "\n", names[i].name, values[i].value);
    }
}

void print_final_stats(uint16_t port_id, uint16_t num_queues, const struct io_counters *totals,
    const struct rte_eth_stats *stats)
{
    int nb_xstats;
    struct rte_eth_xstat_name *names = NULL;
    struct rte_eth_xstat *values = NULL;

    printf("\nFinal Stats\n");
    printf("%-18s %18s\n", "counter", "value");
    printf("%-18s %18" PRIu64 "\n", "sw_tx_packets", totals->tx_packets);
    printf("%-18s %18" PRIu64 "\n", "sw_tx_bytes", totals->tx_bytes);
    printf("%-18s %18" PRIu64 "\n", "sw_rx_packets", totals->rx_packets);
    printf("%-18s %18" PRIu64 "\n", "sw_rx_bytes", totals->rx_bytes);
    printf("%-18s %18" PRIu64 "\n", "nic_tx_packets", stats->opackets);
    printf("%-18s %18" PRIu64 "\n", "nic_tx_bytes", stats->obytes);
    printf("%-18s %18" PRIu64 "\n", "nic_rx_packets", stats->ipackets);
    printf("%-18s %18" PRIu64 "\n", "nic_rx_bytes", stats->ibytes);
    printf("%-18s %18" PRIu64 "\n", "nic_tx_errors", stats->oerrors);
    printf("%-18s %18" PRIu64 "\n", "nic_rx_errors", stats->ierrors);
    printf("%-18s %18" PRIu64 "\n", "nic_rx_missed", stats->imissed);
    printf("%-18s %18" PRIu64 "\n", "nic_rx_nombuf", stats->rx_nombuf);

    nb_xstats = rte_eth_xstats_get_names(port_id, NULL, 0);
    if (nb_xstats <= 0) {
        return;
    }

    names = calloc((size_t)nb_xstats, sizeof(*names));
    values = calloc((size_t)nb_xstats, sizeof(*values));
    if (names == NULL || values == NULL) {
        free(names);
        free(values);
        fprintf(stderr, "Warning: unable to allocate xstats buffers\n");
        return;
    }

    if (rte_eth_xstats_get_names(port_id, names, nb_xstats) != nb_xstats ||
        rte_eth_xstats_get(port_id, values, nb_xstats) != nb_xstats) {
        free(names);
        free(values);
        fprintf(stderr, "Warning: unable to read xstats on port %" PRIu16 "\n", port_id);
        return;
    }

    print_per_queue_xstats(num_queues, names, values, nb_xstats);
    print_other_xstats(names, values, nb_xstats, num_queues);

    free(names);
    free(values);
}
