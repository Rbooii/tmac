#ifndef TMAC_THEME_H
#define TMAC_THEME_H

#include <stdint.h>

typedef struct {
    uint8_t r, g, b;
} tmac_rgb;

typedef struct {
    const char *name;
    tmac_rgb accent;
    tmac_rgb accent2;
    tmac_rgb warn;
    tmac_rgb hot;
    tmac_rgb text;
    tmac_rgb dim;
} tmac_theme;

static const tmac_theme TMAC_THEMES[] = {
    {"mint", {94, 234, 212}, {34, 211, 238}, {245, 158, 11}, {239, 68, 68}, {226, 232, 240}, {100, 116, 139}},
    {"ice", {125, 211, 252}, {96, 165, 250}, {251, 191, 36}, {248, 113, 113}, {226, 232, 240}, {100, 116, 139}},
    {"mono", {224, 224, 224}, {176, 176, 176}, {208, 208, 208}, {255, 255, 255}, {236, 236, 236}, {120, 120, 120}},
};

#define TMAC_THEME_COUNT ((int)(sizeof(TMAC_THEMES) / sizeof(TMAC_THEMES[0])))

#endif
