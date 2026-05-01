#include "pcitop.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <dirent.h>
#include <sys/time.h>

typedef struct {
    char dbdf[32];
    unsigned long long last_rx_bytes;
    unsigned long long last_tx_bytes;
    double last_time;
    double acc_rx_mb;
    double acc_tx_mb;
    double gui_base_rx;
    double gui_base_tx;
} DeviceStats;

#define MAX_STATS 1024
static DeviceStats global_stats[MAX_STATS];
static int num_stats = 0;

static double get_time_sec() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec + tv.tv_usec / 1000000.0;
}

static DeviceStats* get_or_create_stats(const char *dbdf) {
    for (int i = 0; i < num_stats; i++) {
        if (strcasecmp(global_stats[i].dbdf, dbdf) == 0) return &global_stats[i];
    }
    if (num_stats < MAX_STATS) {
        strncpy(global_stats[num_stats].dbdf, dbdf, 31);
        global_stats[num_stats].last_rx_bytes = 0;
        global_stats[num_stats].last_tx_bytes = 0;
        global_stats[num_stats].last_time = 0;
        global_stats[num_stats].acc_rx_mb = 0;
        global_stats[num_stats].acc_tx_mb = 0;
        global_stats[num_stats].gui_base_rx = 0;
        global_stats[num_stats].gui_base_tx = 0;
        return &global_stats[num_stats++];
    }
    return NULL;
}

void mark_gui_cycle(void) {
    for (int i = 0; i < num_stats; i++) {
        global_stats[i].gui_base_rx = global_stats[i].acc_rx_mb;
        global_stats[i].gui_base_tx = global_stats[i].acc_tx_mb;
    }
}

void reset_throughput(void) {
    for (int i = 0; i < num_stats; i++) {
        global_stats[i].acc_rx_mb = 0;
        global_stats[i].acc_tx_mb = 0;
        global_stats[i].gui_base_rx = 0;
        global_stats[i].gui_base_tx = 0;
    }
}

static void normalize_dbdf(const char *in, char *out) {
    unsigned int d, b, s, f;
    if (sscanf(in, "%x:%x:%x.%x", &d, &b, &s, &f) == 4) {
        snprintf(out, 32, "%04x:%02x:%02x.%d", d, b, s, f);
    } else {
        strncpy(out, in, 31);
        out[31] = '\0';
    }
}

void update_nvidia_throughput(PciNode *all_nodes, int total_devices) {
    double now = get_time_sec();
    // Get mapping: index -> DBDF
    FILE *fp = popen("nvidia-smi --query-gpu=index,pci.bus_id --format=csv,noheader,nounits", "r");
    if (!fp) return;

    char line[128];
    char gpu_mappings[16][32]; // Max 16 GPUs
    int max_gpu_idx = -1;
    while (fgets(line, sizeof(line), fp)) {
        int idx;
        char dbdf_raw[64];
        if (sscanf(line, "%d, %s", &idx, dbdf_raw) == 2) {
            if (idx < 16) {
                normalize_dbdf(dbdf_raw, gpu_mappings[idx]);
                if (idx > max_gpu_idx) max_gpu_idx = idx;
            }
        }
    }
    pclose(fp);

    // Get rates
    fp = popen("nvidia-smi dmon -s t -c 1", "r");
    if (!fp) return;

    while (fgets(line, sizeof(line), fp)) {
        if (line[0] == '#' || line[0] == '\n' || strstr(line, "Idx")) continue;
        int idx;
        float rx, tx;
        if (sscanf(line, "%d %f %f", &idx, &rx, &tx) == 3) {
            if (idx >= 0 && idx <= max_gpu_idx) {
                // Find node with this DBDF
                for (int i = 0; i < total_devices; i++) {
                    if (strcasecmp(all_nodes[i].dbdf_str, gpu_mappings[idx]) == 0) {
                        all_nodes[i].rx_rate = rx;
                        all_nodes[i].tx_rate = tx;
                        
                        DeviceStats *s = get_or_create_stats(all_nodes[i].dbdf_str);
                        if (s && s->last_time > 0) {
                            double dt = now - s->last_time;
                            if (dt > 0) {
                                s->acc_rx_mb += rx * dt;
                                s->acc_tx_mb += tx * dt;
                            }
                        }
                        if (s) {
                            s->last_time = now;
                            all_nodes[i].total_rx = (float)s->acc_rx_mb;
                            all_nodes[i].total_tx = (float)s->acc_tx_mb;
                            all_nodes[i].interval_rx = (float)(s->acc_rx_mb - s->gui_base_rx);
                            all_nodes[i].interval_tx = (float)(s->acc_tx_mb - s->gui_base_tx);
                        }
                    }
                }
            }
        }
    }
    pclose(fp);
}

void update_net_throughput(PciNode *all_nodes, int total_devices) {
    double now = get_time_sec();
    for (int i = 0; i < total_devices; i++) {
        char path[256];
        snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/net", all_nodes[i].dbdf_str);
        DIR *dir = opendir(path);
        if (dir) {
            struct dirent *entry;
            while ((entry = readdir(dir)) != NULL) {
                if (entry->d_name[0] == '.') continue;
                
                char stat_path_rx[512];
                char stat_path_tx[512];
                snprintf(stat_path_rx, sizeof(stat_path_rx), "%s/%s/statistics/rx_bytes", path, entry->d_name);
                snprintf(stat_path_tx, sizeof(stat_path_tx), "%s/%s/statistics/tx_bytes", path, entry->d_name);
                
                FILE *f_rx = fopen(stat_path_rx, "r");
                FILE *f_tx = fopen(stat_path_tx, "r");
                if (f_rx && f_tx) {
                    unsigned long long rx_bytes, tx_bytes;
                    if (fscanf(f_rx, "%llu", &rx_bytes) == 1 && fscanf(f_tx, "%llu", &tx_bytes) == 1) {
                        DeviceStats *s = get_or_create_stats(all_nodes[i].dbdf_str);
                        if (s && s->last_time > 0) {
                            double dt = now - s->last_time;
                            if (dt > 0) {
                                float drx = (float)((rx_bytes - s->last_rx_bytes) / 1024.0 / 1024.0);
                                float dtx = (float)((tx_bytes - s->last_tx_bytes) / 1024.0 / 1024.0);
                                all_nodes[i].rx_rate = (float)(drx / dt);
                                all_nodes[i].tx_rate = (float)(dtx / dt);
                                s->acc_rx_mb += drx;
                                s->acc_tx_mb += dtx;
                            }
                        }
                        if (s) {
                            s->last_rx_bytes = rx_bytes;
                            s->last_tx_bytes = tx_bytes;
                            s->last_time = now;
                            all_nodes[i].total_rx = (float)s->acc_rx_mb;
                            all_nodes[i].total_tx = (float)s->acc_tx_mb;
                            all_nodes[i].interval_rx = (float)(s->acc_rx_mb - s->gui_base_rx);
                            all_nodes[i].interval_tx = (float)(s->acc_tx_mb - s->gui_base_tx);
                        }
                    }
                }
                if (f_rx) fclose(f_rx);
                if (f_tx) fclose(f_tx);
                break;
            }
            closedir(dir);
        }
    }
}

void update_nvme_throughput(PciNode *all_nodes, int total_devices) {
    double now = get_time_sec();
    for (int i = 0; i < total_devices; i++) {
        char path[256];
        snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/nvme", all_nodes[i].dbdf_str);
        DIR *dir = opendir(path);
        if (dir) {
            struct dirent *entry;
            while ((entry = readdir(dir)) != NULL) {
                if (entry->d_name[0] == '.') continue;
                
                char ns_path[512];
                snprintf(ns_path, sizeof(ns_path), "%s/%s", path, entry->d_name);
                DIR *ns_dir = opendir(ns_path);
                if (ns_dir) {
                    struct dirent *ns_entry;
                    while ((ns_entry = readdir(ns_dir)) != NULL) {
                        if (strstr(ns_entry->d_name, "nvme") && strstr(ns_entry->d_name, "n1")) {
                            char stat_path[1024];
                            snprintf(stat_path, sizeof(stat_path), "%s/%s/stat", ns_path, ns_entry->d_name);
                            FILE *f = fopen(stat_path, "r");
                            if (f) {
                                unsigned long long rd_sectors, wr_sectors;
                                unsigned long long dummy1, dummy2, dummy4, dummy5, dummy6;
                                if (fscanf(f, "%llu %llu %llu %llu %llu %llu %llu", 
                                           &dummy1, &dummy2, &rd_sectors, &dummy4,
                                           &dummy5, &dummy6, &wr_sectors) == 7) {
                                    
                                    unsigned long long rx_bytes = rd_sectors * 512;
                                    unsigned long long tx_bytes = wr_sectors * 512;
                                    
                                    DeviceStats *s = get_or_create_stats(all_nodes[i].dbdf_str);
                                    if (s && s->last_time > 0) {
                                        double dt = now - s->last_time;
                                        if (dt > 0) {
                                            float drx = (float)((rx_bytes - s->last_rx_bytes) / 1024.0 / 1024.0);
                                            float dtx = (float)((tx_bytes - s->last_tx_bytes) / 1024.0 / 1024.0);
                                            all_nodes[i].rx_rate = (float)(drx / dt);
                                            all_nodes[i].tx_rate = (float)(dtx / dt);
                                            s->acc_rx_mb += drx;
                                            s->acc_tx_mb += dtx;
                                        }
                                    }
                                    if (s) {
                                        s->last_rx_bytes = rx_bytes;
                                        s->last_tx_bytes = tx_bytes;
                                        s->last_time = now;
                                        all_nodes[i].total_rx = (float)s->acc_rx_mb;
                                        all_nodes[i].total_tx = (float)s->acc_tx_mb;
                                        all_nodes[i].interval_rx = (float)(s->acc_rx_mb - s->gui_base_rx);
                                        all_nodes[i].interval_tx = (float)(s->acc_tx_mb - s->gui_base_tx);
                                    }
                                }
                                fclose(f);
                            }
                            break;
                        }
                    }
                    closedir(ns_dir);
                }
                break;
            }
            closedir(dir);
        }
    }
}

void update_throughput(PciNode *all_nodes, int total_devices) {
    update_nvidia_throughput(all_nodes, total_devices);
    update_net_throughput(all_nodes, total_devices);
    update_nvme_throughput(all_nodes, total_devices);
}
