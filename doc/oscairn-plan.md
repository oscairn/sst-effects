# Oscairn extension plan: an effects suite on sst-effects

Status: plan, 2026-11-01. Milestone 0 is being implemented on branch `oscairn`; section 15
records what was built, how it was tested and where it departs from this plan. This fork is
[oscairn/sst-effects](https://github.com/oscairn/sst-effects), forked from
[surge-synthesizer/sst-effects](https://github.com/surge-synthesizer/sst-effects) at `987b39d`.

The goal: Oscairn effects that cover every effect type a Yamaha MONTAGE M / MODX M Performance
can use, so converted Performances keep their effects and sound as they do on the reference
instrument. Yamaha names appear in this document only to say what is matched (see
[Naming](#10-licences-ip-and-naming)); they never appear in code, identifiers, parameter names
or UI.

File paths are relative to this repository unless they start with `oscairn/` (the
[Oscairn](https://github.com/jbschooley/oscairn) repository). Statements about sst-effects were
checked against this checkout; anything not checked is marked **unverified**.

## 1. Goal, scope and relation to RFC 0016

Oscairn RFC 0016 (`oscairn/docs/rfcs/0016-effects-suite.md`, revised 2026-11-01) asks for effects
covering the platform's 104 effect types, measured against reference renders, with several
types allowed to be modes of one effect. This fork is the implementation of RFC 0016 steps 2
to 4 for the effects themselves. Like the synth engines (Dexed as "Octet", Surge XT as "Oscairn
Analog", sfizz for the sample engine), it is a fork of an existing, maintained library:
sst-effects, the Surge Synth Team's header-only effect templates (GPL-3.0-or-later).

In scope:

- one CLAP plugin per effect category, each with algorithm modes covering that category's
  types (section 6), built from this repository as native Oscairn effect contexts (RFC 0007);
- the Part EQs and Master EQ, which are not effect types but are needed by every converted
  Performance (section 4);
- parameters that a converter can fill from a Performance's raw effect values with no loss, and
  a mapping table per type (RFC 0016 step 4 consumes it);
- measurements against reference renders, with analysis code in the measurement repository's
  `fx/` folder.

Out of scope (Oscairn work, RFC 0016 step 1; section 4): placing effects on parts, buses and
wires (the converter's mapping), node roles, and the host-side side-chain test on effect
contexts; also the converter itself (`oscairn/performance-converter`) and DSP/FPGA backends.

### Shape of the work

- **Mostly ports on a forked base.** sst-effects is the base. Where another project has a better
  block it is borrowed, pinned and credited, licence permitting (section 3). Types with no open
  equivalent are written here.
- **One plugin per category, modes inside.** RFC 0016 leaves this open; per-category matches
  RFC 0007's shared native engines (one engine, many independent contexts) and keeps the plugin
  count at 13 (section 6). A mode is one Yamaha type or a family sharing a parameter table.
- **Stock behaviour stays exact.** The library's existing effects keep sounding as upstream.
  Reference-matching changes to a stock effect go behind a per-preset setting that defaults to
  off (section 8), never a global switch. New code goes in new files; hooks into upstream files
  stay small (section 9).

## 2. How sst-effects is structured today

Checked against `987b39d`.

| Piece | Where | Notes |
| --- | --- | --- |
| Build | `CMakeLists.txt` | An `INTERFACE` library (headers only). Tests and examples are opt-in (`SST_EFFECTS_BUILD_TESTS`, `SST_EFFECTS_BUILD_EXAMPLES`); dependencies come from CPM, tracking `main` of sst-basic-blocks, sst-filters, sst-waveshapers and sst-plugininfra, simde 0.7.2, fmt 12.1.0 and the `surge` branch of Surge's eurorack fork |
| Bus effects | `include/sst/effects/` | One class template per effect over an `FXConfig`: `Reverb1`, `Reverb2`, `Delay` (dual line with crossfeed, filters, modulation), `FloatyDelay`, `Flanger`, `Phaser`, `RotarySpeaker`, `Bonsai` (bass saturation), `TreeMonster` (pitch-tracked ring modulation), `Nimbus` (granular, needs eurorack). `Chorus.h` is an empty placeholder |
| Effect contract | `include/sst/effects/EffectCore.h` | `ValidEffectConfiguration` (compile-time block size, power of two and at least 4; storage types; value accessors; tempo sync, deactivation and extension flags; sample rate; RNG; tuning) and `ValidEffect` (`initialize`, `processBlock(L, R)` in place, `suspendProcessing`, `getRingoutDecay`, `paramAt` returning `ParamMetaData`, `onSampleRateChanged`) |
| Reference config | `include/sst/effects/ConcreteConfig.h` | Block size 16, 20 float parameters, no tempo sync, `rand()` for randomness |
| Voice effects | `include/sst/voice-effects/` | A second contract (`VoiceEffectCore.h`, `VoiceEffectConfig`): per-voice float and int parameters, mono or stereo processing. About 45 effects: Compressor, AutoWah, BitCrusher, Slewer, EQs (N-band parametric, 6-band graphic, tilt, morph), WaveShaper, filters (utility, sst-filters' full set, static phaser), modulation (Chorus, Flanger, Phaser, Tremolo, RingMod, FreqShiftMod, NoiseAM, ShepardPhaser, PhaseMod, FMFilter), generators (StringResonator, noise sources, oscillators), utilities (StereoTool, GainMatrix, VolumeAndPan), ShortDelay, Widener, MicroGate, and lifted bus effects (Delay, Reverb1, Reverb2 run as voice effects) |
| Shared | `include/sst/effects-shared/` | Width provider, TreeMonster core |
| Presets | `presets/`, `src/preset-api.*` | Factory presets for the voice effects, compiled in when sst-plugininfra's tinyxml is present |
| Tests | `tests/` | Catch2: every bus effect runs 1,000 blocks of a saw/square input through `ConcreteConfig` with outputs bounded; voice effects are created and run; SFINAE checks. They test that effects run, not what they sound like |
| CI | `.github/workflows/` | On pull requests to `main` only: Linux, macOS x86/arm64, Windows x86/arm64, a Docker Ubuntu 20 build, code checks |

Parameters are described by `sst::basic_blocks::params::ParamMetaData` (range, default, unit
display, tempo-sync and deactivation capability), which maps directly onto CLAP parameter info
and value text. Every effect processes fixed blocks of `FXConfig::blockSize` samples, so a CLAP
wrapper buffers one block and reports that as latency (section 7).

Surge XT has effects that were never moved into this library (`src/common/dsp/effects/` in
Surge: Vocoder, Distortion, Conditioner, Ring Modulator, Frequency Shifter, BBD Ensemble, Chorus,
Exciter, Spring Reverb, Tape, Neuron, Waveshaper, MS Tool, 3-band parametric and 11-band graphic
EQ, Combulator, Resonator, the Airwindows ports). They are GPL-3.0-or-later like this library,
but depend on `SurgeStorage`; porting one means moving its DSP onto the `FXConfig` contract,
which is what upstream itself has been doing since 2023. Those are listed as "borrowed (Surge
XT)" below.

## 3. Licences

Each checked from the project's own licence file and, where the licence file does not settle
"or later", from its source headers (2026-11-01). This fork is GPL-3.0-or-later (`LICENSE.md`),
so a borrowed block must be GPL-3.0-compatible.

| Project | Licence (from its files) | GPL-3.0 compatible | Use here |
| --- | --- | --- | --- |
| sst-effects, sst-basic-blocks, sst-filters, sst-waveshapers | GPL-3.0 (`LICENSE.md` in each; headers say "GPL-3.0-or-later") | Same licence | Base |
| Surge XT | GPL-3.0 (`LICENSE`; headers "GPL-3.0-or-later") | Same licence | Distortion, BBD lines and Ensemble, Exciter, Ring Modulator, Vocoder |
| Airwindows | MIT (`LICENSE`, Chris Johnson) | Yes | Candidate blocks (consoles, saturation, reverbs) where a measurement shows they fit better |
| LSP Plugins | LGPL-3.0 (`COPYING`, `COPYING.LESSER`; headers "version 3 of the License, or any later version") | Yes (LGPL-3.0 code may be combined into a GPL-3.0 work) | Candidate: multiband compressor and crossover design; not borrowed in this plan |
| Dragonfly Reverb | GPL-3.0 (`LICENSE`; plugin headers "version 3 of the License, or any later version") | Same licence | Early-reflection model |
| Freeverb3 (inside Dragonfly, `common/freeverb/`) | GPL (`COPYING` there; headers "version 2 of the License, or (at your option) any later version") | Yes, as GPL-3.0 | `earlyref`, candidate tank reverbs (`progenitor2`, `zrev2`, `nrevb`, `strev`) |
| Signalsmith Stretch | MIT (`LICENSE.txt`, Geraint Luff / Signalsmith Audio) | Yes | Pitch Change |
| setBfree | GPL-2.0 text in `COPYING`; source headers (`b_whirl/whirl.c`) "version 2, or (at your option) any later version" | Yes, as GPL-3.0 (only because of "or later"; each borrowed file's header must be checked when it is copied) | Rotary speaker model (`b_whirl`) |
| Guitarix | `trunk/COPYING`: "GPL version 2 or later", with some files GPL-3.0+ or LGPL-3.0+ | Yes, as GPL-3.0; per-file headers to be checked when copied | Tone stacks for the amp types |
| Neural Amp Modeler core | MIT (`LICENSE`) | Yes | Not planned: needs captures, which must not be captures of the reference |
| Rubber Band | GPL-2.0 text in `COPYING`; headers "version 2 of the License, or (at your option) any later version" (or commercial) | Yes, as GPL-3.0 | Not planned; Signalsmith Stretch covers pitch change |
| Mutable Instruments eurorack (Surge fork, used by Nimbus) | README: "Code (STM32F projects): MIT license", AVR projects GPL-3.0 | Yes | Upstream dependency, unchanged |
| CLAP SDK | MIT (`LICENSE`) | Yes | Plugin API, pinned at 1.2.7 |
| Oscairn extension headers (`core/clap-ext/include/oscairn-clap/`) | MIT (SPDX headers) | Yes | Vendored unchanged in `libs/oscairn-clap/` |

Each borrowed file keeps its header and notice, is pinned to a commit in `libs/` or copied with
its origin recorded in `src/oscairn-clap/CREDITS.md`, and is credited in Oscairn's CREDITS when
the plugin ships.

## 4. Where the effects go in Oscairn, and what the host still needs

RFC 0016 (revised 2026-11-01, `4ee7da6` and `4e84501`) has no slot model. Each Yamaha slot is a
place in Oscairn's ordinary graph (RFC 0011's parts, chains, shared buses and wires), and an
effect is an ordinary CLAP plugin that does not know which slot it fills. This fork provides the
plugins; the converter (`oscairn/performance-converter`) places them.

| Yamaha slot | In Oscairn (RFC 0016) | Plugin from this fork |
| --- | --- | --- |
| Part Insertion A and B | Plugins in the part's chain, in the Part's order: A→B or B→A in series, or Parallel as a split (Parallel is not offered for FM-X and AN-X parts) | Any category plugin, one effect context each |
| Element and drum-key routing to A, B or thru | The sample engine's two outputs per context wired to the two insertion chains | (none) |
| Part 3-band and 2-band EQ | Plugins in the part's chain after the insertions | EQ plugin, modes "Part 3-band" and "Part 2-band" |
| Reverb send | A shared bus (Mix node and chain); each part's Reverb Send, the A/D part's send and Variation-to-Reverb are sends into it | Reverb plugin (13 types allowed there) |
| Variation send | A second shared bus; its send to the Reverb bus is Variation-to-Reverb | Any of 91 types |
| Master Effect, Master EQ | A bus every part plays through; its chain is the master effect, then the EQ | Mostly Dynamics (28 types allowed); EQ plugin mode "Master 5-band" |
| A/D-input insertion A and B | A part with an audio input and its chain | Any of 86 types |
| VCM Rotary slot | A plugin in the chain where the Performance has it | Tremolo & Rotary plugin, rotary modes |
| Side-chain or modulator input (Variation, Insertions, Master Effect; source part 1–16, A/D or Master) | A wire into the plugin's side-chain port | A second stereo input port, "Side-chain", on the Dynamics, Flanger, Phaser, Tech and Misc plugins, used by the 11 marked types |

What the host still needs, per RFC 0016 step 1 (none of it is work in this fork):

- the converter's mapping of each slot onto parts, buses and wires (above);
- optional node roles (`insertion-a`, `insertion-b`, `part-eq`, `reverb`, `variation`,
  `master-effect`, `master-eq`) for device profiles' buttons;
- a test of side-chain on effect contexts. Each context is a full CLAP interface with its own
  ports, so nothing is missing in the host, but no plugin with contexts has had a side-chain
  port. This fork's side-chain plugins will be the first; their tests (section 7) drive the
  side-chain port of a second context directly, and the host-side test can load them.

Two details the converter must carry, which the plugins make possible:

- **Envelope followers.** The platform's dynamic types (Dynamic Flanger, Phaser, Filter, Ring
  Modulator) follow a side-chain part through that part's Envelope Follower Gain, Attack and
  Release. The plugins expose follower gain, attack and release as their own parameters, so the
  converter copies the source part's values into the effect; no host follower node is needed.
- **Tempo.** Tempo-synced modes read the CLAP transport tempo and fall back to 120 BPM when the
  host gives none (RFC 0012 decides how Oscairn's clock reaches the transport).

Per-part effect values controlled by the Super Knob, scenes, controller assignments and motion
sequences are ordinary parameter modulation in Oscairn; the plugins only need each controlled
value to be a CLAP parameter, which they are.

## 5. Usage counts

Decoded with `tools/pfm.py` from Jacob's research repository: every factory performance in the
reference plugin's bundle (3,631 files, all bank prefixes) and every user performance (2,028
files). Only counts are recorded here; no performance data is committed. A slot counts as
**active** when its type is set and it can be heard: the part exists and is switched on and
its insertion switch is on; the Reverb and Variation switches are on and at least one part, the
A/D part or Variation-to-Reverb sends to them; the Master Effect switch is on; the VCM Rotary
switch is on. "Set" counts the type field alone (every performance carries some type in each
common slot, even when the slot is off).

| Slot | Factory set | Factory active | User set | User active |
| --- | --- | --- | --- | --- |
| Part Insertion A | 7,047 | 6,883 | 3,866 | 3,787 |
| Part Insertion B | 6,348 | 6,192 | 3,508 | 3,440 |
| Reverb | 3,625 | 3,194 | 2,026 | 1,505 |
| Variation | 3,443 | 1,803 | 1,723 | 1,058 |
| Master Effect | 3,631 | 868 | 2,028 | 698 |
| A/D Insertion A | 1 | 0 | 43 | 42 |
| A/D Insertion B | 0 | 0 | 43 | 42 |
| VCM Rotary | 3,631 | 30 | 2,028 | 4 |

Top active types per slot (factory / user):

- **Part Insertion A:** VCM EQ 501 2,610 / 1,958; VCM Compressor 376 629 / 174; Classic Compressor 319 / 206; Amp Simulator 1 265 / 199; 2 Modulator 170 / 102; G Chorus 243 / 24; Amp Simulator 2 129 / 111; Classic Flanger 45 / 195; Lo-Fi 99 / 77; Harmonic Enhancer 94 / 68.
- **Part Insertion B:** Tempo Cross Delay 671 / 431; VCM EQ 501 534 / 474; VCM Compressor 376 434 / 177; Rotary Speaker 1 144 / 280; G Chorus 276 / 105; Auto Pan 256 / 113; Amp Simulator 1 198 / 168; Cross Delay 258 / 100; Early Reflection 193 / 115; 2 Modulator 185 / 122.
- **Reverb:** REV-X Hall 1,440 / 765; SPX Hall 304 / 119; R3 Hall 292 / 186; HD Hall 348 / 10; R3 Room 164 / 146; REV-X Room 163 / 117; R3 Plate 127 / 60; HD Room 85 / 48; HD Plate 79 / 11; SPX Room 47 / 25.
- **Variation:** Tempo Cross Delay 630 / 326; 2 Modulator 174 / 111; Tempo Delay Stereo 167 / 113; Delay L/R 127 / 73; Ensemble Detune 119 / 78; G Chorus 95 / 71; Cross Delay 76 / 36; VCM Phaser Stereo 39 / 47; Tempo Delay Mono 36 / 33; VCM Flanger 42 / 24.
- **Master Effect:** VCM Compressor 376 496 / 573; Multi Band Comp 265 / 88; Comp Distortion Delay 24 / 11; Lo-Fi 14 / 8; Parallel Comp 14 / 0; Delay L/R 5 / 8; Uni Comp Down 5 / 6; Isolator 3 / 4; Vinyl Break 6 / 0; VCM Mini Filter 6 / 0.
- **A/D Insertion:** Rotary Speaker 1 only (user 42 + 42).
- **VCM Rotary:** Studio only (30 / 4). Classic and Overdrive are never active in either set.

Top 20 types over all slots (factory + user, active): VCM EQ 501 5,578; VCM Compressor 376
2,484; Tempo Cross Delay 2,213; REV-X Hall 2,205; 2 Modulator 864; Amp Simulator 1 831; G Chorus
814; Classic Compressor 698; Rotary Speaker 1 663; SPX Hall 614; Multi Band Comp 572; Ensemble
Detune 571; Tempo Delay Stereo 526; Cross Delay 507; R3 Hall 478; Lo-Fi 452; Auto Pan 443; Early
Reflection 414; Amp Simulator 2 372; HD Hall 358. Every one of the 104 types is set at least
once.

Other counts that set host and EQ priorities:

| What | Factory | User |
| --- | --- | --- |
| Parts (existing, any engine) | 8,432 | 4,534 |
| Insertion connection Parallel / A→B / B→A | 1,121 / 6,741 / 570 | 1,115 / 2,924 / 495 |
| Part 3-band EQ with any gain off 0 dB | 2,601 | 2,110 |
| Part 2-band EQ with a gain off 0 dB, a non-default type or output level | 831 | 260 |
| Master EQ on with any gain off 0 dB | 684 | 565 |
| Part insertion side-chain source set (A / B) | 120 / 98 | 3 / 3 |
| Common Variation / Insertion A / Insertion B / Master side-chain set | 21 / 19 / 16 / 29 | 0 |

So the priority order RFC 0016 expected holds, with three corrections: the reverbs (REV-X Hall
above all) and Tempo Cross Delay are as common as the compressors, the Part 3-band EQ is used
on about 30% of factory parts and needs to be in the first milestone with VCM EQ 501, and the
chorus family (2 Modulator, G Chorus, Ensemble Detune) outranks the rotary types.

## 6. Coverage of the 104 types

Thirteen plugins. Twelve follow the platform's categories; Misc is split, because VCM EQ 501 is
the most used type of all and belongs with the Part and Master EQs, and Early Reflection is
reverb DSP:

| Plugin | Modes (types) | Base | Borrowed | Written | Side-chain input |
| --- | --- | --- | --- | --- | --- |
| Reverb | 16 (15 reverbs + Early Reflection) | 4 | 2 | 10 | |
| Delay | 9 | 7 | 2 | 0 | |
| Chorus | 6 | 4 | 1 | 1 | |
| Flanger | 5 | 4 | 0 | 1 | yes |
| Phaser | 5 | 3 | 0 | 2 | yes |
| Tremolo & Rotary | 7 | 4 | 3 | 0 | |
| Distortion & Amp | 12 | 1 | 11 | 0 | |
| Dynamics | 7 | 5 | 0 | 2 | yes |
| Wah | 3 | 0 | 0 | 3 | |
| Lo-Fi | 4 | 3 | 0 | 1 | |
| Tech | 18 | 4 | 1 | 13 | yes |
| EQ | VCM EQ 501 + Part 3-band, Part 2-band, Master 5-band | 0 (+3 EQs on sst-filters biquads) | 0 | 1 | |
| Misc | 11 | 3 | 3 | 5 | yes |
| **All 104 types** | | **42** | **23** | **39** | |

**Base** means an sst-effects effect (bus or voice) with a parameter mapping and, where the
reference differs, a reference-matching mode. **Borrowed** means a block from another project
in section 3. **Written** means our own implementation, using sst-basic-blocks and sst-filters
blocks where they fit; these are the types with no open equivalent: the hall/room/plate
characters of the REV-X, R3 and SPX families, the VCM circuit models, and the Tech and Misc
specialities.

Columns: platform type name as printed in the Data List; type number (MSB LSB); the slots that
offer it (Rev, Var, InsA/B, A/D, Rot, Mas); SC = takes a side-chain or modulator; active uses in
the factory and user sets over all slots; plugin; source class; how.

| Type | Hex | Slots | SC | Factory | User | Plugin | Source | How |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| HD HALL | 01 03 | Rev |  | 348 | 10 | Reverb | base | sst Reverb2 (hall tuning) + mapping |
| REV-X HALL | 01 00 | Rev |  | 1440 | 765 | Reverb | written | own FDN hall on sst-basic-blocks delay lines and filters |
| R3 HALL | 01 01 | Rev |  | 292 | 186 | Reverb | written | own tank reverb (character fitted to renders) |
| SPX HALL | 01 02 | Rev,Var,InsA/B,A/D |  | 464 | 150 | Reverb | written | own tank reverb, shared with R3 (character fitted to renders) |
| HD ROOM | 01 13 | Rev |  | 85 | 48 | Reverb | base | sst Reverb2 (room tuning) + mapping |
| REV-X ROOM | 01 10 | Rev |  | 163 | 117 | Reverb | written | own FDN, room settings |
| R3 ROOM | 01 11 | Rev |  | 164 | 146 | Reverb | written | own tank reverb, room settings |
| SPX ROOM | 01 12 | Rev,Var,InsA/B,A/D |  | 162 | 103 | Reverb | written | own tank reverb, room settings |
| HD PLATE | 01 21 | Rev |  | 79 | 11 | Reverb | base | sst Reverb2 (plate tuning: no early reflections, dense diffusion) + mapping |
| R3 PLATE | 01 20 | Rev |  | 127 | 60 | Reverb | written | own tank reverb, plate settings |
| SPX STAGE | 01 30 | Rev,Var,InsA/B,A/D |  | 95 | 37 | Reverb | written | own tank reverb, stage settings |
| SPACE SIMULATOR | 01 40 | Rev |  | 58 | 6 | Reverb | borrowed | Dragonfly / Freeverb3 `earlyref` (GPL-2.0+) for the room-geometry reflections + sst Reverb2 tail |
| GATED REVERB | 01 50 | Var,InsA/B,A/D |  | 55 | 0 | Reverb | written | own tapped-delay reflections with a gate envelope |
| REVERSE REVERB | 01 58 | Var,InsA/B,A/D |  | 103 | 1 | Reverb | written | own tapped-delay reflections with a rising envelope |
| SHIMMER REVERB | 01 60 | Rev |  | 50 | 1 | Reverb | base | sst Reverb2 + own pitch shifter in the feedback path |
| CROSS DELAY | 02 00 | Var,InsA/B,A/D |  | 358 | 149 | Delay | base | sst Delay (dual line, crossfeed, low/high cut) + mapping |
| TEMPO CROSS DELAY | 02 10 | Var,InsA/B,A/D |  | 1426 | 787 | Delay | base | sst Delay, tempo-synced times + mapping |
| TEMPO DELAY MONO | 02 20 | Var,InsA/B,A/D |  | 84 | 61 | Delay | base | sst Delay, mono routing + mapping |
| TEMPO DELAY STEREO | 02 28 | Var,InsA/B,A/D |  | 361 | 165 | Delay | base | sst Delay, tempo-synced stereo + mapping |
| CONTROL DELAY | 02 30 | Var,InsA/B,A/D |  | 39 | 1 | Delay | base | sst Delay with a smoothed, controllable time + mapping |
| DELAY LR | 02 40 | Var,InsA/B,A/D,Mas |  | 233 | 108 | Delay | base | sst Delay, independent L/R lines + mapping |
| DELAY LCR | 02 50 | Var,InsA/B,A/D |  | 76 | 11 | Delay | base | sst Delay + an added centre tap (new file) + mapping |
| ANALOG DELAY RETRO | 02 60 | Var,InsA/B,A/D |  | 48 | 1 | Delay | borrowed | Surge XT BBD delay line (`chowdsp/bbd_utils`, GPL-3.0) + own feedback path |
| ANALOG DELAY MODERN | 02 68 | Var,InsA/B,A/D |  | 36 | 0 | Delay | borrowed | Surge XT BBD delay line (GPL-3.0) + own feedback path |
| G CHORUS | 03 00 | Var,InsA/B,A/D |  | 614 | 200 | Chorus | base | sst voice Chorus + sst Tremolo (AM) + mapping |
| 2 MODULATOR | 03 10 | Var,InsA/B,A/D |  | 529 | 335 | Chorus | base | sst voice Chorus (two voices) + AM + mapping |
| SPX CHORUS | 03 20 | Var,InsA/B,A/D |  | 239 | 110 | Chorus | base | sst voice Chorus + AM + mapping |
| SYMPHONIC | 03 30 | Var,InsA/B,A/D |  | 158 | 35 | Chorus | base | sst voice Chorus (multi-voice) + mapping |
| ENSEMBLE DETUNE | 03 40 | Var,InsA/B,A/D |  | 384 | 187 | Chorus | written | own short-window pitch shifter (shared with Pitch Change's fallback) |
| GS1 ENSEMBLE & TREMOLO | 03 50 | Var,InsA/B,A/D |  | 52 | 0 | Chorus | borrowed | Surge XT BBD Ensemble (GPL-3.0) + sst Tremolo |
| VCM FLANGER | 04 00 | Var,InsA/B,A/D |  | 210 | 116 | Flanger | written | own circuit-style flanger on the Surge XT BBD line |
| CLASSIC FLANGER | 04 10 | Var,InsA/B,A/D |  | 124 | 224 | Flanger | base | sst Flanger + mapping |
| TEMPO FLANGER | 04 20 | Var,InsA/B,A/D |  | 79 | 33 | Flanger | base | sst Flanger, tempo-synced LFO + mapping |
| DYNAMIC FLANGER | 04 30 | Var,InsA/B,A/D | yes | 16 | 10 | Flanger | base | sst Flanger + own envelope follower (side-chain) |
| CONTROL FLANGER | 04 08 | Var,InsA/B,A/D |  | 7 | 0 | Flanger | base | sst Flanger, manual sweep + mapping |
| VCM PHASER MONO | 05 00 | Var,InsA/B,A/D |  | 51 | 55 | Phaser | written | own circuit-style phaser (OTA all-pass stages) |
| VCM PHASER STEREO | 05 10 | Var,InsA/B,A/D |  | 158 | 101 | Phaser | written | own circuit-style phaser, stereo |
| TEMPO PHASER | 05 20 | Var,InsA/B,A/D |  | 144 | 39 | Phaser | base | sst Phaser, tempo-synced + mapping |
| DYNAMIC PHASER | 05 30 | Var,InsA/B,A/D | yes | 18 | 9 | Phaser | base | sst Phaser + own envelope follower (side-chain) |
| CONTROL PHASER | 05 18 | Var,InsA/B,A/D |  | 4 | 0 | Phaser | base | sst Phaser, manual sweep + mapping |
| AUTO PAN | 06 00 | Var,InsA/B,A/D |  | 311 | 132 | Tremolo & Rotary | base | sst voice Tremolo (pan mode) + mapping |
| TREMOLO | 06 10 | Var,InsA/B,A/D |  | 205 | 69 | Tremolo & Rotary | base | sst voice Tremolo + mapping |
| ROTARY SPEAKER 1 | 06 20 | Var,InsA/B,A/D |  | 223 | 440 | Tremolo & Rotary | base | sst RotarySpeaker + mapping |
| ROTARY SPEAKER 2 | 06 30 | Var,InsA/B,A/D |  | 42 | 2 | Tremolo & Rotary | base | sst RotarySpeaker (separate horn/rotor speeds, mic angle) + mapping |
| VCM ROTARY SPEAKER CLASSIC | 06 40 | Rot |  | 0 | 0 | Tremolo & Rotary | borrowed | setBfree `b_whirl` (GPL-2.0+) + own preamp |
| VCM ROTARY SPEAKER OVERDRIVE | 06 41 | Rot |  | 0 | 0 | Tremolo & Rotary | borrowed | setBfree `b_whirl` (GPL-2.0+) + own overdrive preamp |
| VCM ROTARY SPEAKER STUDIO | 06 42 | Rot |  | 30 | 4 | Tremolo & Rotary | borrowed | setBfree `b_whirl` (GPL-2.0+), studio mic settings |
| AMP SIMULATOR 1 | 07 00 | Var,InsA/B,A/D |  | 464 | 367 | Distortion & Amp | borrowed | Surge XT Distortion (pre/post EQ, waveshaper; GPL-3.0) + mapping |
| AMP SIMULATOR 2 | 07 10 | Var,InsA/B,A/D |  | 224 | 148 | Distortion & Amp | borrowed | Surge XT Distortion + sst-waveshapers + mapping |
| COMP DISTORTION | 07 20 | Var,InsA/B,A/D |  | 81 | 48 | Distortion & Amp | borrowed | sst voice Compressor + Surge XT Distortion |
| COMP DISTORTION DELAY | 07 30 | Var,InsA/B,A/D,Mas |  | 138 | 115 | Distortion & Amp | borrowed | sst voice Compressor + Surge XT Distortion + sst Delay |
| US COMBO | 07 40 | Var,InsA/B,A/D |  | 46 | 0 | Distortion & Amp | borrowed | Guitarix tone stacks (GPL-2.0+) + sst-waveshapers preamp + own cabinet filter |
| JAZZ COMBO | 07 41 | Var,InsA/B,A/D |  | 30 | 11 | Distortion & Amp | borrowed | Guitarix tone stacks + sst-waveshapers + own cabinet filter |
| US HIGH GAIN | 07 42 | Var,InsA/B,A/D |  | 6 | 0 | Distortion & Amp | borrowed | Guitarix tone stacks + sst-waveshapers + own cabinet filter |
| BRITISH LEAD | 07 43 | Var,InsA/B,A/D |  | 8 | 18 | Distortion & Amp | borrowed | Guitarix tone stacks + sst-waveshapers + own cabinet filter |
| MULTI FX | 07 44 | Var,InsA/B,A/D |  | 45 | 26 | Distortion & Amp | base | chain of the suite's own blocks: compressor, distortion, wah, phaser/flanger, delay |
| SMALL STEREO | 07 45 | Var,InsA/B,A/D |  | 41 | 27 | Distortion & Amp | borrowed | Surge XT Distortion + own small-cabinet filter |
| BRITISH COMBO | 07 46 | Var,InsA/B,A/D |  | 31 | 1 | Distortion & Amp | borrowed | Guitarix tone stacks + sst-waveshapers + own cabinet filter |
| BRITISH LEGEND | 07 47 | Var,InsA/B,A/D |  | 1 | 0 | Distortion & Amp | borrowed | Guitarix tone stacks + sst-waveshapers + own cabinet filter |
| VCM COMPRESSOR 376 | 08 00 | Var,InsA/B,A/D,Mas | yes | 1560 | 924 | Dynamics | written | own circuit-style feedback compressor (fast attack, programme-dependent release) |
| CLASSIC COMPRESSOR | 08 10 | Var,InsA/B,A/D | yes | 421 | 277 | Dynamics | base | sst voice Compressor (side-chain input) + mapping |
| MULTI BAND COMP | 08 20 | Var,InsA/B,A/D,Mas | yes | 427 | 145 | Dynamics | base | sst-filters Linkwitz-Riley crossovers + three sst Compressors |
| UNI COMP DOWN | 08 30 | Var,InsA/B,A/D,Mas | yes | 41 | 6 | Dynamics | base | sst Compressor + EQ + post HPF + mapping |
| UNI COMP UP | 08 38 | Var,InsA/B,A/D,Mas | yes | 7 | 0 | Dynamics | written | own upward compressor (shares the detector) |
| PARALLEL COMP | 08 40 | Var,InsA/B,A/D,Mas |  | 49 | 0 | Dynamics | base | sst Compressor with dry mix + mapping |
| M/S EQ COMPRESSOR | 08 50 | Var,InsA/B,A/D,Mas |  | 22 | 0 | Dynamics | base | sst Compressor and EQ in mid/side + mapping |
| VCM AUTO WAH | 0A 00 | Var,InsA/B,A/D |  | 50 | 25 | Wah | written | own circuit-style wah filter, LFO-driven |
| VCM TOUCH WAH | 0A 10 | Var,InsA/B,A/D |  | 42 | 42 | Wah | written | own circuit-style wah filter, envelope-driven |
| VCM PEDAL WAH | 0A 20 | Var,InsA/B,A/D |  | 12 | 5 | Wah | written | own circuit-style wah filter, pedal-driven |
| LO-FI | 0B 00 | Var,InsA/B,A/D,Mas |  | 235 | 217 | Lo-Fi | base | sst BitCrusher + sst-filters + mapping |
| NOISY | 0B 10 | Var,InsA/B,A/D |  | 37 | 18 | Lo-Fi | base | sst noise generators (TiltNoise/CorrelatedNoise) + sst BitCrusher + mapping |
| DIGITAL TURNTABLE | 0B 20 | Var,InsA/B,A/D |  | 24 | 0 | Lo-Fi | written | own vinyl noise, crackle and wow |
| BIT CRUSHER | 0B 30 | Var,InsA/B,A/D,Mas |  | 79 | 0 | Lo-Fi | base | sst BitCrusher + mapping |
| RING MODULATOR | 0C 00 | Var,InsA/B,A/D,Mas | yes | 236 | 95 | Tech | base | sst voice RingMod (side-chain as modulator) + mapping |
| CS RING MODULATOR | 0C 08 | Var,InsA/B,A/D,Mas |  | 26 | 0 | Tech | borrowed | Surge XT RingModulator (diode model, GPL-3.0) + mapping |
| DYNAMIC RING MODULATOR | 0C 10 | Var,InsA/B,A/D | yes | 53 | 33 | Tech | base | sst RingMod + own envelope follower (side-chain) |
| DYNAMIC FILTER | 0C 20 | Var,InsA/B,A/D,Mas | yes | 48 | 15 | Tech | base | sst-filters + own envelope follower (side-chain) |
| AUTO SYNTH | 0C 30 | Var,InsA/B,A/D |  | 50 | 28 | Tech | written | own |
| SPIRALIZER P | 0C 38 | Var,InsA/B,A/D,Mas |  | 12 | 0 | Tech | written | own (sst ShepardPhaser as a starting block) |
| TEMPO SPIRALIZER P | 0C 39 | Var,InsA/B,A/D,Mas |  | 3 | 0 | Tech | written | own, tempo-synced |
| SPIRALIZER F | 0C 3A | Var,InsA/B,A/D,Mas |  | 14 | 0 | Tech | written | own (Shepard-style flanger) |
| TEMPO SPIRALIZER F | 0C 3B | Var,InsA/B,A/D,Mas |  | 11 | 0 | Tech | written | own, tempo-synced |
| ISOLATOR | 0C 40 | Var,InsA/B,A/D,Mas |  | 37 | 22 | Tech | written | own three-band kill EQ on sst-filters crossovers |
| SLICE | 0C 50 | Var,InsA/B,A/D,Mas |  | 29 | 5 | Tech | written | own tempo-synced gate patterns |
| TECH MODULATION | 0C 60 | Var,InsA/B,A/D |  | 56 | 27 | Tech | written | own |
| CONTROL FILTER | 0C 28 | Var,InsA/B,A/D |  | 48 | 1 | Tech | base | sst-filters (FiltersPlusPlus) + mapping |
| VINYL BREAK | 0C 70 | Var,InsA/B,Mas |  | 18 | 0 | Tech | written | own (tape/vinyl stop: variable-rate playback buffer) |
| BEAT REPEAT EVEN | 0C 78 | Var,InsA/B,Mas |  | 30 | 0 | Tech | written | own tempo-synced capture and repeat buffer |
| BEAT REPEAT TRIPLET | 0C 79 | Var,InsA/B,Mas |  | 7 | 0 | Tech | written | own, triplet grid |
| BEAT REPEAT EVEN+TRIPLET | 0C 7A | Var,InsA/B,Mas |  | 16 | 0 | Tech | written | own, mixed grid |
| BEAT REPEAT FREE | 0C 7B | Var,InsA/B,Mas |  | 9 | 0 | Tech | written | own, free length |
| VCM EQ 501 | 0D 00 | Var,InsA/B,A/D |  | 3146 | 2432 | EQ | written | own circuit-style 5-band EQ (analog-matched biquads, gain-dependent Q fitted to renders) |
| PRESENCE | 0D 08 | Var,InsA/B,A/D,Mas |  | 14 | 0 | Misc | base | sst EqNBandParametric + mapping |
| HARMONIC ENHANCER | 0D 10 | Var,InsA/B,A/D |  | 214 | 107 | Misc | borrowed | Surge XT Exciter (chowdsp, GPL-3.0) + mapping |
| STEREOPHONIC OPTIMIZER | 0D 18 | Var,InsA/B,A/D |  | 23 | 0 | Misc | written | own |
| TALKING MODULATOR | 0D 20 | Var,InsA/B,A/D |  | 45 | 14 | Misc | written | own formant filter bank (vowel morph) |
| WAVE FOLDER | 0D 28 | Var,InsA/B,A/D,Mas |  | 65 | 0 | Misc | base | sst-waveshapers fold shapes + mapping |
| VCM MINI FILTER | 0D 29 | Var,InsA/B,A/D,Mas |  | 63 | 0 | Misc | written | own circuit-style ladder filter |
| VCM MINI BOOSTER | 0D 2A | Var,InsA/B,A/D,Mas |  | 14 | 0 | Misc | written | own circuit-style booster |
| DAMPER RESONANCE | 0D 30 | Var,InsA/B,A/D |  | 173 | 111 | Misc | written | own sympathetic-string resonator bank (sst StringResonator as a starting block) |
| PITCH CHANGE | 0D 40 | Var,InsA/B,A/D |  | 170 | 85 | Misc | borrowed | Signalsmith Stretch (MIT) pitch shifting |
| EARLY REFLECTION | 0D 50 | Var,InsA/B,A/D |  | 258 | 156 | Reverb | borrowed | Dragonfly / Freeverb3 `earlyref` (GPL-2.0+) |
| VOCODER | 0D 60 | Var,InsA/B,A/D | yes | 13 | 0 | Misc | borrowed | Surge XT Vocoder (GPL-3.0; side-chain as modulator) |
| NOISE GATE+COMP+EQ | 0D 70 | Var,InsA/B,A/D |  | 4 | 0 | Misc | base | own gate + sst Compressor + sst EqNBandParametric |

Reading the table: the plugin and source class are this plan's proposal; "how" names the first
block to try. A type marked base or borrowed moves to written if measurements show the block
cannot reach the reference within its gate; each such change is recorded in section 15.

## 7. Oscairn integration

All new code is in `src/oscairn-clap/` (mirroring the Surge fork's layout), with the Oscairn
extension headers vendored unchanged in `libs/oscairn-clap/`. The upstream library and its
tests are not changed.

| Topic | Design |
| --- | --- |
| Module | One CLAP module, `oscairn-effects.clap`, with one plugin descriptor per category (section 6). IDs `org.oscairn.effects.<category>`, names "Oscairn FX <Category>" (decision 2). Features: audio effect, stereo, and the category's CLAP feature (reverb, delay, chorus, …) |
| Effect contexts (RFC 0007) | Each category plugin implements `oscairn.effect-contexts/draft`: one engine (shared immutable data, sample rate, context count) and up to 64 contexts, each a full CLAP interface with its own parameters, state, history and tail. A context allocates its effect only on `activate`, so idle contexts cost a few hundred bytes. `resources` reports engine and context bytes, capacity, prepared count and tail activity |
| Modes | A stepped "Mode" parameter picks the algorithm. The audio thread never allocates: a mode change asks the host for a main-thread callback, which builds the new effect with the current parameter values and publishes it; the audio thread swaps it in at the next block and hands the old one back to the main thread to free. Until then the old mode keeps playing |
| Parameters | The union of all modes' parameters, each under a module path named after its mode (one editor page per mode), plus Mode. IDs are stable: `mode << 8 \| index`, Mode is 1; modes and indices are never renumbered. Ranges, defaults and value text come from each effect's `ParamMetaData`; values are in physical units (Hz, ms, dB, %), not the platform's raw numbers (decision 10). Parameter metadata never changes with state |
| State | Text, locale-independent: a header line with the category, then `id=value` lines with 17 significant digits. Unknown IDs are ignored (newer state in an older plugin), missing IDs keep defaults. History is not state |
| Block size and latency | sst-effects processes fixed blocks; the wrapper uses 16 samples and buffers one block, reported through the latency extension (16 samples, 0.33 ms at 48 kHz). Output does not depend on the host's block size |
| Tails | `tail_active` and the CLAP tail extension: active while input is non-silent or output has been above −140 dBFS within the last 100 ms; once inactive with silent input, `process` returns `CLAP_PROCESS_SLEEP` |
| Side-chain | Plugins with side-chain types declare a second, non-main stereo input port, "Side-chain", on every context. A mode without side-chain ignores it; a side-chain mode with nothing connected uses the main input, as the reference does when its source is Off |
| Tempo | From the CLAP transport; 120 BPM when absent |
| Sample rates | Oscairn runs mostly at 48 kHz; any rate from 8 kHz to 768 kHz activates. Fitted behaviour is written in seconds and hertz, never in samples at 44.1 kHz, and every fitted-behaviour gate runs at 44.1, 48 and 96 kHz |
| Real-time safety | No allocation, lock or file access in `process`; parameter values cross threads as atomics. Tests count allocations during `process` and run under ASan/UBSan and TSan |
| Platforms | macOS (arm64) for the desktop, Linux ARM64 for the device, Linux x86_64 in CI |

Tests (`src/oscairn-clap/tests/`): metadata validity against the host's load checks
(`oscairn/docs/plugin-porting.md`); bit-exact equality between a stock mode through the CLAP
wrapper and the same sst-effects template driven directly; output independent of host block
size; state round trips; two contexts against two separate instances (isolation, independent
parameters, shared send), capacity exhaustion and parent removal; the side-chain port on a
second context; tails and sleep; no allocation in `process`; activation and fitted behaviour at
44.1, 48 and 96 kHz. Each test is checked to fail with its change undone.

## 8. Stock behaviour, the reference-matching setting and the stock-vs-fork gate

- **Stock modes.** A mode that is an unmodified sst-effects effect (for example "Reverb 2") is
  exposed with the effect's own parameters and renders exactly as the template does.
- **Reference modes.** A mode built to match a platform type is a new mode, even when it is
  built on a stock effect; its parameter mapping and any added stages live in new files.
- **Reference matching on a stock effect.** If a measurement needs a change inside a stock
  effect's DSP, the change goes in a new header that wraps or derives from it, enabled by a
  per-preset parameter ("Reference matching", default off) saved in the context's state. It is
  never a build option or a global switch.

The **stock-vs-fork gate** (`src/oscairn-clap/scripts/stock-gate.sh`) builds one render program
twice, once against this branch's `include/` and once against the upstream commit the branch is
based on (`git merge-base HEAD origin/main`), renders every stock bus effect (defaults and a
fixed set of seeded random parameter vectors, a fixed input, 48 kHz) and requires the two sets of
output to be bit-identical. It runs locally before each push and in CI. It covers the bus
effects; voice effects join it when a mode first uses one.

## 9. Upstreamability

| Change | Upstream? |
| --- | --- |
| `src/oscairn-clap/`, `libs/oscairn-clap/`, `.github/workflows/oscairn.yml`, `doc/oscairn-plan.md` | No; Oscairn-specific. Kept in new files so upstream merges never conflict there |
| Two hooks in `CMakeLists.txt`: the `SST_EFFECTS_BUILD_OSCAIRN` option (which turns on the test dependencies and pins them through `cmake/oscairn-deps.cmake`) and `add_subdirectory(src/oscairn-clap)` | No; a few lines, the only edits to an upstream file |
| New generic effects written here (an envelope follower, an upward compressor, Linkwitz-Riley multiband split, a beat-repeat buffer, a formant filter, a centre-tap delay) | Yes, as new `include/sst/effects/` or `voice-effects/` headers with neutral names, once they have tests. Offered as separate pull requests, one effect each |
| Fixes to stock effects found while measuring | Yes, as separate pull requests; never folded into reference-matching changes |
| Reference-matching modes and fitted constants | No |

Before each report the branch is test-merged with upstream's latest `main` and conflicts are
recorded in section 15.

## 10. Licences, IP and naming

Licences: section 3. IP, as for the other forks (Oscairn RFC 0013):

1. Own implementation: no code, tables or constants copied from the reference product. Fitted
   behaviour is derived from measurements of renders and written as formulas in time and
   frequency; if a verbatim table ever seemed the only way, it would stop here as a decision for
   Jacob.
2. The platform's public Data List is used for what it documents (type numbers, parameter names,
   printed ranges and display tables) in the converter's mapping; that mapping lives with the
   converter or as generated data, not as copied tables in DSP code.
3. Renders, performances and any reference files are never committed, here or in the
   measurement repository. Test fixtures are synthetic.
4. No product names or trademarks of the reference in identifiers, parameter names, mode names,
   UI or committed code. Modes get generic names ("Hall A", "Studio Rotary", "Five-band EQ");
   the mapping from platform type to mode lives in the converter.

## 11. Measurement method

- **Probes.** For each type, a probe performance: one part with a simple dry source (a sine or
  saw element, or a short percussive wave as an impulse-like source), the effect under test in
  the slot under test with chosen parameters, other effects off, and the same performance with
  the effect off as the dry reference. Built with a copy of the research repository's
  `build_performance.py` (it already sets every effect slot's type and parameters), kept outside
  the repositories.
- **Renders.** With the shared recorder only, one batch at a time under its lock, about 15 s of
  warm-up, dropouts detected and re-recorded, renders under the shared measurements folder, never
  committed. The reference renders at 44.1 kHz.
- **Analysis.** In the measurement repository's new `fx/` folder (MIT, analysis code and
  tolerances only): transfer function from wet/dry (magnitude and phase), static curves and
  time constants for dynamics, energy decay and RT60 per octave band for reverbs, delay times
  and feedback decay, modulation rate and depth from the instantaneous delay or gain, harmonic
  distortion versus level. Self-tests on synthetic signals.
- **Fitting.** Parameter maps (raw value to physical unit) are taken from the public Data List
  tables where they exist and measured where they do not; behaviour is fitted in seconds and
  hertz and checked at 44.1, 48 and 96 kHz.
- Tolerances are labelled **proposed** until Jacob accepts them; a table checked only against its
  own fitting renders is labelled as such.

## 12. Milestones

Following RFC 0016's build order (step 2, the most used types, by the counts in section 5; step
3, the rest; step 4, converter coverage). Every pushed commit passes: build, upstream tests, our
tests, ASan/UBSan, TSan, the stock-vs-fork gate, and CI on Linux x86_64, Linux ARM64 and macOS.

| Milestone | Deliverable | Gate (tolerances proposed) |
| --- | --- | --- |
| M0 | Fork builds on macOS and Linux ARM64; CLAP module with one plugin (Reverb, stock modes Reverb 1 and Reverb 2) as effect contexts; tests of section 7; CI; stock-vs-fork gate | All tests and gates green locally and in CI; stock modes bit-exact with the templates |
| M1 | Measurement tooling in `fx/`; EQ plugin: VCM EQ 501, Part 3-band, Part 2-band, Master 5-band | Magnitude response within ±0.5 dB, 20 Hz–16 kHz, over a grid of frequency, gain and Q per band; at 44.1, 48 and 96 kHz |
| M2 | Dynamics: VCM Compressor 376, Classic Compressor, Multi Band Comp; side-chain port and its context test | Static curve within ±1 dB; attack and release time constants within ±15% |
| M3 | Reverb: REV-X Hall and Room, R3 and SPX families, HD; Early Reflection | RT60 per octave band within ±10%, pre-delay within ±1 ms, early-reflection energy within ±1.5 dB, decay spectrum; listening |
| M4 | Delay: Tempo Cross, Cross, Tempo Stereo, Tempo Mono, L/R. Chorus: 2 Modulator, G Chorus, Ensemble Detune, SPX Chorus, Symphonic | Delay times within ±0.1 ms; feedback decay within ±0.5 dB per repeat; LFO rate within ±2%, depth within ±10% |
| M5 | Rotary Speaker 1 and 2, Auto Pan, Tremolo, Amp Simulator 1 and 2, VCM Rotary Studio | Rotor speeds and acceleration within ±5%; harmonic profile versus level within ±2 dB |
| M6 | Remaining types: flangers, phasers, wahs, lo-fi, amps, Tech, Misc, Analog delays, remaining reverbs and rotary modes | Per-type measurements as above |
| M7 | Converter mapping for every type and parameter, as generated data for `oscairn/performance-converter` | The factory set converts with no effect-parameter loss (every raw value maps to a parameter and back) |

## 13. Decisions for Jacob to confirm

1. **Thirteen plugins, not twelve.** Misc is split: VCM EQ 501 joins the Part and Master EQs in
   an EQ plugin, and Early Reflection joins Reverb.
2. **Plugin names and IDs.** "Oscairn FX Reverb", `org.oscairn.effects.reverb`, and so on, to
   avoid clashing with Oscairn's own "Oscairn Delay" (`org.oscairn.delay`).
3. **Parameter layout.** The union of every mode's parameters, a page per mode, stable IDs,
   rather than generic "Parameter 1–24" whose meaning changes with the mode (which a generated
   editor could not label).
4. **Latency.** A fixed 16-sample internal block, reported as 16 samples of latency.
5. **Pinned dependencies** in the Oscairn build only (`cmake/oscairn-deps.cmake`); upstream's own
   test build still tracks `main`.
6. **Types never active** in either set (VCM Rotary Classic and Overdrive) come last.
7. **Amp types from Guitarix tone stacks.** Guitarix is mostly Faust-generated code; borrowing
   means per-file licence checks. The alternative is writing the tone stacks from the published
   circuit analyses.
8. **HD reverbs on Reverb 2.** If measurement shows Reverb 2 cannot reach their character, they
   move to written.
9. **Pitch shifting.** Signalsmith Stretch for Pitch Change; an own short-window shifter for
   Ensemble Detune and Shimmer, where Stretch's latency would be too long.
10. **Physical units.** Plugin parameters are in Hz, ms and dB; the converter turns raw values
    into units using the Data List's tables. The alternative (raw values in the plugin) would
    make the plugin's editor show platform numbers.
11. **Envelope followers in the plugins** (section 4) rather than a host node.
12. **Tempo fallback** of 120 BPM when the host gives no tempo.

## 14. Risks and not verified

- Whether every sst-effects bus effect is deterministic given a deterministic RNG (unverified
  until the M0 tests run it).
- Reverb 2's per-instance memory with its maximum delay sizes; a 64-context engine could need a
  lot of memory if many contexts are prepared (measured in M0).
- Whether the Surge XT effects listed as borrowed port cleanly onto the `FXConfig` contract
  (unverified; each is a port of a `SurgeStorage` class).
- The usage counts' "active" rule does not check whether a part's elements are routed to the
  insertion or whether its volume is audible.
- The VCM types' circuit behaviour is to be found by measurement; no circuit is assumed.

## 15. Implementation log

(Filled in as milestones land.)
