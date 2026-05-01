#include "pcitop.h"
#include <ncurses.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/time.h>

#define SETTINGS_FILE "pcitop.ini"
#define MAX_HIDDEN_DEVICES 1024

char hidden_dbdfs[MAX_HIDDEN_DEVICES][32];
int num_hidden = 0;
bool show_hidden_mode = false;
int selected_line = 0;
int num_lines = 0;
int refresh_ms = 1000;
int throughput_ms = 100;
bool global_logging_enabled = false;
char logging_dbdfs[MAX_HIDDEN_DEVICES][32];
int num_logging_devices = 0;
FILE *log_file = NULL;
char session_ts[32] = "";

static long long get_time_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (long long)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

void load_settings(void) {
    FILE *f = fopen(SETTINGS_FILE, "r");
    if (!f) return;
    char line[64];
    bool in_hidden_section = false;
    bool in_general_section = false;
    bool in_logging_section = false;
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (strcmp(line, "[hidden]") == 0) {
            in_hidden_section = true;
            in_general_section = false;
            continue;
        }
        if (strcmp(line, "[general]") == 0) {
            in_general_section = true;
            in_hidden_section = false;
            continue;
        }
        if (strcmp(line, "[logging]") == 0) {
            in_logging_section = true;
            in_hidden_section = false;
            in_general_section = false;
            continue;
        }
        if (line[0] == '[') {
            in_hidden_section = false;
            in_general_section = false;
            in_logging_section = false;
            continue;
        }
        if (in_hidden_section && line[0] != '\0') {
            char *eq = strchr(line, '=');
            if (eq) *eq = '\0';
            if (num_hidden < MAX_HIDDEN_DEVICES) {
                strncpy(hidden_dbdfs[num_hidden++], line, 31);
            }
        }
        if (in_general_section && line[0] != '\0') {
            if (strncmp(line, "throughput_ms=", 14) == 0) {
                throughput_ms = atoi(line + 14);
                if (throughput_ms < 1) throughput_ms = 1;
                if (throughput_ms > 5000) throughput_ms = 5000;
            } else if (strncmp(line, "logging_enabled=", 16) == 0) {
                global_logging_enabled = (atoi(line + 16) == 1);
            }
        }
        if (in_logging_section && line[0] != '\0') {
            char *eq = strchr(line, '=');
            if (eq) *eq = '\0';
            if (num_logging_devices < MAX_HIDDEN_DEVICES) {
                strncpy(logging_dbdfs[num_logging_devices++], line, 31);
            }
        }
    }
    fclose(f);
}

void save_settings(void) {
    FILE *f = fopen(SETTINGS_FILE, "w");
    if (!f) return;
    fprintf(f, "[general]\n");
    fprintf(f, "throughput_ms=%d\n", throughput_ms);
    fprintf(f, "logging_enabled=%d\n", global_logging_enabled ? 1 : 0);
    fprintf(f, "\n[hidden]\n");
    for (int i = 0; i < num_hidden; i++) {
        fprintf(f, "%s=1\n", hidden_dbdfs[i]);
    }
    fprintf(f, "\n[logging]\n");
    for (int i = 0; i < num_logging_devices; i++) {
        fprintf(f, "%s=1\n", logging_dbdfs[i]);
    }
    fclose(f);
}

bool is_hidden(const char *dbdf) {
    for (int i = 0; i < num_hidden; i++) {
        if (strcmp(hidden_dbdfs[i], dbdf) == 0) return true;
    }
    return false;
}

void toggle_hidden(const char *dbdf) {
    int found_idx = -1;
    for (int i = 0; i < num_hidden; i++) {
        if (strcmp(hidden_dbdfs[i], dbdf) == 0) {
            found_idx = i;
            break;
        }
    }
    if (found_idx != -1) {
        // Remove
        for (int i = found_idx; i < num_hidden - 1; i++) {
            strcpy(hidden_dbdfs[i], hidden_dbdfs[i+1]);
        }
        num_hidden--;
    } else {
        // Add
        if (num_hidden < MAX_HIDDEN_DEVICES) {
            strncpy(hidden_dbdfs[num_hidden++], dbdf, 31);
        }
    }
    save_settings();
}

bool is_logging_device(const char *dbdf) {
    for (int i = 0; i < num_logging_devices; i++) {
        if (strcmp(logging_dbdfs[i], dbdf) == 0) return true;
    }
    return false;
}

void toggle_logging_device(const char *dbdf) {
    int found_idx = -1;
    for (int i = 0; i < num_logging_devices; i++) {
        if (strcmp(logging_dbdfs[i], dbdf) == 0) {
            found_idx = i;
            break;
        }
    }
    if (found_idx != -1) {
        for (int i = found_idx; i < num_logging_devices - 1; i++) {
            strcpy(logging_dbdfs[i], logging_dbdfs[i+1]);
        }
        num_logging_devices--;
    } else {
        if (num_logging_devices < MAX_HIDDEN_DEVICES) {
            strncpy(logging_dbdfs[num_logging_devices++], dbdf, 31);
        }
    }
    save_settings();
}

// Global array to hold flat lines for rendering
typedef struct {
    char tree_str[64];
    char bdf_str[16];
    char vendor_str[32];
    char device_str[128];
    char max_spd_str[32];
    char cur_spd_str[32];
    char rx_rate_str[16];
    char tx_rate_str[16];
    char total_rx_str[16];
    char total_tx_str[16];
    bool is_hidden_node;
    char dbdf_str[32];
} RenderLine;

RenderLine *render_lines = NULL;
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

static void format_size(float size_mb, char *buf, size_t size) {
    if (size_mb == 0.0f) {
        strcpy(buf, "-");
    } else if (size_mb < 1.0f) {
        snprintf(buf, size, "%.1f KB", size_mb * 1024.0f);
    } else if (size_mb < 1024.0f) {
        snprintf(buf, size, "%.1f MB", size_mb);
    } else if (size_mb < 1024.0f * 1024.0f) {
        snprintf(buf, size, "%.1f GB", size_mb / 1024.0f);
    } else {
        snprintf(buf, size, "%.1f TB", size_mb / (1024.0f * 1024.0f));
    }
}

void add_render_line(const char *tree, PciNode *node) {
    if (num_lines >= max_lines) {
        max_lines = max_lines == 0 ? 128 : max_lines * 2;
        render_lines = realloc(render_lines, max_lines * sizeof(RenderLine));
    }
    
    RenderLine *line = &render_lines[num_lines++];
    strncpy(line->tree_str, tree, sizeof(line->tree_str) - 1);
    strncpy(line->bdf_str, node->bdf_str, sizeof(line->bdf_str) - 1);
    strncpy(line->dbdf_str, node->dbdf_str, sizeof(line->dbdf_str) - 1);
    line->is_hidden_node = node->hidden;
    
    format_size(node->interval_rx, line->rx_rate_str, sizeof(line->rx_rate_str));
    format_size(node->interval_tx, line->tx_rate_str, sizeof(line->tx_rate_str));
    format_size(node->total_rx, line->total_rx_str, sizeof(line->total_rx_str));
    format_size(node->total_tx, line->total_tx_str, sizeof(line->total_tx_str));

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
    if (node->hidden && !show_hidden_mode) return;

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
    erase();
    
    attron(A_REVERSE);
    mvprintw(0, 0, "%-16s %-15s %-10s %-25s %-15s %-12s %-12s %-12s %-12s %-12s", 
             "Tree", "B:D.F", "Vendor", "Device", "Max Speed", "Cur Speed", "RX (Interval)", "TX (Interval)", "Sum RX", "Sum TX");
    attroff(A_REVERSE);
    
    int max_y, max_x;
    getmaxyx(stdscr, max_y, max_x);
    
    for (int i = 0; i < max_y - 2 && (i + scroll_y) < num_lines; i++) {
        int idx = i + scroll_y;
        RenderLine *l = &render_lines[idx];
        
        if (idx == selected_line) {
            attron(A_BOLD | COLOR_PAIR(1));
        }
        if (l->is_hidden_node) {
            attron(A_DIM);
        }
        
        // Truncate strings to fit nice columns
        char device_trunc[26];
        strncpy(device_trunc, l->device_str, 25);
        device_trunc[25] = '\0';
        
        char bdf_display[32];
        if (is_logging_device(l->dbdf_str)) {
            snprintf(bdf_display, sizeof(bdf_display), "%s (L)", l->bdf_str);
        } else {
            strncpy(bdf_display, l->bdf_str, sizeof(bdf_display) - 1);
        }

        mvprintw(i + 1, 0, "%-16s %-15s %-10s %-25s %-15s %-12s %-12s %-12s %-12s %-12s",
                 l->tree_str, bdf_display, l->vendor_str, device_trunc, l->max_spd_str, l->cur_spd_str, 
                 l->rx_rate_str, l->tx_rate_str, l->total_rx_str, l->total_tx_str);
        
        if (l->is_hidden_node) {
            attroff(A_DIM);
        }
        if (idx == selected_line) {
            attroff(A_BOLD | COLOR_PAIR(1));
        }
    }

    // Status bar
    mvprintw(max_y - 1, 0, "h:hide/show  H:toggle hidden  r:reset Sums  l:select  L:log [%s]  GUI:%dms  +/-:Tput(%dms)  q:quit", 
             global_logging_enabled ? "ACTIVE" : "OFF", refresh_ms, throughput_ms);

    char version_str[64];
    snprintf(version_str, sizeof(version_str), "pcitop build: %s version: %s", GIT_BUILD, GIT_VERSION);
    mvprintw(max_y - 1, max_x - strlen(version_str) - 1, "%s", version_str);
    
    refresh();
}

#include <unistd.h>
#include <time.h>

void start_logging(void) {
    time_t now_time = time(NULL);
    struct tm *t = localtime(&now_time);
    strftime(session_ts, sizeof(session_ts), "%Y%m%d%H%M%S", t);
    mark_log_start();
    global_logging_enabled = true;
}

void stop_logging(PciNode *all_nodes, int total_devices) {
    if (global_logging_enabled && all_nodes) {
        finalize_logs(all_nodes, total_devices, session_ts);
    }
    global_logging_enabled = false;
}

int main(void) {
    load_settings();
    if (global_logging_enabled) start_logging();

    initscr();
    start_color();
    init_pair(1, COLOR_YELLOW, COLOR_BLUE); // Highlight pair
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);
    
    int scroll_y = 0;
    int max_y, max_x;
    long long last_gui_update = 0;
    long long last_tput_update = 0;
    bool force_gui_render = true;

    struct pci_access *pacc = NULL;
    PciNode *all_nodes = NULL;
    PciNode **roots = NULL;
    int num_roots = 0;
    int total_devices = 0;
    
    while (1) {
        long long now = get_time_ms();
        bool cycle_complete = (now - last_gui_update >= refresh_ms);
        bool tput_needed = (now - last_tput_update >= throughput_ms);

        if (cycle_complete) {
            if (pacc) {
                free_pci_tree(roots, all_nodes);
                cleanup_pci_access(pacc);
            }
            pacc = init_pci_access();
            roots = build_pci_tree(pacc, &num_roots, &all_nodes);
            total_devices = 0;
            for (struct pci_dev *dev = pacc->devices; dev; dev = dev->next) total_devices++;
            
            // Gather final throughput for this cycle
            update_throughput(all_nodes, total_devices);
            
            // Log entries
            if (global_logging_enabled) {
                write_log_entry(all_nodes, total_devices, session_ts);
            }

            build_render_list(roots, num_roots);
            
            getmaxyx(stdscr, max_y, max_x);
            if (selected_line >= num_lines) selected_line = num_lines - 1;
            if (selected_line < 0) selected_line = 0;
            if (selected_line < scroll_y) scroll_y = selected_line;
            if (selected_line >= scroll_y + (max_y - 2)) scroll_y = selected_line - (max_y - 3);
            if (scroll_y > num_lines - (max_y - 2)) scroll_y = num_lines - (max_y - 2);
            if (scroll_y < 0) scroll_y = 0;
            
            render_screen(scroll_y);
            
            // Mark end of cycle: next interval sum starts from now
            mark_gui_cycle();
            
            last_gui_update = now;
            last_tput_update = now;
            force_gui_render = false;
        } else if (tput_needed) {
            update_throughput(all_nodes, total_devices);
            last_tput_update = now;
        }

        if (force_gui_render) {
            getmaxyx(stdscr, max_y, max_x);
            if (selected_line < scroll_y) scroll_y = selected_line;
            if (selected_line >= scroll_y + (max_y - 2)) scroll_y = selected_line - (max_y - 3);
            render_screen(scroll_y);
            force_gui_render = false;
        }

        timeout(throughput_ms / 2);
        int ch = getch();
        if (ch != ERR) {
            if (ch == 'q' || ch == 'Q') {
                break;
            } else if (ch == KEY_UP) {
                if (selected_line > 0) selected_line--;
                force_gui_render = true;
            } else if (ch == KEY_DOWN) {
                if (selected_line < num_lines - 1) selected_line++;
                force_gui_render = true;
            } else if (ch == KEY_NPAGE) {
                selected_line += (max_y - 2);
                if (selected_line >= num_lines) selected_line = num_lines - 1;
                force_gui_render = true;
            } else if (ch == KEY_PPAGE) {
                selected_line -= (max_y - 2);
                if (selected_line < 0) selected_line = 0;
                force_gui_render = true;
            } else if (ch == 'h' || ch == ' ') {
                if (num_lines > 0) {
                    toggle_hidden(render_lines[selected_line].dbdf_str);
                    // Tree structure changed, force a full rebuild cycle
                    last_gui_update = 0; 
                }
            } else if (ch == 'H') {
                show_hidden_mode = !show_hidden_mode;
                last_gui_update = 0;
            } else if (ch == 'r') {
                reset_throughput();
                force_gui_render = true;
            } else if (ch == 'l') {
                if (num_lines > 0) {
                    toggle_logging_device(render_lines[selected_line].dbdf_str);
                }
                force_gui_render = true;
            } else if (ch == 'L') {
                global_logging_enabled = !global_logging_enabled;
                if (global_logging_enabled) start_logging();
                else stop_logging(all_nodes, total_devices);
                save_settings();
                force_gui_render = true;
            } else if (ch == '+' || ch == '=') {
                if (throughput_ms < 5000) {
                    throughput_ms += 1;
                    save_settings();
                }
                force_gui_render = true;
            } else if (ch == '-' || ch == '_') {
                if (throughput_ms > 1) {
                    throughput_ms -= 1;
                    save_settings();
                }
                force_gui_render = true;
            }
        }
    }
    
    if (global_logging_enabled) {
        finalize_logs(all_nodes, total_devices, session_ts);
    }
    if (pacc) {
        free_pci_tree(roots, all_nodes);
        cleanup_pci_access(pacc);
    }
    endwin();
    if (render_lines) free(render_lines);
    
    return 0;
}
