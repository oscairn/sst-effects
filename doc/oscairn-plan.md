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
[Oscairn](https://github.com/oscairn/oscairn) repository). Statements about sst-effects were
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
| Oscairn extension headers (`include/oscairn-clap/` in oscairn/oscairn-clap) | MIT (SPDX headers) | Yes | Vendored unchanged in `libs/oscairn-clap/` |

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
| Delay | 9 | 1 | 2 | 6 | |
| Chorus | 6 | 4 | 1 | 1 | |
| Flanger | 5 | 4 | 0 | 1 | yes |
| Phaser | 5 | 3 | 0 | 2 | yes |
| Tremolo & Rotary | 7 | 1 | 3 | 3 | |
| Distortion & Amp | 12 | 1 | 11 | 0 | |
| Dynamics | 7 | 5 | 0 | 2 | yes |
| Wah | 3 | 0 | 0 | 3 | |
| Lo-Fi | 4 | 3 | 0 | 1 | |
| Tech | 18 | 4 | 1 | 13 | yes |
| EQ | VCM EQ 501 + Part 3-band, Part 2-band, Master 5-band | 0 (+3 EQs on sst-filters biquads) | 0 | 1 | |
| Misc | 11 | 3 | 3 | 5 | yes |
| **All 104 types** | | **33** | **23** | **48** | |

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
| CROSS DELAY | 02 00 | Var,InsA/B,A/D |  | 358 | 149 | Delay | written | own cross delay, the tempo mode's core with ms times (built; section 15) |
| TEMPO CROSS DELAY | 02 10 | Var,InsA/B,A/D |  | 1426 | 787 | Delay | written | own cross delay fitted to renders (built; section 15) |
| TEMPO DELAY MONO | 02 20 | Var,InsA/B,A/D |  | 84 | 61 | Delay | written | own one-line two-tap delay, loop = note time + lag, taps ± diffusion, fitted to renders (built; section 15) |
| TEMPO DELAY STEREO | 02 28 | Var,InsA/B,A/D |  | 361 | 165 | Delay | written | own cross delay with one note time, lag on the loop and ± diffusion on the outputs, fitted to renders (built; section 15) |
| CONTROL DELAY | 02 30 | Var,InsA/B,A/D |  | 39 | 1 | Delay | base | sst Delay with a smoothed, controllable time + mapping |
| DELAY LR | 02 40 | Var,InsA/B,A/D,Mas |  | 233 | 108 | Delay | written | own one-line-per-channel delay, four ms times, fitted to renders (built; section 15) |
| DELAY LCR | 02 50 | Var,InsA/B,A/D |  | 76 | 11 | Delay | written | own one-line four-tap delay, three ms times + shared loop, fitted to renders (built; section 15) |
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
| AUTO PAN | 06 00 | Var,InsA/B,A/D |  | 311 | 132 | Tremolo & Rotary | written | own model fitted to renders, mode "Auto Pan" (built; section 15) |
| TREMOLO | 06 10 | Var,InsA/B,A/D |  | 205 | 69 | Tremolo & Rotary | written | own model fitted to renders, mode "Tremolo" (built; section 15) |
| ROTARY SPEAKER 1 | 06 20 | Var,InsA/B,A/D |  | 223 | 440 | Tremolo & Rotary | written | own two-rotor model fitted to renders, mode "Rotary A" (built; section 15, decision 28) |
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
| Parameters | The union of all modes' parameters, each under a module path named after its mode (one editor page per mode), plus Mode. IDs are stable: `mode << 8 \| index`, Mode is 1; modes and indices are never renumbered. Ranges, defaults and value text come from each effect's `ParamMetaData`; values are in physical units (Hz, ms, dB, %), not the platform's raw numbers (decision 10). IDs, names, ranges and modules never change with state; the parameters of modes other than the selected one are flagged hidden, and the host is asked to rescan parameter info when the mode changes (plugin-porting.md; section 17) |
| State | Text, locale-independent: a header line with the category, then `id=value` lines with 17 significant digits. Unknown IDs are ignored (newer state in an older plugin), missing IDs keep defaults. History is not state |
| Block size and latency | sst-effects processes fixed blocks; the wrapper uses 16 samples and buffers one block, reported through the latency extension (16 samples, 0.33 ms at 48 kHz). Output does not depend on the host's block size |
| Tails | `tail_active` and the CLAP tail extension: active while input is non-silent or output has been above −140 dBFS within the last 100 ms, or within a mode's longest silent stretch (the delay: its longer line plus 100 ms); once inactive with silent input, `process` returns `CLAP_PROCESS_SLEEP` |
| Side-chain | Plugins with side-chain types declare a second, non-main stereo input port, "Side-chain", on every context. A mode without side-chain ignores it; a side-chain mode with nothing connected uses the main input, as the reference does when its source is Off |
| Tempo | From the CLAP transport at every `process` call; 120 BPM when absent (built with the tempo cross delay) |
| Work off the audio thread | Each engine owns a worker thread, started with the first context that needs it (decision 19): the EQs' rate matching runs there and comes back through lock-free triple buffers; the audio thread only posts and wakes. Offline (`clap.render`), the same work runs in process so renders do not depend on timing |
| Editor hints | `oscairn.ui/draft-5`: Mode first by priority, then the selected mode's parameters in their order; a parameter named Mix carries the `mix` role; the draft 4 view fields and the draft 5 `algorithm` and `seconds` callbacks are null (effects have no envelope or map views) |
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
13. **Decided (Jacob, 2026-11-02): higher sample rates sound as the reference does at 44.1 kHz.**
    Fitted effects must match their 44.1 kHz response at 48, 88.2, 96 and 192 kHz within the
    tolerance, gated, with any residual in the known-differences register (section 16). Status:
    met for the four fitted EQ modes (section 15, "Rate matching"): from 20 Hz to 20 kHz within
    0.05 dB where the response is within 20 dB of its peak and 0.5 dB below (tolerance
    proposed), at all four rates, on 39 settings in the plugin tests and 2,400 random settings
    in a stress run. Each 44.1 kHz section is replaced at other rates by up to three biquads
    fitted to its magnitude, so the plugins hold up to three times as many sections there, and
    the coefficients cost 0.13–0.54 ms per change on average (worst seen 2.8 ms), now computed off
    the audio thread (decision 19). Rates below 44.1 kHz are not covered (decision 21). The
    tolerance is accepted (decision 18).
14. **Converter value laws.** The Data List prints rounded labels for the EQ frequency and Q
    tables (125 Hz is 125.9 Hz, 500 Hz is 501.2 Hz). The converter should use the laws measured
    in section 15 (frequency index i is 10^((i + 48) / 40) Hz, Q index j is 0.5 × 2^(j / 12),
    gain is (value − 180) / 10 dB), not the printed labels.
15. **Clock-seeded and uninitialized stock effects.** FloatyDelay, and Phaser's modulator (random
    waves), seed their generators from the clock, and Phaser and RotarySpeaker read memory they
    never initialize with some parameters. Oscairn needs repeatable renders; when those effects
    become modes, the wrapper will reseed them from a fixed seed in a new file (stock DSP
    unchanged), and the uninitialized reads go upstream as bug reports. Section 15, M0.
16. **Memory declaration.** The plugins implement Oscairn's draft `oscairn.memory` extension
    (`memory.h`, vendored from Oscairn `17c43f6`, refreshed from `d7ac261`, and from oscairn/oscairn-clap `7b5c4e0`): `estimate(rate, max_frames)` after state
    load and before activation, `current` after activation, as shared, per-context and asset
    bytes, the same shared and context bytes as `oscairn.effect-contexts`. Oscairn's host does
    not read it yet, so it is tested only through this repository's test host. Until the host
    reads it, Oscairn measures memory from outside, keyed by plugin build, so two modes of one
    plugin share a figure: the request stands to key memory by the context's state or to read
    the estimate (section 15, effect memory).
17. **Measured frequency scales, table or formula (tried both).** For the three-band Part EQ
    and the Master EQ, a table measured from renders and the nearest closed form were built
    behind `OSCAIRN_FREQUENCY_SCALE_TABLE` and compared (section 15, "Decision 17"). The tables
    match at least as well everywhere, cost about 0.8 KB, and are cheaper per conversion, so they
    ship. Neither version meets the 0.05 dB tolerance yet: the remaining error is in laws other
    than the scale (known differences 2 and 5). Confirm the tables (the Master EQ's is the Data
    List's printed scale, which the measurements confirm).
18. **Decided (Jacob, 2026-11-02): the rate gate.** 0.05 dB where the response is within 20 dB of
    its peak and 0.5 dB further down, from 20 Hz to 20 kHz, and 10 dB above 20 kHz as a sanity
    bound.
19. **Decided (Jacob, 2026-11-02): rate-matched coefficients are computed off the audio thread**,
    swapped in atomically at a block boundary, the previous filter playing until then; no
    allocation, lock or waiting in process; correct under fast automation. Built (section 15, "Rate
    matching off the audio thread").
20. **Decided (Jacob, 2026-11-02): the matcher's scratch is allocated only where matching is
    needed** (rates other than 44.1 kHz), at activation. Built (same section).
21. **Decided (Jacob, 2026-11-02): rates below 44.1 kHz are not covered** by the rate matching; the
    fitted EQs there run the matcher's fit up to 0.45 of the rate but nothing is gated (known
    difference 18).
22. **Decided (Jacob, 2026-11-02): the plugins follow every Oscairn recommendation and feature that
    applies to effects** (`oscairn/docs/plugin-porting.md`). The checklist, with status and the test
    that proves each item, is section 17.
23. **Open: the delay's equalizer parameters.** The tempo cross delay's EQ parameters (low and high
    frequency and gain) had no effect on any render (fx16), so the mode leaves them out. If the
    converter must round-trip every raw value (M7), they can be added as parameters that do nothing.
    Since 2026-11-12 this is in doubt: on 02 00, 02 20, 02 28, 02 40 and 02 50 the EQ shelves the
    whole output, dry and wet alike, which a comparison inside one render cannot see (known
    difference 44); whether to model it is open.
24. **Open: plugin version and signing.** The plugins reported version 0.1.0 whatever changed;
    Oscairn keys its measurements by ID and version and keeps a store build per version, so each
    published build should get a new version, and be signed (`oscairn-sign`) with a key of Jacob's.
    Since 2026-11-05 a plugin's version is bumped when its sound or memory changes (Reverb 0.2.0
    with Hall A, 0.4.0 with its rebuild, 0.5.0 with its fixed filters); signing is still open.
25. **Decided (Jacob, 2026-11-05): fix the circuit-style compressor before shipping it. Done
    (2026-11-06):** rebuilt on the reference's structure (section 15, "Circuit compressor
    rebuilt on the reference's structure"). A fresh hold-out (fx26, 20 random settings, four
    sources, never used for fitting) is within 0.154 dB RMS per render, every 5 ms window within
    0.29 dB, so the ±1 dB gate is met everywhere it was missed (ratio 2, saw, near full scale, low
    inputs). The side-chain port is still not built (known difference 29).
26. **Decided (Jacob, 2026-11-05): rebuild Hall A on the reference's structure. Built
    (2026-11-06), gate not met:** written independently on the structure (section 15, "Hall A
    rebuilt on the reference's structure"): room size changes only delays, reverb time only gains,
    nothing is modulated. Fitted on 155 renders (fx19, fx24, fx27) and checked on a fresh hold-out
    (fx28, 16 random settings, generated and recorded after the model was frozen, the plugin's own
    output): decay within 10% in 38 / 69 / 62 / 56 / 69 / 56 / 38% of renders per band (125 Hz to
    8 kHz; the old model's fx24 hold-out: 25-58% from 250 Hz to 8 kHz), energy within 1.5 dB in 7
    of 16 (mean +0.92 dB, sd 2.88; too loud by 3.5-6.6 dB at high ratio 0.1-0.2), onset within 1
    ms in 5 of 16. So the M3 decay and energy gates are still not met (known differences 23 and
    30). The 42-53 ms burst is explained: it is the dry click, not the reverb (known difference
    24, resolved). **Refitted (2026-11-06, Reverb 0.5.0), gates still not met:** the high ratio's
    low-pass and the low ratio's shelf now act once on the whole input, the tank's own filters on
    what recirculates only (section 15, "Hall A's fixed filters"). Fresh hold-out fx30 (16 random
    settings, seed 3030, generated and recorded after the freeze): energy within 1.5 dB in 12 of 16
    (fx28: 7 of 16), decay within 10% in 31 / 44 / 62 / 69 / 69 / 38 / 44% of renders per band,
    onset within 1 ms in 9 of 16. The energy miss at high ratio is gone; the decay gate is as far
    off as before, worst in rooms of size 0-1 and at decay settings below 30.
27. **Decided (Jacob, 2026-11-05): Hall A's coefficients are designed off the audio thread.
    Revised (2026-11-06) with the rebuild, as the task allowed after measuring:** the rebuilt Hall
    A's reverb time sets only gains, so a change costs a few first-order designs and is made in
    process; every gain and coefficient then ramps over 2 ms. Measured: worst process call 3.7-5.6
    µs at 192 kHz with one change per 64-sample block (the timing test's gate is a quarter block,
    83 µs, unchanged); the click test (unchanged gate, -60 dBFS) gives -67.9 to -81.3 dBFS, and
    -32.4 to -62.0 dBFS with the ramps removed. Hall A no longer uses the engine's worker; the EQs
    still do. Before the rebuild (kept for the record): the loop filters' design ran on the
    engine's worker with the EQs' exchange and a 2 ms crossfade; the click gate of -60 dBFS was
    set after seeing -62.7 to -77.1 dBFS and accepted by Jacob (2026-11-06), who also accepted
    leaving CI's macOS timing margin as it was (worst call 60.8 µs against the 83 µs gate in run
    42172719359).
28. **Decided (Jacob, 2026-11-11): the two-rotor rotary type (ROTARY SPEAKER 1) is an own model**,
    a dedicated mode in this repository ("Rotary A" of a new plugin, Oscairn FX Tremolo & Rotary),
    not sst RotarySpeaker and not the setBfree rotary model (`b_whirl`). Built (section 15, "Rotary A"). Open points
    from building it, for Jacob:
    - **The effect's main control.** The mode has no Mix and no Output Level (the reference has
      neither), so no parameter carries the `mix` role and Oscairn puts the first parameter by
      priority (Mode) on the Effects knob. The editor-hints test now expects no role for it. A
      `mix` role on Speed, or Balance, would be a choice.
    - ~~**CI's privacy step lists the plugin IDs** (`.github/workflows/oscairn.yml`) and does not name
      `org.oscairn.effects.tremolo-rotary`; the workflow was left unchanged in this round. The
      check passes for it locally~~: resolved 2026-11-12, the step takes the ids from the plugin's
      own factory (`oscairn-effects-privacy-host --list`) and fails if that lists none.
    - ~~The shelves miss the rate gate and their law is not found~~: resolved 2026-11-11 (section 15,
      "Rotary A: the shelves, and clipping in the probes").
    - **The reference clips the effect's output at about +8 dB** (known difference 41). It only shows
      when the part's volume or what follows the effect lowers the level by more than that; at volume
      100 the instrument's output clips first. Adding a hard clip at that level to the mode would be
      reference behaviour; not added.

## 14. Risks and not verified

- Determinism of the stock effects: settled in M0 (decision 15).
- Memory: settled in section 15 (effect memory). Modes written later size their buffers the
  same way: for the running rate and the longest reach of their parameters.
- Tempo sync: buffers are sized for unsynced parameters (the configuration never syncs). When
  tempo-synced modes arrive, their sizes must include the longest synced time at the slowest
  tempo the host can give.
- Whether the Surge XT effects listed as borrowed port cleanly onto the `FXConfig` contract
  (unverified; each is a port of a `SurgeStorage` class).
- The usage counts' "active" rule does not check whether a part's elements are routed to the
  insertion or whether its volume is audible.
- The VCM types' circuit behaviour is to be found by measurement; no circuit is assumed.

## 15. Implementation log

### Milestone 0: groundwork (done 2026-11-01)

Built:

- `src/oscairn-clap/`: the CLAP module `oscairn-effects.clap` (`OscairnEffects.cpp`, entry in
  `OscairnEffectsEntry.cpp`), the effects' configuration (`OscairnConfig.h`: block size 16, 32
  parameter slots, a per-effect xorshift generator with a fixed seed instead of `rand()`), and the
  first category plugin, Oscairn FX Reverb, with the stock Reverb 1 and Reverb 2 as modes. Mode
  changes are built on the main thread and swapped in at a block boundary; up to 64 effect
  contexts share an engine; state is text; latency 16 samples; denormals are flushed while
  processing; `CLAP_PROCESS_SLEEP` once the tail has ended.
- `libs/oscairn-clap/`: `effect-contexts.h` vendored unchanged from Oscairn `4e84501`.
- `cmake/oscairn-deps.cmake` and two hooks in `CMakeLists.txt` (the only upstream file touched):
  the option, pinned dependencies (sst-basic-blocks `38cf04a`, sst-filters `87a171b`,
  sst-waveshapers `dd12f31`, sst-plugininfra `8e4271c`, eurorack `888298b`, simde 0.7.2, CLAP
  1.2.7) built position independent, and `add_subdirectory`.
- Tests (`tests/OscairnEffectsTests.cpp`), each checked to fail with its change undone (a
  mutation per test; the threaded test's mutation, a non-atomic mode field, is caught only under
  TSan): the host's load checks; stock modes bit-exact with the templates; block-size
  independence (1, 37, 512 and mixed blocks, with parameter events); state round trip and bad
  state; contexts independent of each other and equal to separate instances, capacity 64,
  parent removal; mode changes built on the main thread; no allocation in `process`; tail and
  sleep; Reverb 2's T30 at 44.1, 48 and 96 kHz (1.8545, 1.8544, 1.8546 s; proposed tolerance
  5%); main-thread work beside a processing thread. The denormal flush has no test.
- The stock-vs-fork gate (`scripts/stock-gate.sh`, `tests/StockRender.cpp`): 10 stock bus effects
  × 9 parameter sets, rendered with this branch's headers and with the base commit's, must be
  bit-identical. Checked to fail when a stock header is changed audibly.
- CI (`.github/workflows/oscairn.yml`): Linux x86_64, Linux ARM64 and macOS arm64 run upstream's
  tests, ours and the gate; Linux ARM64 runs both suites under ASan/UBSan and TSan.

Departures and findings:

- **Stock effects are not repeatable as shipped** (decision 15). The gate first failed against
  itself: FloatyDelay and Phaser's modulator seed from the clock, and with some parameters Phaser
  and RotarySpeaker read uninitialized memory (seen by filling allocations with two different
  bytes). The render program reseeds those generators and fills every allocation with one byte.
- The tail extension reports "infinite" while the tail is active and 0 after; per-mode lengths
  come when modes report them (`shortcut:` comment in the code).
- The first CI runs failed on the gate (a relative build path; SSE4.1 intrinsics on x86_64,
  fixed with `-march=native` for the render programs as upstream's test target does), on an
  allocation-form mismatch under Linux ASan (the tests now define every form), and on a timing
  assumption in the threaded test. Fixed in `fecc822`, `20cd378` and `8664880`; CI run
  41360666098 on `8664880` passed all five jobs, with the gate passing in the three that run it.
- Local checks before each push: macOS arm64 build, upstream tests, ours, ASan/UBSan, TSan and
  the gate; Linux ARM64 (Docker, Ubuntu 24.04, GCC 13.3) build, ours and the gate, and
  ASan/UBSan for the allocation fix. Linux x86_64 was checked only in CI.
- Upstream test merge: `upstream/main` is still `987b39d`, the base, so there was nothing to
  merge and no conflict.

### Effect memory (done 2026-11-01)

Why a prepared Reverb 2 context took 14.5 MB: upstream sizes its arrays for 384 kHz at every
rate: 12 allpasses (4 input, 8 in the tank) of 131,072 floats (6.3 MB), 4 delay lines of
131,072 floats (2.1 MB) and a predelay of 1,536,000 floats (6.1 MB). No oversampling. At a given
rate it reaches much less: room size scales lengths by at most 2, so the longest allpass is
121 ms and the longest delay 357.6 ms plus 5 ms of modulation, and predelay is at most 2 s.

What changed: a hook in `include/sst/effects/Reverb2.h` (four constants; a configuration
without the new members gets upstream's sizes) and `SizedConfig<Rate>` in `OscairnConfig.h`.
Each mode is instantiated for 44.1, 48, 88.2, 96, 176.4 and 192 kHz; a context builds the
smallest that covers the running rate (above 192 kHz, upstream's sizes), on the main thread at
activation, state load and mode change, never in `process`. Reverb 2 renders bit-identically
with any buffers at least that long (the allpasses wrap at their length, the delays are masked
and the predelay wraps at its size): a test renders defaults and both extremes (room size ±1,
predelay 2 s and 4 ms, modulation 0 and full) at 44.1, 48, 50, 96 and 192 kHz against upstream's
sizes, and fails if the delay or predelay buffer is halved. The stock gate still passes (the
hook is inactive for its configuration).

Reverb 1 keeps upstream's sizes on purpose: its 32,768-sample lines wrap with a fixed mask, and
its predelay reaches 8 s (its range is 2^0 to 2^3 s), beyond the mask, so a smaller buffer would
change what it plays. Recorded rather than changed.

Per context, as the plugins report it, in RFC 0025's draft breakdown (bytes):

| Mode | Rate | Shared (engine), before → after | Context, before | Context, after | Assets | Resident growth, before → after |
| --- | --- | --- | --- | --- | --- | --- |
| Reverb 1 | 44.1 kHz | 16 → 16 | 2,230,456 | 2,230,456 | 0 | 2.27 MB → 2.24 MB |
| Reverb 1 | 48 kHz | 16 → 16 | 2,230,456 | 2,230,456 | 0 | 2.25 MB → 2.24 MB |
| Reverb 1 | 96 kHz | 16 → 16 | 2,230,456 | 2,230,456 | 0 | 2.30 MB → 2.25 MB |
| Reverb 2 | 44.1 kHz | 16 → 16 | 14,534,184 | 872,856 | 0 | 12.74 MB → 0.77 MB |
| Reverb 2 | 48 kHz | 16 → 16 | 14,534,184 | 1,188,856 | 0 | 12.76 MB → 1.19 MB |
| Reverb 2 | 96 kHz | 16 → 16 | 14,534,184 | 2,375,928 | 0 | 12.75 MB → 2.40 MB |
| Five-band EQ | any | 16 → 16 | 1,156 | 1,156 (58,016 since rate matching) | 0 | below a page |
| Five-band EQ, after decision 20 | 44.1 kHz / 48 and 96 kHz | 16 → 120 | 58,016 | 1,368 / 62,592 | 0 | not measured |
| Two-band EQ, after decision 20 | 44.1 kHz / 48 and 96 kHz | 16 → 120 | 57,088 | 1,088 / 59,032 | 0 | not measured |
| Three-band EQ, after decision 20 | 44.1 kHz / 48 and 96 kHz | 16 → 120 | 57,368 | 1,152 / 60,192 | 0 | not measured |
| Master EQ, after decision 20 | 44.1 kHz / 48 and 96 kHz | 16 → 120 | 57,992 | 1,344 / 62,568 | 0 | not measured |
| Tempo Cross Delay | 44.1 / 48 / 96 kHz | 120 | (new) | 1,052,476 / 1,145,212 / 2,286,516 (528,180 / 574,548 / 1,145,196 before its long notes halved, 2026-11-12) | 0 | not measured |
| Cross Delay | 44.1 / 48 / 96 kHz | 120 | (new) | 581,900 / 633,012 / 1,262,116 | 0 | not measured |
| Tempo Stereo Delay | 44.1 / 48 / 96 kHz | 120 | (new) | 1,052,476 / 1,145,212 / 2,286,516 | 0 | not measured |
| L/R Delay | 44.1 / 48 / 96 kHz | 120 | (new) | 581,332 / 632,444 / 1,261,548 | 0 | not measured |
| L/C/R Delay | 44.1 / 48 / 96 kHz | 120 | (new) | 291,212 / 316,768 / 631,320 | 0 | not measured |
| Tempo Mono Delay | 44.1 / 48 / 96 kHz | 120 | (new) | 1,050,764 / 1,143,496 / 2,284,804 (531,376 / 578,176 / 1,154,164 before its 262,144-sample limit, 2026-11-12) | 0 | not measured |
| Circuit Compressor | any | 120 | (new) | 792 | 0 | not measured |
| Rotary A | 44.1 kHz / 48 and 96 kHz | 120 | (new) | 34,152 / 147,864 | 0 | not measured |
| Auto Pan | 44.1 kHz / 48 and 96 kHz | 120 | (new) | 9,428 / 68,468 | 0 | not measured |
| Tremolo | 44.1 kHz / 48 and 96 kHz | 120 | (new) | 9,404 / 68,444 | 0 | not measured |

"Context" is the context's own storage plus its effect (`sizeof`, arm64 macOS build), the same
figure `resources` reports; since decision 20 it includes the effect's own heap allocations (the
EQs' matcher state, the delay's lines). The engine grew from 16 to 120 bytes with its worker; the
worker thread's stack (allocated by the system when the thread starts, only for an engine running
a matched EQ) is not counted. "Resident growth" is macOS `phys_footprint` growth per context,
averaged over eight contexts each created and activated in a fresh process after one warm-up
instance, for `b966ac3` (before) and this change (after). Old Reverb 2 grew by less than its
size, presumably because some zero pages were never made resident; the new figures track the
computed bytes.

RFC 0025 (read, not edited). Today Oscairn measures a plugin's memory from outside: a worker's
share of its process, or what the engine process grew by while the plugin loaded, every second,
keyed by plugin build (ID and version). For that to be right, a plugin's memory should have
settled once it is activated and its state loaded, and grow by a predictable amount per context.
Done here:

- an effect is allocated at activation for the mode the state selects, so loading state before
  activating allocates once, for the right mode;
- loading state while active now builds the new mode before the call returns, instead of on a
  later main-thread callback; until the audio thread swaps it in and the old one is freed, both
  are held (and reported);
- idle contexts hold only their own storage (1–2 KB); a prepared context's size is a function of
  mode and rate only;
- the plugins implement `oscairn.memory` (decision 16): `estimate` (no allocation; after state
  load, before activation; again after another state load) and `current` (after activation;
  false before) give shared, per-context and asset bytes. `resources` (effect contexts) and
  `current` report the same bytes, both counting a mode change's old and new effects until the
  old one is freed. Tests, through the repository's own test host: for every mode at 44.1,
  48, 88.2, 96, 176.4 and 192 kHz, the estimate equals what activation holds, `current` and
  `resources`; the estimate follows another state load; both extensions agree through a mode
  change. Each fails with its law broken (estimate ignoring the rate, a wrong shared figure,
  `current` ignoring the effect being swapped in). The maximum block size does not change
  what the plugins allocate.

Requests for the Oscairn session (not implemented here): measurement keyed only by plugin build
gives every mode of a category plugin one figure, the largest seen; reading the plugins'
`oscairn.memory` estimate, or keying by the context's state, would fix that. For effect
contexts the per-context cost is `context_bytes` from `resources` (or the extension), which the
budget does not read today.

### Milestone 1: measurement tooling and the five-band EQ (in progress)

Measurement code is in the measurement repository's `fx/` (MIT, synthetic self-tests only):
stereo reading, the transfer function from dry and wet renders of a harmonic source (each
harmonic's amplitude ratio, so renders need not be aligned), standard EQ section responses, and
`compare_transfer.py`, which compares our effect's transfer function with the reference's
against the same dry render. `tools/OscairnEffectsRender.cpp` runs a WAV file through a plugin.

Probes: one AN-X-engine part, a saw (no drift, filters off) at note 24 (harmonics every 32.7 Hz)
or note 12, through Insertion A, other effects off, and dry and flat-EQ references. Three batches
on the shared recorder at 44.1 kHz, no dropouts found: fx1 (66 renders: peaking bands at five
frequencies × three gains × three Qs, shelves, output level, bands 2 and 4), fx2 (30: shelves at
+3, +6, +12 and −6 dB from note 12, a level check), fx3 (51: peaking Q near Nyquist, eight
frequencies 4–20 kHz × six Qs). Repeatability, dry against dry or flat: 0.002 dB in fx1, 0.001
dB in fx3, 0.12 dB in fx2 (around 9–17 kHz; the note-12 source varies between renders, so fx2
is used below 2 kHz only).

Laws fitted to the renders (no free parameters left; reference behaviour, 44.1 kHz):

| Section | Law | Worst deviation, model against renders |
| --- | --- | --- |
| Flat setting | Transparent | 0.002 dB (fx1) |
| Output level | Gain (value − 180) / 10 dB | 11.9999 dB measured for +12 |
| Peaking bands 2–4 | Bilinear peaking section at the set frequency and gain; effective Q = Q × 10^(−\|gain\| / 40) × r, r = min(1, 1.25517 − 4.8172e-5 × f/Hz − 0.1 × log10 Q); bands 2, 3 and 4 identical | 0.007 dB over 96 renders (fx1, fx3), 20 Hz–19 kHz. r fitted on 43 points, residual 0.00015, and independent of gain |
| Shelves | Bilinear shelf, slope 0.5, the set frequency as the corner on the flat side: the prewarped frequency scaled by 10^(\|gain\|/40), up for the low shelf, down for the high shelf | 0.022 dB over 12 renders (fx1); fx2's +3, +6 and −6 dB shelves agree to 0.033 dB below 2 kHz |
| Parameter values | Frequency index i: 10^((i + 48) / 40) Hz; Q index j: 0.5 × 2^(j/12) | Follows from the fits above (decision 14) |
| Level | No level dependence seen for a 12 dB louder input, within fx2's 0.12 dB repeatability (0.175 dB worst) | Not resolved below about 0.2 dB |

The circuit-style EQ type measured as linear standard sections; no circuit behaviour was found at
these levels.

Built: `effects/FiveBandEq.h` (written; follows the sst-effects effect contract) as mode
"Five-band EQ" of a new plugin, Oscairn FX EQ (`org.oscairn.effects.eq`). Parameters are in Hz,
Q and dB in the platform's parameter order.

Gates (tolerances proposed):

- Against the reference renders, the plugin run on each probe's flat render with converted
  values: worst 0.0225 dB over fx1's 61 renders and 0.0017 dB over fx3's 48, 20 Hz–19 kHz
  (proposed tolerance 0.05 dB). fx2: 0.033 dB below 2 kHz, up to 0.63 dB above (source noise,
  see above). Checked against the renders the laws were fitted to; no separate hold-out batch
  yet.
- In the plugin tests: transparent when flat to 1e-9; the fitted design to 0.01 dB at 44.1 kHz
  (three settings, 20 Hz–20 kHz); at 48 and 96 kHz, within 0.05 dB of the 44.1 kHz design up to
  4 kHz for settings with bands up to 11.2 kHz (worst 0.0093 dB at 48 kHz, 0.0447 dB at 96 kHz;
  up to 0.355 dB from 4 to 16 kHz, printed). The setting with a wide band at 20 kHz is printed,
  not gated: 1.14 dB (48 kHz) and 1.17 dB (96 kHz) below 4 kHz (decision 13). Each of these tests
  fails with its law changed (output gain, Q law constant, shelf slope, rate matching).

#### Hold-outs, and the five-band EQ's band network (2026-11-02)

The first hold-out (fx5: 12 random full settings, recorded after the laws above were fixed)
failed: only one setting matched to 0.05 dB, and three missed by 4.8 to 11 dB, always where
narrow, strong bands overlap. Freeing any band's values did not help, so the bands do not
combine in series. They form the boost-and-cut network of a console EQ: with BP_i a unity-peak
band-pass at band i's frequency and Q (Q × r, the widening law above), the three peaking bands
give

    (1 + Σ over boosts (G_i − 1) BP_i) / (1 + Σ over cuts (1/G_i − 1) BP_i),

and the shelves stay in series. A band alone is the same peaking section as before (the
`Q × 10^(−|gain|/40)` law is exactly this form), so every single-band fit still holds. This
matched fx5 to 0.019 dB; because the structure was chosen after seeing fx5, a fresh hold-out
followed (fx8: 12 new random settings, seed 502), which the plugin matches to **0.0285 dB**
(20 Hz–19 kHz, not used in any fit). Gate against all renders: fx1 0.0225 dB, fx3 0.0017 dB,
fx5 0.0193 dB, fx8 0.0285 dB. The plugin solves the cut network's delay-free loop per sample;
a test setting with overlapping boosts fails a series implementation.

#### Two-band EQ (Part EQ), measured fx4, fx6, fx8; built

| Section | Law | Against renders |
| --- | --- | --- |
| Frequency | Index i: 10^(1.2 + i/80) Hz | High-pass and low-pass at 282, 1,189, 5,012, 17,783 Hz within 0.2 Hz of the fit |
| Low- and high-pass | First-order bilinear at the set frequency | 0.0374 dB worst (fx4) |
| Shelves | As the five-band EQ's (slope 0.5, corner on the flat side); the Q parameter has no effect | 0.0062 dB |
| Peak | Q × 10^(−\|gain\|/40), no widening at high frequencies (6.7, 10.3, 15.8 kHz fit exactly) | 0.0485 dB at and above 891 Hz |
| Gain | (value − 64) / 2 dB; output level the same | 0.0011 dB for +12 |

Open: peaks below about 300 Hz. Centres fitted from narrow bands scatter around the law (110,
134, 155, 205, 246 Hz for nominal 100, 126, 158, 200, 251 Hz) and one probe misses by 1.2 dB at a
harmonic. With harmonics every 32.7 Hz, a narrow band below 300 Hz meets only one or two of them,
so the probes cannot tell a different frequency law from a fitting artefact. Next: the same
settings at several notes, so the harmonics of the probes interleave. Hold-out fx8 (10 random
settings, both bands active): 8 within 0.027 dB, two with a peak below 300 Hz at 0.26 and 0.20
dB. Built as mode "Two-band EQ" of Oscairn FX EQ, tests as for the five-band EQ.

#### Master EQ (5-band), measured fx5, fx8; not built yet

- Peaks: Q × 10^(−\|gain\|/40), no high-frequency widening (Q 12 at 10 kHz fits 6.02); bands
  2–4 identical; 1 dB gain steps; the Q parameter has no effect on the shelves.
- Frequencies: at 1.0, 3.2 and 10.0 kHz the centres are the printed table values exactly (not
  a formula: 3,200, not 20 × 10^(44/20) = 3,170). At index 14 (printed 100 Hz) even a very wide
  band centres near 110 Hz, and index 24 (315 Hz) near 311 Hz. Open, with the two-band EQ's
  low end.
- Low shelf: the shared shelf law with the printed frequency as corner (0.001 dB).
- High shelf: Q 0.5, cookbook frequency = printed frequency with its prewarped value divided by
  √2 (0.026 dB at 2, 8 and 16 kHz); at 500 Hz it misses by 0.14/0.80 dB (+12/−12). The hold-out
  cannot separate √2 from 10^(\|gain\|/80) (equal at ±12 dB; fx8 favours neither).
- Bands combine in series, not as a network: fx8 hold-out, series worst 2.09 dB, network 3.47
  dB. The remaining error sits at 130–330 Hz (the low indices) and in the high shelf.

#### Three-band EQ (Part EQ), measured fx4, fx6; not built yet

- Low and high bands: the shared slope-0.5 shelves (0.002 dB), with corner 2 × T(i); mid band:
  a peak with Q × 10^(−\|gain\|/40) at T(i) (0.02 dB residual, gain fitted 12.02 for 12; the Q
  falls above 3 kHz, to 0.69 of nominal at 11 kHz, by a law not yet found). Gain (value − 64)
  × 0.375 dB.
- T(i) is one frequency scale for all three bands (the mid centre equals the shelf corner / 2
  at shared indices, and both match the printed ends: 50.1 Hz at 46, 139.7 Hz at 83, 503.8 Hz at
  131). It is not log-linear: steps of 17 land near 50, 80, 125, 200, 315, 500 Hz but measure
  81.8, 126.8, 325 Hz, and the high end bends. It looks tabulated; pinning it down means a sweep
  over every index (about 210), possibly with interleaved notes. Decision 17.

#### Decision 17: the frequency scales, table against formula (2026-11-02)

Sweeps: fx11a (218 renders: the three-band EQ at every index 46–255, the mid band at Q 2.6 for
83–241, the low shelf below, the high shelf above, and overlaps) and fx11b (112: every Master EQ
index of its peaks, low shelf and high shelf). Fresh hold-out fx12 (12 random settings of each
EQ). Results:

- **Three-band scale T(i)** (low shelf corner 2T, mid centre T): piecewise linear in Hz, the
  octave split into 25.5 equal steps (the step doubles every 25.5 indices), with a warp above
  about 3 kHz. Nearest closed form: F = 55.113 × 2^⌊u⌋ × (1 + frac u), u = (i − 50.5)/25.5,
  f = 44100/(π k) tan(π k F / 44100), k = 1.2059; within 1.52% of the table (RMS 0.69%).
- **Three-band high shelf**: its own mapping, the same linear-octave code (54.267 Hz) compressed
  by the bilinear transform, 44100/π atan(π F / 44100), within 0.66% of 17 measured indices. Its
  corner does not depend on the gain (it is the 12 dB shelf's corner), as for the low shelf;
  this rule was found on the fx12 hold-out, so fx12 counts as fitting data for it and a further
  hold-out is due.
- **Three-band mid Q**: falls near Nyquist as the cookbook's bandwidth-in-octaves form predicts
  (2.5% RMS in Q); Q index law 0.7 × (10.3/0.7)^(j/31) (within 1.2% of three measured values).
- **Master EQ scale**: the Data List's printed values, confirmed by measurement (low shelf 0.0%,
  high shelf ≤ 0.8%, peaks ≤ 0.4% from index 23); the nearest formula 20 × 10^(i/20) is up to
  5.16% off.

| Scale | Version | Size | Per conversion (test benchmark, same build) | Fit renders: worst / RMS of per-render worst, dB | Hold-out fx12: worst / RMS, dB |
| --- | --- | --- | --- | --- | --- |
| Three-band | Table (196 measured + 14 formula entries) | 840 B data + 312 B code | 2.7 ns | fx11a 0.63 / 0.098 | 1.96 / 0.58 (0.29 without ho01) |
| Three-band | Formula | 916 B code (+ libm `tan`, `exp2`) | 8.1 ns | fx11a 0.92 / 0.21 | 2.06 / 0.65 (0.59 without ho01) |
| Master | Table (printed, 61 entries) | 244 B data (61 floats; computed, not measured) | not benchmarked (a table lookup) | fx11b 1.58 / 0.24 | 4.53 / 1.55 |
| Master | Formula 20 × 10^(i/20) | not measured (one libm `pow`) | not benchmarked | fx11b 1.55 / 0.27 | 4.46 / 1.63 |

CPU per block is the same for both versions: the conversion runs in the converter (once per
value), and the plugins take Hz. Three-band sizes from the object file of a probe built both ways (arm64, -O2); its
conversion times from the plugin test's benchmark. ho01 is a wide mid band at 5 kHz
(known difference 5); the Master EQ's errors are at 65–200 Hz (known difference 2) in both
versions, and above 300 Hz the table is better (mho01 0.03 dB against 0.58 dB). Shipped: the
tables (decision 17).

Built: modes "Three-band EQ" and "Master EQ" of Oscairn FX EQ (`effects/PartAndMasterEq.h`),
and the converter's scales (`converter/FrequencyScales.h`). Shelves now keep their 44.1 kHz
half-gain frequency at other rates.

Not done in M1 yet: the low-frequency peak question (known differences 1 and 2), the three-band
mid near Nyquist (5), a further three-band hold-out, and listening.

#### Rate matching (decision 13, 2026-11-02)

The shipped rule (each peak's centre and mid-gain edges, each shelf's half-gain frequency kept in
Hz) held to 0.05 dB below 4 kHz for moderate settings only, and nothing was checked at 88.2 or
192 kHz. The previous attempt (a direct-form biquad fitted to each section's complex response)
was badly conditioned at 192 kHz. What replaced both (`effects/RateMatch.h`):

- The EQs always design their sections at 44.1 kHz. At 44.1 kHz they run them through the same code as before
  (the 44.1 kHz tests are unchanged and pass to 0.01 dB; bit-identity with the previous build was
  not checked). At any other rate each section's magnitude
  is matched by a cascade of one to three biquads; the five-band EQ's band network is first
  factored into three biquads (its numerator and denominator are degree-6 polynomials; roots by
  the Aberth iteration, grouped into sections by least root distance), so the network's own
  delay-free loop is not needed there.
- Each section is fitted on 48 log-spaced points from 20 Hz to 20 kHz, 12 more from 14 to 20 kHz
  and 17 around each of its resonances, to 0.01 dB where the whole EQ is within 20 dB of its peak
  and 0.1 dB below (a fifth of the gate, so that sections add up within it). Above
  20 kHz, 12 points hold the 44.1 kHz response's continuation loosely.
- Starts, in order, until one meets that tolerance: the section's roots moved to keep their
  frequencies in Hz; a Sanathanan–Koerner linear least-squares fit of |H|² as a ratio of
  polynomials in sin²(πf/rate), spectrally factored (sign-changing roots moved first); the first
  start with an extra resonant pair near 22.05 kHz; Sanathanan–Koerner of degree 4 and 6. Each is
  polished by Levenberg–Marquardt on the dB error in analog quadratics at the running rate (log
  natural frequency, damping), which always give a stable real cascade through the bilinear
  transform and are well conditioned at any rate. A first-order high-pass keeps its zero at DC
  exactly. Fits whose response above 20 kHz exceeds the section's 20–22.05 kHz level by more than
  1 dB rank below those that do not (sections whose continuations cancel in the product, as a
  near-Nyquist pole and zero split over two sections can, stay cancelled).
- The previous fit of each section is the first start for the next (parameter changes are mostly
  small steps); an unchanged section reuses its result if it still meets the tolerance.

Developed against a Python reference of the same steps (scratch, not committed), then ported.
Results:

| Rate | Before (old rule restored), same 39 settings: within 20 dB / below | After: within 20 dB of the peak / below / above 20 kHz (39 settings) | After, stress run (2,400 random settings, matcher alone): within 20 dB / below |
| --- | --- | --- | --- |
| 48 kHz | 3.50 / 4.49 dB | 0.011 / 0.077 dB / +1.3 dB | 0.021 / 0.16 dB |
| 88.2 kHz | 5.54 / 12.0 dB | 0.016 / 0.12 dB / +0.5 dB | 0.024 / 0.26 dB |
| 96 kHz | 5.71 / 12.3 dB | 0.017 / 0.24 dB / +0.4 dB | 0.035 / 0.20 dB |
| 192 kHz | 6.32 / 13.4 dB | 0.023 / 0.29 dB / +0.9 dB | 0.036 / 0.35 dB |

Per mode, before (within 20 dB of the peak, 48 / 88.2 / 96 / 192 kHz): five-band 2.37 / 5.39 /
5.57 / 6.25 dB, two-band 1.72 / 5.54 / 5.71 / 6.32, three-band 3.50 / 3.94 / 3.94 / 3.92, Master
1.36 / 3.48 / 3.55 / 3.80. The worst cases are the random settings with bands near 20 kHz; the
fixed setting with a wide band at 20 kHz was 1.47 dB off at 48 kHz.
"Before" is the plugin test run with the old rule restored (each mode fails the new gate at every
rate). The stress run's above-20 kHz excess reached 7.8 dB (known
difference 10). The tests gate 0.05 dB / 0.5 dB (proposed) and, above 20 kHz, 10 dB as a sanity
bound (proposed; set after the stress run). Each new test fails with its change undone (the old
rule restored per mode; an allocation inserted in the matcher fails the allocation test, which now
also changes EQ parameters in process at 48 kHz).

Cost, measured (arm64 macOS; plugin test in the RelWithDebInfo build, stress and sweep programs at
-O2): computing a five-band EQ's coefficients cold takes 0.35–0.40 ms on average and 1.2 ms at
worst over 40 random settings in the plugin test; the stress run saw means of 0.13–0.54 ms per
mode and rate and a worst of 2.8 ms; sweeping one five-band peak from 1 to 20 kHz in 302 steps
costs 0.20–0.24 ms per step (worst 1.4 ms). It ran in process when a parameter changed, at most
once per 16-sample block, allocating nothing; since decision 19 it runs off the audio thread.
Memory: every EQ context grows from about 1.2 KB
to about 58 KB (the matcher's fixed scratch arrays), at every rate (since decision 20 only at rates
other than 44.1 kHz).

### First measurements of the compressor, the cross delay and the hall reverb (2026-11-02)

Measurement only; nothing built for these types yet. Analysis in the measurement repository's
`fx/dynamics.py` (tone envelope, step time constant, echoes, per-octave decay times).

**Circuit-style compressor (fx7, 69 renders).** The displayed values follow formulas the
converter can use: level 60 × log10(v/200) dB, attack 0.022 + 50.378 × (v/200)^2.5 ms,
release 10.88 + 533.34 × (v/200)² ms. The behaviour is not fitted yet:

- Static curve: the probe was too hot. With a sine at −24 dB RMS and input levels −36 to 0 dB,
  the output sits at −1 to −2 dBFS RMS for every ratio; at −60 dB input it still gives −5.6 to
  −7.3 dBFS, so the type adds about 50 dB of gain and its threshold and knee lie below the
  levels probed. The output level parameter moved the output by 15.5 dB for a displayed −18 dB,
  suggesting saturation near full scale.
- Time constants: inconclusive. The pilot tone was already compressed by about 23 dB, so the
  louder tone added only about 4 dB more and the 63% times (21 to 224 ms) do not follow the
  attack and release settings.
- Next: input levels 5–60 and a quieter source for the static curve; a pilot below threshold
  and a burst well above it for the time constants.

**Tempo cross delay (fx9, 29 renders).** The recorder starts each note up to about 70 ms late,
and the probes were wet only, so absolute echo times across renders are not usable. Within one
render with dry and wet (balance D=W) the echo is 499.9 ms after the click for a quarter note at
120 BPM. Measured so far:

- feedback ±32 decays 5.95 dB per repeat (20 log10(32/63) would be 5.88 dB); ±63 decays
  0.07 dB per repeat;
- input L and R give a ping-pong (repeats every 1 s on one side, offset 0.5 s on the other);
  L&R feeds both sides equally;
- the longest delay is about 1.487 s (a 3 s setting gave 1.487 s, near 65,536 samples at
  44.1 kHz);
- the Performance's tempo (90, 150 BPM) did not change the delay: the reference follows its
  host's tempo, and the recorder's host runs at 120 BPM. Tempo behaviour needs the recorder to
  set the host tempo, or is taken from the parameter definitions.
- Next: every probe with the dry click in the render (balance D=W), so times are measured within
  each render; the high damping, lag and EQ probes again.

**Hall reverb (fx10, 28 renders).** Reverb only, a click source. Decay times per octave are
repeatable (two identical renders agree within 0.04 s in every band); levels and onsets are not
(timing jitter as above, and an 11.6 dB level difference between the two identical renders,
not explained yet).

| Setting (others factory-typical) | T60 at 125 Hz / 1 kHz / 8 kHz, s |
| --- | --- |
| Reverb time index 0 / 16 / 32 / 48 / 69 (displayed 0.3 / 1.9 / 3.5 / 5.5 / 18.0 s) | 0.69/0.68/0.67, 1.67/1.73/1.30, 2.94/2.87/1.80, 3.94/4.06/2.31, 9.00/8.36/3.40 |
| Room size 0 / 15 / 31 | 0.86/0.84/0.71, 1.61/1.75/1.17, 3.03/3.00/1.87 |
| High ratio 0.1 / 0.5 / 1.0 | 2.92/2.25/1.17, 2.95/2.79/1.58, 2.95/2.97/3.02 |
| Low ratio 0.1 / 1.4 | 2.96/2.83/1.78, 3.34/2.98/1.81 |
| Diffusion, initial delay, HPF, LPF, decay | T60 unchanged within 0.1 s |

So room size scales the decay as much as the time parameter does, and the high ratio is a
frequency-dependent decay (8 kHz T60 is 0.4× the low-band value at 0.1, 1.0× at 1.0). Next:
the dry click in each render for onsets and levels, a grid over time and room size, the
early-reflection pattern, and then the reverb design (written, per section 6).

### Batch fx16: three-band hold-out, compressor steps, delay laws, hall decay (2026-11-02)

100 renders (78 single notes, 22 sequences), one batch on the shared recorder, all written; held
tones checked for dropouts (none; the sequences have deliberate gaps). Analysis scripts are kept
outside the repositories (`fit_fx16.py`, `gate_peq.py`, `fit_dly3.py`).

- **The four delay probes without an echo (fx14) were a detector failure**, not behaviour: the
  echo detector cross-correlated each render with another render's click, and the click is not
  repeatable (known difference 12), so the template did not match. With each render's own click
  as the template every fx14 probe echoes: lag −32 → 467.98 ms, +32 → 531.97 ms, +63 →
  562.97 ms, equal 500 ms times without feedback → 499.98 ms (the lag adds 1 ms per step).
- **Tempo cross delay laws (fx16, fitted on these renders only):** feedback gain = v / 63.5 for
  displayed feedback v (v = 8, 16, 24, 40, 48, 56, 63 give 18.00, 11.98, 8.46, 4.02, 2.43,
  1.10, 0.07 dB per repeat; the law predicts 17.99, 11.97, 8.45, 4.02, 2.43, 1.10, 0.07);
  negative values decay alike. The first echo equals the dry click in every octave band (0.00
  dB) at balance D=W, so the wet path has no filter. High damping filters the feedback path only:
  per repeat, at ratio 0.1, −0.31 / −1.42 / −7.5 dB at 4 / 8 / 16 kHz relative to 125 Hz; at 0.5,
  −0.29 / −1.12 / −3.7 dB; flat at 1.0. The EQ parameters (low and high gain ±12 dB) change
  neither the first echo nor the repeats in any band (to 0.01 dB). Lag applies to every repeat
  (repeats at multiples of 436.98 and 562.97 ms). Input modes 3 and 4 (L-MODE2, R-MODE2): the
  first echo on one side at one note, then repeats every two notes on each side, interleaved.
  Eighth and dotted eighth with feedback ping-pong at 374.99 / 249.98 ms as expected.
- **Three-band EQ hold-out (fresh, fx16, seed 1616, never used for fitting; task: the
  gain-independent high-shelf corner):** the four settings with only the high shelf set match to
  0.0087–0.053 dB (rule holds; one 0.003 dB over the proposed 0.05 dB). With the low shelf and the
  mid band also set, 4 of 8 miss by 0.23–0.64 dB, at 1.2–4.7 kHz, every one with a mid Q index of
  20 or more (Q 4.1–8.6): the mid band's Q index law (known difference 9) or its centre near
  those Qs, not the shelf. Worst 0.64 dB, RMS of per-render worst 0.30 dB.
- **Compressor steps:** the velocity steps failed as steps — with amplitude velocity sensitivity
  +255 a velocity of 40 is silent, so the probe toggled the tone off and on and release could not
  be measured. Steady state, though, explains the earlier puzzle: at a fixed input the output
  level depends strongly on attack (−11.95, −10.36, −4.52, −2.71, −2.71 dB output for attack 0,
  50, 100, 150, 200 at release 0), and less on release. That is what an asymmetric envelope
  follower on the rectified signal does (fast attack and slow release ride the peaks, slow
  attack and fast release sit lower in the waveform), not an RMS detector: the detector law is
  next to fit, from these levels, with two-level steps (velocity 127 against about 90, or an
  amplitude-LFO square) for the time constants.
- **Hall reverb with oscillator phase reset:** the click is still not repeatable (energies
  −0.74, −7.64, −8.70 dB over three identical renders; shapes differ by about the peak value
  after alignment), so phase reset alone does not fix it; levels and onsets remain unusable. The
  decay times are repeatable (three identical renders within 0.1 s up to 4 kHz) and give a grid:
  mid-band (1 kHz) T60 for time index 0 / 8 / 16 / 24 / 32 / 40 / 48 / 56 / 64 / 69 at room size
  29: 0.85 / 0.90 / 1.75 / 2.42 / 2.89–2.97 / 3.53 / 4.07 / 4.82 / 6.02 / 8.39 s; room size 0 / 8
  / 15 / 22 / 31 at time 32: 0.84 / 1.25 / 1.76 / 2.37 / 3.06 s; at time 8, room 0 / 15: 0.22 /
  0.66 s; at time 56, 1.94 / 3.21 s (room and time do not scale separately); high ratio 0.1 /
  0.3 / 0.5 / 1.0 gives 4 kHz T60 2.27 / 1.90 / 2.12 / 3.13 s against about 3.0 s at 125 Hz;
  diffusion, initial delay, decay, HPF and LPF leave T60 within about 0.25 s up to 4 kHz (HPF 30 lengthens
  the low bands by 0.4 s).

Nothing was built from fx16 yet: the delay needs the dry/wet balance law (not probed) and tempo
from the host transport (the wrapper has none yet); the compressor needs its detector and time
constants; the reverb needs a repeatable source for levels and onsets — next, a source without an
oscillator under a short envelope (a steady tone or noise switched off, or a sample-based click
in the AWM2 engine).

### Rate matching off the audio thread (decisions 19 and 20, 2026-11-02)

Design (`OffThread.h`, `MatchedCascade` in `effects/FiveBandEq.h`):

- Each engine owns a worker thread, started (on the main thread) with the first effect that needs
  it, joined when the engine goes; contexts of one engine share it. An EQ registers its cascade
  with the worker when it is built, at rates other than 44.1 kHz only, and leaves when it is freed
  (on the main thread, waiting for the worker to finish that cascade if it is matching it).
- On a parameter change the audio thread designs the 44.1 kHz sections (a few microseconds), puts
  them in a triple buffer and wakes the worker: an atomic increment and `notify_one`, a futex or
  ulock wake, with no lock, allocation or waiting. The worker matches only the newest design and
  publishes the result, tagged with its generation, through a second triple buffer; the audio
  thread takes it at the next block boundary if it is newer than what plays, so a stale result
  never replaces a newer one. Until then the previous coefficients keep playing.
- The swap crossfades from the previous cascade over 2 ms. Each 44.1 kHz section keeps a slot (the
  shelves, then the network's sections), so switching a band on or off no longer moves the other
  sections' states, and the matcher's previous fits are keyed by slot. A result that arrives
  during a crossfade waits for it to end.
- Offline (the plugins now implement `clap.render`), and without a worker (the templates driven
  directly), the match runs in process, so an offline render does not depend on timing. The first
  match, when the effect is built on the main thread, is applied at once.
- The matcher's 58 KB, the buffers and the second cascade are allocated when the effect is built,
  only at rates other than 44.1 kHz (decision 20); memory table in "Effect memory" above.

Measured (arm64 macOS, RelWithDebInfo plugin tests):

| What | Off the audio thread | In process (the change undone) |
| --- | --- | --- |
| Worst `process` call while sweeping one band, 301 changes, one per 64-sample block (333 µs) at 192 kHz | 1.8–5.2 µs in every run seen (the test gates the best of three below 83 µs) | 805–872 µs |
| Coefficients after one change, against those matched in process (offline) | bit-identical at 48 and 192 kHz | |
| Response after a 201-step sweep, against in process and against a plugin built with the final setting | 0.00000 dB (gate 0.01 dB) | |
| Click above 8 kHz in the 50 ms after a band switches on or off, 48 / 96 / 192 kHz | −60.3 to −69.6 dBFS (44.1 kHz, which matches nothing: −23.1 to −71.5 dBFS) | without the crossfade: −11.5 to −16.5 dBFS when a band switches off |

The time from a change to its swap was not measured directly: it is the match itself (0.1–3 ms,
above), a wake, and the next block boundary; under fast automation a design can wait for the one
being matched, so at most about two match times, then the 2 ms crossfade. TSan passes locally and
in CI with the worker running during the sweep tests, where each plugin is destroyed right after
its sweep, possibly while the worker is still matching.
The Oscairn host check (section 17) runs the EQ modes at 48 kHz, so through the worker, in the
engine and in a worker process.

Found on the way: at 44.1 kHz, where nothing is matched, switching a band off clicks (−23 dBFS
above 8 kHz in the test): the band's contribution stops within a block, as any parameter jump
does there. The reference's parameter smoothing is not measured (known difference 19).

### Batches fx17 and fx18 (2026-11-02)

fx17: 87 renders and 38 sequences; fx18 (fresh hold-out for the delay, seed 1818): 11 renders and
38 sequences. One batch at a time under the recorder lock (fx17 in four parts, each with a warm-up),
renders in the measurements folder; no dropouts (tone renders' envelopes agree across all 22,
every render as long as asked, Q-sweep renders steady). Analysis in the measurement repository's
`fx/delay.py` and `fx/reverb.py`, driven by `fit_fx17.py`, `fit_damp.py`, `fit_q3.py` and
`gate_dly.py` (scratch drivers).

- **Delay balance (fx17, a tone with the echo a second later):** dry and wet are cos and sin of
  (π/2)(v − 1)/126: the wet-to-dry ratio within each render is within 0.004 dB of the law for all
  21 values; absolute levels scatter by 0.15 dB with the tone's own envelope.
- **Input modes (fx17):** L feeds the L>R line only, R the R>L line only (ping-pong, feedback at
  every crossing). The probes with the source panned hard left showed nothing on the right at all:
  the part's pan comes after its insertion effects, so they cannot tell whether L&R mixes its
  inputs (known difference 22).
- **High damping (fx16, fx17, every value):** a two-tap filter (1 − g) + g z⁻¹ in the feedback
  path, g = 5(1 − ratio)/9, within 0.001 of a free fit at every value (fitted bin by bin on
  successive echoes). An earlier fit on octave-band averages gave a table that was 0.5–2 dB off
  at 16 kHz after a few repeats; the bin fit replaced it.
- **Hall reverb with a sampled click (fx17):** the AWM2 click is bit-identical in every render, so
  subtracting it leaves the reverb exactly (residual −300 dB), and the reverb itself repeats:
  identical renders give the same level to 0.01 dB. Wet energy relative to the dry click
  (send and return at maximum): −2.89 / −3.23 dB (L / R) at factory settings; −5.7 dB at time 0,
  +0.5 dB at time 69; −3.96 / −4.74 dB at diffusion 0 / 5 (factory 10); −11.9 dB at high ratio
  0.1 and +0.5 dB at 1.0. Onset (−30 dB of the peak, 0.5 ms windows) = initial delay + 48.4 ms
  (initial delay 0.1, 67.8, 100.8, 200 ms). Decay times agree with fx16.
- **Three-band mid Q (fx17, a Q-index sweep at 507, 1,286 and 2,660 Hz):** the index law holds to
  within 1.7% at 507 Hz for every index; what misses at higher centres is the centre, which moves
  with Q, by more the higher it is: +1.9 / +5.3 / +12.1% at index 0 and −0.5 / −1.4 / −2.8% at
  index 31, about zero near Q 2.6 (the Q the frequency table was measured at). A first joint law
  (centre shift proportional to frequency and to 1/Q − 1/2.6) reaches only 0.57 dB, so nothing
  ships yet (known differences 5 and 9).
- **Compressor steps failed again (fx17, fx18):** the second note, at velocity 90 or 64, is silent in
  the dry renders too, with or without a millisecond between the note-off and the note-on: the
  probes' amplitude velocity sensitivity setting silences any velocity below 127. fx16's
  explanation (velocity 40 too quiet) was wrong for the same reason. Next: default velocity
  sensitivity, or an amplitude-LFO square. The steady levels repeat fx16's to 0.01 dB.

### Tempo cross delay (built 2026-11-02)

Mode "Tempo Cross Delay" of a new plugin, Oscairn FX Delay (`org.oscairn.effects.delay`),
`effects/TempoCrossDelay.h`: written, not the sst Delay the plan proposed, because the measured
structure is a plain cross delay whose laws are simpler to state directly.

| Law | Fitted on | Against renders |
| --- | --- | --- |
| Lines L>R (left in, right out) and R>L, cross-fed through the feedback gain v / 63.5 at every crossing; L and R inputs feed one line each, L&R both | fx14, fx16, fx17 | echo times exact, levels within 0.03 dB (first six echoes per side, every octave band within 30 dB of the strongest) |
| Mode 2: the input side's line receives the other's output at the feedback gain squared, the other at unity; at negative feedback that leg inverts (fb·\|fb\|, added 2026-11-12 from fx18 `dho01` and the review batch's `tcd_m2l_n37` / `n20`) | fx16 | 0.32 dB after five loops (0.06 dB per loop; known difference 21) |
| Length by the delay family's time law (section 15, "Cross Delay"): the note value at the tempo cut to 0.1 ms, plus the lag, t tenths of a ms giving floor(t × 18063 / 4096) samples at 44.1 kHz, both lines (changed 2026-11-12 from round(T·44100) + round(lag·44.1) − 1) | fx14, fx16, fx17, fx18 | every first echo at the reference's own sample: 1/1 triplet 58,797 (d_t5_15_fb), 1/32 dotted with lag −12 3,602 (dho04), 1/8 triplet with lag −17 6,597 (dho06), 1/32 dotted with lag −31 2,765 (dho07), which the old law missed by 1–2 samples (known difference 20, resolved) |
| High damping g = 5(1 − r)/9 on the feedback only | fx16, fx17 | 0.01–0.02 dB |
| Long notes: while a line would reach 131,072 samples its note halves (`halvedNote`, the tempo stereo type's limit, per line); changed 2026-11-12 from a cap at 65,535 samples read from fx9's "about 1.487 s", which was this halving seen through the recorder's onset jitter | delay-eq-2026-11-12 `tcd_t17/18/19` (click, input L&R) | 4 and 5 quarters at 120 BPM play 88,198 and 110,247 samples on both channels, 6 quarters 66,148 (132,297 halved); the cap played all three at 65,535 |
| Dry cos, wet sin of (π/2)(v − 1)/126 | fx17 | 0.004 dB (ratio within a render) |
| Tempo from the transport, 120 BPM without one | (recorder fixed at 120 BPM) | not measured at other tempos |

Gate against all renders (`gate_dly.py`, each render's own dry click through the plugin, every echo
compared): fitting renders (fx16, fx17, 34) worst 0.32 dB and 0.18 ms; fresh hold-out fx18 (10
random settings, never fitted except the lag's rounding, which was chosen after seeing it) worst
0.26 dB and 0.14 ms, both from Mode 2 and triplet lengths; the eight settings in the other modes
within 0.03 dB.

At other rates the lines keep their length in seconds (a first-order all-pass carries the
fraction, so no repeat loses level) and the damping is a 65-tap linear-phase filter fitted to the
two-tap's magnitude, its 32-sample delay taken off the lines and added to the input. Plugin test:
at 48, 88.2, 96 and 192 kHz the echo lands within a sample of its time and each repeat's response
is within 0.04 dB of the 44.1 kHz filter from 20 Hz to 20 kHz, at damping 0.1, 0.4, 0.7 and 0.9
(proposed tolerance 0.05 dB). The tests fail with each law changed (lag truncated, the old damping
law, the two-tap at every rate, a length left fractional at 44.1 kHz, the input not delayed with
the damping filter, the tail rule at 100 ms).

Not built: the delay's EQ parameters (no effect measured; decision 23). Not measured: tempo other
than 120 BPM; whether a line halves more than once at slow host tempos (assumed, as for the
stereo type).

### Batches fx19 to fx24: the compressor's probes, the hall reverb (2026-11-05)

fx19 (112 renders: compressor pilot and the hall grid), fx20 (11: AWM2 LFO laws), fx21 (33), fx22 (55)
and fx23 (14) for the compressor, fx24 (14: fresh hall hold-out, seed 2424, recorded after the
hall's tables were fixed). One batch at a time under the recorder lock, each with a warm-up,
renders in the measurements folder. Every render was written; the tone renders' envelopes show no
gaps and the click renders subtract to −300 dB. fx22 was recorded while a build ran on the same
machine; its envelopes were checked the same way and show nothing. Analysis drivers are kept
outside the repositories (`probes.py`, `batch19.py`–`batch24.py`, `cfit.py`, `rev_fit.py`,
`gate_rev.py`); the measurement repository's `fx/` gained `main_onset_ms`, `align` and `gain_db`.

- **A repeatable step source for the compressor.** An AWM2 part playing the Sine wave with its
  element LFO as a square on amplitude, key-on reset, is bit-identical between renders after an
  integer shift, so dry and wet renders align sample by sample (correlation 0.9998). The AWM2
  element LFO follows LFO Extended Speed (LFO Speed has no effect): v / 12 Hz (6 gives a period of
  1.96 s, 12 of 0.99 s); amplitude depth 64 gives a 13.1 dB step, 32 a 6.0 dB step, 127 silence.
- **Compressor: what the steps show (fx21–fx23).** Not built. The output level law is exact again
  (60 log10(v / 200) dB). Below a level that the input-level setting reaches at about 110–120 (a
  sine at −21.8 dBFS), the gain does not change within a render at all: a 13 dB step down and back
  changes it by under 0.2 dB, from the first 2 ms on. Yet across renders that gain depends on the
  input level, attack and release settings, and not as fixed offsets: the steady output rises only
  about 1 dB per 3.5 dB of input-level setting from 50 to 100 (taking the input level as
  60 log10(v / 200) dB), and the attack setting raises the gain at input 60 (+3.6 dB from 10 to 175)
  but lowers it at input 100 (−3.0 dB from 0 to 200). Above that level the steps do compress, with
  visible attack (about 10–50 ms at attack 100) and release (about 300 ms to recover 5 dB at release
  100), and slower attack settings compress more, not less. The side-chain input level (0, 79 or
  127) changes nothing without a side-chain source. No detector (peak or power follower, feed-forward
  or feedback) with a static gain law reproduces gains that depend on a setting but not on the
  signal, so the type stays unbuilt: a decision for Jacob (decision 25).
- **Hall reverb grid (fx19).** Decay times per octave band for reverb time index 0–69 × room size
  0–31 (10 × 6), high ratio 0.1–1.0, low ratio, diffusion, decay, HPF, LPF. Above the shortest times
  and smallest room, each band's decay rate is the 1 kHz rate plus an amount that depends on the
  high ratio and hardly on the time, and grows as the room gets smaller; at reverb times 0.3 and
  1.1 s the decay is the same flat 0.17–0.99 s set by the room size alone. The wet energy depends
  on the reverb time (−5.4 dB at 0.3 s to 0 dB at 18 s) and hardly on the room size. The HPF is a
  first-order high-pass; the LPF is 0.4 + 0.6 times a first-order low-pass (−2.37 dB at its
  cutoff, a −7.7 dB floor). The tail begins at the initial delay plus 23.8 / 31.2 / 37.3 / 44.0 /
  48.4 / 52.7 ms at room size 0 / 8 / 15 / 22 / 29 / 31, and the decay parameter moves it
  (−57.1, −44.5, −31.8, 0, +34.5 ms at 0, 16, 32, 55, 63). Some renders, not others of the same
  setting, carry a short burst at 42–53 ms (unexplained then; explained 2026-11-06 as the dry click,
  known difference 24).

### Hall reverb (built 2026-11-05)

(Superseded 2026-11-06 by "Hall A rebuilt on the reference's structure" below.)

Mode "Hall A" (id 3) of Oscairn FX Reverb, `effects/HallReverb.h`: written. It matches the
measured statistics, not the reference's structure: an eight-line feedback delay network (Hadamard
mixing, lines 14–63 ms scaled by room size, four input all-passes whose coefficient follows the
diffusion), each line with a seven-band graphic EQ (shelves at 177 Hz and 5.66 kHz, peaks at Q 0.7,
solved through the bands' interaction matrix with two correction passes) that gives every octave
band its decay time, a predelay to the measured onset and a tap there. Its parameters are the
platform's in physical units (reverb time in s, initial delay in ms, filters in Hz); the decay
time is 1 / T = 1 / Tf(time, size) + (eh(high ratio) + el(low ratio)) × √(L29 / L(size)), with
Tf, eh and el measured tables (fx19; the same kind of decision as 17). The level is the network's
own energy plus measured offsets for time, high and low ratio, diffusion and decay.

| What | Fitting renders fx19 (100) | Fresh hold-out fx24 (12, never fitted) |
| --- | --- | --- |
| Decay time per band within 10% (250 Hz / 500 / 1k / 2k / 4k / 8k) | 90 / 91 / 95 / 97 / 95 / 88% of renders | 25 / 58 / 50 / 58 / 50 / 25% |
| Median decay error per band (125 Hz … 8 kHz) | 7.8 / 2.6 / 1.8 / 1.5 / 2.0 / 3.2 / 5.4% | 79 / 17 / 7.9 / 10.8 / 9.0 / 8.8 / 16% |
| Energy re the dry click | mean +0.03 dB, sd 0.49, worst −2.1 (room size 0 at the shortest times) | mean +0.12 dB, sd 0.75, worst +1.64 (11 of 12 within ±1.5 dB) |
| Tail onset within 1 ms | 89 of 100 (the rest: the burst renders and the shortest times) | 7 of 12 |

So the hold-out fails the proposed M3 decay gate (±10% per band): the model's combination of
settings does not generalise. Its decays are mostly too short (−10 to −30% in the mid bands) where
the low ratio is 1.0 or more or the diffusion low (rho03, 04, 05, 06, 09, 10), and the 125–250 Hz
bands collapse where the HPF removes them (the reference's low bands keep a long decay there).
Known difference 23. The level and the onset rule hold.

Plugin tests: the fitted model at its table points; the network's measured decay per band within
6% of the design at 44.1 kHz for three settings (fails with a 10% error in the loop gain); the
onset within a sample; the LPF's −2.37 dB at its cutoff; and at 48, 88.2, 96 and 192 kHz the decay
per band within 5% of 44.1 kHz's (worst seen 3.4%), the onset within 0.1 ms and the band energies
within 1.5 dB (worst 1.34 dB, at 8 kHz, 192 kHz: the 44.1 kHz bilinear filters' warping toward
22 kHz is not reproduced, known difference 23; 0.30 dB at 48 kHz); the rate test fails with the line lengths and the onset fixed in 44.1 kHz
samples. Coefficients are designed in process at a parameter change (eight lines × seven
sections and three 7 × 7 solves), not on the worker; not timed. Memory per context: 128,324 /
139,652 / 279,184 / 558,240 bytes of lines at 44.1 / 48 / 96 / 192 kHz plus the effect itself (the
memory tests cover the mode at every rate). Reverb plugin version 0.2.0 (decision 24).

### Circuit-style compressor (built 2026-11-05)

Mode "Circuit Compressor" (id 1) of a new plugin, Oscairn FX Dynamics
(`org.oscairn.effects.dynamics`, version 0.1.0), `effects/CircuitCompressor.h`: written. Its
parameters are the platform's in physical units: Input Level and Output Level in dB (the
converter's law is 60 log10(v / 200); the bottom of the range, −139 dB, is the platform's 0, off),
Ratio (2, 4, 8, 12, 20), Attack and Release in ms (the displayed laws, fx7). Output Level carries
the `mix` role.

What the steps of fx21–fx23 showed is a makeup gain that depends on the settings and never on
the signal. Below threshold the gain is the input level plus the output level plus a fixed 42.03 dB
plus that makeup; the attack term's sign follows log L, which is why the attack setting raised
the gain at input 60 and lowered it at input 100. Laws, each constant fitted to renders:

| Law | Fitted on | Against renders |
| --- | --- | --- |
| Makeup = 3.01 dB + 20 log10 L × (−e − 0.6667 s ka + 0.2025 s kr), capped at 23.73 dB: L the input gain raised by 24.08 dB, s = 1 − 1/R, e = 0.7552 / R above L = 1 and 0.4997 below (with L held at 0.005 or more in that term), ka = min(1, log10(attack / 1 ms) / 1.4996) above 1 ms, kr = min(1, log10(release / 40.7 ms)) above 40.7 ms; R = 1.992, 3.890, 7.554, 11.00, 17.19 for the five ratio settings | 70 renders with no compression (fx13 AN-X sine; fx21–fx23 AWM2 square-LFO sine) | rms 0.16 dB, worst 0.66 dB (input 13, known difference 27); the free fit lands on these values (for example 1.0 ms, 1.4996 decades, 40.7 ms) |
| Detector: the larger of \|L\| and \|R\| of the scaled input through two cascaded followers with the same coefficients, 1 / (0.4367 τa fs) rising and 1 / (0.4843 τr fs) falling at 44.1 kHz, τ the displayed times | 99 renders: the compressing fx21–fx23 steps and the fx7 statics | per-render envelope rms (5 ms windows) median 0.15 dB over all 172 fitting renders, 11 over 1 dB (ratio 2, fx7's full-scale renders, low inputs) |
| Gain law: soft knee 8.40 dB wide at −36.06 dB of the scaled input, slope 1 − 1/K, K = 3.218, 4.044, 4.698, 4.982, 5.227 (it flattens at high ratio settings, as fx7/fx13 suggested) | same | same |
| Output clipped at full scale (fx7's loud renders peak at exactly 1.000) | fx7 | |

Fresh hold-out fx25 (18 random settings, seed 2525; three sources: the square-LFO sine at 13 dB
and 6 dB steps and a saw; recorded after the model was fixed; the plugin's own output compared):
median 0.32 dB rms, worst 1.88 dB; 15 of 18 within 1 dB rms. The seven sine settings that stay
below threshold match within 0.08 dB rms, the two such saw settings within 0.26 dB. The misses are compressing at ratio 2 (cho02, cho11: 1.88 and 1.13 dB)
and with the saw (cho13, cho14, cho16: 0.82, 1.52, 0.71 dB, peaks up to 7.4 dB at the steps),
known difference 26. The M2 static gate (±1 dB) is therefore not met for those cases; the time
constants are fitted inside the envelope fit, not gated on their own.

Plugin tests: the measured steady gains at 44.1 kHz (six settings below threshold within 0.15 dB,
four compressing within 0.5 dB; fails with the attack term removed), and at 48, 88.2, 96 and 192
kHz the gain in 5 ms windows over a step within 0.1 dB of 44.1 kHz's (worst seen 0.069 dB; fails
with the 44.1 kHz follower coefficients used at every rate). Memory 792 bytes per context, no
buffers. Not built: the side-chain port (known difference 29).

Measurement fix found on the way: the analyses aligned dry and wet on the first 20,000 samples,
which for a steady sine can land whole periods off and put errors of up to 12 dB at the steps; the
measurement repository's `fx/dynamics.py` now aligns on the whole signal (`f348568`). fx21–fx23's
earlier conclusions were steady-state levels and are unaffected.

### Circuit compressor rebuilt on the reference's structure (2026-11-06)

Decision 25. The first version's misses came from a structure the fit could not reach. The
reference's structure (read, then implemented independently; curves fitted, not copied) and what
each part explains:

| Part | What it explains |
| --- | --- |
| Input level held to steps of 1/16384 | The 6.02 dB steps at input settings of 25 or less (known difference 27) |
| First-order shelves (-3 dB at 2 kHz, then +3 dB at 8 kHz, its direct part clipped at full scale), a 0.43 Hz DC block, clipped again | Part of the saw's difference: the detector and the gain see a tilted spectrum |
| Detector: the louder channel after a first-order 180 Hz high-pass, two cascaded followers that rise at the attack rate and fall at the release rate, each moving 2 / N per sample (N the displayed time in 44.1 kHz samples) | The fitted 0.4367 and 0.4843 time scales were these factors of 1/2 seen through the wrong gain law |
| Gain curve per ratio, soft knee at -48.7 / -44.5 / -43.4 / -43.0 / -42.8 dB of full scale, slopes 0.495 / 0.746 / 0.871 / 0.913 / 0.947 (fitted to the reference's curves within 0.1 dB above the knee, 0.3 dB in it); its gain c is at most 1/2, and the signal is c (g x + (1/2 - g) lp(x)) with g = sqrt(c / 2) and lp a 15 kHz Butterworth low-pass | The ratio-dependent knee and slopes (the first version's flattening slopes K were the saturation below seen as compression) |
| A saturating curve, tanh of a quartic, slope 4 at zero, flat at 1 from 0.6 (within 0.002 of the reference's) | Ratio 2's squash near full scale (known difference 28) and the loud saw's steps |
| Output level times 8, then the makeup gain (the law of the first version, now with the ratios 2, 4, 8, 12, 20, 0.75 / R, -0.66, 0.2, in steps of 1/2048 and at most 32767/2048), each stage clipped at full scale | The settings-dependent gain below threshold |
| The reference's level after the effect is 7.91 dB above its input's (one constant, fitted on all 172 fitting renders); the effect runs 7.91 dB down, the output is raised again and clipped at full scale | Why the threshold seemed 10 dB away and the clipping order seemed reversed |

With only that one constant fitted to renders, the structure reproduces the 172 fitting renders
(fx7, fx13 statics; fx21-fx23 steps) within 0.212 dB RMS per render (median 0.052), the 80 statics
within 0.005 dB. The plugin's own output against the reference (`ours.py`, ours aligned to the
reference by correlation):

| Hold-out | Renders | Median RMS | Worst RMS | Worst 5 ms window | Within ±1 dB |
| --- | --- | --- | --- | --- | --- |
| fx25 (the old hold-out; this model never fitted on it) | 18 | 0.060 dB | 0.181 dB | 0.29 dB | 18 / 18 |
| fx26 (fresh, seed 2626: sine steps of 13 and 6 dB and a saw at A4, a saw at A2; attack and release over 0..200; recorded after the model was fixed) | 19 (a 20th, input 1, is silent in both) | 0.069 dB | 0.154 dB | 0.29 dB | 19 / 19 |

(Old numbers, fx25 against the first version: median 0.32 dB, worst 1.88 dB RMS, peaks of 7.4 dB.
This round's analysis first showed spikes of the same size at the steps when it aligned both outputs
to the dry render separately; aligning ours to the reference directly removes them, so part of the
old peaks may have been the analysis. Windows are reported now.)

Plugin tests: the steady gains (unchanged tolerances; now within 0.005 dB below threshold and 0.12 dB compressing), the
low input steps (inputs 8-20 within 0.1 dB) and ratio 2 at input 200 (-3.29 dBFS RMS, peak 0.810;
the test fails on the first version; these three tolerances, 0.1 dB, 0.3 dB and 0.02, accepted by
Jacob 2026-11-06), and the rate test (worst 0.064 dB at 192 kHz, gate 0.1 dB;
filters keep the 44.1 kHz design's bilinear corners, followers their time constants). Memory 1,096
bytes per context, no buffers. Dynamics plugin version 0.2.0. Not built: the side-chain port (the
reference's side-chain variant is a separate design; known difference 29).

### Hall A: what the hold-out failures point to (2026-11-05)

Reference behaviour of the hall (its structure; only partly checked against renders, as noted):

- per side, the input passes a first-order HPF (input only) and a first-order low shelf at 800 Hz
  whose gain is 10 × low ratio − 10 dB, held at +2 dB above 1.2;
- two diffuser stages, then a crossfade of the diffused and undiffused signal by diffusion / 10:
  diffusion changes no coefficient in any loop;
- an early-reflection line with four taps, placed by initial delay, room size and the decay
  parameter (which moves the block, as the onsets measured in fx19 show, and raises the later two
  taps' gains);
- no feedback delay network: one long line per side with one feedback tap and many output taps
  whose gains follow the reverb time, then a loop of nested lossy all-passes holding the
  high-ratio low-pass (first order; about 1.0 to 4.0 kHz for 0.1 to 0.8, 10 kHz at 0.9, off at 1.0)
  and the low-ratio shelf again;
- room size scales the delays and never a gain; reverb time sets gains and never a delay; the
  output is 0.6 × a first-order low-pass (the LPF) plus 0.4 × unfiltered, as fx19 measured; no
  modulation.

What that explains in fx24: low ratio acts per pass of a loop whose length follows the room, and
reaches 1 kHz (an 800 Hz shelf), while the model's low-ratio rates were measured at room size 29
only and scaled by the square root of the line length; diffusion cannot change the decay, so the
low-diffusion misses come from how the model and the decay estimate treat the early part; the HPF
is on the input only, so the reference's 125–250 Hz decay survives a high HPF, and the model's
collapse there needs a look at the model, not a new law. Nothing in the structure is random, so
the burst at 42–53 ms (present in some renders, absent in others of the same setting) is still
unexplained. Not built this round (decision 26).

### Hall A rebuilt on the reference's structure (2026-11-06)

Decision 26. `effects/HallReverb.h` is rewritten on the structure of the previous subsection, implemented
independently: its own delays, its own tap pattern, every value derived or fitted to renders. Per side:

- a first-order HPF, then a first-order low shelf at 800 Hz with gain min(10 x low ratio - 10, +2)
  dB (the reference's law, not fitted);
- three all-passes (3.3-8.9 ms) and a crossfade of the diffused and plain signal by diffusion / 10,
  so diffusion changes no coefficient;
- one long line: four early-reflection taps at initial delay + shift(decay) + a + b x S(size),
  a and b per tap measured on the diffusion-0 renders (fx19 `r_d0*`, fx27 `g_er_*`: the first
  left tap is 24.8 / 36.5 / 47.6 ms after the initial delay at size 0 / 15 / 29; a and b from sizes
  0 and 29 predict size 15 within 0.5 ms with the delay scale S fitted to decays), the later two raised by the decay setting, the first one
  present only above the shortest reverb time (it is absent at 0.3 s, which is why fx19's onsets
  at that time are 75 ms, the second tap); through one all-pass to the output;
- three taps after the first reflection feed the tank, a loop through both sides: an all-pass, the
  high-ratio low-pass, a line, a nested all-pass, a low shelf, a line, then the other side times the
  loop gain;
- the output is the reflections plus four taps of the tank's lines, through the LPF (0.4 + 0.6
  times a first-order low-pass, fx19) and the level.

Room size sets three delay scales (input all-passes and reflections, the tank's lines, the tank's
all-passes); reverb time sets the loop gain, the level and the first reflection's gain; nothing is
modulated. The plugin test "room size moves only delays, reverb time only gains, nothing is
modulated" checks this on the core (fails with a level that follows the size).

Fitted (least squares on per-band log decay, level and onset; staged, the analysis of
`fx/reverb.py` and `fx/dynamics.py` on model and reference alike), on fx19 (100 renders), fx24 (12,
the old model's hold-out, now fitting data) and the new fx27 (43: low ratio x time x size, high
ratio x time x size, diffusion x time, decay x size, HPF x low ratio, diffusion 0 at three sizes):

| Grid | Values |
| --- | --- |
| Loop gain at reverb time 0.3 / 1.1 / 1.9 / 2.7 / 3.5 / 4.3 / 5.5 / 9.5 / 13.5 / 18 s | 0 / 0 / 0.370 / 0.501 / 0.537 / 0.656 / 0.668 / 0.723 / 0.746 / 0.826 |
| Level (dB) at the same times | -9.10 / -8.63 / -8.44 / -8.46 / -7.94 / -8.13 / -7.86 / -7.60 / -7.11 / -7.24 |
| Delay scale at room size 0 / 8 / 15 / 22 / 29 / 31: all-passes and reflections; tank lines; tank all-passes | 0.202 / 0.408 / 0.578 / 0.788 / 0.969 / 1.009; 0.307 / 0.470 / 0.618 / 0.719 / 0.759 / 1.012; 0.231 / 0.392 / 0.576 / 0.730 / 1.208 / 0.880 |
| High-ratio low-pass, 0.1 ... 0.8 | 2,326 x 3.17^((h - 0.1) / 0.7) Hz (a log-linear law through the fitted 0.1 and 0.8 values; the free per-value fit was not monotonic and no better), 19.7 kHz at 0.9, off at 1.0 |
| Loop shelf (dB per pass) at low ratio 0.1 / 0.4 / 0.7 / 1.0 / 1.4 | -0.67 / -0.57 / 0 / +0.76 / +1.57 |
| All-pass gains: diffusers; tank; nested outer, inner | 0.62 (not fitted); 0.610; 0.572, 0.485 |
| Decay setting 0 / 16 / 32 / 55 / 63: shift (ms); later reflections (dB) | -56.7 / -44.5 / -32.4 / 0 / +34.3 (fx19's measured onset shifts were -57.1 / -44.5 / -31.8 / 0 / +34.5); -20 (the fit's bound) / -20 / -20 / 0 / +5.0 |

Results (decay per octave band within 10%, the M3 gate; energy re the dry click within 1.5 dB, M3's
early-reflection tolerance applied to the whole reverb; onset from the side signal, `side_onset_ms`,
within 1 ms):

| Renders | Within 10% per band (125 Hz / 250 / 500 / 1k / 2k / 4k / 8k) | Median decay error per band | Energy | Onset within 1 ms |
| --- | --- | --- | --- | --- |
| Fitting, all 155 (core, which equals the plugin's output within 2e-9) | 45 / 68 / 69 / 71 / 68 / 64 / 65% | 11.7 / 5.2 / 6.2 / 5.0 / 6.6 / 7.7 / 7.3% | mean +0.42 dB, sd 1.37, worst +6.41; 127 within 1.5 dB | 99 |
| of which fx19 (100) | 47 / 73 / 73 / 72 / 72 / 73 / 70% | 11.0 / 4.3 / 5.9 / 3.8 / 5.0 / 7.3 / 5.9% | mean +0.32, sd 1.05; 89 within | 67 |
| of which fx24 (12) | 8 / 42 / 50 / 25 / 42 / 33 / 50% | 31.4 / 12.1 / 9.1 / 14.2 / 15.1 / 15.1 / 10.3% | mean +0.63, sd 2.58; 6 within | 4 |
| of which fx27 (43) | 49 / 63 / 65 / 81 / 65 / 51 / 56% | 10.4 / 6.4 / 7.0 / 5.0 / 8.0 / 9.8 / 9.7% | mean +0.58, sd 1.50; 32 within | 28 |
| **Fresh hold-out fx28** (16 random settings, seed 2828, generated and recorded after the model was frozen; the plugin's own output through `oscairn-effects-render`) | 38 / 69 / 62 / 56 / 69 / 56 / 38% | 13.9 / 6.9 / 5.3 / 6.1 / 9.2 / 8.2 / 13.9% | mean +0.92 dB, sd 2.88, worst +6.56; 7 of 16 within | 5 of 16 (median 1.8 ms) |

Against the first Hall A (fx24, its hold-out: 25 / 58 / 50 / 58 / 50 / 25% from 250 Hz to 8 kHz,
energy 11 of 12 within 1.5 dB) the decays generalise better and the energy worse. Neither gate is
met. Where the hold-out misses: short times at small rooms (rho07, rho14: 1.0-1.2 s, sizes 6-13,
20-50% short), the 125 Hz and 8 kHz bands, and the level at high ratio 0.1-0.2 (rho02, 03, 07,
08, 09: 1.7-6.6 dB too loud; the reference loses about 9 dB from high ratio 0.8 to 0.1 even at
short times, fx19 `r_h1`, which a low-pass in the loop alone does not give; known difference 30).
The fitting set shows the same level miss (fx19 `r_h1` +4.7 dB, fx27 `g_h1_*` +4.9 and +5.0 dB, and
-2.4 dB at high ratio 1.0), so that one is the structure as built; the short small-room misses are
larger on the hold-out than on the grid (fx19 `r_t8_s8`, `r_t16_s8` within 16%).

**The 42-53 ms burst (known difference 24) is the dry click.** In the renders that carry it, the
residual after subtracting the dry click is identical in L and R (correlation 1.000; the reverb's
own output correlates 0.02) and confined to the click's last milliseconds: in fx19 `r_h1` it is
minus the dry click from 42 to 52 ms (regression -0.995), in `r_h8` extra sound from 52.5 to 60 ms
where the dry render is silent. The probe's note (held 50 ms) ends about 10 ms earlier or later
than in the dry render, so the subtraction leaves the difference. It is not the reverb, and it is
not modelled. The measurement repository's `fx/reverb.py` now has `side_onset_ms` (the side signal
(L - R) / sqrt 2 has no centred content), used for every onset above.

Plugin tests: the structure (above); the decay per band within 10% of three fitting renders (fx19
`r_h5`, `r_h6`, `r_t24_s31`; fails with the 3.5 s loop gain 9% lower) and the onset at the first
reflection within a sample (fails with the tank fed before the first reflection); the LPF's -2.37
dB at its cutoff; the rate test (unchanged gates: decay within 5%, energy within 1.5 dB, onset
within 0.1 ms; worst seen 3.4% and 1.21 dB at 8 kHz, 192 kHz), for which the delays keep their 44.1
kHz length in seconds and the first-order low-passes keep the 44.1 kHz design's response at 6.5 kHz
(fails with either undone: 8.3% at 48 kHz, 1.67 dB at 192 kHz); the click and timing tests of
decision 27. Memory per context: 231,424 bytes at 44.1 kHz, 251,612 at 48 kHz, 500,160 at 96 kHz
(lines of 228,540 / 248,728 / 497,276 bytes; 146,680 bytes before). Reverb plugin version 0.4.0.
The parameters are unchanged (names, units, ranges, defaults), so the converter's Hall A mapping
needs no change.

### Hall A's fixed filters and the fx30 hold-out (2026-11-06)

Where the high ratio acts, found from the renders: against high ratio 1.0, the energy each ratio
takes off an octave band is the same in every window from 0 to 300 ms after the onset (fx19 `r_h1`
... `r_h9`, fx27 `g_h*_t48_s15`; `win.py`), early reflections included, and only after about
300 ms does a per-pass loss appear (at high ratio 0.1, 1 kHz: -4.1 to -4.6 dB in every window up to 800 ms, then -8.4 /
-13.7 / -20.0 dB in 0.8-1.5 / 1.5-2.5 / 2.5-4 s). Filtering the ratio-1.0 render's first 300 ms by one
first-order bilinear low-pass reproduces every ratio's within 0.15 dB from 500 Hz to 8 kHz
(`hrpre.py`), at 1,000 / 1,402 / 1,803 / 2,201 / 2,806 / 3,208 / 3,611 / 4,013 / 10,088 Hz for 0.1
... 0.9; at 1.0 there is none. The low ratio works the same way (fx19 `r_l1`, `r_l4`, `r_l14`
against `r_l10`, `lrpre.py`): a first-order shelf of the input's law, 10 x low ratio - 10 dB held at
+2 dB, whose corner nearer DC is 800 Hz for a cut and a boost alike (its geometric centre is 800 x
10^(|gain| / 40) Hz; within 0.16 dB from 125 Hz to 4 kHz, where centring it at 800 Hz missed by up
to 1.8 dB). The HPF and the LPF match their first-order laws within 0.05 dB (0.26 dB at 125 Hz for
the 2 kHz LPF; `flt.py`).

So `effects/HallReverb.h` now passes the input through the HPF, the shelf (at that corner) and the
high ratio's low-pass (`inHighHz`, the fitted cutoffs above) ahead of the diffusers, so every part
of the reverb is filtered once; and the tank's low-pass and low shelf act on the signal that comes
back from the other side only, so the first pass carries the fixed filtering alone. Nothing else in
the structure changed.

A probe batch fx29 (26 renders, plus one re-recorded in fx29b after a 718-sample dropout: reverb
time index 4 / 8 / 12 x room size 4 / 10 x decay 10 / 35 / 55, and diffusion 0 at the shortest
time, sizes 8 and 29, decay 0 / 20 / 40 / 63) shows what the decay setting does at short times:
it shapes the early envelope from falling (decay 0: about -20 dB over 200 ms at size 8) to flat
(63: flat for about 200 ms, then falling), and at the shortest times that envelope sets the
measured decay. A fitted gain on the first two reflections by decay setting changed the fit by
under 2 points per band and was left out.

Refitted, staged (`stage.py`, `stageL.py`, `scanhr.py`; scratch, not committed), on fx19, fx24,
fx27, fx28 (seen, so now fitting data) and fx29: the loop's cutoffs per high ratio (2,051 / 2,681
/ 4,582 / 3,505 / 4,800 / 5,600 / 6,848 / 5,989 / 20,000 Hz for 0.1 ... 0.9; not monotonic, as
before), the loop shelf (-0.48 / -0.51 / +0.26 / +0.63 / +1.17 dB per pass at low ratio 0.1 / 0.4 /
0.7 / 1.0 / 1.4), the loop gains (0.026 / 0.050 / 0.371 / 0.477 / 0.542 / 0.640 / 0.672 / 0.717 /
0.737 / 0.825 at 0.3 ... 18 s), the levels (-7.30 ... -4.99 dB) and size 31's line and tank
all-pass scales (1.029, 1.08).

| Renders | Within 10% per band (125 Hz / 250 / 500 / 1k / 2k / 4k / 8k) | Median decay error per band | Energy | Onset within 1 ms |
| --- | --- | --- | --- | --- |
| Fitting, all 197 (core) | 41 / 62 / 61 / 65 / 68 / 62 / 53% | 13.0 / 7.2 / 6.9 / 5.9 / 6.4 / 7.2 / 9.1% | mean +0.17 dB, sd 0.91, worst +3.42; 169 within 1.5 dB (was 127 of 155) | 106 |
| of which fx19 (100) | 43 / 68 / 69 / 67 / 68 / 67 / 54% | 12.3 / 6.3 / 4.1 / 4.6 / 5.5 / 7.1 / 8.2% | mean -0.00, sd 0.53; 95 within | 66 |
| of which fx24 (12) | 42 / 50 / 42 / 33 / 33 / 50 / 58% | 14.3 / 10.0 / 11.0 / 17.6 / 15.2 / 12.3 / 8.9% | mean -0.14, sd 1.05; 8 within | 4 |
| of which fx27 (43) | 42 / 58 / 53 / 67 / 70 / 51 / 56% | 13.2 / 8.2 / 9.2 / 4.6 / 7.7 / 9.8 / 9.1% | mean +0.17, sd 0.84; 39 within | 24 |
| of which fx28 (16, the old hold-out) | 38 / 62 / 62 / 69 / 81 / 56 / 31% | 12.7 / 6.3 / 7.1 / 6.5 / 6.3 / 9.0 / 12.4% | mean -0.15, sd 1.33; 12 within (was 7) | 5 |
| of which fx29 (26) | 31 / 50 / 50 / 65 / 73 / 69 / 58% | 17.5 / 11.4 / 10.8 / 6.6 / 5.4 / 6.9 / 8.6% | mean +1.17, sd 1.08; 15 within | 7 |
| **Fresh hold-out fx30** (16 random settings, seed 3030, generated and recorded after the model was frozen; the plugin's own output through `oscairn-effects-render`) | 31 / 44 / 62 / 69 / 69 / 38 / 44% | 17.4 / 11.3 / 6.4 / 6.5 / 6.2 / 10.9 / 10.9% | mean +0.03 dB, sd 1.51, worst +3.48; 12 of 16 within | 9 of 16 (median 0.9 ms) |

Against fx28 on the 0.4.0 model (energy 7 of 16, mean +0.92 dB, sd 2.88; decay 38 / 69 / 62 / 56 /
69 / 56 / 38%), the energy now nearly meets the gate and the decays do not improve. Where fx30
misses: the smallest rooms (`rho08`, size 0: 24-41% short; `rho15`, size 1: 18-70% long), decay
settings below 30 (`rho14`, decay 0: +3.5 dB; `rho08`, `rho05`), the 125 Hz band where the
HPF is high (`rho02`, `rho07`: +87 and +99%) and the 4-8 kHz bands at short times. Neither M3 gate is
met: decay per band, and early-reflection energy, applied here to the whole reverb.

Plugin tests: "the ratios' fixed filters act on the whole reverb from its first reflection on"
(narrow-band power, 11 frequencies over 1/3 octave, of the first 12-150 ms for the high ratio and
12-300 ms for the low ratio, against the renders' values; within 0.75 dB, proposed, set after
seeing 0.52 dB at 8 kHz; fails without the input low-pass by up to 14.7 dB, and without the
shelf's corner by 1.9 dB); the other Hall A tests unchanged (the structure test also covers the
input low-pass), all passing; the decay test's three fitting renders failed (up to 15%) until
the 0.5 and 0.6 loop cutoffs and size 31's tank all-pass scale were refitted on them. Rate test: decay within 4.0%, band energies within 1.23 dB (8 kHz, 192
kHz) of 44.1 kHz's. Clicks -70.8 to -92.7 dBFS (gate -60). Worst process call 3.5-3.8 µs at 192
kHz (gate 83 µs). Lines of 229,512 / 249,792 / 499,388 bytes at 44.1 / 48 / 96 kHz (228,540 /
248,728 / 497,276 before; the memory tests cover every rate). Parameters unchanged (names, units,
ranges, defaults), so the converter needs no change. Reverb plugin version 0.5.0.

### Reset as a fresh instance (2026-11-06)

Oscairn resets a used instance before giving it another Performance's state, or before playing it
again with its own, and it must then render sample for sample as a fresh instance given that state
(plugin-porting.md, "Expect a second life"). Reset re-initialized the effect, which left Reverb 2's
tail and delay lines as they were, and an EQ change loaded after it was matched on the worker and
crossfaded in later. Now:

- Reset puts the effect back as built, in place and without allocating: a stock mode (no
  allocations of its own) is constructed again where it is; an Oscairn mode clears everything in
  its `initialize`; then the parameters are set and it is initialized, as after building it.
- Until the first block after activation or a reset nothing has been heard, so a state loaded then
  gets an effect built for it on the main thread (its EQ coefficients matched there), swapped in at
  the first block; both effects are held until then, and the memory extensions say so, as in a mode
  change. A load once the effect has played keeps it, and its tail, as before.
- An EQ result the worker has matched but not yet applied is applied at the reset, without a
  crossfade.

What reset keeps: the parameters, the mode, the host's tempo and the render mode. One case is not
as a fresh instance: an EQ change made within the few milliseconds before a reset with no state
load after it, still being matched on the worker, arrives with its 2 ms crossfade a few blocks
later, as any change does (a fresh instance matches at once). The plugin test "Reset leaves a fresh
instance at once in every mode" compares every mode at 44.1 and 48 kHz with a fresh instance (the
same state again, another state, no load with every parameter moved a block before the reset, and a
load while deactivated) and runs reset under the allocation counter; it fails without the change
and with each of the in-place construction, the parameters set before initializing, the build for
a load before the first block (from activation, and from reset), its end at the first block, the
buffers' clear and the EQ's applied result left out.

### Rotary A: the two-rotor rotary speaker (built 2026-11-11)

Decision 28. Mode "Rotary A" (id 1) of a new plugin, Oscairn FX Tremolo & Rotary
(`org.oscairn.effects.tremolo-rotary`, version 0.1.0), `effects/RotarySpeaker.h`: written. The
plugin will also hold the tremolo, auto-pan and other rotary types (section 6); none of them is
built. Measurements: the first rotary batch (`rotary-2026-11-11`: steady per-note probes, rates,
mic 0/30/60, ramps, statics, drive 16-127) and batch b (`rotary-2026-11-11b`, recorded for this
mode: 25 renders and 42 sequences, then all 61 mic angle values; every job written, no gaps in the
held tones), both in the shared measurements folder, never committed. Analysis drivers are kept
outside the repositories (`make_probes3.py`, `make_probes4.py`, `suite.py`, `cmp_ramp.py`,
`cmp_steady.py`, `cmp_static.py`, `fit3.py`, `fitx.py`, built on the first batch's `rot.py`); the
plugin is run on synthetic sines (the probes' sources) by a scratch copy of
`tools/OscairnEffectsRender.cpp` that also switches parameters at given times.

Structure, as the measurements show it:

- A crossover on the mono sum: the rotor takes a second-order low-pass, the horn a second-order
  high-pass, inverted. Fitted with modulation off (depth 0, rotors stopped; c_bal1, c_bal127,
  c_bal64): rotor 619.5 Hz, horn 999.2 Hz, Q 0.70 both, gains 9.18 and 6.81 dB; rms 0.024 dB, worst
  0.25 dB. With the horn not inverted the best fit is 3.7 dB off.
- Each rotor reads its band from a delay line through modulated taps: one per output channel,
  whose delay swings sinusoidally with the rotor's angle and whose gain dips by an AM depth 90
  degrees ahead; the rotor also has a centre tap shared by both channels. The pitch deviation in
  cents is the same at every frequency (a pure delay swing): the horn's delay is a clean sinusoid of
  0.745 ms peak at the default setting at 2.6 and 7 kHz alike. The rotor's two taps were found by
  fitting the complex envelope of r_slow_n36/48/55 (fast likewise): channel tap at half the mic
  angle each side, centre tap weight 0.83 at angle offset 0.1 degrees, both with the same swing,
  AM 90 degrees ahead of the swing, centre AM 1.42 times the channel tap's; residual 2.3% of the
  envelope. The horn has a second path that this model leaves out (known difference 32).
- With depth 0 the static response is flat (c_flat within 0.8 dB from 33 Hz to 18 kHz), so the taps
  share their centre: their delays differ only by the swing.

| Law | Fitted on | Against the fitting renders |
| --- | --- | --- |
| Speeds in Hz as set; the fast and slow speeds are separate parameters | r_slow, r_fast, r_rate_* (0.42-9.76 Hz) | rotation rate within 0.09% (rotor) and 0.12% (horn) wherever the analysis tracks it |
| Rotor ramp: linear in Hz, the whole slow-fast difference in the ramp time | r_ramp_def, t64, t127, jacob | slope within 0.6% (up) and 2.6% (down) except def down -7.4% (table below) |
| Horn ramp: exponential with the time constant when slowing down, 0.925 times it when speeding up | same | time constant within 3.4% (up) and 5.2% (down) |
| Delay swing (peak) at 100 % depth: rotor 2.13 ms slow, 1.72 ms fast; horn 1.69 ms slow, 1.21 ms fast; proportional to the depth | r_slow, r_fast, c_slow_d*, c_fast_d*, b_r_* | the reference's cents per hertz of rotation grow in proportion (rotor slow 0.120, 0.118, 0.118, 0.1166, 0.1145, 0.1095 per step at 4, 16, 32, 64, 96, 127; horn slow 0.1445 per step from 24 to 127) |
| AM depth: rotor 0.73 slow / 0.74 fast, horn 0.45 / 0.71, full from 14.0 / 26.7 % (rotor) and 17.4 / 40.3 % (horn) depth, in proportion below | c_slow_d4-d40, c_fast_d8-d96, steady renders | the reference's AM stops growing between depths 12 and 20 (slow) and 24 and 40 (fast); AM fundamental within 0.32 dB median |
| Swing and AM depth switch with the speed switch, gliding over 20 ms | rates at equal speed in both modes (r_rate_slow63 2.65 Hz against r_rate_fast64 2.69 Hz: 5.71 against 4.67 cents per hertz) | |
| Mic angle in steps (table below), half the effective angle each side | d_mic0-60 (every value), r_mic*, c_mic* | L/R pitch phase within 1.4 degrees on the slow mic probes (rotor and horn); fast, the horn's is 25-56 degrees off (32) |
| Balance: rotor cos, horn sin of (pi / 4)(1 + b / 100) | c_bal1-127 | within 0.05 dB at the six balances not used in the fit (c_bal16-112) |
| Level with modulation: rotor +0.5 dB, horn -2.2 dB at full AM depth, in proportion below | r_slow, r_fast gains | per-note gain, 65 Hz-12.5 kHz, slow and fast: median 0.33 dB, worst 1.11 dB |
| Shelves: first-order, the low shelf on the rotor's band only, the high shelf on the horn's (section below) | batch c | within 0.08 dB at Jacob's settings |
| Latency: the tap centre is the larger of the two speeds' swings at the depth, plus a sample; the plugin reports its 16-sample block | c_lat_* (Variation slot beside the dry part) | the reference's output at depth 0 aligns with its dry path within 3 samples |

Mic angle steps (the platform value v, angle 3v degrees): the rotor's taps take effective angles 0,
30, 60, 90, 120, 150 and 180 degrees from v = 0, 10, 12, 26, 36, 50 and 52; the horn's 0, 31, 61,
75, 88, 105, 121 and 180 from v = 0, 12, 14, 26, 28, 36, 38 and 52 (its L/R pitch phase in each step;
d_mic*). The plugin takes degrees and applies the steps at the midpoints between values.

**Mapping law for the converter** (raw value to plugin parameter; parameter 10 is unused):

| Raw parameter | Plugin parameter | Law |
| --- | --- | --- |
| 16 Speed Control | Speed | 0 Slow, 1 Fast |
| 1, 2 Rotor / Horn Speed Slow (0-63) | Rotor / Horn Slow Speed, Hz | 2.65 i / 63 (the Data List's table 1 prints this rounded to 0.01 Hz) |
| 3, 4 Rotor / Horn Speed Fast (64-127) | Rotor / Horn Fast Speed, Hz | the Data List's table 1 value at i (measured 2.692, 4.374, 6.729, 7.402, 9.757 Hz at 64, 80, 91, 93, 100) |
| 5 Rotor Slow/Fast Time | Rotor Ramp Time, s | 0.0964 v |
| 6 Horn Slow/Fast Time | Horn Time Constant, s | -0.0964 / ln(1 - 2.4 / v) for v >= 3, 0 for v <= 2 |
| 7, 8 Drive Rotor / Horn | Rotor / Horn Depth, % | 100 v / 127 |
| 9 Rotor/Horn Balance (1-127) | Balance, % | 100 (v - 64) / 63 |
| 11, 13 EQ Low / High Frequency | Low / High Shelf Frequency, Hz | the Data List's table 3 value at i |
| 12, 14 EQ Low / High Gain (52-76) | Low / High Shelf Gain, dB | v - 64 |
| 15 Mic L-R Angle (0-60) | Mic Angle, degrees | 3 v |

Ramps, the reference against the plugin on the same timeline (wheel at 6 and 18 s), each analysed
with the first batch's rate tracker and fit (`ramps.py`): the rotor's linear slope and the horn's
time constant, error of the plugin's against the reference's. The fitting set is def, t64, t127
and jacob (Jacob's setting); hold, ho0 and ho1 are hold-outs (b_r_ramp_hold from the first batch,
c_ramp_ho0/1 from batch b, never fitted). M5 asks for speeds and acceleration within 5%.

| Ramp | Rotor up | Rotor down | Horn up | Horn down |
| --- | --- | --- | --- | --- |
| def (fit) | -0.5% | -7.4% | +3.4% | -0.1% |
| t127 (fit) | -0.0% | -2.6% | -2.7% | +0.5% |
| t64 (fit) | +0.6% | +2.0% | +1.2% | +5.2% |
| jacob (fit) | +1.7% | +0.6% | -1.5% | -1.3% |
| hold (hold-out) | +1.0% | -5.4% | -1.0% | -0.5% |
| ho0 (hold-out) | +1.1% | -5.7% | -1.7% | -2.3% |
| ho1 (hold-out) | +2.6% | +7.9% | -2.0% | -1.7% |

So the speeds meet M5 everywhere and the horn's acceleration meets it except t64 down (+5.2%);
the rotor's slowing slope misses it in four of seven ramps, by -7.4 to +7.9% with both signs. The
plugin's ramps are symmetric and its up slopes match within 2.6%, so these are most likely the
tracker's fit of the reference's slowing ramp (its median track deviation there is 2-7%); not
verified (known difference 37).

Steady modulation, two-note probes (196 and 2637 Hz), the plugin against the reference
(`suite.py`): absolute errors, median / worst. Rows where the reference shows no modulation at
the note (depth 0, speed 0 or the analysis' limit at 39.7 Hz) are left out. Recomputed 2026-11-11 with the shelves on the rotor
and horn paths (batch c); many of these recordings are clipped (section below), which lowers the
reference's AM fundamental.

| Set | Rotor rate | Rotor AM fundamental | Rotor pitch deviation | Rotor L/R pitch phase | Horn rate | Horn AM | Horn pitch | Horn L/R pitch phase |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Fitting (42 / 41 probes) | 0.02 / 0.09% | 0.32 / 2.02 dB | 2.0 / 13.6% | 2 / 20 deg | 0.03 / 0.12% (one tracker failure at 45%) | 0.31 / 1.36 dB | 0.9 / 66% | 2 / 96 deg |
| Hold-out c_ho0-5 (12 / 7) | 0.03 / 0.22% | 1.33 / 3.38 dB | 10.7 / 37.1% | 2 / 3 deg | 0.10 / 0.32% (ho3 and ho4 slow left out: clipped, below) | 0.24 / 1.40 dB | 16.6 / 74% | 15 / 20 deg |
| Jacob's setting, slow / fast | +0.02 / -0.03% | +0.54 / +0.51 dB | -0.4 / -2.2% | +1 / +1 deg | +0.01 / -0.02% | +0.05 / -0.98 dB | -0.5 / -13.0% | -2 / -2 deg |

The worst cases: the rotor at high depth (96-124) with a small mic angle, where the reference's
pitch deviation grows less than in proportion and its AM less (hold-outs ho3-ho5: +13 to +20%,
+1.7 to +3.4 dB; known difference 33); the horn in fast mode, whose pitch measure is 13-22% low at
depths 16-40 and whose L/R phase the reference's second path moves (32, 36). Two hold-out horn rows,
ho3 slow and ho4 slow, read as 0.73 and 1.095 Hz with almost no modulation: their recordings were
clipped at full scale (below); the clean repeats read 0.166 and 0.966 Hz, and the plugin is within
0.38% and 0.20% of them.

Statics (modulation off, saw at note 24, 33 Hz-16 kHz where the reference is above -30 dB): flat
0.01 dB, the crossover's rotor-only and horn-only 0.14 / 0.00 dB, the six balances not fitted
within 0.05 dB; shelves within 0.31 dB (section below).

Plugin tests ("Rotary A: ..."), each checked to fail with its law broken: steady speeds at 44.1,
48 and 96 kHz within 0.3% (fails with the phase step at 44.1 kHz at every rate: 8.7% off); ramps,
every rotation within 0.05 turns of the law at 44.1, 48 and 96 kHz (worst seen 0.032; fails with the
horn's up ratio removed, 0.23 turns, and with an exponential rotor ramp, 5.0 turns); the horn's swing
and AM depth by depth setting and the rotor's swing doubling (fails with the AM in proportion to
the depth, 0.11 off, and with the swing as the square root, 870%); the crossover and balance against
19 reference levels within 0.15 dB, and at 48 and 96 kHz within the rate gate (fails with the horn
not inverted, 7.4 dB, and with a linear balance, 3.2 dB; it does not fail with the crossover designed
at the running rate instead of keeping the 44.1 kHz corners, whose difference at the tested points is
within the gate); the horn's L/R phase at twelve mic angles within 2 degrees (fails with a continuous
angle, 33 degrees); linearity: twice the input gives twice the output to 1e-5 and harmonics stay
below -100 dB (fails with a saturating output). "Processing never allocates" switches speed, depth and
a shelf while playing. The generic tests (load checks, editor hints, ports, memory, reset) include
the new plugin. Memory: 34,152 bytes per context at 44.1 kHz (two 4,096-sample lines), 147,864 at
other rates (the shelves' matchers).

### Rotary A: the shelves, and clipping in the probes (batch c, 2026-11-11)

Batch c (`rotary-2026-11-11c`, 56 renders and 21 sequences, every job written): the shelves alone
(rotors stopped, depth 0) on a saw at velocity 40, peaks 0.10-0.51, at Jacob's seven shelf settings
(his 41 uses of the type), a grid of six frequencies by three gains per shelf, the default shelves at
balance 1 and 127, and eight random hold-outs (seed 1112); clean repeats at velocity 60 of the
two-tone probes; and a hot input at part volume 40. Analysis drivers outside the repositories
(`make_probes5.py`, `eqc.py`, `model_static.py`, `task1.py`, `rategate.py`).

**Shelves.** Each is a first-order section, bilinear at 44.1 kHz with K = tan(pi f / 44100) at the
set frequency f and A = 10^(|gain| / 20): a low-shelf boost is (s + K A) / (s + K), a cut
(s + K) / (s + K A); a high-shelf boost (A s + K) / (s + K), a cut (s + K) / (A s + K). The low shelf
acts on the rotor's band only and the high shelf on the horn's, before they are mixed; batch b's
fits failed because they put the shelves on the whole output, and its +12 dB probes clipped. Against
the renders, worst error within 20 dB of each setting's peak (33 Hz-16 kHz): Jacob's seven settings
0.011-0.079 dB, the hold-outs 0.006-0.188 dB, the grid within 0.073 dB except 32 Hz (0.29-0.31 dB at
-6 and +6 dB, 0.12 at +12). With the shelves on the whole output instead, batch b's probes miss by
0.2-3.2 dB. At other rates each shelf runs the EQs' rate-matched replacement of its 44.1 kHz design
(`MatchedCascade`, on the engine's worker): the whole static response stays within 0.043 dB of 44.1
kHz's at 48, 88.2, 96 and 192 kHz within 20 dB of its peak (seven settings including 16 kHz at
±12 dB and 2 kHz at +12 dB), so the rate gate is met. Plugin test "Rotary A: the low shelf acts on
the rotor, the high shelf on the horn, as measured": 40 reference levels at Jacob's three most used
settings and two hold-outs within 0.15 dB at 44.1 kHz and within the rate gate at 48, 96 and 192 kHz;
it fails with the shelves on the whole output (0.25 dB), without rate matching (0.058 dB) and with a
cut's corner placed as a boost's (0.16 dB). The reset test now gives Rotary A, as the EQs, time for
the worker before a reset without a state load.

**Clipping in the probes.** Jacob's listening pair 03 (his setting, two tones at velocity 127)
sounded different because the reference recording is hard-clipped at full scale: 992,674 samples at
exactly 1.000. The source was not (the dry two tones peak at 0.995 with no flat top); the effect
raises the 196 Hz tone by 8.7 dB at his setting (low shelf +9 dB), so the instrument's output clips.
The plugin's render for that pair was fed 6 dB less and was never clipped, so the pair was not a fair
comparison. At velocity 60 nothing clips (peaks 0.63), and the plugin's level at his setting matches
the reference's: -0.23 / -0.56 dB at 196 / 2637 Hz slow, -0.19 / +1.02 dB fast, rms of the whole
ramp -13.2 against -13.0 dB. A hot input at part volume 40 shows flat tops at ±0.388 in each channel,
not at the same instants in L and R, so the reference also clips per channel after the taps, at
2.51 times the level a full-scale dry signal has there (about +8 dB); known difference 41.

185 of the 366 reference recordings of the first two batches touch full scale. Clipping moved nothing
much but the AM fundamental where it was heavy: clean repeats against the clipped originals keep
rates and L/R phases, and pitch deviations within 0.1% on the default-depth probes (1-4% on hold-outs
ho3 and ho4 fast, and ho4 fast's horn reads 140.7 cents clean against 93.7 clipped), but move the AM
fundamental by -0.2 to +0.9 dB (mic sweeps and rates), up to +1.7 dB (rotor) and +2.0 dB (horn) on
hold-outs ho3 and ho4. So the steady table's AM columns for the r_mic, r_rate, c_mic, c_ho and
c_jacob probes, and the mic-angle trade-off of known difference 33, were measured on clipped
recordings; the per-note probes (r_slow, r_fast) that set the AM depths were not clipped (at most one
sample). Against the clean repeats the plugin's rotor AM is +1.13 / +0.78 / +0.19 dB at mic 0 / 30 /
60 (was +2.01 / +1.21 / +0.52 against the clipped ones), +1.4 to +2.0 dB at ho3 and ho4 (was +2.8 to
+3.4), Jacob's setting -0.01 / +0.01 dB slow / fast. Clean ramps: Jacob's rotor +1.3 / -2.6%, horn
-2.9 / -0.1%; the default's rotor -0.6 / -6.7%, horn -2.1 / -3.8% (up / down), as on the clipped
recordings.

### Cross Delay (built 2026-11-11)

Mode "Cross Delay" of Oscairn FX Delay (`org.oscairn.effects.delay`, mode 2), `effects/CrossDelay.h`:
the tempo mode's signal path (the shared `CrossCore`) with the two line times as ms values (0.1 to
1,638.3, steps of 0.1) instead of note lengths, no lag, and its own balance law. Measured against 13
renders of the reference type 02 00 (batch `measurements/qwen-cross-delay-2026-11-11/`, analysis
`analyse.py`, gate `results.md`: 57 gated metrics, all pass, worst 0.05 dB and 0.023 ms), 8 more
(`measurements/qwen-night-2026-11-12/cross_delay_extra/`: Mode 2 and four balance positions, gate
42/42) and 6 hold-out balance positions (`measurements/qwen-night-2026-11-12-review/cross_delay/`).
Every metric is measured inside one render against its own click (known difference 12), so
absolute levels are not gated.

Raw mapping for the converter (CROSS DELAY, Data List p. 160; the batches' pfms were written with
these laws and every render matched them):

| ins_a param | raw | plugin parameter |
| --- | --- | --- |
| 1 Delay Time L>R | ms × 10 (1–16,383 = 0.1–1,638.3 ms) | Cross Delay/Time L>R (ms) |
| 2 Delay Time R>L | ms × 10 (same) | Cross Delay/Time R>L (ms) |
| 3 Feedback Level | v + 64 (1–127 = −63..+63) | Cross Delay/Feedback |
| 4 Input Select | 0 L, 1 R, 2 L&R, 3 L Mode 2, 4 R Mode 2 | Cross Delay/Input (same order) |
| 5 Feedback High Damping | ratio × 10 (1–10 = 0.1–1.0) | Cross Delay/High Damping |
| 10 Dry/Wet Balance | v + 64 (1–127, 64 = D=W) | Cross Delay/Mix (same order) |
| 13–16 EQ low/high freq+gain | Data List p. 160 / Table #3 | none (known difference 44: the EQ acts on the whole output, not built) |

| Law | Probe | Against renders |
| --- | --- | --- |
| Line length: the delay family's time law, t = ms × 10 tenths of a ms giving floor(t × 18063 / 4096) samples at 44.1 kHz (`tenthsSamples`, `msLineSeconds`), both lines; 18063 / 4096 = 4.4099121 per 0.1 ms is the one 12-bit fixed-point factor inside both measured brackets (02 40 / 02 50: 4.40990–4.40994; 02 28 / 02 20: 4.409908–4.409915), and the factor every delay type's renders fit (changed 2026-11-12 from round(ms × 44.1) − 1, known difference 46) | cd_500, cd_input_r, cd_input_lr, cd_time_max | exact (22,049 / 30,869 / 13,229 samples; 1,638.3 ms 72,247, which round(ms × 44.1) − 1 put one later) |
| Times reach 1,638.3 ms; the ceiling is the parameter's, not the tempo mode's 65,535 samples | cd_time_max, cd_time_swap | echoes at 1,638.3 ms on their own side and 1,638.4 ms crossed, within 0.023 ms |
| Tempo ignored | cd_500_t120 vs cd_500_t150 | echo times agree to 0.000 ms at 500 / 1,000 / 1,500 / 2,000 ms |
| Feedback v / 63.5 per crossing, negative inverts (+−+− per crossing) | cd_fb_max, cd_fb_low, cd_fb_neg | within 0.01 dB (−0.07 / −18.00 dB per crossing), signs match |
| Damping two-tap g = 5(1 − r)/9, feedback path only (echo 1 unfiltered) | cd_damp_min, cd_damp_half | every octave band 125 Hz–16 kHz within 0.05 dB |
| L feeds line A only, R line B only, L&R splits (L→A, R→B), not summed | cd_input_r, cd_input_lr | echo levels within 0.01 dB of the render's own click; a summed feed would sit 6.0 dB higher (known difference 22, for a centred source) |
| Mode 2 as the tempo mode's: the fed line answers at unity, crosses at unity, and comes back at fb·\|fb\| per two crossings, so at negative feedback each repeat inverts | cd_m2l_40, cd_m2l_n37, cd_m2r_40, cd_m2r_n37 | times within 0.000 ms; unity leg 0.00 dB; loop −7.97 / −9.32 dB against fb²'s −8.03 / −9.38 (known difference 21); signs ++−−++ at −37 |
| Balance: the side being turned down falls by the square of its distance to the end, ((63 − \|v\|) / 63)², the other stays at unity (`msBalance`; until 2026-11-12 the pair was scaled to equal power): wet/dry = 40·log10((63 − \|v\|) / 63) dB with the sign of v. NOT the tempo mode's equal-power pair (±4.77 dB at ±21) | fitted: cd_mix_m48/m21/p21/p48; hold-out: cd_mix_p10/m35/p35/m55/p55/p59 | fitted −24.94 / −7.05 / +7.04 / +24.93 dB, ours within 0.01; hold-outs +3.00 / −14.09 / +14.09 / −35.86 / +35.85 / +47.89 dB, ours within 0.01 |
| Shortest line 0.1 ms rounds to the sample grid, the all-pass holds level | cd_short_wet | offset 0 samples, level 0.00 dB |
| The EQ parameters do not act in the feedback path | cd_eq | the echo's bands against the render's own click move at most 0.004 dB (low 32 Hz +12 dB, high 16.0 kHz −12 dB). That ratio cancels an EQ on the whole output, and the click's own spectrum in cd_eq is −2.5 / −7.1 dB at 8 / 16 kHz against the batch's other renders: the EQ shelves dry and wet alike (known difference 44, not built; decision 23) |

The balance law has no fitted constant: it was found on the four first positions and then held
on six recorded afterwards. Its normalisation needs levels across renders, which the click
cannot give (known difference 12); a steady saw can (batch `measurements/delay-eq-2026-11-12/`,
analysis `analyse.py`): with the EQ flat, the dry at D=W is level with the dry-only render's
(0.000 dB in 02 00, 02 40 and 02 50, where equal power would be −3.01 dB, as the tempo types
measure: −3.010 / −3.010 / −3.011 dB in 02 10, 02 20 and 02 28), and the wet-only render's echo
is level with the dry-only render's dry (−0.002 dB in all six types); the dry-only render is
level with the part without the insertion (−0.001 dB). So both sides are at unity at D=W in the
ms types (known difference 43, resolved 2026-11-12).

Known differences: the 1,638.3 ms echo was one sample above the reference with round(ms × 44.1) − 1;
the family's time law (2026-11-12) puts it on the reference's sample (known difference 46,
resolved); the
EQ is not built (known difference 44); the Mode 2 loop is 0.06 dB per loop louder than fb²
(known difference 21); absolute levels are not gated (known difference 12). Rates other than
44.1 kHz have no renders; the mode shares the tempo mode's all-pass and linear-phase damping,
tested for 02 10.

Tests: "Cross delay: the laws measured against the 02 00 renders" and "Cross delay: Mode 2 and
the balance, as measured" hold the times, feedback, damping, routing, the Mode 2 signs at −37 and
the reference's wet/dry at all ten balance positions (±0.05 dB); the tempo cross delay's Mode 2
section holds the sign at −20.

### Tempo Stereo Delay (built 2026-11-12)

Mode "Tempo Stereo Delay" of Oscairn FX Delay (`org.oscairn.effects.delay`, mode 3),
`effects/TempoStereoDelay.h`: the cross delay's `CrossCore` with one note length for both lines,
a Lag on both, an L/R Diffusion that moves only where each line is read for the output (the
left later and the right earlier by its ms; the loop keeps the note plus Lag), and a fixed L&R
feed (input L feeds the line that plays right, input R the one that plays left; no
input-select parameter). Measured against 14 renders of the reference type 02 28 (batch
`measurements/qwen-night-2026-11-12/tempo_stereo/`) and 29 more that keep the dry click in the
render (batch `measurements/qwen-night-2026-11-12-review/tempo_stereo/`, four job lists; the
night batch's wet-only renders hold no click, so they could gate gaps between echoes but no
absolute time). Every metric is inside one render against its own click (known difference 12).
No recording is clipped (peaks −21.5 to −29.3 dBFS) and each holds its note.

Raw mapping for the converter (TEMPO DELAY STEREO, Data List p. 160; the pfms were written
with these laws and every render matched them):

| ins_a param | raw | plugin parameter |
| --- | --- | --- |
| 1 Delay Time | Table #13 index 0–19 | Tempo Stereo Delay/Delay Time (the same index) |
| 2 Feedback Level | v + 64 (1–127 = −63..+63) | Tempo Stereo Delay/Feedback |
| 3 Feedback High Dump | ratio × 10 (1–10 = 0.1–1.0) | Tempo Stereo Delay/High Damping |
| 4 L/R Diffusion | 64 + ms (1–127 = −63..+63 ms) | Tempo Stereo Delay/L/R Diffusion (ms, the same value) |
| 5 Lag | 64 + ms (1–127 = −63..+63 ms) | Tempo Stereo Delay/Lag (ms) |
| 10 Dry/Wet Balance | v + 64 (1–127, 64 = D=W) | Tempo Stereo Delay/Mix (same order) |
| 13–16 EQ low/high freq+gain | Data List p. 160 / Table #3 | none (the EQ acts on the whole output; not built, known difference 44) |

| Law | Probe | Against renders |
| --- | --- | --- |
| Each line by the family's time law, floor(t × 18063 / 4096) samples at 44.1 kHz for t tenths of a ms, at least one: t = the note time truncated to 0.1 ms + Lag, + Diffusion for the left output and − Diffusion for the right. Any factor from 44.09908 to 44.09915 per ms fits (18063 / 4096 per 0.1 ms is 44.099121; shipped as 44.0991 until 2026-11-12); round(ms × 44.1) − 1 misses 12 of the 20 settings by a sample | 20 settings: notes 1/32 triplet to 6 quarters, Lag −63..+32, Diffusion −63..+63 (review `rs_*`, night `ts_tmin`) | every echo at the reference's own sample, 1 to 130,533 samples (table in the test "every echo time measured on the 02 28 renders") |
| The note time halves when the longer line would reach 131,072 samples: 6 quarters (3,000 ms) plays 1,500 ms, also with Diffusion; Lag −40 (2,960 ms, 130,533 samples) does not halve, Lag −15 (2,985 ms, 131,637) does; Lag −28 (131,062) does not, Lag −27 (131,106) does (2026-11-12) | rs_t16–t19, rs_t19l15/l40, rs_t19o1/o2, rs_t18o1; delay-eq-2026-11-12 `ts_t19_lag36/37` | the threshold lies between 131,063 and 131,106 samples, so 131,072 (2^17) is taken; ours plays 131,062 and 64,958 as the reference. Night ts_tmax's 66,148-sample spacing is this halving, not a cap: 4 and 5 quarters play 88,198 and 110,247. The note halves as often as it takes (`halvedNote`; only host tempos under about 60 BPM ask twice, which the recorder cannot) |
| Diffusion moves the output, not the loop: with feedback, every repeat follows the first by the note plus Lag on both channels | rs_dfb (Diffusion +63, feedback +40), rs_dfb2 (1/4, Diffusion −40, Lag +10, feedback −40) | left 17,476 + k × 14,698, right 11,919 + k × 14,698; rs_dfb2 left 20,726 / right 24,254 + k × 22,490; ours identical |
| Feedback v / 63.5 per repeat, negative inverts each repeat | ts_base, ts_fbmax, ts_fbneg, rs_dfb2 | −11.98 / −0.07 / −4.02 dB per repeat, sign pattern matches, ours within 0.01 dB |
| Damping two-tap g = 5(1 − r)/9, feedback path only | ts_dampmin, ts_damphalf | repeats within 0.01 dB broadband of the reference |
| Balance: the tempo family's linear position, wet/dry = tan((π/4)(v + 63)/63) | ts_base (D=W), rs_mixm21 / rs_mixp21 / rs_mixp48 | −4.77 / +4.77 / +14.46 dB in the reference, the law and ours (02 00's position would give ±7.04 / +24.93) |
| Host tempo, performance tempo ignored | ts_t150 | echo lags equal ts_base's |
| The EQ acts on the whole output, dry and wet alike | ts_eq against the other dry-bearing renders | echo/click inside the render 0.00 dB (so no EQ in the loop or the wet path alone), but the click itself is −3.1 dB at 8 kHz and −7.5 dB at 16 kHz against the mean of 31 renders without EQ (spread 1.2 / 0.9 dB) at high gain −12 dB, 16 kHz |

Results: the four job lists of the review batch were recorded one after another, each designed
from what the previous showed, so all 29 renders count as fitted, except rs_t19l15 / rs_t19l40,
recorded to test the 131,072-sample threshold guessed from rs_t19o1 / rs_t19o2 (both as
predicted). Ours against the reference, after the change: every echo time 0 samples off in all
43 renders (33 with a click, 10 wet-only by gap), levels within 0.013 dB.

The night batch's own write-up was wrong in four places, all hidden by wet-only renders: the
diffusion is 1 ms per raw step applied in opposite directions to the two outputs, not 2 ms per
step on the right line alone (both channels' echoes were 63 ms off at the range ends); the time
law is floor(ms × 44.0991), not round(ms × 44.1) − 1 plus a separately rounded offset; 6 quarters
halves rather than capping at 66,148 samples (the cap would have played 4 and 5 quarters
at 1,500 ms); and the EQ is not inert. Its "hold-out" ts_tmax was used to set the cap. The
changed code passes the same night renders as before (0 samples, ≤ 0.013 dB).

`CrossCore` gained optional output taps (`Tap`, a reader sharing a `Line`'s buffer, and
`outputTaps` in the mode); the other cross modes don't declare it and keep their output.

Tests: "Tempo stereo delay: every echo time measured on the 02 28 renders" holds the 20 settings
above, both channels; "… the lines loop at the note plus Lag, Diffusion moves only the output"
holds rs_dfb's repeats with feedback −40, the crossing, 48 kHz in seconds and the balance; the
state round trip and value texts as before. Each fails without its law: the night diffusion
convention and round(ms × 44.1) − 1 fail the time table, removing the halving fails it at 6
quarters, diffusion inside the loop fails the repeat test.

Unverified: the halving threshold's last 44 samples (between 131,063 and 131,106) and whether it
halves again at slow host tempos (the recorder runs at 120 BPM); behaviour with a true stereo
source (input L to the right-playing line is the family's convention, known difference 22);
rates other than 44.1 kHz (tested in seconds, not recorded).

### L/R Delay (built 2026-11-12)

Mode "L/R Delay" of Oscairn FX Delay (`org.oscairn.effects.delay`, mode 4), `effects/LRDelay.h`:
one line per channel with two taps each — input L feeds the L line, which answers on the left
after Time L and recirculates through its own feedback tap every Feedback Time 1; the R line
mirrors with Time R and Feedback Time 2 — and nothing crosses between the channels. The line
primitives (`Line`, `Damping`, `dampingCoefficient`, `msLineSeconds`) and the balance law
(`squaredBalancePosition`) are the delay family's unchanged; only the routing is the mode's
own (the shared `CrossCore` routes crosswise, which this parameter set contradicts). Measured
against 14 renders of the reference type 02 40 (batch `measurements/qwen-night-2026-11-12/delay_lr/`,
analysis `analyse.py`, gate `results.md`: 48 gated metrics, all pass; worst 0.08 dB and 0.05 ms)
and 8 more at D=W (batch `measurements/qwen-night-2026-11-12-review/delay_lr/`: the EQ's output
action, a second balance position, the dry end, and four times that tell the time law's rounding
apart). The first batch's probes are wet only and carry no dry click, so they gate echo-to-echo
spacings, cross-channel offsets and echo ratios inside one render (known difference 12); the
review batch's carry their own click, so its times are absolute. No recording clips (peaks
−16 to −27 dBFS) and each holds its note.

Structure, settled by the batch: with Time L 200 / Time R 500 / Feedback Time 1 150 / Feedback
Time 2 300 the left echo grid is {200 + k × 150 ms} and the right's {500 + k × 300} (lr_pair;
absolute in lr_hold_mix and lr_pair_dw: 8,819 and 22,049 samples after the click), and swapping
only the feedback times moves each grid with its own channel's parameter (lr_hold_swap). A
cross-coupled reading (times summed into two cross lines) would put the left first echo at
800 ms and leave lr_hold_swap's grids unchanged; both probes fail it. At feedback 0 each channel
holds one echo and the far channel sits at the recording floor, −67 dB (lr_cross).

Raw mapping for the converter (DELAY LR, Data List p. 160; the batches' pfms were written with
these laws and every render matched them):

| ins_a param | raw | plugin parameter |
| --- | --- | --- |
| 1 Delay Time L | ms × 10 (1–16,383 = 0.1–1,638.3 ms) | L/R Delay/Time L (ms) |
| 2 Delay Time R | ms × 10 (same) | L/R Delay/Time R (ms) |
| 3 Feedback Time 1 | ms × 10 (same) | L/R Delay/Feedback Time 1 (ms) |
| 4 Feedback Time 2 | ms × 10 (same) | L/R Delay/Feedback Time 2 (ms) |
| 5 Feedback Level | v + 64 (1–127 = −63..+63) | L/R Delay/Feedback |
| 6 Feedback High Damp | ratio × 10 (1–10 = 0.1–1.0) | L/R Delay/High Damping |
| 10 Dry/Wet Balance | v + 64 (1–127, 64 = D=W) | L/R Delay/Mix (same order) |
| 13–16 EQ low/high freq+gain | Data List p. 160 / Table #3 | none yet: the EQ shelves the whole output (known difference 44); dropped until the mode has it |

| Law | Probe | Against renders |
| --- | --- | --- |
| One line per channel, its own delay time and loop spacing; no cross-feed | lr_pair, lr_cross, lr_hold_swap (hold-out) | left spacing 6,614 samples, right 13,229, cross-channel offset 13,230, ours identical; lr_hold_swap's grids follow the swap, ours identical |
| Line length by the family's time law, floor(ms × 10 × 18063 / 4096) samples at 44.1 kHz on every tap (`msLineSeconds`; round(ms × 44.1) − 1 until 2026-11-12); a line's loop tap composes in series, so echoes run past the parameter's own maximum | lr_pair, lr_hold_grid (hold-out), lr_tmax, lr_law_a/b (hold-outs, review batch) | exact at 150/200/300/500 ms; where the old round(ms × 44.1) − 1 differed, the reference and the family's law agree on every time of both batches (0.1 to 1,638.3 ms): 0.1 ms gives 4 samples (old 3), 11.3 / 29.1 / 57.9 ms give 498 / 1,283 / 2,553 samples (old one fewer), 1,111.1 / 1,414.1 / 1,555.5 / 1,638.3 give 48,998 / 62,360 / 68,596 / 72,247 (old one more each); ours now identical (known difference 46, resolved) |
| Times reach 1,638.3 ms; 0.1 ms is the shortest | lr_tmax | the left echoes 72,247 apart in the reference and ours (72,248 with the old law), the series echo −4.02 dB under echo 1 (ours −4.01); the 0.1 ms line recirculates every 4 samples (a fitted comb of the click leaves no residual; ours 4, old 3) and its first echo sits 72,243 samples before the left's first (ours 72,243, old 72,245) |
| Feedback v / 63.5 per loop, negative inverts (sign +−+− per loop) | lr_fbsign, lr_fblevel | decay −0.07 / −18.00 dB per loop both channels (law −0.07 / −17.99, ours −0.07 / −17.99), sign pattern matches |
| Damping two-tap g = 5(1 − r)/9, feedback path only (echo 1 unfiltered) | lr_dampmin, lr_damphalf | every octave band 125 Hz–16 kHz within 0.08 dB of the reference (−0.05 / −1.48 / −5.31 dB per loop at 125 Hz / 8 / 16 kHz, r = 0.1) |
| Balance: the cross delay's law (`msBalance`), wet/dry = 40·log10((63 − \|v\|) / 63) dB with the sign of v, the near side at unity (delay-eq-2026-11-12 `lr_dw` / `lr_wet` against `lr_dry`: 0.000 / −0.002 dB), not the tempo modes' equal-power pair | lr_hold_mix, lr_bal_m48 (hold-out, review batch) | wet/dry at v = +21: reference +7.04 dB, ours +7.04 (linear +4.77); lr_hold_mix chose the law and the code changed after it, so it is not a hold-out here. At v = −48: reference −24.94, ours −24.94 (linear −14.46) |
| The ends: dry exactly zero at D<W63, wet exactly zero at D63>W | lr_pair, lr_dryend (review batch) | lr_pair's dry residual ≤ −75 dB re the click (ours ≤ −80); lr_dryend's left minus right is zero to the sample, so no echo at all |
| Tempo ignored | lr_pair vs lr_tempo | echo lags agree to 0.000 ms at four points per channel |
| The EQ parameters shelve the whole output, dry and wet alike, and nothing in the feedback path | lr_eq vs lr_pair; lr_eq_dw vs lr_pair_dw (review batch) | at low 32 Hz +12 dB / high 16.0 kHz −12 dB: the loop's echo-to-echo band ratios move 0.00 dB and the in-render echo/click ratio ≤ 0.013 dB in every band, while the click itself (and in lr_eq the first echo) lose 4.0 / 8.3 dB (2.8 / 7.3 dB) at 8 / 16 kHz against the other renders' spectral shape, whose scatter there is 0.4–0.9 dB. Not built (known difference 44) |

Results: fitted probes worst 0.08 dB (damping bands; feedback and wet-end rows 0.00–0.01 dB)
and 0–1 sample on every spacing and offset. Hold-outs: lr_hold_grid, lr_hold_swap and the review
batch's lr_law_a/b ≤ 1 sample (≤ 0.023 ms) on every time, lr_bal_m48 0.00 dB; the EQ is the one
measured action the mode lacks. Absolute levels remain ungatable with this rig (click-to-echo
spread 7.4 dB across renders, known difference 12).

Each channel's line is one buffer read twice, for the output at Time L or R and for the loop
at its feedback time. At rates other than 44.1 kHz the damping is a 32-sample linear-phase
filter and only the loop is shortened by its delay, so the output taps keep their time down to
0.1 ms; feedback times shorter than 33 samples at those rates lengthen the loop to 33.

Tests: six cases in `tests/OscairnEffectsTests.cpp` ("L/R delay: …") hold the grids on both
channels at 44.1 kHz, their lengths in seconds within a sample at 48, 88.2, 96, 176.4 and
192 kHz, the negative-feedback sign alternation, the range ends and the series loop
(72,248 / 144,496 samples, the 0.1 ms line at 3), the tempo independence, the balance (+7.04
and −24.94 dB at v = +21 / −48), the feedback-path damping, the tail held until the longest
tap answers, the state round trip and the value texts. Each fails when its code is broken:
the linear balance position, a cross-coupled read, Feedback Time 2 on the L loop, the damping
filter's delay left in the loop at other rates, and a tail limit from the loop times only
(pass 1's, which let the plugin sleep before a Time L longer than both loops answered).

### L/C/R Delay (built 2026-11-12)

Mode "L/C/R Delay" of Oscairn FX Delay (`org.oscairn.effects.delay`, mode 5), `effects/LcrDelay.h`:
one recirculating line read at four offsets. The input (the mean of the two inputs) feeds the
line, which answers on the left after Time L, on the right after Time R, on both channels after
Time C at (Level C / 127)², and every Feedback Time feeds itself back through the family's
two-tap damping at feedback / 63.5. Each channel's echo grid is {its time + k × Feedback Time}
together with {Time C + k × Feedback Time}. The line primitives (`Line`, `Damping`,
`dampingCoefficient`, `msLineSeconds`) are the delay family's; the routing is the mode's own (the
parameter set has one feedback time, which neither `CrossCore` nor `LRDelay` fits). Measured
against 12 renders of the reference type 02 50 (batch
`measurements/qwen-night-2026-11-12/delay_lcr/`, analysis `analyse.py`, gate `results.md`: 79
gated metrics, all pass) and 7 more (batch `measurements/qwen-night-2026-11-12-review/delay_lcr/`:
Level C and balance hold-outs, a D=W pair with and without the EQ). Every probe drives the effect
with the same click on both channels (known difference 22). The wet-only renders carry no dry
click, so in them only echo spacings, cross-channel offsets and echo-to-echo levels are measured;
absolute echo times are measured in-render only where the click is present (lcr_hold_mix,
lcr_r_dw, lcr_r_eqdw, lcr_r_balm48).

Structure: with Time L 200 / Time R 300 / Time C 560 / Feedback Time 150 ms the left channel's
echoes sit at {200 + k × 150} ∪ {560 + k × 150} and the right's at {300 + k × 150} ∪
{560 + k × 150}, the 560 grid at the same level on both channels (lcr_grid). A per-channel
reading has no centre tap; the cross reading of `CrossCore` would put Time L's echo on the right.
Level C 0 removes the 560 grid from both channels and leaves the others unchanged (lcr_level0).

Raw mapping for the converter (DELAY LCR, Data List p. 161; the batches' pfms were written with
these laws and every render matched them):

| ins_a param | raw | plugin parameter |
| --- | --- | --- |
| 1 Delay Time L | ms × 10 (1–16,383 = 0.1–1,638.3 ms) | L/C/R Delay/Time L (ms) |
| 2 Delay Time R | ms × 10 (same) | L/C/R Delay/Time R (ms) |
| 3 Delay Time C | ms × 10 (same) | L/C/R Delay/Time C (ms) |
| 4 Feedback Time | ms × 10 (same) | L/C/R Delay/Feedback Time (ms) |
| 5 Feedback Level | v + 64 (1–127 = −63..+63) | L/C/R Delay/Feedback |
| 6 Delay Level C | raw as printed (0–127) | L/C/R Delay/Level C (same value) |
| 7 Feedback High Damp | ratio × 10 (1–10 = 0.1–1.0) | L/C/R Delay/High Damping |
| 10 Dry/Wet Balance | v + 64 (1–127, 64 = D=W) | L/C/R Delay/Mix (same order) |
| 13–16 EQ low/high freq+gain | Data List p. 161 / Table #3 | none: the EQ acts on the whole output and is not modelled (known difference 44); neutral at the factory values 22/64/46/64 |

| Law | Probe | Against renders |
| --- | --- | --- |
| One line, four taps; the centre tap answers on both channels | lcr_grid, lcr_level0, lcr_tempo, lcr_eq | every echo of both channels down to −44 dB at the reference's spacings and offsets, ours identical to the sample; Level C 0 removes exactly the centre grid |
| Tap length by the family's time law, floor(ms × 10 × 18063 / 4096) samples at 44.1 kHz (`msLineSeconds`; round(ms × 44.1) − 1 until 2026-11-12), kept in seconds at other rates; the loop composes in series, so echoes run past the parameter's maximum | lcr_hold_mix, lcr_r_dw (in-render, from the click); lcr_grid, lcr_hold_grid, lcr_tmax (spacings and offsets) | 8,819 / 13,229 / 24,695 samples after the click at 200 / 300 / 560 ms, exact; spacings 6,614 (150 ms) and 14,698 (333.3 ms) and the hold-out's offsets exact. lcr_tmax (no click; offsets from its 800 ms centre echo): the 0.1 ms, 1,638.3 ms and 2 × 1,638.3 ms echoes sit within 2 samples (0.045 ms) of ours (known difference 46) |
| Feedback v / 63.5 per loop, negative inverts every loop | lcr_grid, lcr_fblevel, lcr_fbsign | −4.02 / −18.00 / −0.07 dB per loop (laws −4.01 / −17.99 / −0.07), ours within 0.03 dB over 15 loops; sign +−+− matches |
| Damping two-tap g = 5(1 − r)/9, feedback path only (echo 1 unfiltered) | lcr_dampmin, lcr_damphalf | every octave band 125 Hz–16 kHz within 0.04 dB of the reference (−0.32 / −1.41 / −5.26 dB per loop at 4 / 8 / 16 kHz at r = 0.1, −0.26 / −1.09 / −3.63 at r = 0.5) |
| Level C gain (raw / 127)², unity at 127, silent at 0 | lcr_hold_level (fitted: it set the law), lcr_r_lc32, lcr_r_lc100 (hold-outs) | centre over side echo −11.91 dB at 64; hold-outs −23.95 dB at 32 and −4.15 dB at 100, the law and ours within 0.005 dB (raw / 127 would give −11.97 and −2.08) |
| Balance: the cross delay's law (`msBalance`), wet/dry = 40·log10((63 − \|v\|) / 63) dB with the sign of v, the near side at unity (delay-eq-2026-11-12 `lcr_dw` / `lcr_wet` against `lcr_dry`: 0.000 / −0.002 dB) | lcr_hold_mix, lcr_r_balm48 (hold-outs), lcr_r_dw | echo / click +7.04 dB at D<W21 and −24.94 dB at D48>W, ours within 0.01 dB (the tempo modes' linear position gives +4.77 / −14.46); 0.00 dB at D=W |
| Tempo ignored | lcr_tempo vs lcr_grid | identical spacings and levels at 150 BPM |
| EQ parameters act on the whole output, dry and wet alike, outside the loop | lcr_eq vs lcr_grid; lcr_r_eqdw vs lcr_r_dw; lcr_r_eqwet vs lcr_r_grid | loop transfer unchanged (echo-to-echo levels within 0.02 dB); in-render echo / click 0.00 dB with the EQ on, the click and the echo with the same spectrum; but with low 32 Hz +12 dB / high 16 kHz −12 dB the 16 kHz octave band sits 7.3 dB lower and the 8 kHz band 2.9 dB lower re 1 kHz than in 13 renders without EQ (three EQ renders −22.2 to −23.1 dB, no-EQ renders −14.6 to −16.1 dB with one at −19.7). The low shelf's action is not measurable with this click |

Results: fitted probes worst 0.04 dB (damping bands; levels and feedback 0.00–0.03 dB), times
0–2 samples with round(ms × 44.1) − 1 at the two range ends of lcr_tmax, which the family's
time law (2026-11-12) puts on the reference's offsets (known difference 46, resolved), all others exact. Hold-outs:
lcr_hold_grid's times exact, lcr_hold_mix and lcr_r_balm48 within 0.01 dB, lcr_r_lc32 and
lcr_r_lc100 within 0.005 dB. Absolute levels across renders are not gatable (known difference 12).

At rates other than 44.1 kHz the damping is a 32-sample linear-phase filter; only the feedback tap
is shortened by its delay, so the output taps keep their time down to 0.1 ms (a delayed input, as
the cross modes use, puts every tap shorter than 33 samples at 33 samples: 0.69 ms instead of
0.1 ms at 48 kHz). Feedback times shorter than 33 samples at those rates lengthen the loop to 33.
The line is one buffer read four times.

Tests: four cases in `tests/OscairnEffectsTests.cpp` ("L/C/R delay: …") hold the grids and the
centre tap at 44.1 and 48 kHz, the Level C law at the three measured values, the 0.1 ms taps and
the loop at 48 to 192 kHz, the negative-feedback alternation, the two-tap damping, the range ends
and the series loop (72,248 / 144,496 samples, the 0.1 ms tap at 3), tempo independence, the
balance, the state round trip and the value texts. Each fails with its law changed: raw /
127 for Level C, the centre on one channel, the linear balance, tap lengths one sample long, the
feedback's sign dropped, the damping removed, the feedback tap not shortened at other rates, and
the input delayed by the damping's length.

Unverified for this type: the line's feed with a true stereo input (known difference 47), the EQ's
shelf laws (known difference 44), and rates other than 44.1 kHz against renders (none recorded).

### Tempo Mono Delay (built 2026-11-12, measured 2026-11-12)

Mode "Tempo Mono Delay" of Oscairn FX Delay (`org.oscairn.effects.delay`, mode 6),
`effects/TempoMonoDelay.h`: ONE recirculating line. The mono-mean input feeds the line, which
feeds itself back through the family's two-tap damping every note time plus Lag, and is read for
the left channel L/R Diffusion later than that and for the right channel L/R Diffusion earlier,
so each channel's echoes sit on {time + lag ± diffusion + k × (time + lag)}. Measured against two
batches of the reference type 02 20: `measurements/qwen-night-2026-11-12/tempo_mono/` (13
renders; analysis `analyse.py`, gate `results.md`) and the review batch
`measurements/qwen-night-2026-11-12-review/tempo_mono/` (11 renders, every one with the dry
click in it, so every time is measured inside one render against its own click, known
difference 12). The first batch's wet-only renders (balance D<W63 cuts the dry completely) carry
no click: they gave the echo spacings and the left-right gap, and their absolute positions only
against the recorder's lead-in, which varies by more than 90 ms between renders; the review
batch replaced every conclusion that rested on that.

Raw mapping for the converter (TEMPO DELAY MONO, Data List p. 160; both batches' pfms were
written with these laws and every render matched them):

| ins_a param | raw | plugin parameter |
| --- | --- | --- |
| 1 Delay Time | Table #13 index 0–19 | Tempo Mono Delay/Delay Time (the same index) |
| 2 Feedback Level | v + 64 (1–127 = −63..+63) | Tempo Mono Delay/Feedback |
| 3 Feedback High Damp | ratio × 10 (1–10 = 0.1–1.0) | Tempo Mono Delay/High Damping |
| 4 L/R Diffusion | 64 + ms (1–127 = −63..+63 ms, as printed) | Tempo Mono Delay/L/R Diffusion (ms) |
| 5 Lag | 64 + ms (1–127 = −63..+63 ms) | Tempo Mono Delay/Lag (ms) |
| 10 Dry/Wet Balance | v + 64 (1–127, 64 = D=W) | Tempo Mono Delay/Mix (same order) |
| 13–16 EQ low/high freq+gain | Data List p. 160 / Table #3 | none: they act on the whole output and the mode does not build them (known difference 44) |

| Law | Probe | Against renders |
| --- | --- | --- |
| One line, both channels on one grid with no diffusion: echoes at the note time and every multiple after it | tm_base, tm_t150 | 22,049-sample spacing, five echoes per channel, L = R to 0.00 dB, ours identical |
| Line length by the family's time law (floor(t × 18063 / 4096) samples at 44.1 kHz, t in tenths of a ms; 44.0991 per ms until 2026-11-12), t = the note time truncated to 0.1 ms plus Lag and ± Diffusion (the tempo stereo type's law, without its halving) | tm_base, tm_tmin, tmx_t18, tmx_tmax | 22,049 (quarter), 1,834 (1/32 triplet, "41.6 ms"; the untruncated cross law gives 1,837), and 5 and 6 quarters 110,247 and 132,297 samples after the click and again between echoes 1 and 2, all exact (round(ms × 44.1) − 1 gives 110,249 / 132,299) |
| No halving at 3,000 ms: the line holds the full 6 quarters at 120 BPM; the note halves only when a tap would reach 262,144 samples, twice the stereo type's per-line limit (one line in place of two; changed 2026-11-12 from a clamp at 132,297) | tmx_tmax | first echo 132,297 samples after the render's own click; the stereo type halves 6 quarters to 66,148. The 262,144 limit is reached only under about 62 BPM, which the recorder cannot test (known difference 45) |
| Feedback v / 63.5 per loop, negatives decay alike and flip polarity once per loop | tm_base, tm_fbmax, tm_fbneg, tm_tmin | −11.98 / −0.07 dB per loop (law −11.97 / −0.07), sign pattern +−+− at v = −63, ours within 0.01 dB |
| Damping two-tap g = 5(1 − r)/9, feedback path only (echo 1 unfiltered) | tm_dampmin, tm_damphalf | every octave band 125 Hz–16 kHz within 0.05 dB of the reference |
| Lag: 1 ms per raw step on the loop and both taps | tmx_lagn, tmx_lagp, tm_lagneg | echoes at 19,271 / 24,827 samples (22,049 ∓ round(63 × 44.1)) on both channels and repeating at that same spacing; ours identical |
| L/R Diffusion: 1 ms per raw step on each tap, the left later and the right earlier than the loop, which it does not move | tmx_diffp, tmx_diffn, tm_diffpos, tm_diffneg | at raw +63 the left echo at 24,827 and the right at 19,271 samples, both repeating every 22,049; raw −63 mirrors it; the gap is 5,556 samples (two roundings of 63 ms), ours identical |
| Balance: the tempo types' linear position x = (v + 63)/126, equal power (`tempoBalance`; delay-eq-2026-11-12: the dry at D=W 3.010 dB under the dry-only render) | tm_base, tm_tmin, tmx_mixp21, tmx_mixm21 | wet/dry 0.00 / +4.77 / −4.77 dB at D=W / ±21 (02 00's position would give ±7.04); the wet end cuts the dry |
| Host tempo drives the grid, the performance tempo does not | tm_t150 vs tm_base | the grid stays 22,049 samples, not 17,639 |
| The EQ parameters act on the whole output, not in the loop | tm_eq vs tm_damphalf; tmx_eqlo, tmx_eqhi, tmx_eqflat | the loop's per-band drop moves ≤ 0.03 dB up to 4 kHz (0.12 / 0.29 dB at 8 / 16 kHz) and echo/click stays 0.00 dB in every band, but the click itself changes shape: high 16 kHz −12 dB takes 7.4 dB off its 16 kHz band, low 2.0 kHz +12 dB lifts 125 Hz–1 kHz by 10–13 dB against 16 kHz, against a scatter (s.d. over 13 EQ-free renders) of 0.6–0.84 dB from 2 kHz up and 2.3–4.2 dB below 1 kHz (known difference 44) |

Results: on the night batch, worst 0.05 dB (damping, 16 kHz; tm_eq's loop drop is 0.28 dB off at 16 kHz, the reference's own +0.29 dB against tm_damphalf) and 0 samples on every lag; the
review batch, which the plugin was then corrected to, matches to 0 samples and 0.01 dB. Hold-outs: none are left unfitted. The
night batch's three (tm_tmax, tm_lagneg, tm_eq) each turned out to need the review batch to be
read, and the review batch changed the code (the diffusion law), so all its renders count as
fitting renders.

The review batch changed two laws. The time law is now the tempo stereo type's floor(ms ×
44.0991), which the review batch's 5- and 6-quarter echoes need (round(ms × 44.1) − 1 put them 2
samples late) and which every other time of both batches fits too. Diffusion: the first version held the left tap at the note
time, shortened the right tap by the whole gap and read the raw step as 2 ms (range −126..126 ms),
because the wet-only renders showed only the gap. With the click in the render the two taps
split symmetrically about the loop, so the parameter is now the printed −63..+63 ms (default
8 ms, the factory bank's most common raw 72) and moves each tap by its value. Lag was already
right (the loop follows it); the unhalved 6-quarter line, read at first from the lead-in alone,
is confirmed.

The line is one buffer read three times: the loop at the note plus Lag and the two output taps.
At rates other than 44.1 kHz only the loop is shortened by the damping filter's 32-sample delay,
so the output taps keep their time.

Tests: the four "Tempo mono delay" cases hold the 22,049-sample grid on both channels at 44.1 kHz
and, with the loop, in seconds at 48 to 192 kHz, the 1,834-sample truncation, the 110,247- and
132,297-sample long notes, the feedback decays and polarity, the linear balance at five positions,
the symmetric diffusion (24,827 / 19,271) with the loop at the note time, the lag on the loop and
both taps (19,271, repeating at 19,271), the host tempo, the two-tap damping, the state round trip
and the value texts. Each fails one case when its law is changed: the night diffusion law, a loop
without the lag, round(ms × 44.1) − 1, the cross delay's balance, and the damping filter's delay
left in the loop (or taken off an output tap) at other rates.

Unverified for this type: the line's limit (262,144 samples, taken as twice the stereo type's),
which only host tempos under about 62 BPM reach (known difference 45), the EQ's
shapes (known difference 44), rates other than 44.1 kHz (tested, not recorded), and the line's
feed with a true stereo source (known difference 47: every probe drives both channels with the
same click).

### Auto Pan (built 2026-11-12)

Mode "Auto Pan" (id 2) of Oscairn FX Tremolo & Rotary, `effects/AutoPan.h`, for AUTO PAN (06 00).
Measurements: batch autopan_b (14 renders: rates, depth, waves 6/15/20, L/R, L turn, F/R in L<>R,
three hold-outs, one of them clipped at the instrument's output and not used), autopan_c (42: the
wave law, the six directions, L/R at four waves, F/R in the turn directions, ten EQ settings at
velocity 50, a stereo source, three hold-outs) and autopan_d (32: L/R at the other 23 waves, four
more mid-band settings). The source is a held AN-X saw at 110 Hz (velocity 90, peaks 0.25; the EQ
probes at velocity 50); for the stereo probes, an AWM2 saw element panned hard left, which is the
only way found to give a part insertion a stereo input. Every recording was checked for samples at
full scale: autopan_b's third hold-out (EQ at +12 dB) has 2,181 and is left out; nothing else
touches it. Analysis drivers are kept outside the repositories (`cmp.py` renders the plugin from
the reference's own dry note and compares, `apfit.py`, `lrfit2.py`, `frfit.py`, `eqfit.py`,
`summary.py`, `rates.py`).

Structure, as the measurements show it:

- The three-band EQ first: first-order shelves, exactly Rotary A's law (bilinear at 44.1 kHz; the
  five-band EQ's slope-0.5 shelf is the same response), and a peak with Q = width x 10^(-|gain| /
  40) (the cookbook peak with the three-band EQ's effective Q, no widening). At L/R depth 0 the
  effect is otherwise unity (ap_fr: 1.000 in both channels).
- Mono feeds both channels the mean of the input, Stereo each channel its own (st_mono: both
  channels at half the left input; st_stereo: the right output stays silent).
- One LFO at the set speed in Hz. Each channel has its own attenuation u in [0, 1]; its gain is
  1 - (L/R depth) u. The channels read the LFO at offsets set by the direction: the right half a
  turn after the left in L<>R and L/R, the left a sixth of a turn ahead in L>>R, the right a sixth
  ahead in L<<R, the left a quarter ahead in L turn, the right a quarter ahead in R turn (dir1-4:
  -60.0, +60.0, -90.0, +90.0 degrees right against left; w0, ap_hardlr: 180.0).
- In L<>R to R turn u is a triangle, 0 at the LFO's peak and 1 at its trough, times
  (1 + wave / 128)^2 and clipped at 1: wave 0 is a linear crossfade (the gains sum to 1), higher
  waves hold the far channel silent for longer (the fitted slope 2k: 1.003, 1.047, 1.096, 1.162,
  1.248, 1.334, 1.409, 1.484 at waves 0, 3, 6, 10, 15, 20, 24, 28; the law gives 1, 1.047, 1.096,
  1.162, 1.248, 1.337, 1.410, 1.484).
- In L/R the LFO is a sine: u = steepness x (0.069 - cos), clipped to [0, 1], so each channel is
  fully on for 48% of the period and falls with a steepness measured at every wave (0.85 at wave 0,
  rising smoothly to 1.644 at 17, then 1.93, 2.07, 2.55, 3.08, 3.64, 4.26, 5.13, 6.08, 7.09, 8.69 and
  34 at 28, nearly a switch). At wave 0 the far channel only reaches 0.091 (lr_w0).
- In the turn directions only, F/R depth adds a delay that grows with the same u: quieter is
  further away, a Doppler swing (fr127, fr64, fr8: 31, 16 and 2 steps of 4; none in L<>R, ap_fr,
  or L/R, fr127_d5). The level does not change with F/R depth alone.
- The speed is Hz as set and does not follow the transport (tempos 90, 120 and 150 on the rate
  probes give the table's rates).

| Law | Fitted on | Against the fitting renders |
| --- | --- | --- |
| Speed in Hz (the Data List's table 1) | ap_slow, ap_fast, ap_depth, ap_maxrate, every 0.67 Hz probe | -0.016% at 0.67, 2.19 and 39.7 Hz (phase drift over the render); ap_slow by the fit, -0.04% |
| L/R depth: gain 1 - (v / 127) u | ap_depth, lr_d64 (v 64), the full-depth probes | depth 0.5040 fitted, 64 / 127 = 0.5039 |
| Wave: u = min(1, (1 + w / 128)^2 (1 - triangle) / 2) | w0, w3, w10, w24, w28, ap_wave06, ap_wave15, ap_slow | slope within 0.3% of the law at every wave (above) |
| Directions: phase offsets 0/180, 60/0, 0/60, 90/0, 0/90, 0/180 degrees (left/right) | dir1-4, dir1_w20, dir3_w20, w*, lr_* | within 0.05 degrees of the plugin's (L/R phase, the plugin against the reference) |
| L/R: a sine with threshold 0.069 and a measured steepness per wave | lr_w0-28 (every wave) | model fit rms 0.24-0.87% of full gain per wave (wave 28: 1.7%) |
| F/R: delay up to 124 samples at 44.1 kHz times the F/R depth (4 floor(v / 4) samples), times u, in the turn directions only | fr127, fr64, fr8, fr127_d4w20, fr_lr_d3, ap_turn | fitted 123.5, 63.7, 7.96 samples for 124, 64, 8; the plugin's swing within 0.4% of the reference's through the same analysis (fr8: 3.7%, a 0.18 ms swing) |
| EQ: first-order shelves, peak with Q = width x 10^(-abs(gain) / 40) | eq0-eq9 | shelves within 0.032 dB, peaks at 630, 1600 and 4000 Hz within 0.075 dB; the peak at 315 Hz, width 12: 1.25 dB (known difference 50) |
| Mono / Stereo | st_mono, st_stereo, st_mono_d3 | the right output silent with Stereo; Mono half the left input in both, within 0.14 dB |

Whole-render comparison (`summary.py`): the plugin renders the reference's own dry note with the
mapped settings, its LFO aligned to the reference's by one time shift per render; "rms" is the rms
gain error per channel in fractions of full gain over the held note, "p99" the 99th percentile of
the gain error in dB where both gains are above -20 dB (pointwise, so the steepest edges dominate
it), "L/R phase" the right channel's LFO phase against the left's, reference minus plugin.

| Set | rms | p99 | L/R phase | Delay swing | Level | Depth (max / min) |
| --- | --- | --- | --- | --- | --- | --- |
| Fitting: rates, depth, waves in L<>R (11) | <= 0.3%; ap_maxrate 1.4% (39.7 Hz, the analysis window is a third of its period); ap_slow 4.5% (its longer release falls inside the window) | <= 0.32 dB; ap_maxrate 1.01 dB | <= 0.10 deg | — | <= 0.011 dB | <= 0.013 dB |
| Fitting: directions (8) | <= 0.2% | <= 0.34 dB | <= 0.013 deg | <= 0.4% | <= 0.011 dB | <= 0.008 dB |
| Fitting: L/R, all waves (31) | <= 1.6% | <= 1.67 dB | <= 0.05 deg | <= 0.2% | <= 0.003 dB | <= 0.14 dB |
| Fitting: F/R (7) | <= 0.2% | <= 0.13 dB | <= 0.006 deg | <= 0.4% (fr8 3.7%) | <= 0.007 dB | — |
| Hold-out ap_holdout1 (R turn, wave 27, F/R 115, L/R 119, Mono, EQ) | 0.3% | 0.35 dB | 0.008 deg | -0.12% | 0.006 dB | 0.08 dB |
| Hold-out ap_holdout2 (L>>R, wave 2, 0.42 Hz, EQ) | 0.4% | 0.13 dB | 0.004 deg | — | 0.000 dB | 0.001 dB |
| Hold-out ho0 (L>>R, L/R 18, F/R 65, EQ) | 0.0% | 0.01 dB | 0.005 deg | — | 0.001 dB | 0.003 dB |
| Hold-out ho1 (L turn, 3.11 Hz, L/R 25, F/R 18, EQ) | 0.4% | 0.21 dB | 0.04 deg | +0.15% | 0.016 dB | 0.014 dB |
| Hold-out ho2 (R turn, wave 14, L/R 112, F/R 70, high shelf +10 dB at 560 Hz) | 1.3% | 1.52 dB | 0.014 deg | +0.16% | 0.16 dB | 0.26 dB |
| Hold-out EQ: mid 315 Hz w2 / 1.6 kHz w12 / 100 Hz w5 / 10 kHz w3 (autopan_d) | | 0.19 / 0.01 / 2.70 / 0.03 dB worst harmonic | | | | |

So the rate (M4/M5 ±2%), L/R phase (±5 degrees), depth (±1 dB) and level (±0.5 dB) tolerances are
met on every fitting render and every hold-out; the EQ (±0.5 dB) is met except the mid band at its
lowest centres with a narrow width (315 Hz at width 12, 100 Hz at width 5; known difference 50).
The pointwise p99 is above 1 dB only on the steep L/R waves and on ho2, at the edges: there a
one-period analysis window straddles a fast gain change (known differences 51 and 52).

**Mapping law for the converter** (raw value to plugin parameter; parameters 10, 14 and 16 are
unused):

| Raw parameter | Plugin parameter | Law |
| --- | --- | --- |
| 1 LFO Speed (0-127) | LFO Speed, Hz | the Data List's table 1 at v (2.65 v / 63 for v <= 63, as Rotary A's slow speeds) |
| 2 L/R Depth | L/R Depth, % | 100 v / 127 |
| 3 F/R Depth | F/R Depth, % | 100 floor(v / 4) / 31 |
| 4 Pan Direction (0-5) | Pan Direction | v (L<>R, L>>R, L<<R, L turn, R turn, L/R) |
| 5 LFO Wave (0-28) | LFO Wave | v |
| 6, 8, 11 EQ Low / High / Mid Frequency | Low Shelf / High Shelf / Mid Frequency, Hz | the Data List's table 3 value at v |
| 7, 9, 12 EQ Low / High / Mid Gain (52-76) | Low Shelf / High Shelf / Mid Gain, dB | v - 64 |
| 13 EQ Mid Width (1-120) | Mid Width | v / 10 |
| 15 Input Mode | Input Mode | 0 Mono, 1 Stereo |

Plugin tests, each checked to fail with its law broken: "Auto Pan: the LFO speed, and the wave's
clipped triangle, at 44.1, 48 and 96 kHz" (the reference's share of each period below half gain at
waves 0, 15 and 28, within 0.005; fails with the wave ignored, 0.502 against 0.5996 at wave 15);
"Auto Pan: the directions set the channels' phase offsets, as measured" (within 2 degrees; fails
with every direction at L<>R's offsets, 180 against -60 degrees); "Auto Pan: direction L/R follows a
sine with a steepness per wave, as measured" (below half gain and the floor at waves 0, 14, 20 and
27; fails with one steepness for every wave and with the threshold at 0); "Auto Pan: F/R swings the
delay in the turn directions only, at 44.1, 48 and 96 kHz" (2.8005 and 1.445 ms within 1%; fails
with the swing in every direction and with 128 samples instead of 124, 2.868 ms); "The rotary
plugin's insertion modes: Mono feeds both channels the input's mean, Stereo each its own" (fails
with the input mode ignored); "The rotary plugin's insertion modes: the three-band EQ as measured,
at 44.1 kHz and within the rate gate" (40 reference levels within 0.05 dB; fails with the peak's Q
without the gain factor and with a cut's corner moved by the gain). "Processing never allocates"
switches the speed, the direction and a band while playing; the state and value-text tests cover
the mode's thirteen parameters. Rotary A's context is 34,252 bytes at 44.1 kHz now (the plugin's parameter values for the new modes). Rotary A shares the shelf design and the cubic tap read; its output
is bit-identical (its renders at 44.1 and 96 kHz compared before and after, and its tests).
Memory: 9,428 bytes per context at 44.1 kHz (two 1,024-sample lines), 68,468 at other rates (the
EQ's matcher). Tremolo & Rotary plugin version 0.2.0 (decision 24).

### Tremolo (built 2026-11-12)

Mode "Tremolo" (id 3) of Oscairn FX Tremolo & Rotary, `effects/Tremolo.h`, for TREMOLO (06 10).
Measurements: batch trem_b (14 renders: rates, AM depth, PM, phase +90 degrees, EQ, three
hold-outs; its EQ probe clipped at the instrument's output, 1,765 samples at full scale, and is not
used), trem_c (36: PM at 4-124, AM at 4-96, AM with PM, four phase values, ten EQ settings at
velocity 50, a stereo source, three hold-outs), trem_d (16) and trem_e (13): phase values around
every step of the law. Source and analysis as for Auto Pan.

Structure, as the measurements show it:

- The three-band EQ as Auto Pan's (the same laws: within 0.051 dB at every EQ probe but the narrow
  315 Hz peak, known difference 50), then a fixed
  +2 dB: with both depths 0 every harmonic is 2.00 dB above the dry note (tr_am0), and the AM's peak
  is the same 1.259.
- Mono feeds both channels the mean of the input, Stereo each channel its own (st_*).
- One sine LFO at the set speed in Hz. The left channel reads it at its phase, the right at its
  phase plus the phase difference (tr_phase: the right 90.0 degrees ahead at +90), for the AM and
  the PM alike (pm_ph94).
- Per channel a gain 1 - (AM depth) (1 - cos) / 2: a raised cosine from the peak down to 1 - depth
  (tr_slow and tr_phase reach silence at 127; AM 4, 32, 64 and 96 dip to 0.968, 0.748, 0.496 and
  0.244 of the peak, against 1 - v / 127 = 0.969, 0.748, 0.496, 0.244). A triangle fits about 20 times
  worse.
- Per channel a delay that swings sinusoidally by 2 floor(v / 4) samples at 44.1 kHz (peak) around
  its centre, shortest at the gain's peak (ampm; tr_pm against tr_depth): a vibrato. The deviation
  in time is the same at every harmonic (a pure delay swing). PM 4, 8, 12, 32, 64, 100, 124 and 127
  give 1.99, 4.01, 6.01, 16.07, 32.1, 50.18, 62.23 and 62.2 samples against 2, 4,
  6, 16, 32, 50, 62 and 62: the depth acts in 32 steps.
- The phase difference acts in steps too: the right channel's lead is one of sixteen phases (every
  multiple of 30 degrees and the odd multiples of 45), the nearest to the printed angle, with the
  negative side's angles stretched slightly (law below).
- The speed is Hz as set and does not follow the transport (tempos 90, 120, 150).

| Law | Fitted on | Against the fitting renders |
| --- | --- | --- |
| Speed in Hz (table 1) | tr_slow, tr_fast, tr_depth, tr_maxrate | -0.016% at 0.67 and 2.19 Hz, +0.004% at 39.7 Hz; tr_slow -0.04% |
| +2 dB, then AM 1 - (v / 127)(1 - cos) / 2 | tr_am0, am4-96, tr_depth, tr_slow, tr_fast | peak 1.2585-1.2592 (+1.997 to +2.002 dB); trough within 0.002 of the law |
| PM 2 floor(v / 4) samples at 44.1 kHz, shortest delay at the gain's peak | pm4-124, tr_pm, tr_pm_mid, ampm, pm_ph94 | 1.994-2.008 samples per step; the plugin's swing within 0.65% of the reference's through the same analysis (pm4, a 2-sample swing: 2.3%) |
| Right channel ahead by the phase difference, AM and PM | 29 values: tr_phase, ph4-124, ampm_ph94, pm_ph94 | within 0.01 degrees of the plugin with the converter's snapped angle |
| EQ: Auto Pan's | eq0-eq9 | as Auto Pan, 2.00 dB higher: within 0.051 dB except the 315 Hz width-12 peak (1.25 dB, known difference 50) |
| Mono / Stereo | st_mono, st_stereo, st_mono_pm | Stereo leaves the right output silent, Mono gives both half the left input |

| Set | rms | p99 | L/R phase | Delay swing | Level | Depth (max / min) |
| --- | --- | --- | --- | --- | --- | --- |
| Fitting: speed and AM (8) | <= 0.4%; tr_maxrate 1.3% (the analysis window) | <= 0.31 dB; tr_maxrate 0.68 dB | 0.000 deg | — | <= 0.003 dB | <= 0.002 dB |
| Fitting: PM (12) | <= 0.2% | <= 0.07 dB | <= 0.007 deg | -0.65% to +0.45% (pm4 +2.3%) | <= 0.007 dB | <= 0.018 dB |
| Fitting: phase (30) | <= 0.3% | <= 0.25 dB | <= 0.009 deg | — | <= 0.004 dB | <= 0.018 dB |
| Hold-out tr_holdout1 (2.65 Hz, AM 74, PM 21, phase raw 20, Mono, EQ) | 0.7% | 0.16 dB | 0.004 deg | -0.28% | 0.007 dB | 0.006 dB |
| Hold-out tr_holdout2 (0.88 Hz, AM 113, PM 3, phase raw 67, EQ) | 0.3% | 0.08 dB | 0.000 deg | — | 0.015 dB | 0.009 dB |
| Hold-out tr_holdout3 (0.21 Hz, AM 110, PM 91, phase raw 26, Mono, EQ) | 0.1% | 0.03 dB | 0.005 deg | -0.66% | 0.002 dB | 0.005 dB |
| Hold-out ho0 (0.29 Hz, AM 15, PM 52, phase raw 18, mid 110 Hz width 11.9) | 2.9% | 0.18 dB | 0.017 deg | -0.53% | 0.08 dB | 0.04 dB |
| Hold-out ho1 (0.46 Hz, AM 21, PM 104, phase raw 37, EQ) | 2.7% | 0.19 dB | 0.000 deg | -1.0% | 0.17 dB | 0.001 dB |
| Hold-out ho2 (0.29 Hz, AM 33, PM 68, phase raw 9, EQ) | 0.1% | 0.03 dB | 0.008 deg | -0.66% | 0.001 dB | 0.015 dB |

Every tolerance is met on every fitting render and every hold-out (rate ±2%, depth ±1 dB, level
±0.5 dB, L/R phase ±5 degrees); the hold-outs' phase values 9, 18, 20, 26, 37 and 67 were never
fitted and land on the law's phase within 0.02 degrees. ho0 and ho1's 2.7-2.9% rms is a level
offset of 0.1-0.2 dB, with their mid band at 110 and 180 Hz (known difference 50).

**Mapping law for the converter** (raw value to plugin parameter; parameters 4, 5, 10 and 16 are
unused):

| Raw parameter | Plugin parameter | Law |
| --- | --- | --- |
| 1 LFO Speed (0-127) | LFO Speed, Hz | the Data List's table 1 at v, as Auto Pan |
| 2 AM Depth | AM Depth, % | 100 v / 127 |
| 3 PM Depth | PM Depth, % | 100 floor(v / 4) / 31 (100% = 62 samples at 44.1 kHz) |
| 6-9, 11-13 EQ | as Auto Pan | as Auto Pan |
| 14 LFO Phase Difference (4-124) | LFO Phase Difference, degrees | the angle 3 (v - 64) for v >= 64 and 180 (v - 62.5) / 58.5 below, snapped to the nearest of the sixteen phases 90 q + 0, 30, 45 or 60 degrees (q = 0-3, shown -180 to +180), a tie going to the phase nearer 0; so 65-69 give 0, 70-76 30, 77-81 45, 82-89 60, 90-99 90, 100-106 120, 107-111 135, 112-119 150, 120-124 180, 58-63 0, 51-57 -30, 46-50 -45, 39-45 -60, 29-38 -90, 22-28 -120, 17-21 -135, 9-16 -150, 4-8 180 |
| 15 Input Mode | Input Mode | 0 Mono, 1 Stereo |

The plugin takes the phase difference and the PM depth as continuous values; the steps are the
converter's.

Plugin tests, each checked to fail with its law broken: "Tremolo: +2 dB at the LFO's peak, a sine AM
to 1 - depth, at the set speed, at 44.1, 48 and 96 kHz" (fails without the +2 dB, 0.997, and with a
triangle AM, by its third harmonic); "Tremolo: PM swings the delay by 62 samples at 44.1 kHz at full
depth, shortest at the gain's peak" (1.411 and 0.729 ms within 1%; fails with a 2 ms swing, 1.999
ms, and with the longest delay at the gain's peak, 0.01 degrees against 180); "Tremolo: the phase
difference puts the right channel ahead, for AM and PM alike" (fails with the left ahead); the
insertion modes' Mono/Stereo and EQ tests now include Tremolo (the EQ against trem_c's 40 levels).
"Processing never allocates" switches speed, depths, a band and the input mode while playing.
Memory: 9,404 bytes per context at 44.1 kHz, 68,444 at other rates.

## 16. Known differences from the reference

One entry per difference, kept up to date. "Reproduce" names the probe batch (built by the probe
scripts kept outside the repositories, `probes_*.py`) and the comparison (`compare_transfer.py`
and `dynamics.py` in the measurement repository's `fx/`, driven by `gate_*.py`). Sizes are worst
cases over the stated renders, 20 Hz–19 kHz unless stated.

| # | What differs | Size and where | Cause | Status | Reproduce | Code |
| --- | --- | --- | --- | --- | --- | --- |
| 1 | Two-band EQ peaks below about 300 Hz | 1.2 dB at 163 Hz (index 80, Q 8, fx4); hold-out fx8 0.26 and 0.20 dB at 98 and 294 Hz; fitted centres scatter (110, 134, 155, 205, 246 Hz for nominal 100, 126, 158, 200, 251) | Unknown: a different low-frequency law, or too few harmonics (every 32.7 Hz) inside a narrow band to fit it | Unresolved; next: the same settings at several notes | fx4, fx6, fx8 two-band probes; `gate_peq.py … two` | `effects/TwoBandEq.h` (peak), `converter` (none yet) |
| 2 | Master EQ peaks at indices 14–22 (printed 100–225 Hz) | Fitted centres within ±10% of printed (fx11b); hold-outs fx8 up to 2.09 dB and fx12 up to 4.53 dB, at 65–330 Hz | As #1 | Unresolved | fx5, fx8, fx11b; `check_master.py`, `fit_scale.py fx11b` | `effects/PartAndMasterEq.h`, `converter/FrequencyScales.h` (`masterPrintedHz`) |
| 3 | Master EQ high shelf at 500 Hz | 0.14 dB (+12 dB) and 0.80 dB (−12 dB) | Unknown; at 2–16 kHz it fits to 0.026 dB | Unresolved | fx5 `m5sh_f28_*`; `check_master.py` | `masterHighShelf` |
| 4 | Master EQ high shelf: √2 or 10^(\|g\|/80) | Equal at ±12 dB, the only gains probed alone | The probes cannot tell them apart | Unresolved; √2 shipped | fx5, fx8 | `masterHighShelf` |
| 5 | Three-band EQ mid band above about 4 kHz with a Q far from 2.6 | Up to 10.3 dB (fx4/fx6, index 241, Q 0.7 and 10.3); 2.9 dB at index 202, Q 10.3; hold-out fx12 1.96 dB (5 kHz, Q 1.0) | Its centre moves with Q near Nyquist (index 239: 12.3 kHz at Q 2.6, 11.0 kHz at Q 10.3); the law is two-dimensional and not fitted | Unresolved | fx4, fx6, fx11a; `gate_peq.py … three` | `midWidening`, `converter/FrequencyScales.h` |
| 6 | Three-band mid at its top indices at Q 2.6 | 0.63 dB at index 241 (18.3 kHz), 0.33 at 239 | The bandwidth law (2.5% RMS in Q) | Unresolved | fx11a | `midWidening` |
| 7 | Three-band low shelf at index 182 | 0.32 dB | Unknown | Unresolved | fx11a `lo_i182` | `shelf` |
| 8 | Three-band high-shelf scale measured at 17 indices only | Formula within 0.66% of those; corner rule (gain-independent), found on fx12, confirmed on the fresh hold-out fx16: shelf-only settings 0.0087–0.053 dB | A full table needs a sweep of 125 indices | Formula shipped; rule held on a fresh hold-out | fx6, fx11a, fx12, fx16 `p3ho02/05/08/11` | `threeBandHighShelfHalfGainHz`, `threeBandHighShelfCornerHz` |
| 9 | Three-band mid: the centre moves with Q | Q-index sweep fx17: the index law holds within 1.7% at 507 Hz; the centre moves +1.9 / +5.3 / +12.1% at index 0 and −0.5 / −1.4 / −2.8% at index 31 at 507 / 1,286 / 2,660 Hz, so the shipped law misses by up to 1.9 dB at 2.7 kHz; hold-out fx16 misses of 0.23–0.64 dB come from this | Not the Q law: a centre shift that grows with frequency and away from Q 2.6; a first joint law reaches 0.57 dB | Unresolved; next: the shift's law (same sweep at more centres) | fx4, fx6, fx16 `p3ho*`, fx17 `q3_*`; `fit_q3.py` | `midWidening`, converter |
| 10 | Fitted EQs at 48, 88.2, 96 and 192 kHz | Within the gate (accepted, decision 18) everywhere checked: worst 0.023 dB within 20 dB of the peak and 0.29 dB below over the plugin tests' 39 settings, 20 Hz–20 kHz; 0.036 / 0.35 dB over 2,400 random settings (stress run, matcher alone). Above 20 kHz (where the reference has no response) the matched EQs may exceed the design's peak up to 22.05 kHz by up to 7.8 dB (stress run; 1.3 dB in the tests) | The reference is a 44.1 kHz bilinear design; a biquad at another rate cannot follow its warping, so each section is replaced by up to three fitted biquads, and above 20 kHz only a loose bound is kept | In-band: met (decision 13). Above 20 kHz: deliberate, sanity bound 10 dB in the tests (proposed, set after seeing 7.8 dB) | Plugin test "Fitted EQs keep their 44.1 kHz response at 48, 88.2, 96 and 192 kHz"; stress program (scratch, not committed) | `effects/RateMatch.h`, `MatchedCascade` in `effects/FiveBandEq.h` |
| 11 | Reverb 1 keeps upstream's buffer sizes (2.2 MB per context) | Not a difference from the reference; a stock mode | Its output depends on the buffer size (predelay beyond the mask) | Deliberate (stock exact) | `tests/StockRender.cpp` | `include/sst/effects/Reverb1.h` |
| 12 | Hall reverb renders: level differs between identical renders | 11.6 dB between two identical renders (fx10); onsets differ by up to 70 ms | Found (fx15): the source, not the reverb. The dry click itself varies by 26 dB across four identical renders (−36.7, −31.8, −10.7, −26.6 dB): the AN-X oscillator runs free, so its phase under the very short envelope differs at each note. The click's shape changes too, so subtracting a click template does not separate the reverb, and fx15's levels and onsets are not usable either | Measurement fix still open: oscillator phase reset (fx16) did not make the click repeatable (energies −0.7, −7.6, −8.7 dB over three renders); next a source without an oscillator under a short envelope | fx10, fx15, fx16 `click*`; `fit_rev.py`, `fit_rev2.py`, `fit_fx16.py click` | none (measurement) |
| 13 | (Superseded by 23 for the built Hall A.) Hall reverb measurements; displayed time against measured decay; level and onset now measured (fx17: wet −2.89 / −3.23 dB re the dry click at factory settings, onset = initial delay + 48.4 ms) | Mid-band (1 kHz) T60 for reverb time indices 0 / 8 / 16 / 24 / 32 / 40 / 48 / 56 / 64 / 69 at room size 29: 0.85 / 0.90 / 1.75 / 2.42 / 2.9 / 3.53 / 4.07 / 4.82 / 6.02 / 8.39 s (fx16; displayed 0.3 / … / 18.0 s); room size 0 / 8 / 15 / 22 / 31 at time 32: 0.84 / 1.25 / 1.76 / 2.37 / 3.06 s; time and room do not scale separately (time 8: room 0 / 15 give 0.22 / 0.66 s; time 56: 1.94 / 3.21 s); high ratio 0.1 shortens 4 kHz to about 0.75 of the low bands (fx16) and 8 kHz to about 0.4 (fx10). Levels and onsets not measured (known difference 12) | Reference behaviour | To be fitted when the reverb mode is built | fx10, fx15, fx16 `rv_*`; `fit_fx16.py rev` | none yet |
| 14 | Types not built | 92 of the 104 types (the five-band EQ, the cross delay, the tempo cross delay, the tempo stereo and tempo mono delays, the L/R and L/C/R delays, the hall reverb, the circuit-style compressor, the two-rotor rotary, the auto pan and the tremolo are built), plus the Part three-band and two-band EQs and the Master EQ, which are built | Not reached yet | Planned (section 12) | — | — |
| 15 | (Superseded by 26–29 for the built Circuit Compressor.) Compressor measurements fx7–fx23 | Static curve (fx7, fx13), steps (fx21–fx23): explained by a makeup gain set by the settings (section 15) | Reference behaviour | Built 2026-11-05 | fx7, fx13, fx21–fx23 | `effects/CircuitCompressor.h` |
| 16 | Tempo cross delay at tempos other than 120 BPM | Not measured | The recorder's host is fixed at 120 BPM | The plugin follows the transport tempo by the note values (assumed) | — | `effects/TempoCrossDelay.h` |
| 17 | Stock FloatyDelay and Phaser are not repeatable; Phaser and RotarySpeaker read uninitialized memory | Not a difference from the reference; stock behaviour | Clock-seeded generators; missing initialisation | Decision 15; not yet modes | `tests/StockRender.cpp` | upstream headers |
| 18 | Fitted EQs below 44.1 kHz | Not gated; the matcher fits up to 0.45 of the rate | Not covered, by Jacob's decision (decision 21) | Deliberate | — | `effects/RateMatch.h` |
| 19 | Parameter jumps in the EQs click at 44.1 kHz | Switching a five-band EQ band off: −23 dBFS above 8 kHz within 50 ms (plugin test "Rate-matched EQs change their number of sections without clicks", printed) | The coefficients change within a block; the reference's smoothing is not measured | Unresolved; needs a probe that moves a parameter while a tone plays | — | `effects/FiveBandEq.h` |
| 20 | Tempo cross delay: some lengths with triplet or dotted values and lag | Was 1–2 samples (≤ 0.05 ms) per crossing with round(T·44100) + round(lag·44.1) − 1: Whole/3 2 samples long (fx16), 8th/3 with lag −17 2, 32nd dotted 1–2 | The reference cuts the note to 0.1 ms and uses the family's time law, floor(t × 18063 / 4096) | Resolved 2026-11-12: every first echo of d_t5_15_fb, dho04, dho06 and dho07 at the reference's own sample (plugin test "Tempo cross delay: the measured routing …", section "Triplet and dotted notes …") | fx16 `d_t5_15_fb`, fx18 `dho04/06/07`; `lag.py` of the review batch | `lineSeconds`, `tenthsSamples` |
| 21 | Cross delay / tempo cross delay Mode 2 loop gain | The loop through the squared leg is 1.0073 times fb² in the reference (0.063 dB per loop), the same at v = 40, −37 and −20 and in both types (02 10: fx16, fx18, review `tcd_m2l_n37/n20`; 02 00: cross_delay_extra `cd_m2*`; per-loop ratios 1.00724–1.00733) | A constant factor not found in the law; the sign is resolved: at negative feedback the squared leg inverts once per loop in both types, fb·\|fb\| (shipped 2026-11-12) | Magnitude unresolved (inside the gate) | fx16 `d_in3/4`, fx18 `dho00/01/02`, cross_delay_extra `cd_m2*`, review cross_delay `tcd_m2l_*` | `CrossCore::processBlock` |
| 22 | Cross delay / tempo cross delay L&R input: mixed or one line per side | For 02 00: resolved, one line per side — an L&R render's echoes sit 0.00 dB off "L→A, R→B"; a summed feed would be +6.0 dB higher (at least for a centre-panned source) | For the tempo type: panned probes are panned after the insertion | Shipped as one line per side | qwen-cross-delay-2026-11-11 `cd_input_r/lr`, fx17 `d_panL_*` | `TempoCrossDelay::processBlock` |
| 23 | Hall A decay times | Hall A 0.5.0 (2026-11-06), fresh hold-out fx30 (16 random settings): 31 / 44 / 62 / 69 / 69 / 38 / 44% of renders within 10% per band from 125 Hz to 8 kHz, median errors 6.2-17.4%; on its 197 fitting renders 41-68%. Worst: the smallest rooms (fx30 `rho08`, size 0, 24-41% short; `rho15`, size 1, 18-70% long), decay settings below 30, 125 Hz under a high HPF, 4-8 kHz at short times. The plugin test's impulse and the renders' click give different 8 kHz decays at size 31 (r_t24_s31: -15% against -8% before size 31's tank all-pass scale was refitted). At other rates the decays stay within 4.0% and the band energies within 1.23 dB of 44.1 kHz's (plugin test) | The structure as built (this design's own delays, three fitted room-size scales, four early reflections) does not reach the reference's band decays; at short times the decay setting's early envelope (fx29) sets the decay, and the model's four reflections do not follow it | Unresolved; next: the early pattern's gains and density by decay setting (fx29's diffusion-0 renders), and the smallest rooms' delays | fx19, fx24, fx27, fx28, fx29, fx30; scratch `ev.py`, `holdout.py` | `effects/HallReverb.h` (`fitted`) |
| 24 | (Resolved 2026-11-06, measurement.) Hall reverb: a burst at 42–53 ms in some reference renders | Up to -7 dB of the tail's peak (fx19 `r_h1`) | The dry click, not the reverb: the residual is identical in L and R (correlation 1.000) and is the click's last 10 ms, cut short or prolonged where the note ended a block apart from the dry render | Resolved: onsets now use `fx/reverb.py` `side_onset_ms`, which the centred residual does not reach | fx19 `r_h1`, `r_h8`, `r_base2`; fx24 | — |
| 25 | Hall A's early reflections and echo density | Four reflections per side on a law measured from diffusion-0 renders (the first's law, from sizes 0 and 29, predicts size 15 within 0.5 ms; gains not fitted except a common level and the decay setting's dB); the first reflection is absent at the shortest reverb time, as in the reference. fx29 shows the decay setting shaping the early envelope from falling (decay 0) to flat for about 200 ms (63); the model only raises its later two reflections, so at decay settings below 30 it is 1.7-3.4 dB too loud and up to 37% short at short times (fx29, fitting renders). Echo density not compared; no modulation (as the reference) | The reflections' gains and the tank's first arrivals were not fitted to the renders' early part; a gain on the first two reflections by decay setting was tried and gained under 2 points per band | Partly resolved | fx19 `r_d0*`, fx27 `g_er_*`, fx29 `q_er_*`, `q_t*`; scratch `taps.py` | `effects/HallReverb.h` (`erGain`, `erFixedMs`, `erScaledMs`, `decayDb`) |
| 26 | (Resolved 2026-11-06.) Circuit compressor compressing at ratio 2, and with harmonic-rich sources | Fresh hold-out fx26 within 0.154 dB RMS, every 5 ms window within 0.29 dB (was fx25 1.88 dB RMS, peaks 7.4 dB) | The structure was not modelled; rebuilt (section 15) | Resolved | fx25, fx26; `ours.py` | `effects/CircuitCompressor.h` |
| 27 | (Resolved 2026-11-06.) Circuit compressor at input settings of 25 or less | Inputs 8-20 within 0.06 dB (plugin test) | The input level is held to steps of 1/16384 | Resolved | fx13 `st_*_in8`-`in25` | `heldLevel` |
| 28 | (Resolved 2026-11-06.) Circuit compressor near full scale at ratio 2 | fx7 `st_r0_in200`: -3.29 dBFS RMS against -3.30, peak 0.810 | A saturating curve and staged clipping | Resolved | fx7 `st_r0_in180`-`in200` | `saturate`, `processBlock` |
| 29 | Circuit compressor has no side-chain input | The reference takes a side-chain source (parts, A/D, master); its side-chain level changed nothing without a source (fx23) | Not built in the first version; the plugin's port layout will change when it is | Deliberate for now | fx23 `s_sc127` | `OscairnEffects.cpp` (ports) |
| 30 | (Mostly resolved 2026-11-06.) Hall A level at low and high ratios | Was too loud by 4.6-5.0 dB at high ratio 0.1 (fx19 `r_h1`, fx27 `g_h1_*`). Now the 26 high-ratio renders of fx19 and fx27 are within 1.5 dB (sd 0.29 dB), and fx30's energy is 12 of 16 within 1.5 dB (fx28 was 7 of 16). Left: this design's tank recirculates within the first 300 ms, so its per-pass loss begins earlier than the reference's: over 12-300 ms after the onset it takes 0.3-0.6 dB more than the reference off 1-4 kHz at high ratio 0.1 (12-150 ms: within 0.52 dB) | The reference low-passes the whole input once by the high ratio (cutoffs fitted to the renders' first 300 ms) and shelves it once by the low ratio, corner 800 Hz nearest DC; the model's in-loop filters only acted inside the tank. The rest: this design's shorter loop | Fixed filters resolved; the earlier per-pass loss deliberate for now (needs the reference's loop lengths) | fx19 `r_h*`, `r_l*`, fx27 `g_h*`; scratch `win.py`, `hrpre.py`, `lrpre.py`, `refnb.py`; plugin test "the ratios' fixed filters act on the whole reverb from its first reflection on" | `effects/HallReverb.h` (`pre`, `inHighHz`, `shelf`) |
| 31 | Tempo cross delay at its shortest times at rates other than 44.1 kHz | A time that rounds below 33 samples at the running rate (a 1/32 triplet or a shorter value with a negative lag: below 0.69 ms at 48 kHz, 0.17 ms at 192 kHz) plays at 33 samples; at 44.1 kHz every length down to one sample is exact | The damping filter's 32-sample delay comes off the line, which is held at a sample so the fractional all-pass stays stable (before 2026-11-06 it was not: a line below a sample put the all-pass's pole outside the unit circle and the output grew without bound) | Deliberate | Plugin test "Tempo cross delay stays stable at its shortest time at every rate" | `Line::setLength` |
| 32 | Rotary A: the horn's second path | At a mic angle above 0 the reference's horn amplitude dips by up to 17 dB within a rotation, more often at higher notes, and its static (stopped) response has a comb; in fast mode its L/R pitch phase is 108-150 degrees at the default angle (180 in the plugin), and its AM keeps growing with depth past the slow-mode saturation | A second horn path whose delay differs from the main tap's by the swing (at angle 0 the comb vanishes, as the taps coincide); two-tap fits with a shared or a crossed second tap reached 0.34-0.53 of the envelope and were dropped | Unresolved; the horn is one tap per channel | first batch r_slow_n88-127, r_fast_*; batch b c_fast_d*; scratch `fit2.py`-`fit5.py`, `suite.py` | `effects/RotarySpeaker.h` (`centreTap` for the horn is 0) |
| 33 | Rotary A: the rotor at high depth and small mic angles | Hold-outs ho3-ho5 (depth 97-124, angle 27-96 degrees): pitch deviation +13 to +20%, AM fundamental +1.7 to +3.4 dB; at the default depth the mic sweep is within 0.5 dB (180 degrees) to 2.0 dB (0 degrees) | The reference's pitch deviation grows less than in proportion above about 64 (0.1095 cents per hertz per step at 127 against 0.120 at 4) and its AM interacts with the swing; the centre tap's AM ratio (1.42) is the envelope fit's, which keeps the default angle's phases (a lower ratio, 1.1-1.3, trades 0.5-1.5 dB at angle 0 for 4-15 degrees of phase at 180) | Unresolved | c_ho3-5, b_r_slow_dr96/127, d_mic*; `suite.py` | `fitted.pitchMsFull`, `centreAmRatio` |
| 34 | (Resolved 2026-11-11.) Rotary A: shelves | Batch c: within 0.08 dB at Jacob's settings, 0.19 dB on the hold-outs, 0.31 dB at 32 Hz | The shelves act on the rotor's and the horn's bands, not the output; batch b's +12 dB probes clipped | Resolved | batch c `e_*`; `eqc.py` | `RotarySpeaker::shelf` |
| 35 | (Resolved 2026-11-11.) Rotary A: shelves at other rates | Within 0.043 dB of 44.1 kHz's static response at 48-192 kHz | Rate-matched replacement of the 44.1 kHz design | Resolved | `rategate.py`; plugin test | `shelfMatch` |
| 36 | Rotary A: the horn's pitch deviation in fast mode by depth | -22 to -12% at depths 16-40, +17% at 96 (c_fast_d*), against the reference's measure | The reference's horn deviation is not proportional to depth in fast mode as measured (its second path, 32, and the analysis' bandwidth at large deviations); the plugin is calibrated at the default depth | Unresolved | c_fast_d*; `suite.py` | `fitted.pitchMsFull` |
| 37 | Rotary A: ramps' fine structure | Rotor slowing slope -7.4 to +7.9% in four of seven ramps (both signs; speeding up within 2.6%); horn t64 slowing +5.2% | The plugin ramps continuously per block; whether the reference changes speed in steps within a ramp is not resolved by the tracker, whose fit of a slowing ramp scatters | Unresolved | first batch r_ramp_*, b_r_ramp_hold; batch b c_ramp_ho*; `cmp_ramp.py` | `RotarySpeaker::processBlock` |
| 38 | Rotary A: stereo input | The plugin sums its input to mono before the crossover | Every probe's source is centred (the part's pan comes after its insertions), so a stereo input was never measured | Unmeasured | — | `RotarySpeaker::processBlock` |
| 39 | Rotary A: delay against the dry path | The reference's output at depth 0 aligns with its dry path within 3 samples (c_lat_rot, rotors stopped); the plugin's taps sit at the larger swing plus a sample (0.82 ms at the default depth, up to 2.1 ms at 100 %) on top of its reported 16-sample block | The reference's tap centre with modulation on is not measured | Unresolved | batch b c_lat_* | `centreTarget` |
| 40 | Rotary A: rotor angles at activation | The plugin starts both rotors at angle 0; with a speed of 0 the stopped angle sets the static response | Not measured | Unmeasured | first batch r_static* (stopped from the start) | `RotarySpeaker::initialize` |
| 41 | Rotary A: the reference's output clip | The reference clips each channel after the taps at 2.51 times a full-scale dry signal's level there (about +8 dB; batch c `e_jacob_hot_vol40`, flat tops at ±0.388 at part volume 40); the plugin does not clip | Not modelled | Proposed for Jacob (decision 28): a hard clip at that level; not added | batch c `e_jacob_hot_vol40`, `e_dry_two_hot_vol40`; `task1.py` | `RotarySpeaker::processBlock` |
| 42 | Rotary A: residuals measured on clipped recordings | AM fundamentals in the steady table (r_mic, r_rate, c_mic, c_ho, c_jacob) are 0.1-2.0 dB low in the reference from clipping; rates, pitch deviations and phases unaffected | Probes too hot (two tones at velocity 100-127) | Re-measured clean for the probes listed in section 15; the rest not repeated | batch c `e_*` against batch a/b; `task1.py` | — |
| 43 | Cross, L/R and L/C/R Delay (02 00, 02 40, 02 50) balance: the normalisation | Was 3.01 dB at D=W: the reference keeps the near side at unity (dry at D=W level with the dry-only render, 0.000 dB; the wet-only echo level with it, −0.002 dB), the plugin scaled the pair to equal power as the tempo types are (−3.010 dB there) | The in-render ratios (40·log10((63 − \|v\|) / 63) dB, ten positions within 0.01 dB) could not tell the two apart (known difference 12) | Resolved 2026-11-12 with a steady saw: `msBalance` for the ms types, `tempoBalance` for the tempo types | delay-eq-2026-11-12 `*_dry`, `*_dw`, `*_wet`, `none`; `analyse.py`; cross_delay_extra `cd_mix_*`, review cross_delay `cd_mix_*`, `tcd_mix_*`, fx17 `d_bal*` | `msBalance`, `tempoBalance` |
| 44 | Delay modes: the EQ parameters | Not built in any delay mode, but they act: on 02 00, 02 20, 02 28, 02 40 and 02 50 the EQ shelves the whole output, dry and wet alike, and nothing in the loop. With the EQ on, echo-to-click and echo-to-echo ratios inside a render stay within the method's noise (≤ 0.03 dB per octave band up to 4 kHz), while the click's own spectrum follows the EQ against same-batch renders without it: high shelf 16.0 kHz −12 dB gives −7.1 to −8.3 dB at 16 kHz and −2.5 to −4.0 dB at 8 kHz (02 00 cd_eq, 02 20 tm_eq, 02 28 ts_eq, 02 40 lr_eq_dw, 02 50 lcr_r_eqdw), against a scatter of 0.6–1.5 dB there; 02 20's low shelf 2.0 kHz +12 dB raises 125 Hz–1 kHz by 10–13 dB against 16 kHz (tmx_eqlo). Below 1 kHz the click's own scatter (2–5 dB) hides a low shelf at 32 Hz | Decision 23's evidence (fx16) and the first passes compared echoes or loops with the click inside one render, which an EQ on the whole output leaves unchanged; the shelves' frequency and gain laws are not measured | Open for Jacob (decision 23): build an output EQ per type after a fitting batch (a tone or noise source, not the click), or keep it out | qwen-cross-delay-2026-11-11 `cd_eq`; tempo_mono `tm_eq`, review `tmx_eqlo/eqhi/eqflat`; tempo_stereo `ts_eq`; delay_lr `lr_eq`, review `lr_eq_dw` vs `lr_pair_dw`; delay_lcr `lcr_eq`, review `lcr_r_eqdw`, `lcr_r_eqwet` | none (parameters not built) |
| 45 | Tempo Stereo and Tempo Mono Delay (02 28, 02 20), and Tempo Cross Delay (02 10): the longest lines | 02 28 halves the note time when the longer line would reach a threshold between 131,063 and 131,106 samples (ts_t19_lag36/37: 131,062 whole, 131,106 halved to 64,958); 02 10 halves each line at the same length (tcd_t17/18/19: 88,198 and 110,247 whole, 132,297 halved to 66,148). The plugin halves at 131,072 (2^17), as often as it takes. 02 20 does not halve at 132,297; the plugin's limit for its one line is 262,144, twice the stereo type's | Whether the notes halve more than once, and 02 20's own limit, are reached only at host tempos under about 60 BPM; the recorder's host runs at 120 BPM | Threshold measured to 44 samples; repeated halving and 02 20's limit inferred (2026-11-12; was a cap at 65,535 for 02 10 and a clamp at 132,297 for 02 20) | review batches `tempo_stereo` `rs_t19*`, `tempo_mono` `tmx_t18`, `tmx_tmax`; delay-eq-2026-11-12 `ts_t19_lag36/37`, `tcd_t17/18/19` | `halvedNote`, `lineLimit`, `monoLimit` |
| 46 | Cross, L/R and L/C/R Delay (02 00, 02 40, 02 50): the ms time law's rounding | Was one sample (≤ 0.023 ms) where ms × 44.1 is not whole: round(ms × 44.1) − 1 against the reference's floor(ms × 10 × c), c between 4.40990 and 4.40994 (02 40: 0.1 / 11.3 / 29.1 / 57.9 ms are 4 / 498 / 1,283 / 2,553 samples; 1,111.1 / 1,414.1 / 1,555.5 / 1,638.3 ms are 48,998 / 62,360 / 68,596 / 72,247) | One time law for the family: floor(t × 18063 / 4096) for t tenths of a ms, the one 12-bit fixed-point factor inside both brackets (the tempo types' 4.409908–4.409915 too) | Resolved 2026-11-12: all six modes use `tenthsSamples`; every measured time of the six types fits it | delay_lr `lr_tmax`, `lr_hold_grid`; review batches `lr_law_a/b`, `delay_lcr`; delay_lcr `lcr_tmax`; qwen-cross-delay-2026-11-11 `cd_time_max` | `tenthsSamples`, `msLineSeconds` |
| 47 | L/C/R and Tempo Mono Delay (02 50, 02 20): the line's feed with a true stereo input | Both modes feed their one line the mean (l + r) / 2. Every probe drives the effect with the same click on both channels (the part's pan sits after the insertions, known difference 22), so the mean, a left-only and a right-only feed give the same render; a summed feed is ruled out (it would put the echo 6 dB over the click at D=W, measured 0.00 dB in lcr_r_dw, lcr_hold_mix and tm_base) | The rig cannot deliver a stereo effect input | Unresolved; needs a source with different content on the two channels before the effect | review delay_lcr `lcr_r_dw`; delay_lcr `lcr_hold_mix`; tempo_mono `tm_base` | `LcrDelay::processBlock`, `TempoMonoDelay::processBlock` |
| 48 | Auto Pan, Tremolo: the LFO's phase at activation | The reference's LFO starts when the performance loads (in these probes about 0.45 s before the note; Tremolo then 214 degrees past its peak); the plugin's starts at its peak when the mode is activated | Not measured beyond the probes' onsets | Unmeasured | autopan_b, trem_b phases at three speeds | `initialize` |
| 49 | Auto Pan, Tremolo: the modulated delay's centre | The reference's delay against its dry path is not measured (the types have no dry/wet mix); the plugin's tap is at least one sample, and with F/R or PM at 0 the input passes with no delay | Unmeasured | Unmeasured | — | `processBlock` |
| 50 | The insertion modes: EQ mid band at low centres | 315 Hz (index 24): fitted centre 310.5-312.5 Hz (-0.8 to -1.5%), so 1.25 dB at 330 Hz at width 12 and +12 dB, 0.19 dB at width 2; 100 Hz (index 14) at width 5: fitted centre about 110 Hz, 2.70 dB at 110 Hz; 630 Hz +0.3% (0.075 dB); 1.6, 4 and 10 kHz exact | The converter uses the printed frequency; the reference's centre at these indices differs, perhaps with the width (as known difference 9) | Unresolved; next: a mid sweep over index and width | autopan_c eq7, eq9, autopan_d mid*, trem_c eq9; `eqfit.py` | converter (table 3) |
| 51 | Auto Pan: direction L/R at its steepest waves | The steepness per wave is fitted from 9 ms analysis windows; at waves 24-28 the edges take a few milliseconds, and wave 28's steepness fits about as well anywhere from 34 to 65; pointwise gain errors up to 6 dB on the edges, rms within 1.6% of full gain | The analysis' time resolution | Partly resolved | autopan_d lr_w24-28; `lrfit2.py` | `lrSteepness` |
| 52 | Auto Pan: isolated spikes in the reference's turn directions | ho2 (R turn, F/R 70, L/R 112, high shelf +10 dB): single 20-sample windows off by up to 0.22 of full gain, 1.1% of the windows | Unknown; the flat-EQ F/R probes show none, and their noise between harmonics matches the plugin's within 0.5 dB | Unresolved | autopan_c ho2; `cmp.py` | — |
| 53 | Auto Pan, Tremolo: parameter changes | Applied from the next block without smoothing (a depth, speed or direction change steps the gain or the delay); the reference's smoothing is not measured | Unmeasured | Unmeasured | — | `processBlock` |
| 54 | Auto Pan, Tremolo: the reference's output clip | Two probes with an EQ band at +12 dB clipped at full scale at the instrument's output (autopan_b ap_holdout3, trem_b tr_eq); whether the effect itself clips (as Rotary A's does, known difference 41) is not measured; the plugin does not clip | Unmeasured | Unmeasured | ap_holdout3, tr_eq | — |

## 17. Oscairn porting checklist (decision 22)

Every item of `oscairn/docs/plugin-porting.md` (read at Oscairn `f0d8f68`) that applies to an
effect, with its status and what proves it. "Host check" is Oscairn's `oscairn-plugin-check`, built
from a clone of Oscairn (`fbc8e12`) outside both repositories, run on this branch's
`oscairn-effects.clap` at 48 kHz for Reverb (both modes), every EQ mode with a gain set, and the
delay with feedback, in the engine and with `OSCAIRN_PLUGIN_WORKER` set: all passed (2026-11-02).
Again on 2026-11-05, built from a clone of the local Oscairn checkout at `72afff2` (pulling from
GitHub failed): Dynamics (the circuit compressor compressing), Reverb (Hall A, Reverb 1, and Reverb 2
by default), the five-band and three-band EQs with a gain set, and the delay, in the engine and in
`oscairn-plugin-worker`: all passed. On 2026-11-06, built from a clone of the local Oscairn checkout at
`2c74d3e` (its main; fetching GitHub failed again): Reverb with Hall A rebuilt (0.4.0) in the engine
and in `oscairn-plugin-worker`, and Dynamics: all passed. Oscairn FX Tremolo & Rotary (2026-11-11) has not been through the host check; its own tests and
the privacy check pass.

| Item (plugin-porting.md) | Status | Proof |
| --- | --- | --- |
| A `clap_entry` with a plugin factory; `init`, `destroy` and every function filled in | Met | Plugin test "Every plugin passes the host's load checks"; host check "loads in a part" |
| Audio outputs; at most 64 channels; no port without channels | Met (one stereo input, one stereo output) | Load-checks test |
| Note ports | Not applicable (effects) | — |
| Params extension complete; unique IDs; finite ranges; defaults in range; at most 32,768 | Met | Load-checks test |
| State and audio-ports extensions complete | Met | Load-checks test |
| `oscairn.memory` complete; the declaration fits the budget | Met (draft vendored from oscairn/oscairn-clap `7b5c4e0`) | "Declared memory equals what activation holds…", "The memory estimate follows another state load…" for every plugin and mode at 44.1–192 kHz |
| Activates at the host's rate and maximum block | Met: 8–768 kHz, any block | "Output does not depend on the host's block size", the rate tests; host check at 48 kHz, 256-frame blocks |
| Audio port layout constant while loaded | Met | "Audio ports stay the same whatever the mode or state" (fails with a mode-dependent port count) |
| Build for macOS arm64 and Linux arm64 | Met in CI (macOS arm64, Linux ARM64, Linux x86_64); no packaged bundle yet | CI jobs |
| State deterministic, architecture-independent, saves right after a load | Met (text, 17 digits, locale-independent) | "State round-trips…": a save straight after a load gives the same bytes; the same tests pass on arm64 and x86_64 in CI (no test compares bytes across machines) |
| Ports, note dialect and metadata the same whatever state | Met for ports and IDs; the hidden flag follows the mode, as recommended for modes | "Audio ports stay the same…", "Editor hints…" |
| Every sound-relevant setting a parameter, with name, module path, value text, stepped and hidden flags | Met: one module per mode, value text from `ParamMetaData`, stepped for integer parameters, other modes hidden | "Editor hints…" |
| `oscairn.ui` hints and roles | Met: Mode first, the mode's parameters in order, `mix` on a parameter named Mix (Reverb 1 and 2, the delay), or on Output Level in a mode without one (five-band and two-band EQs; Oscairn `cf540a9` otherwise takes the first continuous parameter) | "Editor hints…" (fails without the hidden flag or the rescan, or without the Output Level role) |
| Render mode (`clap.render`, Oscairn `9bfc302`) | Met: set once after init, before state load and activation; the first set, or activation without one, fixes the mode for the instance's lifetime; offline matches EQs in process | "The render mode is set once and kept for the instance's lifetime" (fails when a later set may change the mode) |
| A mode that changes which controls matter hides the others and rescans info | Met | "Editor hints…" |
| Real-time safety: no allocation, lock, file access or waiting in `process` | Met; the EQs' matching (decision 19) runs off the audio thread; Hall A's changes are a few first-order designs, made in process (decision 27, revised; worst call 3.7-5.6 µs at 192 kHz) | "Processing never allocates" (including EQ changes at 48 kHz); "Rate matching runs off the audio thread" (worst call 1.8–5.2 µs at 192 kHz in the runs seen); ASan/UBSan and TSan in CI |
| A second life: after `reset` (and a state load, or none) it renders as a fresh instance given that state, at once; reset allocates nothing | Met, but an EQ change still being matched when a reset comes without a load (section 15, "Reset as a fresh instance") | "Reset leaves a fresh instance at once in every mode" |
| `CLAP_PROCESS_SLEEP` when idle | Met; a delay sleeps only after its longest gap | "The tail ends, and the plugin then sleeps"; "Tempo cross delay keeps playing between its echoes" |
| `NOTE_CHOKE`, per-note volume, pitch bend, CLAP notes | Not applicable (no note input) | — |
| Effect contexts with tails (RFC 0007) | Met: up to 64 contexts per engine, `resources` with tail activity | "Effect contexts are independent and share one engine" |
| Side-chain inputs where types need them | Not yet: the circuit compressor is built without its side-chain port (known difference 29) | — |
| Presets (CLAP preset discovery) | Not implemented: no factory presets of these modes exist yet | — |
| Transport and tempo | Met: tempo from the transport at every `process`, 120 BPM without one | "Tempo cross delay: the measured routing…" (a quarter at 90 BPM from the transport) |
| Steady cost; honest version | CPU: matching off the audio thread, so a change no longer costs a block (the worker's CPU is outside the host's per-block measurement). Version: 0.1.0 whatever changes (decision 24) | Timing test above |
| Worker process: main thread, time limit, memory limit, restart | Works in a worker: the host check passes with the plugin in `oscairn-plugin-worker`. The engine's worker thread runs only for matched EQs; its stack is not in the declaration (within the 64 MiB margin) | Host check with `OSCAIRN_PLUGIN_WORKER` |
| Tested with `oscairn-plugin-check` | Met (above); the reopened renders of Reverb and the delay differ by 0.0004–0.003 RMS, which the check reports as state not saved (the outgoing Performance's tail rings into the reopened one) | Host check output |
| Production runner (`core/lv2-host/tests/production/run.py`) | Not run: its probe needs Oscairn built with LV2 | — |
| Oscairn's own tests against these plugins (offline determinism, `*-workers`) | Not run beyond the host check: they are written for Oscairn's bundled plugins | — |
| Signed builds (`oscairn-sign`) | Not done: needs Jacob's key (decision 24) | — |

Requests for the Oscairn session:

1. **`clap.render`.** Done in Oscairn `9bfc302`; the plugins keep the mode it sets (section 17). Before that, Oscairn did not call it. These plugins
   compute EQ coefficients on a worker thread in real time, so an offline render with parameter
   changes at rates other than 44.1 kHz depends on timing unless the host sets offline mode; with
   it they compute in process and render deterministically.
2. **Measurement keyed by declared size** is in place (RFC 0025); the plugins' figures changed
   (EQ contexts now 1–1.4 KB at 44.1 kHz, about 60 KB elsewhere) without a version change
   (decision 24), so old measurements under 0.1.0 are stale.
3. **Roles for effects.** Only instruments fill role banks; the EQ plugin could declare the EQ
   roles (low/mid/high gain and frequency, mid Q, output) if effects filled the EQ bank. Today an
   effect without a `mix` role puts its first parameter by priority, Mode, on the Effects knob; a
   rule that skips a stepped mode parameter there would suit category plugins.
4. **Worker threads in plugins.** Nothing in plugin-porting.md says whether a plugin may run its own
   threads in a worker process, or how their CPU is counted; these plugins start one per engine
   for EQ matching at rates other than 44.1 kHz.

## Releasing

The package version is the CMake version of the Oscairn project (`src/oscairn-clap/CMakeLists.txt`);
each plugin's own version lives in its descriptor in `src/oscairn-clap/OscairnEffects.cpp` (the
release scripts read them there). Bump what changed, commit, then tag the package version and push
the tag:

```
git tag oscairn-v0.2.0
git push origin oscairn-v0.2.0
```

The [release workflow](../.github/workflows/release.yml) builds the single-file `.clap` for each
platform (`macos-arm64`, `linux-arm64`), runs upstream's tests, ours and the privacy check, and the
device dependency checks, and creates a GitHub Release holding `oscairn-effects-<platform>.clap`,
`licenses.tar.gz` and `oscairn-release.json`. The tag version must match the package version; the
release fails if it does not. To try the workflow without a tag, run it manually
(`workflow_dispatch`) with *dry run* checked: everything runs and the files land as a workflow
artifact instead of a release. The repo-specific facts (names, ids, plugins, licences, test
commands) live in `src/oscairn-clap/release/release.conf`; the scripts there are the setBfree
fork's `oscairn/tools/release`, extended for several plugins in one descriptor file, a CLAP
pinned by CPM rather than vendored, and extra configure flags. The stock-vs-fork gate stays in `oscairn.yml`;
the release workflow does not repeat it.
