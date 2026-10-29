#include "sensors.h"

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <math.h>
#include <notify.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <time.h>

/* Private (but stable since 10.5) IOHID event system entry points. */
typedef void *IOHIDEventSystemClientRef;
typedef void *IOHIDServiceClientRef;
typedef void *IOHIDEventRef;

extern IOHIDEventSystemClientRef IOHIDEventSystemClientCreate(CFAllocatorRef allocator);
extern void IOHIDEventSystemClientSetMatching(IOHIDEventSystemClientRef client, CFDictionaryRef matching);
extern CFArrayRef IOHIDEventSystemClientCopyServices(IOHIDEventSystemClientRef client);
extern IOHIDEventRef IOHIDServiceClientCopyEvent(IOHIDServiceClientRef service, int64_t type, int32_t options, int64_t timestamp);
extern double IOHIDEventGetFloatValue(IOHIDEventRef event, int32_t field);
extern CFTypeRef IOHIDServiceClientCopyProperty(IOHIDServiceClientRef service, CFStringRef key);

#define HID_USAGE_PAGE_APPLE_VENDOR 0xff00
#define HID_USAGE_TEMPERATURE 5
#define HID_EVENT_TYPE_TEMPERATURE 15
#define HID_EVENT_FIELD_TEMPERATURE (15 << 16)

#define MAX_ENTRIES 96
#define BATTERY_REFRESH_MS 2000
#define THERMAL_KEY "com.apple.system.thermalpressurelevel"

enum {
    GROUP_DIE = 0,
    GROUP_BOARD,
    GROUP_NAND,
    GROUP_BATTERY,
};

typedef struct {
    IOHIDServiceClientRef service;
    char name[48];
    int group;
    bool valid;
    double celsius;
} sensor_entry;

struct tmac_sensors {
    IOHIDEventSystemClientRef client;
    CFArrayRef services;
    sensor_entry entries[MAX_ENTRIES];
    int count;

    int thermal_token;
    bool thermal_registered;

    uint64_t last_battery_ms;
    int battery_pct;
    bool battery_pct_valid;
};

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
}

static int classify(const char *name)
{
    if (strstr(name, "tdie") != NULL)
        return GROUP_DIE;
    if (strstr(name, "tdev") != NULL)
        return GROUP_BOARD;
    if (strstr(name, "NAND") != NULL)
        return GROUP_NAND;
    if (strstr(name, "gas gauge battery") != NULL)
        return GROUP_BATTERY;
    return -1;
}

static int read_battery_percent(void)
{
    io_service_t svc = IOServiceGetMatchingService(kIOMainPortDefault,
                                                   IOServiceMatching("AppleSmartBattery"));
    if (!svc)
        return -1;

    int pct = -1;
    CFTypeRef soc = IORegistryEntryCreateCFProperty(svc, CFSTR("StateOfCharge"),
                                                    kCFAllocatorDefault, 0);
    if (soc) {
        if (CFGetTypeID(soc) == CFNumberGetTypeID()) {
            int v = 0;
            if (CFNumberGetValue((CFNumberRef)soc, kCFNumberIntType, &v))
                pct = v;
        }
        CFRelease(soc);
    }

    if (pct < 0) {
        int cap = -1, max = -1;
        CFTypeRef cap_ref = IORegistryEntryCreateCFProperty(svc, CFSTR("CurrentCapacity"),
                                                            kCFAllocatorDefault, 0);
        CFTypeRef max_ref = IORegistryEntryCreateCFProperty(svc, CFSTR("MaxCapacity"),
                                                            kCFAllocatorDefault, 0);
        if (cap_ref && CFGetTypeID(cap_ref) == CFNumberGetTypeID())
            CFNumberGetValue((CFNumberRef)cap_ref, kCFNumberIntType, &cap);
        if (max_ref && CFGetTypeID(max_ref) == CFNumberGetTypeID())
            CFNumberGetValue((CFNumberRef)max_ref, kCFNumberIntType, &max);
        if (cap_ref)
            CFRelease(cap_ref);
        if (max_ref)
            CFRelease(max_ref);
        if (cap >= 0 && max > 0)
            pct = (int)((double)cap * 100.0 / (double)max + 0.5);
    }

    IOObjectRelease(svc);
    return pct;
}

tmac_sensors *tmac_sensors_open(void)
{
    tmac_sensors *s = calloc(1, sizeof(*s));
    if (!s)
        return NULL;

    s->battery_pct = -1;
    s->last_battery_ms = 0;

    s->client = IOHIDEventSystemClientCreate(kCFAllocatorDefault);
    if (!s->client)
        goto fail;

    CFMutableDictionaryRef matching = CFDictionaryCreateMutable(
        kCFAllocatorDefault, 2, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    if (!matching)
        goto fail;

    int page = HID_USAGE_PAGE_APPLE_VENDOR;
    int usage = HID_USAGE_TEMPERATURE;
    CFNumberRef page_num = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &page);
    CFNumberRef usage_num = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &usage);
    if (!page_num || !usage_num) {
        if (page_num)
            CFRelease(page_num);
        if (usage_num)
            CFRelease(usage_num);
        CFRelease(matching);
        goto fail;
    }
    CFDictionarySetValue(matching, CFSTR("PrimaryUsagePage"), page_num);
    CFDictionarySetValue(matching, CFSTR("PrimaryUsage"), usage_num);
    CFRelease(page_num);
    CFRelease(usage_num);

    IOHIDEventSystemClientSetMatching(s->client, matching);
    CFRelease(matching);

    s->services = IOHIDEventSystemClientCopyServices(s->client);
    if (!s->services)
        goto fail;

    CFIndex service_count = CFArrayGetCount(s->services);
    for (CFIndex i = 0; i < service_count && s->count < MAX_ENTRIES; i++) {
        IOHIDServiceClientRef service =
            (IOHIDServiceClientRef)CFArrayGetValueAtIndex(s->services, i);
        if (!service)
            continue;

        char name[48] = {0};
        CFTypeRef product = IOHIDServiceClientCopyProperty(service, CFSTR("Product"));
        if (product) {
            if (CFGetTypeID(product) == CFStringGetTypeID())
                CFStringGetCString((CFStringRef)product, name, sizeof(name), kCFStringEncodingUTF8);
            CFRelease(product);
        }

        int group = classify(name);
        if (group < 0)
            continue;

        sensor_entry *entry = &s->entries[s->count++];
        entry->service = service;
        entry->group = group;
        snprintf(entry->name, sizeof(entry->name), "%s", name);
    }

    if (notify_register_check(THERMAL_KEY, &s->thermal_token) == NOTIFY_STATUS_OK)
        s->thermal_registered = true;

    return s;

fail:
    tmac_sensors_close(s);
    return NULL;
}

void tmac_sensors_close(tmac_sensors *s)
{
    if (!s)
        return;
    if (s->thermal_registered)
        notify_cancel(s->thermal_token);
    if (s->services)
        CFRelease(s->services);
    if (s->client)
        CFRelease(s->client);
    free(s);
}

int tmac_sensor_count(const tmac_sensors *s)
{
    return s ? s->count : 0;
}

void tmac_sensors_poll(tmac_sensors *s, tmac_metrics *out)
{
    if (!s || !out)
        return;

    double die_hot = -INFINITY, die_cool = INFINITY, die_sum = 0.0;
    int die_n = 0;
    double board_sum = 0.0, nand_sum = 0.0, batt_sum = 0.0;
    int board_n = 0, nand_n = 0, batt_n = 0;

    for (int i = 0; i < s->count; i++) {
        sensor_entry *entry = &s->entries[i];
        double celsius = NAN;

        IOHIDEventRef event = IOHIDServiceClientCopyEvent(
            entry->service, HID_EVENT_TYPE_TEMPERATURE, 0, 0);
        if (event) {
            celsius = IOHIDEventGetFloatValue(event, HID_EVENT_FIELD_TEMPERATURE);
            CFRelease(event);
        }

        /* Apple leaves unused PMU channels at roughly -1.x C. */
        bool valid = isfinite(celsius) && celsius > -0.5 && celsius < 150.0;
        entry->valid = valid;
        entry->celsius = valid ? celsius : NAN;
        if (!valid)
            continue;

        switch (entry->group) {
        case GROUP_DIE:
            if (celsius > die_hot)
                die_hot = celsius;
            if (celsius < die_cool)
                die_cool = celsius;
            die_sum += celsius;
            die_n++;
            break;
        case GROUP_BOARD:
            board_sum += celsius;
            board_n++;
            break;
        case GROUP_NAND:
            nand_sum += celsius;
            nand_n++;
            break;
        case GROUP_BATTERY:
            batt_sum += celsius;
            batt_n++;
            break;
        default:
            break;
        }
    }

    memset(out, 0, sizeof(*out));
    out->die_hot = NAN;
    out->die_avg = NAN;
    out->die_cool = NAN;
    out->board = NAN;
    out->nand = NAN;
    out->battery = NAN;
    out->battery_pct = -1;
    out->thermal_level = -1;

    out->die_count = die_n;
    out->board_count = board_n;
    out->nand_count = nand_n;
    out->battery_count = batt_n;

    if (die_n > 0) {
        out->die_hot = die_hot;
        out->die_cool = die_cool;
        out->die_avg = die_sum / (double)die_n;
    }
    if (board_n > 0) {
        out->board = board_sum / (double)board_n;
        out->has_board = true;
    }
    if (nand_n > 0) {
        out->nand = nand_sum / (double)nand_n;
        out->has_nand = true;
    }
    if (batt_n > 0) {
        out->battery = batt_sum / (double)batt_n;
        out->has_battery = true;
    }

    if (s->thermal_registered) {
        uint64_t state = 0;
        if (notify_get_state(s->thermal_token, &state) == NOTIFY_STATUS_OK) {
            out->thermal_level = (int)state;
            out->has_thermal = true;
        }
    }

    uint64_t now = now_ms();
    if (s->last_battery_ms == 0 || now - s->last_battery_ms >= BATTERY_REFRESH_MS) {
        s->battery_pct = read_battery_percent();
        s->battery_pct_valid = s->battery_pct >= 0;
        s->last_battery_ms = now;
    }
    out->battery_pct = s->battery_pct;
    out->has_battery_pct = s->battery_pct_valid;
}

const char *tmac_thermal_name(int level)
{
    switch (level) {
    case 0:
        return "nominal";
    case 1:
        return "fair";
    case 2:
        return "serious";
    case 3:
        return "critical";
    default:
        return "n/a";
    }
}

int tmac_core_count(void)
{
    int cores = 0;
    size_t len = sizeof(cores);
    if (sysctlbyname("hw.ncpu", &cores, &len, NULL, 0) != 0)
        return 0;
    return cores;
}

void tmac_device_label(char *out, size_t n)
{
    static const struct {
        const char *model;
        const char *label;
    } models[] = {
        {"Mac15,12", "MacBook Air 13\" (M3)"},
        {"Mac15,13", "MacBook Air 15\" (M3)"},
        {"Mac15,3", "MacBook Pro 14\" (M3)"},
        {"Mac15,6", "MacBook Pro 14\" (M3 Pro)"},
        {"Mac15,8", "MacBook Pro 14\" (M3 Max)"},
        {"Mac15,10", "MacBook Pro 16\" (M3 Max)"},
        {"Mac15,4", "iMac 24\" (M3)"},
        {"Mac15,5", "iMac 24\" (M3)"},
        {"Mac14,2", "MacBook Air 13\" (M2)"},
        {"Mac14,15", "MacBook Air 15\" (M2)"},
        {"Mac14,5", "MacBook Pro 14\" (M2 Pro)"},
        {"Mac14,6", "MacBook Pro 14\" (M2 Max)"},
        {"Mac14,9", "MacBook Pro 16\" (M2 Pro)"},
        {"Mac14,10", "MacBook Pro 16\" (M2 Max)"},
        {"Mac14,3", "Mac mini (M2)"},
        {"Mac14,12", "Mac mini (M2 Pro)"},
        {"Mac14,13", "Mac Studio (M2 Max)"},
        {"Mac14,14", "Mac Studio (M2 Ultra)"},
        {"MacBookAir10,1", "MacBook Air (M1)"},
        {"MacBookPro17,1", "MacBook Pro 13\" (M1)"},
        {"MacBookPro18,1", "MacBook Pro 16\" (M1 Pro)"},
        {"MacBookPro18,2", "MacBook Pro 16\" (M1 Max)"},
        {"MacBookPro18,3", "MacBook Pro 14\" (M1 Pro)"},
        {"MacBookPro18,4", "MacBook Pro 14\" (M1 Max)"},
        {"Macmini9,1", "Mac mini (M1)"},
        {"Mac13,1", "Mac Studio (M1 Max)"},
        {"Mac13,2", "Mac Studio (M1 Ultra)"},
        {"Mac13,3", "iMac 24\" (M1)"},
    };

    char model[64] = {0};
    size_t len = sizeof(model);
    if (sysctlbyname("hw.model", model, &len, NULL, 0) != 0 || model[0] == '\0') {
        snprintf(out, n, "Mac");
        return;
    }
    model[sizeof(model) - 1] = '\0';

    for (size_t i = 0; i < sizeof(models) / sizeof(models[0]); i++) {
        if (strcmp(model, models[i].model) == 0) {
            snprintf(out, n, "%s", models[i].label);
            return;
        }
    }
    snprintf(out, n, "%s", model);
}
