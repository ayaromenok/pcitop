#include "pcitop.h"
#include <ncurses.h>
#include <stdlib.h>
#include <string.h>

// Global array to hold flat lines for rendering
typedef struct {
    char tree_str[64];
    char bdf_str[16];
    char vendor_str[32];
    char device_str[128];
    char max_spd_str[32];
    char cur_spd_str[32];
} RenderLine;

RenderLine *render_lines = NULL;
int num_lines = 0;
int max_lines = 0;

static int get_gen_num(float speed) {
    if (speed >= 64.0f) return 6;
    if (speed >= 32.0f) return 5;
    if (speed >= 16.0f) return 4;
    if (speed >= 8.0f) return 3;
    if (speed >= 5.0f) return 2;
    if (speed >= 2.5f) return 1;
    return 0;
}

void add_render_line(const char *tree, PciNode *node) {
    if (num_lines >= max_lines) {
        max_lines = max_lines == 0 ? 128 : max_lines * 2;
        render_lines = realloc(render_lines, max_lines * sizeof(RenderLine));
    }
    
    RenderLine *line = &render_lines[num_lines++];
    strncpy(line->tree_str, tree, sizeof(line->tree_str) - 1);
    strncpy(line->bdf_str, node->bdf_str, sizeof(line->bdf_str) - 1);
    
    // Vendor might be shortened
    strncpy(line->vendor_str, node->vendor_str, sizeof(line->vendor_str) - 1);
    
    strncpy(line->device_str, node->device_str, sizeof(line->device_str) - 1);
    
    if (node->max_speed > 0.0f) {
        int gen = get_gen_num(node->max_speed);
        int dev_gen = get_gen_num(node->device_max_speed);
        if (dev_gen > gen) {
            snprintf(line->max_spd_str, sizeof(line->max_spd_str), "Gen%d (HW:G%d) x%d", gen, dev_gen, node->max_width);
        } else {
            snprintf(line->max_spd_str, sizeof(line->max_spd_str), "Gen%d x%d", gen, node->max_width);
        }
    } else {
        strcpy(line->max_spd_str, "-");
    }
    
    if (node->cur_speed > 0.0f) {
        int gen = get_gen_num(node->cur_speed);
        snprintf(line->cur_spd_str, sizeof(line->cur_spd_str), "Gen%d x%d", gen, node->cur_width);
    } else {
        strcpy(line->cur_spd_str, "-");
    }
}

void flatten_tree(PciNode *node, const char *prefix, bool is_last) {
    char tree_col[128];
    char new_prefix[128];
    
    if (strcmp(prefix, "") == 0) {
        snprintf(tree_col, sizeof(tree_col), "+-");
        snprintf(new_prefix, sizeof(new_prefix), "  ");
    } else {
        snprintf(tree_col, sizeof(tree_col), "%s%s", prefix, is_last ? "\\-" : "+-");
        snprintf(new_prefix, sizeof(new_prefix), "%s%s", prefix, is_last ? "  " : "| ");
    }
    
    add_render_line(tree_col, node);
    
    for (int i = 0; i < node->num_children; i++) {
        flatten_tree(node->children[i], new_prefix, i == node->num_children - 1);
    }
}

void build_render_list(PciNode **roots, int num_roots) {
    num_lines = 0;
    for (int i = 0; i < num_roots; i++) {
        flatten_tree(roots[i], "", i == num_roots - 1);
    }
}

void render_screen(int scroll_y) {
    clear();
    
    attron(A_REVERSE);
    mvprintw(0, 0, "%-16s %-10s %-10s %-45s %-15s %-15s", 
             "Tree", "B:D.F", "Vendor", "Device", "Max Speed", "Cur Speed");
    attroff(A_REVERSE);
    
    int max_y, max_x;
    getmaxyx(stdscr, max_y, max_x);
    
    for (int i = 0; i < max_y - 1 && (i + scroll_y) < num_lines; i++) {
        RenderLine *l = &render_lines[i + scroll_y];
        
        // Truncate strings to fit nice columns
        char device_trunc[46];
        strncpy(device_trunc, l->device_str, 45);
        device_trunc[45] = '\0';
        
        mvprintw(i + 1, 0, "%-16s %-10s %-10s %-45s %-15s %-15s",
                 l->tree_str, l->bdf_str, l->vendor_str, device_trunc, l->max_spd_str, l->cur_spd_str);
    }
    
    refresh();
}

int main(void) {
    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);
    timeout(1000); // 1-second refresh
    
    int scroll_y = 0;
    int max_y, max_x;
    
    while (1) {
        struct pci_access *pacc = init_pci_access();
        
        int num_roots = 0;
        PciNode *all_nodes = NULL;
        PciNode **roots = build_pci_tree(pacc, &num_roots, &all_nodes);
        build_render_list(roots, num_roots);
        
        getmaxyx(stdscr, max_y, max_x);
        if (scroll_y > num_lines - (max_y - 1)) {
            scroll_y = num_lines - (max_y - 1);
        }
        if (scroll_y < 0) scroll_y = 0;
        
        render_screen(scroll_y);
        
        free_pci_tree(roots, all_nodes);
        cleanup_pci_access(pacc);
        
        int ch = getch();
        if (ch == 'q' || ch == 'Q') {
            break;
        } else if (ch == KEY_UP) {
            if (scroll_y > 0) scroll_y--;
        } else if (ch == KEY_DOWN) {
            if (scroll_y < num_lines - (max_y - 1)) scroll_y++;
        } else if (ch == KEY_NPAGE) {
            scroll_y += (max_y - 2);
            if (scroll_y > num_lines - (max_y - 1)) scroll_y = num_lines - (max_y - 1);
        } else if (ch == KEY_PPAGE) {
            scroll_y -= (max_y - 2);
            if (scroll_y < 0) scroll_y = 0;
        }
    }
    
    endwin();
    if (render_lines) free(render_lines);
    
    return 0;
}
