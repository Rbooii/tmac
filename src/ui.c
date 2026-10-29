#include "ui.h"

#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define PI 3.14159265358979323846

typedef struct {
    char *p;
    size_t len;
    size_t cap;
    int oom;
} sbuf;

static void sb_init(sbuf *b, char *mem, size_t cap)
{
    b->p = mem;
    b->len = 0;
    b->cap = cap;
    b->oom = 0;
    if (cap > 0)
        mem[0] = '\0';
}

static void sb_putn(sbuf *b, const char *s, size_t n)
{
    if (b->oom || n == 0)
        return;
    if (b->len + n >= b->cap) {
        b->oom = 1;
        return;
    }
    memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = '\0';
}

static void sb_puts(sbuf *b, const char *s)
{
    sb_putn(b, s, strlen(s));
}

static void sb_printf(sbuf *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

static void sb_printf(sbuf *b, const char *fmt, ...)
{
    if (b->oom)
        return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(b->p + b->len, b->cap - b->len, fmt, ap);
    va_end(ap);
    if (n < 0) {
        b->oom = 1;
        return;
    }
    if ((size_t)n >= b->cap - b->len) {
        b->oom = 1;
        return;
    }
    b->len += (size_t)n;
}

static void ui_flush(sbuf *b)
{
    size_t off = 0;
    while (off < b->len) {
        ssize_t n = write(STDOUT_FILENO, b->p + off, b->len - off);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (n == 0)
            break;
        off += (size_t)n;
    }
}

static void cursor_to(sbuf *b, int x, int y)
{
    sb_printf(b, "\x1b[%d;%dH", y + 1, x + 1);
}

static void put_spaces(sbuf *b, int n)
{
    for (int i = 0; i < n; i++)
        sb_puts(b, " ");
}

static int text_width(const char *s)
{
    int width = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if ((*p & 0xC0) != 0x80)
            width++;
    }
    return width;
}

static tmac_rgb mixc(tmac_rgb a, tmac_rgb b, double t)
{
    if (t < 0.0)
        t = 0.0;
    if (t > 1.0)
        t = 1.0;
    tmac_rgb out;
    out.r = (uint8_t)((double)a.r + ((double)b.r - (double)a.r) * t + 0.5);
    out.g = (uint8_t)((double)a.g + ((double)b.g - (double)a.g) * t + 0.5);
    out.b = (uint8_t)((double)a.b + ((double)b.b - (double)a.b) * t + 0.5);
    return out;
}

static tmac_rgb scale_rgb(tmac_rgb c, double f)
{
    if (f < 0.0)
        f = 0.0;
    if (f > 1.0)
        f = 1.0;
    tmac_rgb out;
    out.r = (uint8_t)((double)c.r * f + 0.5);
    out.g = (uint8_t)((double)c.g * f + 0.5);
    out.b = (uint8_t)((double)c.b * f + 0.5);
    return out;
}

static int rgb_to_256(tmac_rgb c)
{
    int mx = c.r, mn = c.r;
    if (c.g > mx)
        mx = c.g;
    if (c.b > mx)
        mx = c.b;
    if (c.g < mn)
        mn = c.g;
    if (c.b < mn)
        mn = c.b;

    if (mx - mn < 10) {
        int v = ((int)c.r + (int)c.g + (int)c.b) / 3;
        if (v < 8)
            return 16;
        if (v > 238)
            return 231;
        return 232 + (v - 8) * 24 / 247;
    }

    int r = ((int)c.r * 5 + 127) / 255;
    int g = ((int)c.g * 5 + 127) / 255;
    int b = ((int)c.b * 5 + 127) / 255;
    return 16 + 36 * r + 6 * g + b;
}

static void set_fg(const tmac_ui *ui, sbuf *b, tmac_rgb c)
{
    if (ui->color_mode == 2)
        sb_printf(b, "\x1b[38;2;%d;%d;%dm", (int)c.r, (int)c.g, (int)c.b);
    else if (ui->color_mode == 1)
        sb_printf(b, "\x1b[38;5;%dm", rgb_to_256(c));
}

static void set_bold(const tmac_ui *ui, sbuf *b, int on)
{
    if (ui->color_mode == 0)
        return;
    sb_puts(b, on ? "\x1b[1m" : "\x1b[22m");
}

static void sgr_reset(const tmac_ui *ui, sbuf *b)
{
    if (ui->color_mode != 0)
        sb_puts(b, "\x1b[0m");
}

static int detect_color(void)
{
    if (!isatty(STDOUT_FILENO))
        return 0;
    const char *no_color = getenv("NO_COLOR");
    if (no_color && no_color[0] != '\0')
        return 0;
    const char *term = getenv("TERM");
    if (!term || strcmp(term, "dumb") == 0)
        return 0;
    const char *color_term = getenv("COLORTERM");
    if (color_term && (strcmp(color_term, "truecolor") == 0 ||
                       strcmp(color_term, "24bit") == 0))
        return 2;
    if (strstr(term, "256color") != NULL)
        return 1;
    return 1;
}

static void query_size(tmac_ui *ui)
{
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0 && ws.ws_col > 0) {
        ui->rows = ws.ws_row;
        ui->cols = ws.ws_col;
    }
}

static void format_temp(char *out, size_t n, double v)
{
    if (!isfinite(v))
        snprintf(out, n, "--.-");
    else
        snprintf(out, n, "%.1f", v);
}

static tmac_rgb temp_color(const tmac_ui *ui, double t)
{
    const tmac_theme *th = ui->theme;
    if (!isfinite(t))
        return th->dim;
    if (t <= 55.0)
        return th->accent;
    if (t <= 75.0)
        return mixc(th->accent, th->warn, (t - 55.0) / 20.0);
    if (t <= 92.0)
        return mixc(th->warn, th->hot, (t - 75.0) / 17.0);
    return th->hot;
}

static void hist_push(tmac_ui *ui, double value)
{
    ui->hist[ui->hist_pos] = value;
    ui->hist_pos = (ui->hist_pos + 1) % TMAC_HIST_MAX;
    if (ui->hist_len < TMAC_HIST_MAX)
        ui->hist_len++;
}

static double hist_at(const tmac_ui *ui, int index_from_oldest)
{
    int start = (ui->hist_pos - ui->hist_len + TMAC_HIST_MAX) % TMAC_HIST_MAX;
    return ui->hist[(start + index_from_oldest) % TMAC_HIST_MAX];
}

int ui_theme_index(const char *name)
{
    if (!name)
        return -1;
    for (int i = 0; i < TMAC_THEME_COUNT; i++) {
        if (strcmp(name, TMAC_THEMES[i].name) == 0)
            return i;
    }
    return -1;
}

bool ui_theme_valid(const char *name)
{
    return ui_theme_index(name) >= 0;
}

bool ui_init(tmac_ui *ui, bool color, const char *theme_name)
{
    memset(ui, 0, sizeof(*ui));

    ui->theme_index = ui_theme_index(theme_name);
    if (ui->theme_index < 0)
        ui->theme_index = 0;
    ui->theme = &TMAC_THEMES[ui->theme_index];

    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO))
        return false;

    const char *term_name = getenv("TERM");
    if (term_name && strcmp(term_name, "dumb") == 0)
        return false;

    if (color)
        ui->color_mode = detect_color();

    ui->buf_cap = 512 * 1024;
    ui->buf = malloc(ui->buf_cap);
    if (!ui->buf)
        return false;

    if (tcgetattr(STDIN_FILENO, &ui->orig_termios) != 0) {
        free(ui->buf);
        ui->buf = NULL;
        return false;
    }
    ui->termios_saved = true;

    struct termios raw = ui->orig_termios;
    raw.c_lflag &= ~(tcflag_t)(ICANON | ECHO | ISIG);
    raw.c_iflag &= ~(tcflag_t)(IXON | ICRNL);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) != 0) {
        ui_shutdown(ui);
        return false;
    }

    ui->rows = 24;
    ui->cols = 80;
    query_size(ui);

    sbuf b;
    sb_init(&b, ui->buf, ui->buf_cap);
    sb_puts(&b, "\x1b[?1049h\x1b[?25l\x1b[2J\x1b[H");
    ui_flush(&b);

    ui->active = true;
    return true;
}

void ui_shutdown(tmac_ui *ui)
{
    if (!ui)
        return;

    if (ui->active) {
        const char *seq = "\x1b[0m\x1b[?25h\x1b[?1049l";
        size_t len = strlen(seq);
        size_t off = 0;
        while (off < len) {
            ssize_t n = write(STDOUT_FILENO, seq + off, len - off);
            if (n <= 0)
                break;
            off += (size_t)n;
        }
        ui->active = false;
    }

    if (ui->termios_saved) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &ui->orig_termios);
        ui->termios_saved = false;
    }

    free(ui->buf);
    ui->buf = NULL;
}

int ui_read_key(tmac_ui *ui)
{
    if (ui->key_pos < ui->key_len)
        return (int)ui->keys[ui->key_pos++];

    unsigned char buf[64];
    ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
    if (n <= 0)
        return -1;

    /* Skip escape sequences such as arrow keys entirely. */
    if (buf[0] == 0x1b && n > 1)
        return 0;

    if (n > (ssize_t)sizeof(ui->keys))
        n = (ssize_t)sizeof(ui->keys);
    memcpy(ui->keys, buf, (size_t)n);
    ui->key_len = (int)n;
    ui->key_pos = 1;
    return (int)ui->keys[0];
}

void ui_cycle_theme(tmac_ui *ui)
{
    ui->theme_index = (ui->theme_index + 1) % TMAC_THEME_COUNT;
    ui->theme = &TMAC_THEMES[ui->theme_index];
}

void ui_reset_graph(tmac_ui *ui)
{
    ui->hist_len = 0;
    ui->hist_pos = 0;
}

void ui_tick(tmac_ui *ui, const tmac_metrics *m, bool sample)
{
    ui->frame++;

    const double targets[6] = {
        m->die_hot, m->die_avg, m->die_cool, m->board, m->nand, m->battery,
    };

    for (int i = 0; i < 6; i++) {
        if (!isfinite(targets[i])) {
            if (!ui->primed[i])
                ui->disp[i] = NAN;
            continue;
        }
        if (!ui->primed[i]) {
            ui->disp[i] = targets[i];
            ui->primed[i] = true;
        } else {
            ui->disp[i] += (targets[i] - ui->disp[i]) * 0.16;
        }
    }

    if (sample && isfinite(m->die_hot))
        hist_push(ui, m->die_hot);
}

static void draw_header(tmac_ui *ui, sbuf *b, const char *model, int cores, int cols)
{
    const tmac_theme *th = ui->theme;

    cursor_to(b, 0, 0);
    set_fg(ui, b, th->accent);
    set_bold(ui, b, 1);
    sb_puts(b, "tmac");
    set_bold(ui, b, 0);

    double pulse = ui->paused ? 0.45 : 0.55 + 0.45 * (0.5 + 0.5 * sin(ui->frame * 0.12));
    set_fg(ui, b, scale_rgb(th->accent, pulse));
    sb_puts(b, " \xe2\x97\x8f");

    int left_width = 6;
    char right[160];
    if (cols >= 58 && model[0] != '\0')
        snprintf(right, sizeof(right), "%s \xc2\xb7 %d-core", model, cores);
    else if (cols >= 34 && model[0] != '\0')
        snprintf(right, sizeof(right), "%s", model);
    else
        right[0] = '\0';

    if (right[0] != '\0') {
        int right_width = text_width(right);
        int pad = cols - 1 - left_width - right_width;
        if (pad >= 2) {
            put_spaces(b, pad);
            set_fg(ui, b, th->dim);
            sb_puts(b, right);
        }
    }

    sgr_reset(ui, b);
}

static void draw_big(tmac_ui *ui, sbuf *b, const tmac_metrics *m, int y, int cols)
{
    const tmac_theme *th = ui->theme;
    double value = ui->disp[0];
    char num[16];
    format_temp(num, sizeof(num), value);

    cursor_to(b, 0, y);
    put_spaces(b, 3);

    tmac_rgb color = temp_color(ui, value);
    if (isfinite(value) && value > 80.0)
        color = scale_rgb(color, 0.82 + 0.18 * sin(ui->frame * 0.25));

    set_fg(ui, b, color);
    set_bold(ui, b, 1);
    sb_puts(b, num);
    set_bold(ui, b, 0);
    set_fg(ui, b, th->dim);
    sb_puts(b, "\xc2\xb0" "C");

    int left_width = 3 + (int)strlen(num) + 2;

    if (cols >= 30) {
        put_spaces(b, 3);
        set_fg(ui, b, th->dim);
        sb_puts(b, "SOC DIE");
        left_width += 3 + 7;
    }

    const char *word = m->has_thermal ? tmac_thermal_name(m->thermal_level) : "n/a";
    bool with_fanless = m->has_thermal && ui->fanless && cols >= 78;
    char status[64];
    snprintf(status, sizeof(status), "thermal: %s%s", word,
             with_fanless ? " \xc2\xb7 fanless" : "");
    int status_width = text_width(status);

    int pad = cols - 1 - left_width - status_width;
    if (pad >= 2) {
        put_spaces(b, pad);
        set_fg(ui, b, th->dim);
        sb_puts(b, "thermal: ");

        if (!m->has_thermal)
            set_fg(ui, b, th->dim);
        else if (m->thermal_level <= 0)
            set_fg(ui, b, th->accent);
        else if (m->thermal_level == 1)
            set_fg(ui, b, th->warn);
        else
            set_fg(ui, b, th->hot);
        sb_puts(b, word);

        if (with_fanless) {
            set_fg(ui, b, th->dim);
            sb_puts(b, " \xc2\xb7 fanless");
        }
    }

    sgr_reset(ui, b);
}

static void draw_spark(tmac_ui *ui, sbuf *b, int y, int cols)
{
    static const char *const spark[9] = {
        " ", "\xe2\x96\x81", "\xe2\x96\x82", "\xe2\x96\x83", "\xe2\x96\x84",
        "\xe2\x96\x85", "\xe2\x96\x86", "\xe2\x96\x87", "\xe2\x96\x88",
    };
    const tmac_theme *th = ui->theme;

    cursor_to(b, 0, y);
    put_spaces(b, 3);

    int available = cols - 4;
    if (available > 160)
        available = 160;

    if (available < 8 || ui->hist_len < 2) {
        set_fg(ui, b, th->dim);
        sb_puts(b, "collecting samples");
        sgr_reset(ui, b);
        return;
    }

    int count = ui->hist_len < available ? ui->hist_len : available;
    double lo = INFINITY, hi = -INFINITY;
    for (int i = 0; i < count; i++) {
        double v = hist_at(ui, ui->hist_len - count + i);
        if (!isfinite(v))
            continue;
        if (v < lo)
            lo = v;
        if (v > hi)
            hi = v;
    }
    if (!isfinite(lo)) {
        set_fg(ui, b, th->dim);
        sb_puts(b, "collecting samples");
        sgr_reset(ui, b);
        return;
    }

    double span = hi - lo;
    if (span < 2.0) {
        double mid = (hi + lo) * 0.5;
        lo = mid - 1.0;
        hi = mid + 1.0;
        span = 2.0;
    }

    int leading = available - count;
    if (leading > 0)
        put_spaces(b, leading);

    for (int i = 0; i < count; i++) {
        double v = hist_at(ui, ui->hist_len - count + i);
        double norm = isfinite(v) ? (v - lo) / span : 0.0;
        if (norm < 0.0)
            norm = 0.0;
        if (norm > 1.0)
            norm = 1.0;
        int level = 1 + (int)(norm * 7.0);
        if (level > 8)
            level = 8;
        set_fg(ui, b, mixc(th->dim, th->accent2, (double)(level - 1) / 7.0));
        sb_puts(b, spark[level]);
    }

    sgr_reset(ui, b);
}

static void draw_bar(tmac_ui *ui, sbuf *b, const char *label, double value, int y, int cols)
{
    const tmac_theme *th = ui->theme;

    cursor_to(b, 0, y);
    put_spaces(b, 3);
    set_fg(ui, b, th->dim);
    sb_printf(b, "%-5s ", label);

    char text[16];
    format_temp(text, sizeof(text), value);
    set_fg(ui, b, temp_color(ui, value));
    sb_printf(b, "%5s", text);
    put_spaces(b, 2);

    int bar_width = cols - 17;
    if (bar_width > 44)
        bar_width = 44;
    if (bar_width < 6)
        bar_width = 6;

    double norm = isfinite(value) ? value / 100.0 : 0.0;
    if (norm < 0.0)
        norm = 0.0;
    if (norm > 1.0)
        norm = 1.0;
    int fill = (int)lround(norm * (double)bar_width);
    if (fill < 0)
        fill = 0;
    if (fill > bar_width)
        fill = bar_width;

    set_fg(ui, b, temp_color(ui, value));
    for (int i = 0; i < fill; i++)
        sb_puts(b, "\xe2\x96\x88");

    set_fg(ui, b, scale_rgb(th->dim, 0.55));
    for (int i = fill; i < bar_width; i++)
        sb_puts(b, "\xe2\x96\x91");

    sgr_reset(ui, b);
}

static void draw_metric(tmac_ui *ui, sbuf *b, const char *label, double value)
{
    const tmac_theme *th = ui->theme;
    char text[16];
    format_temp(text, sizeof(text), value);

    set_fg(ui, b, th->dim);
    sb_puts(b, label);
    sb_puts(b, "  ");
    set_fg(ui, b, temp_color(ui, value));
    sb_puts(b, text);
}

static void draw_battery(tmac_ui *ui, sbuf *b, const tmac_metrics *m)
{
    const tmac_theme *th = ui->theme;

    set_fg(ui, b, th->dim);
    sb_puts(b, "BATTERY  ");

    if (m->has_battery) {
        char text[16];
        format_temp(text, sizeof(text), m->battery);
        set_fg(ui, b, temp_color(ui, m->battery));
        sb_puts(b, text);
    }

    if (m->has_battery_pct) {
        if (m->has_battery) {
            set_fg(ui, b, th->dim);
            sb_puts(b, " \xc2\xb7 ");
        }
        set_fg(ui, b, th->text);
        sb_printf(b, "%d%%", m->battery_pct);
    }

    if (!m->has_battery && !m->has_battery_pct) {
        set_fg(ui, b, th->dim);
        sb_puts(b, "--");
    }
}

static void draw_secondary(tmac_ui *ui, sbuf *b, const tmac_metrics *m, int y, int cols)
{
    if (cols < 40)
        return;

    if (cols >= 72) {
        cursor_to(b, 0, y);
        put_spaces(b, 3);
        draw_metric(ui, b, "BOARD", m->board);
        put_spaces(b, 5);
        draw_metric(ui, b, "NAND", m->nand);
        put_spaces(b, 5);
        draw_battery(ui, b, m);
    } else {
        cursor_to(b, 0, y);
        put_spaces(b, 3);
        draw_metric(ui, b, "BOARD", m->board);
        put_spaces(b, 5);
        draw_metric(ui, b, "NAND", m->nand);

        cursor_to(b, 0, y + 1);
        put_spaces(b, 3);
        draw_battery(ui, b, m);
    }

    sgr_reset(ui, b);
}

static void draw_help(tmac_ui *ui, sbuf *b, int y)
{
    static const char *const keys[4] = {"q / esc", "space", "+ / -", "t / r"};
    static const char *const desc[4] = {
        "quit", "pause / resume", "sample interval", "theme / reset graph",
    };
    const tmac_theme *th = ui->theme;

    cursor_to(b, 0, y);
    put_spaces(b, 3);
    set_fg(ui, b, th->dim);
    set_bold(ui, b, 1);
    sb_puts(b, "KEYS");
    set_bold(ui, b, 0);

    for (int i = 0; i < 4; i++) {
        cursor_to(b, 0, y + 1 + i);
        put_spaces(b, 3);
        set_fg(ui, b, th->accent);
        sb_printf(b, "%-9s", keys[i]);
        set_fg(ui, b, th->dim);
        sb_puts(b, desc[i]);
    }

    sgr_reset(ui, b);
}

static void draw_footer(tmac_ui *ui, sbuf *b, int y, int cols, int interval_ms)
{
    const tmac_theme *th = ui->theme;
    char left[160];

    if (ui->paused)
        snprintf(left, sizeof(left), "PAUSED   space resume   h help");
    else if (cols >= 72)
        snprintf(left, sizeof(left),
                 "q quit  space pause  +/- interval  t theme  h help");
    else if (cols >= 50)
        snprintf(left, sizeof(left), "q quit  space pause  +/- interval  h help");
    else
        snprintf(left, sizeof(left), "q quit  space pause  h help");

    int max_left = cols - 3;
    if (max_left < 0)
        max_left = 0;
    int left_len = (int)strlen(left);
    if (left_len > max_left)
        left[max_left] = '\0';

    cursor_to(b, 0, y);
    put_spaces(b, 3);
    if (ui->paused) {
        set_fg(ui, b, th->warn);
        set_bold(ui, b, 1);
        sb_puts(b, left);
        set_bold(ui, b, 0);
    } else {
        set_fg(ui, b, th->dim);
        sb_puts(b, left);
    }

    char right[64];
    snprintf(right, sizeof(right), "%d ms \xc2\xb7 %s", interval_ms, ui->theme->name);
    int left_width = 3 + (int)strlen(left);
    int right_width = text_width(right);
    int pad = cols - left_width - right_width - 1;
    if (pad >= 3) {
        put_spaces(b, pad);
        set_fg(ui, b, th->dim);
        sb_puts(b, right);
    }

    sgr_reset(ui, b);
}

static void draw_tiny(tmac_ui *ui, sbuf *b, const tmac_metrics *m, int rows, int cols,
                      int interval_ms)
{
    (void)m;

    draw_big(ui, b, m, 1, cols);

    if (rows >= 7 && cols >= 24) {
        draw_bar(ui, b, "HOT", ui->disp[0], 2, cols);
        draw_bar(ui, b, "AVG", ui->disp[1], 3, cols);
        draw_bar(ui, b, "COOL", ui->disp[2], 4, cols);
    }

    draw_footer(ui, b, rows - 1, cols, interval_ms);
}

void ui_render(tmac_ui *ui, const tmac_metrics *m, const char *model, int cores,
               int interval_ms)
{
    int old_rows = ui->rows;
    int old_cols = ui->cols;
    query_size(ui);
    ui->fanless = strstr(model, "MacBook Air") != NULL;

    sbuf b;
    sb_init(&b, ui->buf, ui->buf_cap);

    if (ui->rows != old_rows || ui->cols != old_cols)
        sb_puts(&b, "\x1b[2J");

    int rows = ui->rows;
    int cols = ui->cols;
    if (rows < 3 || cols < 20) {
        ui_flush(&b);
        return;
    }

    sb_puts(&b, "\x1b[0m");
    for (int y = 0; y < rows; y++) {
        cursor_to(&b, 0, y);
        sb_puts(&b, "\x1b[K");
    }

    draw_header(ui, &b, model, cores, cols);

    if (rows < 12) {
        draw_tiny(ui, &b, m, rows, cols, interval_ms);
        sgr_reset(ui, &b);
        ui_flush(&b);
        return;
    }

    int tight = rows < 16;
    int gap1 = tight ? 0 : 1;
    int gap2 = tight ? 0 : 1;
    int secondary_lines = (cols >= 72) ? 1 : (cols >= 40 ? 2 : 0);
    int height = 2 + gap1 + 3 + gap2 + secondary_lines;

    int top = 1 + (rows - 2 - height) / 2;
    if (top < 1)
        top = 1;
    if (top + height > rows - 1)
        top = rows - 1 - height;
    if (top < 1)
        top = 1;

    draw_big(ui, &b, m, top, cols);
    draw_spark(ui, &b, top + 1, cols);

    int y = top + 2 + gap1;

    if (ui->help && cols >= 34) {
        draw_help(ui, &b, y);
    } else {
        if (cols >= 24) {
            draw_bar(ui, &b, "HOT", ui->disp[0], y, cols);
            draw_bar(ui, &b, "AVG", ui->disp[1], y + 1, cols);
            draw_bar(ui, &b, "COOL", ui->disp[2], y + 2, cols);
        }
        draw_secondary(ui, &b, m, y + 3 + gap2, cols);
    }

    draw_footer(ui, &b, rows - 1, cols, interval_ms);

    sgr_reset(ui, &b);
    ui_flush(&b);
}
