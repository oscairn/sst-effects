# Saved states of published package versions

One directory per published `org.oscairn.effects` package version, one per plugin inside it,
holding states saved by that version's own published macOS arm64 build (release record
sha256 checked, state saved through the host's `oscairn-plugin-check`, which keeps the
saved state of the plugin it plays). Each `.state` file is a plugin state exactly as the
plugin saves it (`oscairn-effects-1 <plugin id>` and one `parameter id=value` line per
parameter).

| Package version | Plugins in it |
| --- | --- |
| 0.1.0, 0.1.1 | reverb, eq, delay, dynamics |
| 0.3.5, 0.3.6, 0.3.7, 0.3.8 | reverb, eq, delay, dynamics, tremolo-rotary, chorus, amp-chain |

Per plugin: `default.state` (nothing changed), then non-default states. Reverb has
`mode-N.state` for every mode that version has (Mode set, plus two of the mode's parameters
moved). Every other plugin has `tweak-mode-D.state` (its default mode, two parameters moved)
and, when that version has more than one mode, `mode-N.state` for its highest mode.

Why every published version is covered: the state format (`oscairn-effects-1`, the
plugin id, parameter ids) has not changed since 0.1.0. Every parameter id a covered version
saves exists in the current build with the same module, name, range, default and stepping,
except Mode, whose range only grew (modes are never renumbered), and the current load skips
unknown ids and keeps defaults for missing ones. No published version needs a conversion.

Every `.state` is loaded by the test "States saved by published package versions open in
this build" (`../OscairnEffectsTests.cpp`): it must load, every parameter must read back as
saved, and the plugin must render a finite signal. The 0.3.8 states (the newest published
format) also have a `.render` file written by the published 0.3.8 build: the RMS of each 1200
samples of the left, then the right channel, for the test's own signal at 48 kHz in 37-sample
blocks. The current build must match within a relative 1e-4.

When a version is published, add its directory the same way and keep the older ones.
