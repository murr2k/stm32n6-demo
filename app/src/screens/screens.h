/*
 * Screen registry.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef SCREENS_H
#define SCREENS_H

#include <zephyr/sys/util.h>

#include "platform/gfx.h"
#include "ui/shell.h"

extern const struct screen screen_home;
extern const struct screen screen_deck;
extern const struct screen screen_siglab;
extern const struct screen screen_explorer;
extern const struct screen screen_touchlab;
extern const struct screen screen_cinema;
extern const struct screen screen_hostlink;
extern const struct screen screen_coremap;
extern const struct screen screen_specimen;

#endif
