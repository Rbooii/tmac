#ifndef TMAC_SENSORS_H
#define TMAC_SENSORS_H

#include <stdbool.h>
#include <stddef.h>

typedef struct tmac_sensors tmac_sensors;

typedef struct {
    double die_hot;
    double die_avg;
    double die_cool;
    double board;
    double nand;
    double battery;
    int battery_pct;
    int thermal_level;

    int die_count;
    int board_count;
    int nand_count;
    int battery_count;

    bool has_board;
    bool has_nand;
    bool has_battery;
    bool has_battery_pct;
    bool has_thermal;
} tmac_metrics;

/* Opens the private HID temperature sensor set. Returns NULL when the
 * IOHID event system is unavailable. */
tmac_sensors *tmac_sensors_open(void);
void tmac_sensors_poll(tmac_sensors *s, tmac_metrics *out);
void tmac_sensors_close(tmac_sensors *s);

/* Number of discovered temperature services (all groups). */
int tmac_sensor_count(const tmac_sensors *s);

/* Friendly machine label, e.g. "MacBook Air 13\" (M3)". */
void tmac_device_label(char *out, size_t n);
int tmac_core_count(void);

const char *tmac_thermal_name(int level);

#endif
