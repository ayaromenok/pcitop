#include "pcitop.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// PCI Express Capability Registers
#ifndef PCI_EXP_LNKCAP
#define PCI_EXP_LNKCAP 0x0c
#endif
#ifndef PCI_EXP_LNKSTA
#define PCI_EXP_LNKSTA 0x12
#endif
#ifndef PCI_EXP_LNKCAP2
#define PCI_EXP_LNKCAP2 0x2c
#endif

struct pci_access *init_pci_access(void) {
    struct pci_access *pacc = pci_alloc();
    pci_init(pacc);
    return pacc;
}

void cleanup_pci_access(struct pci_access *pacc) {
    pci_cleanup(pacc);
}

void shorten_vendor_name(char *vendor_str) {
    if (!vendor_str) return;
    
    // Check and replace specific long strings
    if (strstr(vendor_str, "Advanced Micro Devices, Inc.") != NULL) {
        strcpy(vendor_str, "AMD");
    } else if (strstr(vendor_str, "NVIDIA Corporation") != NULL) {
        strcpy(vendor_str, "NV");
    } else if (strstr(vendor_str, "Realtek Semiconductor Co., Ltd.") != NULL) {
        strcpy(vendor_str, "RTL");
    } else if (strstr(vendor_str, "ADATA Technology Co., Ltd.") != NULL) {
        strcpy(vendor_str, "ADT");
    }
}

static float decode_link_speed(int speed_code) {
    switch (speed_code) {
        case 1: return 2.5f;
        case 2: return 5.0f;
        case 3: return 8.0f;
        case 4: return 16.0f;
        case 5: return 32.0f;
        case 6: return 64.0f;
        default: return 0.0f;
    }
}

static void read_pcie_link_info(struct pci_dev *dev, float *max_spd, int *max_wd, float *cur_spd, int *cur_wd, float *dev_max_spd) {
    *max_spd = 0.0f; *max_wd = 0;
    *cur_spd = 0.0f; *cur_wd = 0;
    *dev_max_spd = 0.0f;

    struct pci_cap *cap = pci_find_cap(dev, PCI_CAP_ID_EXP, PCI_CAP_NORMAL);
    if (!cap) return;
    int cap_pos = cap->addr;

    // LnkCap is 32-bit at cap_pos + 0x0c
    // max speed: bits 0-3
    // max width: bits 4-9
    u32 lnkcap = pci_read_long(dev, cap_pos + PCI_EXP_LNKCAP);
    *max_spd = decode_link_speed(lnkcap & 0xf);
    *max_wd = (lnkcap >> 4) & 0x3f;

    // LnkSta is 16-bit at cap_pos + 0x12
    // cur speed: bits 0-3
    // cur width: bits 4-9
    u16 lnksta = pci_read_word(dev, cap_pos + PCI_EXP_LNKSTA);
    *cur_spd = decode_link_speed(lnksta & 0xf);
    *cur_wd = (lnksta >> 4) & 0x3f;

    // LnkCap2 (PCIe 2.0+) at cap_pos + 0x2c
    // Supported Link Speeds Vector: bits 7:1
    u32 lnkcap2 = pci_read_long(dev, cap_pos + PCI_EXP_LNKCAP2);
    if (lnkcap2 > 0 && lnkcap2 != 0xffffffff) {
        if (lnkcap2 & (1 << 6)) *dev_max_spd = 64.0f;
        else if (lnkcap2 & (1 << 5)) *dev_max_spd = 32.0f;
        else if (lnkcap2 & (1 << 4)) *dev_max_spd = 16.0f;
        else if (lnkcap2 & (1 << 3)) *dev_max_spd = 8.0f;
        else if (lnkcap2 & (1 << 2)) *dev_max_spd = 5.0f;
        else if (lnkcap2 & (1 << 1)) *dev_max_spd = 2.5f;
    }
    if (*dev_max_spd == 0.0f) *dev_max_spd = *max_spd;
}

PciNode **build_pci_tree(struct pci_access *pacc, int *num_roots, PciNode **out_all_nodes) {
    pci_scan_bus(pacc);

    int total_devices = 0;
    for (struct pci_dev *dev = pacc->devices; dev; dev = dev->next) {
        total_devices++;
    }

    if (total_devices == 0) {
        *num_roots = 0;
        if (out_all_nodes) *out_all_nodes = NULL;
        return NULL;
    }

    PciNode *all_nodes = calloc(total_devices, sizeof(PciNode));
    if (out_all_nodes) *out_all_nodes = all_nodes;
    int idx = 0;

    for (struct pci_dev *dev = pacc->devices; dev; dev = dev->next) {
        pci_fill_info(dev, PCI_FILL_IDENT | PCI_FILL_CLASS | PCI_FILL_CAPS);
        
        PciNode *node = &all_nodes[idx++];
        node->dev = dev;
        snprintf(node->bdf_str, sizeof(node->bdf_str), "%02x:%02x.%d", dev->bus, dev->dev, dev->func);
        snprintf(node->dbdf_str, sizeof(node->dbdf_str), "%04x:%02x:%02x.%d", dev->domain, dev->bus, dev->dev, dev->func);
        node->hidden = is_hidden(node->dbdf_str);
        node->rx_rate = 0.0f;
        node->tx_rate = 0.0f;
        node->total_rx = 0.0f;
        node->total_tx = 0.0f;
        
        char vendor_buf[256];
        char device_buf[256];
        pci_lookup_name(pacc, vendor_buf, sizeof(vendor_buf), PCI_LOOKUP_VENDOR, dev->vendor_id, 0);
        pci_lookup_name(pacc, device_buf, sizeof(device_buf), PCI_LOOKUP_DEVICE, dev->vendor_id, dev->device_id);

        strncpy(node->vendor_str, vendor_buf, sizeof(node->vendor_str) - 1);
        shorten_vendor_name(node->vendor_str);
        
        strncpy(node->device_str, device_buf, sizeof(node->device_str) - 1);
        
        read_pcie_link_info(dev, &node->max_speed, &node->max_width, &node->cur_speed, &node->cur_width, &node->device_max_speed);

        // Check if bridge
        u8 header_type = pci_read_byte(dev, PCI_HEADER_TYPE) & 0x7f;
        if (header_type == PCI_HEADER_TYPE_BRIDGE || header_type == PCI_HEADER_TYPE_CARDBUS) {
            node->is_bridge = true;
            node->primary_bus = pci_read_byte(dev, PCI_PRIMARY_BUS);
            node->secondary_bus = pci_read_byte(dev, PCI_SECONDARY_BUS);
            node->subordinate_bus = pci_read_byte(dev, PCI_SUBORDINATE_BUS);
        } else {
            node->is_bridge = false;
        }
    }

    // Now link parents and children
    PciNode **roots = malloc(total_devices * sizeof(PciNode*));
    *num_roots = 0;

    for (int i = 0; i < total_devices; i++) {
        PciNode *node = &all_nodes[i];
        
        // Find parent: a bridge whose secondary bus matches the node's bus, 
        // or a broader match if not strictly secondary. 
        // Usually, node->dev->bus matches parent's secondary_bus.
        PciNode *parent = NULL;
        for (int j = 0; j < total_devices; j++) {
            PciNode *pot_parent = &all_nodes[j];
            if (pot_parent->is_bridge && pot_parent->secondary_bus == node->dev->bus) {
                // To break ties or odd cases, we might want to check subordinate bus too
                // but secondary_bus == bus is the direct parent.
                parent = pot_parent;
                break;
            }
        }
        
        // Sometimes devices are on bus 0, which has no upstream bridge in the list
        if (parent) {
            node->parent = parent;
            if (parent->num_children < MAX_CHILDREN) {
                parent->children[parent->num_children++] = node;
            }
        } else {
            roots[(*num_roots)++] = node;
        }
    }

    return roots;
}

void free_pci_tree(PciNode **roots, PciNode *all_nodes) {
    if (all_nodes) free(all_nodes);
    if (roots) free(roots);
}
