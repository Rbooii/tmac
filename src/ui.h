#ifndef TMAC_UI_H
#define TMAC_UI_H

#include <stdbool.h>
#include <stddef.h>
#include <termios.h>

#include "sensors.h"
#include "theme.h"

#define TMAC_HIST_MAX 400

typedef struct {
    bool active;
    int color_mode; /* 0 = plain, 1 = ansi 256, 2 = truecolor */
    bool paused;
    bool help;
    bool fanless;
    int rows;
    int cols;
    int frame;
    int theme_index;
    const tmac_theme *theme;

    double disp[6]; /* hot, avg, cool, board, nand, battery */
    bool primed[6];

    double hist[TMAC_HIST_MAX];
    int hist_len;
    int hist_pos;

    char *buf;
    size_t buf_cap;

    unsigned char keys[64];
    int key_len;
    int key_pos;

    struct termios orig_termios;
    bool termios_saved;
} tmac_ui;

bool ui_init(tmac_ui *ui, bool color, const char *theme_name);
void ui_shutdown(tmac_ui *ui);
int ui_read_key(tmac_ui *ui);
void ui_tick(tmac_ui *ui, const tmac_metrics *m, bool sample);
void ui_render(tmac_ui *ui, const tmac_metrics *m, const char *model, int cores,
               int interval_ms);
void ui_cycle_theme(tmac_ui *ui);
void ui_reset_graph(tmac_ui *ui);

bool ui_theme_valid(const char *name);
int ui_theme_index(const char *name);

#endif
