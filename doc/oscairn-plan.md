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
| Delay | 9 | 6 | 2 | 1 | |
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
| **All 104 types** | | **41** | **23** | **40** | |

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
| TEMPO CROSS DELAY | 02 10 | Var,InsA/B,A/D |  | 1426 | 787 | Delay | written | own cross delay fitted to renders (built; section 15) |
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
| Parameters | The union of all modes' parameters, each under a module path named after its mode (one editor page per mode), plus Mode. IDs are stable: `mode << 8 \| index`, Mode is 1; modes and indices are never renumbered. Ranges, defaults and value text come from each effect's `ParamMetaData`; values are in physical units (Hz, ms, dB, %), not the platform's raw numbers (decision 10). IDs, names, ranges and modules never change with state; the parameters of modes other than the selected one are flagged hidden, and the host is asked to rescan parameter info when the mode changes (plugin-porting.md; section 17) |
| State | Text, locale-independent: a header line with the category, then `id=value` lines with 17 significant digits. Unknown IDs are ignored (newer state in an older plugin), missing IDs keep defaults. History is not state |
| Block size and latency | sst-effects processes fixed blocks; the wrapper uses 16 samples and buffers one block, reported through the latency extension (16 samples, 0.33 ms at 48 kHz). Output does not depend on the host's block size |
| Tails | `tail_active` and the CLAP tail extension: active while input is non-silent or output has been above −140 dBFS within the last 100 ms, or within a mode's longest silent stretch (the delay: its longer line plus 100 ms); once inactive with silent input, `process` returns `CLAP_PROCESS_SLEEP` |
| Side-chain | Plugins with side-chain types declare a second, non-main stereo input port, "Side-chain", on every context. A mode without side-chain ignores it; a side-chain mode with nothing connected uses the main input, as the reference does when its source is Off |
| Tempo | From the CLAP transport at every `process` call; 120 BPM when absent (built with the tempo cross delay) |
| Work off the audio thread | Each engine owns a worker thread, started with the first context that needs it (decision 19): the EQs' rate matching runs there and comes back through lock-free triple buffers; the audio thread only posts and wakes. Offline (`clap.render`), the same work runs in process so renders do not depend on timing |
| Editor hints | `oscairn.ui/draft-3`: Mode first by priority, then the selected mode's parameters in their order; a parameter named Mix carries the `mix` role |
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
    (`memory.h`, vendored from Oscairn `17c43f6`, refreshed from `d7ac261`): `estimate(rate, max_frames)` after state
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
24. **Open: plugin version and signing.** The plugins reported version 0.1.0 whatever changed;
    Oscairn keys its measurements by ID and version and keeps a store build per version, so each
    published build should get a new version, and be signed (`oscairn-sign`) with a key of Jacob's.
    Since 2026-11-05 a plugin's version is bumped when its sound or memory changes (Reverb 0.2.0
    with Hall A); signing is still open.
25. **Open: the circuit-style compressor.** Its steps (fx21–fx23, section 15) show a gain that
    depends on the input level, attack and release settings but not on the signal below a level,
    which no detector-and-gain-law model fitted here reproduces. Options: read the reference's
    implementation for the structure (allowed, not copied), or probe further (input level 100–200
    in fine steps, a slow amplitude ramp instead of steps, a burst after silence). Nothing is built.
26. **Open: Hall A ships with a hold-out that fails its decay gate** (section 15, "Hall reverb"):
    level and onset hold, decay times per band do not generalise to random settings. Keep it as a
    first version (the alternative is to hold it back until a further grid over low ratio,
    diffusion and HPF is measured).
27. **Open: Hall A designs its coefficients in process**, at a parameter change (eight lines × seven
    sections, three 7 × 7 solves), as the delay designs its damping filter, rather than on the
    engine's worker as the EQs do. Not timed yet; moving it to the worker is the alternative.

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
| Tempo Cross Delay | 44.1 / 48 / 96 kHz | 120 | (new) | 527,944 / 574,312 / 1,144,960 | 0 | not measured |

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
| Mode 2: the input side's line receives the other's output at the feedback gain squared, the other at unity | fx16 | 0.32 dB after five loops (0.06 dB per loop; known difference 21) |
| Length round(T·44100) + round(lag·44.1) − 1 samples at 44.1 kHz, both lines, at most 65,535 | fx14, fx16, fx17; the lag's rounding chosen on fx18 | up to 2 samples per crossing for triplets and dotted values with lag (known difference 20) |
| High damping g = 5(1 − r)/9 on the feedback only | fx16, fx17 | 0.01–0.02 dB |
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
than 120 BPM, the longest length (65,535 samples is from fx9's "about 1.487 s").

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
  setting, carry a short burst at 42–53 ms (unexplained; known difference 24).

### Hall reverb (built 2026-11-05)

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
| 14 | Types not built | 101 of the 104 types (the five-band EQ, the tempo cross delay and the hall reverb are built), plus the Part three-band and two-band EQs and the Master EQ, which are built | Not reached yet | Planned (section 12) | — | — |
| 15 | Compressor type not built; steps now measured (fx21–fx23, AWM2 square-LFO source) but not explained (decision 25). Earlier: fx16–fx18 steps failed (the probes' velocity sensitivity silenced the quieter note) | Static curve measured (fx7, fx13): a fixed +65.9 dB gain after the input-level law, output-level law exact, digital silence at the two lowest input levels, compression from about −14 dBFS RMS with a soft knee and effective ratios of about 3.2 / 3.7 / 4.6 for settings 2 / 4 / 20. fx16: at a fixed input the output level moves 9.2 dB with the attack setting (−11.95 dB at attack 0 to −2.71 dB at 150–200, release 0): the detector is an asymmetric follower on the rectified signal, not RMS. Time constants still not measured (the fx16 steps went to silence) | Probe design | fx21–fx23: below a level the gain does not move within a render (a 13 dB step changes it by under 0.2 dB) yet depends across renders on input level, attack and release, with the attack's effect changing sign between input 60 and 100; above it the steps compress, slower attack settings more. Unresolved (decision 25) | fx7, fx13, fx16 `c_*`, fx21 `w_*`, fx22 `l_*`/`c_*`, fx23 `s_*`; `cfit.py` | none yet |
| 16 | Tempo cross delay at tempos other than 120 BPM | Not measured | The recorder's host is fixed at 120 BPM | The plugin follows the transport tempo by the note values (assumed) | — | `effects/TempoCrossDelay.h` |
| 17 | Stock FloatyDelay and Phaser are not repeatable; Phaser and RotarySpeaker read uninitialized memory | Not a difference from the reference; stock behaviour | Clock-seeded generators; missing initialisation | Decision 15; not yet modes | `tests/StockRender.cpp` | upstream headers |
| 18 | Fitted EQs below 44.1 kHz | Not gated; the matcher fits up to 0.45 of the rate | Not covered, by Jacob's decision (decision 21) | Deliberate | — | `effects/RateMatch.h` |
| 19 | Parameter jumps in the EQs click at 44.1 kHz | Switching a five-band EQ band off: −23 dBFS above 8 kHz within 50 ms (plugin test "Rate-matched EQs change their number of sections without clicks", printed) | The coefficients change within a block; the reference's smoothing is not measured | Unresolved; needs a probe that moves a parameter while a tone plays | — | `effects/FiveBandEq.h` |
| 20 | Tempo cross delay: some lengths with triplet or dotted values and lag | 1–2 samples (≤ 0.05 ms) per crossing; Whole/3 is 2 samples short (fx16), 8th/3 with lag −17 2, 32nd dotted 1–2 | The reference's rounding of those values is not found | Unresolved | fx16 `d_t5_15_fb`, fx18 `dho04/06/07`; `gate_dly.py` | `lineSeconds` |
| 21 | Tempo cross delay Mode 2 loop gain | 0.06 dB per loop louder in the reference than the feedback gain squared (v = 40 and −37) | Unknown law; two values seen | Unresolved | fx16 `d_in3/4`, fx18 `dho00/01/02` | `TempoCrossDelay::processBlock` |
| 22 | Tempo cross delay L&R input: mixed or one line per side | Indistinguishable with a centred source | Panned probes are panned after the insertion | Shipped as one line per side; needs a stereo source before the effect | fx17 `d_panL_*` | `TempoCrossDelay::processBlock` |
| 23 | Hall A decay times on settings away from the measured grid | Fresh hold-out fx24 (12 random settings): only 25–58% of renders within 10% per band; mid bands mostly 10–30% short where the low ratio is 1.0 or more or the diffusion low; 125–250 Hz far off where the HPF removes those bands. On the fitting grid fx19: 88–97% within 10% from 250 Hz to 8 kHz. At other rates the band energies at 8 kHz differ from 44.1 kHz's by up to 1.34 dB (the bilinear filters' warping) | The model adds per-setting tables and treats low ratio, diffusion and HPF as not changing the decay, which the hold-out contradicts; room size 0 and the shortest times are a different regime | Unresolved; next: a grid over low ratio × diffusion × reverb time, and the HPF's effect on the low bands | fx19 `r_*`, fx24 `rho*`; `gate_rev.py`, `rsum.py` | `effects/HallReverb.h` |
| 24 | Hall reverb: a burst at 42–53 ms in some reference renders only | Up to −7 dB of the tail's peak (fx19 `r_h1`), absent from renders of identical settings (`r_h8` against `r_base2`) | Unknown: not repeatable, so possibly the recorder | Not modelled; onsets are measured outside 42–58 ms | fx19, fx24; `fx/reverb.py` `main_onset_ms` | — |
| 25 | Hall A's early reflections, echo density and modulation | Not measured; the mode matches decay times, level and onset only | A statistical design | Deliberate for the first version | — | `effects/HallReverb.h` |

## 17. Oscairn porting checklist (decision 22)

Every item of `oscairn/docs/plugin-porting.md` (read at Oscairn `f0d8f68`) that applies to an
effect, with its status and what proves it. "Host check" is Oscairn's `oscairn-plugin-check`, built
from a clone of Oscairn (`fbc8e12`) outside both repositories, run on this branch's
`oscairn-effects.clap` at 48 kHz for Reverb (both modes), every EQ mode with a gain set, and the
delay with feedback, in the engine and with `OSCAIRN_PLUGIN_WORKER` set: all passed (2026-11-02).

| Item (plugin-porting.md) | Status | Proof |
| --- | --- | --- |
| A `clap_entry` with a plugin factory; `init`, `destroy` and every function filled in | Met | Plugin test "Every plugin passes the host's load checks"; host check "loads in a part" |
| Audio outputs; at most 64 channels; no port without channels | Met (one stereo input, one stereo output) | Load-checks test |
| Note ports | Not applicable (effects) | — |
| Params extension complete; unique IDs; finite ranges; defaults in range; at most 32,768 | Met | Load-checks test |
| State and audio-ports extensions complete | Met | Load-checks test |
| `oscairn.memory` complete; the declaration fits the budget | Met (draft vendored from Oscairn `d7ac261`) | "Declared memory equals what activation holds…", "The memory estimate follows another state load…" for every plugin and mode at 44.1–192 kHz |
| Activates at the host's rate and maximum block | Met: 8–768 kHz, any block | "Output does not depend on the host's block size", the rate tests; host check at 48 kHz, 256-frame blocks |
| Audio port layout constant while loaded | Met | "Audio ports stay the same whatever the mode or state" (fails with a mode-dependent port count) |
| Build for macOS arm64 and Linux arm64 | Met in CI (macOS arm64, Linux ARM64, Linux x86_64); no packaged bundle yet | CI jobs |
| State deterministic, architecture-independent, saves right after a load | Met (text, 17 digits, locale-independent) | "State round-trips…": a save straight after a load gives the same bytes; the same tests pass on arm64 and x86_64 in CI (no test compares bytes across machines) |
| Ports, note dialect and metadata the same whatever state | Met for ports and IDs; the hidden flag follows the mode, as recommended for modes | "Audio ports stay the same…", "Editor hints…" |
| Every sound-relevant setting a parameter, with name, module path, value text, stepped and hidden flags | Met: one module per mode, value text from `ParamMetaData`, stepped for integer parameters, other modes hidden | "Editor hints…" |
| `oscairn.ui` hints and roles | Met: Mode first, the mode's parameters in order, `mix` on a parameter named Mix (Reverb 1 and 2, the delay), or on Output Level in a mode without one (five-band and two-band EQs; Oscairn `cf540a9` otherwise takes the first continuous parameter) | "Editor hints…" (fails without the hidden flag or the rescan, or without the Output Level role) |
| Render mode (`clap.render`, Oscairn `9bfc302`) | Met: set once after init, before state load and activation; the first set, or activation without one, fixes the mode for the instance's lifetime; offline matches EQs in process | "The render mode is set once and kept for the instance's lifetime" (fails when a later set may change the mode) |
| A mode that changes which controls matter hides the others and rescans info | Met | "Editor hints…" |
| Real-time safety: no allocation, lock, file access or waiting in `process` | Met; the EQs' matching moved off the audio thread (decision 19) | "Processing never allocates" (including EQ changes at 48 kHz); "Rate matching runs off the audio thread" (worst call 1.8–5.2 µs at 192 kHz in the runs seen); ASan/UBSan and TSan in CI |
| `CLAP_PROCESS_SLEEP` when idle | Met; a delay sleeps only after its longest gap | "The tail ends, and the plugin then sleeps"; "Tempo cross delay keeps playing between its echoes" |
| `NOTE_CHOKE`, per-note volume, pitch bend, CLAP notes | Not applicable (no note input) | — |
| Effect contexts with tails (RFC 0007) | Met: up to 64 contexts per engine, `resources` with tail activity | "Effect contexts are independent and share one engine" |
| Side-chain inputs where types need them | Not yet needed: no built type takes a side-chain (the compressor is not built) | — |
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

