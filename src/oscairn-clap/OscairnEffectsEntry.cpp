/*
 * Oscairn effects: sst-effects as CLAP plugins for the Oscairn host (doc/oscairn-plan.md).
 * GPL-3.0-or-later, see OscairnEffects.cpp.
 */
#include "OscairnEffects.h"

static bool entryInit(const char *path) { return oscairn_effects_entry.init(path); }
static void entryDeinit() { oscairn_effects_entry.deinit(); }
static const void *entryFactory(const char *id) { return oscairn_effects_entry.get_factory(id); }

extern "C" CLAP_EXPORT const clap_plugin_entry_t clap_entry = {CLAP_VERSION_INIT, entryInit,
                                                               entryDeinit, entryFactory};
