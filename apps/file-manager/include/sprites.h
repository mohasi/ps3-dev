#pragma once

// sprites - named regions for the app spritesheet

#include "gfx.h"

enum SpriteId {
    SPRITE_CHEVRON,
    SPRITE_CHECKBOX,
    SPRITE_CHECKBOX_CHECKED,
    SPRITE_SEPARATOR,
    SPRITE_HOVER,
    SPRITE_FOLDER_ICON,
    SPRITE_GENERIC_FILE_ICON,
    SPRITE_TEXT_FILE_ICON,
    SPRITE_AUDIO_FILE_ICON,
    SPRITE_COUNT
};

static const SpriteRegion sprites[SPRITE_COUNT] = {
    [SPRITE_CHEVRON]           = {   0,   0,   7, 12 },
    [SPRITE_CHECKBOX]          = {  18,   6,  30, 30 },
    [SPRITE_CHECKBOX_CHECKED]  = {  50,   6,  31, 30 },
    [SPRITE_SEPARATOR]         = {   0,  13,   3,  2 },
    [SPRITE_HOVER]             = {   7, 113,  33, 58 },
    [SPRITE_FOLDER_ICON]       = {   3,  49,  63, 53 },
    [SPRITE_GENERIC_FILE_ICON] = { 254,  45,  50, 63 },
    [SPRITE_TEXT_FILE_ICON]    = { 133,  45,  52, 63 },
    [SPRITE_AUDIO_FILE_ICON]   = { 362,  45,  49, 62 },
};

#define SPRITE_FULL ((SpriteRegion){0})
