#ifndef PCITOP_H
#define PCITOP_H

#include <pci/pci.h>
#include <stdbool.h>

#define MAX_CHILDREN 256

// Node representing a PCI device in the topology tree
typedef struct PciNode {
    struct pci_dev *dev;
    char bdf_str[16];
    char vendor_str[128];
    char device_str[128];
    
    // Link speeds
    float max_speed;
    int max_width;
    float cur_speed;
    int cur_width;
    float device_max_speed; // Hardware capability from LNKCAP2
    char dbdf_str[32];
    bool hidden;
    float rx_rate; // MB/s
    float tx_rate; // MB/s

    // Topology
    int primary_bus;
    int secondary_bus;
    int subordinate_bus;
    bool is_bridge;

    struct PciNode *parent;
    struct PciNode *children[MAX_CHILDREN];
    int num_children;
} PciNode;

// Function prototypes
struct pci_access *init_pci_access(void);
void cleanup_pci_access(struct pci_access *pacc);

// Scans devices and returns an array of root nodes (and updates num_roots)
PciNode **build_pci_tree(struct pci_access *pacc, int *num_roots, PciNode **out_all_nodes);
void free_pci_tree(PciNode **roots, PciNode *all_nodes);

// String replacement for vendors
void shorten_vendor_name(char *vendor_str);

// Settings management
void load_settings(void);
void save_settings(void);
bool is_hidden(const char *dbdf);
void toggle_hidden(const char *dbdf);

// Throughput management
void update_throughput(PciNode *all_nodes, int total_devices);

#endif // PCITOP_H
