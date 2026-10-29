#include "sensors.h"
#include "ui.h"

#include <errno.h>
#include <math.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define TMAC_VERSION "1.0.0"
#define FRAME_MS 50

static volatile sig_atomic_t g_quit = 0;

static void handle_signal(int sig)
{
    (void)sig;
    g_quit = 1;
}

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
}

static void sleep_ms(long ms)
{
    if (ms <= 0)
        return;
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

static void usage(FILE *out)
{
    fprintf(out,
            "tmac " TMAC_VERSION " \xe2\x80\x94 realtime temperature monitor for Apple silicon Macs\n"
            "\n"
            "Usage: tmac [options]\n"
            "\n"
            "Options:\n"
            "  --once             print one snapshot and exit\n"
            "  --interval <ms>    sensor sample interval, 100-5000 (default 250)\n"
            "  --theme <name>     mint | ice | mono (default mint)\n"
            "  --plain            disable colors\n"
            "  -h, --help         show this help\n"
            "  -v, --version      show version\n"
            "\n"
            "Keys while running:\n"
            "  q / esc   quit          space   pause / resume\n"
            "  + / -     interval      t       next theme\n"
            "  r         reset graph   h       help\n");
}

static bool parse_interval(const char *text, int *out)
{
    if (!text || !*text)
        return false;
    errno = 0;
    char *end = NULL;
    long value = strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0')
        return false;
    if (value < 100 || value > 5000)
        return false;
    *out = (int)value;
    return true;
}

static void print_snapshot(const tmac_metrics *m, const char *model, int cores)
{
    printf("tmac  %s", model);
    if (cores > 0)
        printf(" \xc2\xb7 %d-core", cores);
    printf("\n");

    if (m->die_count > 0) {
        printf("  SOC die   %.1f \xc2\xb0" "C   hot %.1f \xc2\xb7 cool %.1f\n",
               m->die_avg, m->die_hot, m->die_cool);
    } else {
        printf("  SOC die   n/a\n");
    }

    if (m->has_board)
        printf("  Board     %.1f \xc2\xb0" "C\n", m->board);
    else
        printf("  Board     n/a\n");

    if (m->has_nand)
        printf("  NAND      %.1f \xc2\xb0" "C\n", m->nand);
    else
        printf("  NAND      n/a\n");

    if (m->has_battery || m->has_battery_pct) {
        printf("  Battery   ");
        if (m->has_battery)
            printf("%.1f \xc2\xb0" "C", m->battery);
        if (m->has_battery && m->has_battery_pct)
            printf(" \xc2\xb7 ");
        if (m->has_battery_pct)
            printf("%d%%", m->battery_pct);
        printf("\n");
    } else {
        printf("  Battery   n/a\n");
    }

    printf("  Thermal   %s\n",
           m->has_thermal ? tmac_thermal_name(m->thermal_level) : "n/a");
    printf("  Sensors   %d die \xc2\xb7 %d board \xc2\xb7 %d nand \xc2\xb7 %d battery\n",
           m->die_count, m->board_count, m->nand_count, m->battery_count);
}

int main(int argc, char **argv)
{
    bool once = false;
    bool plain = false;
    int interval = 250;
    const char *theme = "mint";

    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];

        if (strcmp(arg, "--once") == 0) {
            once = true;
        } else if (strcmp(arg, "--plain") == 0 || strcmp(arg, "--no-color") == 0) {
            plain = true;
        } else if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0) {
            usage(stdout);
            return 0;
        } else if (strcmp(arg, "--version") == 0 || strcmp(arg, "-v") == 0) {
            printf("tmac %s\n", TMAC_VERSION);
            return 0;
        } else if (strcmp(arg, "--interval") == 0) {
            if (i + 1 >= argc || !parse_interval(argv[++i], &interval)) {
                fprintf(stderr, "tmac: --interval needs a value between 100 and 5000 ms\n");
                return 1;
            }
        } else if (strncmp(arg, "--interval=", 11) == 0) {
            if (!parse_interval(arg + 11, &interval)) {
                fprintf(stderr, "tmac: --interval needs a value between 100 and 5000 ms\n");
                return 1;
            }
        } else if (strcmp(arg, "--theme") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "tmac: --theme needs a name (mint, ice, mono)\n");
                return 1;
            }
            theme = argv[++i];
        } else if (strncmp(arg, "--theme=", 8) == 0) {
            theme = arg + 8;
        } else {
            fprintf(stderr, "tmac: unknown option '%s' (try --help)\n", arg);
            return 1;
        }
    }

    if (!ui_theme_valid(theme)) {
        fprintf(stderr, "tmac: unknown theme '%s' (try mint, ice, mono)\n", theme);
        return 1;
    }

    char model[96];
    tmac_device_label(model, sizeof(model));
    int cores = tmac_core_count();

    tmac_sensors *sensors = tmac_sensors_open();
    if (!sensors || tmac_sensor_count(sensors) == 0) {
        fprintf(stderr, "tmac: no temperature sensors found on this Mac\n");
        if (sensors)
            tmac_sensors_close(sensors);
        return 2;
    }

    tmac_metrics metrics;
    tmac_sensors_poll(sensors, &metrics);

    if (once || !isatty(STDOUT_FILENO)) {
        print_snapshot(&metrics, model, cores);
        tmac_sensors_close(sensors);
        return 0;
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    tmac_ui ui;
    if (!ui_init(&ui, !plain, theme)) {
        print_snapshot(&metrics, model, cores);
        tmac_sensors_close(sensors);
        return 0;
    }

    ui_tick(&ui, &metrics, true);

    uint64_t last_poll = now_ms();

    while (!g_quit) {
        uint64_t frame_start = now_ms();

        int key;
        while (!g_quit && (key = ui_read_key(&ui)) >= 0) {
            switch (key) {
            case 0:
                break;
            case 'q':
            case 3:  /* ctrl-c */
            case 4:  /* ctrl-d */
            case 27: /* esc */
                g_quit = 1;
                break;
            case ' ':
                ui.paused = !ui.paused;
                break;
            case '+':
            case '=':
                interval += 50;
                if (interval > 2000)
                    interval = 2000;
                break;
            case '-':
            case '_':
                interval -= 50;
                if (interval < 100)
                    interval = 100;
                break;
            case 't':
                ui_cycle_theme(&ui);
                break;
            case 'r':
                ui_reset_graph(&ui);
                break;
            case 'h':
            case '?':
                ui.help = !ui.help;
                break;
            default:
                break;
            }
        }
        if (g_quit)
            break;

        uint64_t now = now_ms();
        bool sampled = false;
        if (!ui.paused && now - last_poll >= (uint64_t)interval) {
            tmac_sensors_poll(sensors, &metrics);
            last_poll = now;
            sampled = true;
        }

        ui_tick(&ui, &metrics, sampled);
        ui_render(&ui, &metrics, model, cores, interval);

        uint64_t elapsed = now_ms() - frame_start;
        if (elapsed < FRAME_MS)
            sleep_ms(FRAME_MS - (long)elapsed);
    }

    ui_shutdown(&ui);
    tmac_sensors_close(sensors);
    return 0;
}
