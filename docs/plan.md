# Oscairn Effects: plan

## 1. Status

Package version 0.3.5 (the version measured below): seven plugins, each with its own version (Reverb 0.5.9, EQ 0.1.9,
Delay 0.2.7, Dynamics 0.3.1, Tremolo & Rotary 0.2.7, Chorus 0.2.4, Amp Chain 0.2.2). Implementation language today:
C++ for every effect type (the table in section 4). Everything in Oscairn's plugin set is to end up in Rust: the engines port after their coverage work, and new effect types are written in Rust from the start.

Reference effect types are named here by a stable key of this project's own (E-001 to E-102, one per type the
reference's factory set uses switched on) and a generic description.

## 2. Supported

The plugins, their modes and the reference effect types the converter maps onto them today. The source is
`OscairnEffects.cpp` (plugins and modes) and `effects/` (the effects) in the plugin's folder under `src/` (paths in
this document are relative to that folder); every
plugin offers a Mode parameter and each mode's parameters under a module named after the mode, and mode ids are
never reused.

| Plugin | Modes | Reference types mapped onto it |
|---|---|---|
| `org.oscairn.effects.reverb` | Reverb 1, Reverb 2, Hall A | E-001 hall reverb (first family) |
| `org.oscairn.effects.eq` | Five-band EQ, Two-band EQ, Three-band EQ, Master EQ | E-090 five-band EQ (modeled analog) |
| `org.oscairn.effects.delay` | Tempo Cross Delay, Cross Delay, Tempo Stereo Delay, L/R Delay, L/C/R Delay, Tempo Mono Delay | E-016 cross delay; E-017 tempo-synced cross delay; E-018 tempo-synced mono delay; E-019 tempo-synced stereo delay; E-021 left/right delay; E-022 left/center/right delay |
| `org.oscairn.effects.dynamics` | Circuit Compressor, Compressor A | E-058 compressor (modeled analog); E-059 compressor (classic) |
| `org.oscairn.effects.tremolo-rotary` | Rotary A, Auto Pan, Tremolo | E-041 auto pan; E-042 tremolo; E-043 rotary speaker (variant 1) |
| `org.oscairn.effects.chorus` | Chorus A, Chorus B, Dual Modulator, Chorus C, Dual Detune | E-025 chorus (first family); E-026 dual modulator; E-027 chorus (second family); E-028 wide multi-voice chorus; E-029 dual detune |
| `org.oscairn.effects.amp-chain` | Amp Chain A | E-054 amp and effects chain |

The two-band, three-band and master EQ modes carry the part EQs and the master EQ, which are not effect types.
19 of the 102 effect types used switched on in the stock set are mapped in at least one slot.

## 3. Coverage

Measured with the effects package 0.3.5 on 2026-11-17. Two sets of Performances of the reference were converted: its factory (stock) set of 3631 Performances and a personal library of 87 Performances. Neither set has a Performance with no sound-affecting drop yet (0 of 3631, 0 of 87): every Performance still needs work somewhere, in this plugin or elsewhere.

Effect types used switched on in the stock set (insertion A and B, the variation and reverb sends, the master effect,
the rotary bus and the audio-input insertion): 102. Mapped by the converter in at least one slot: 19, with 13,218
uses (each use is one switched-on slot; summed over the effect-type table's rows). Not mapped anywhere: 83, with
7,803 uses. One mapped type, E-054 amp and effects chain, is mapped for one of its variants only (19 uses); its other
variants (28 uses) are counted with the mapped types here.

The personal library has no per-type effect table in the inputs; its effect gaps are in the table below where they
also occur in the stock set, and listed separately where they do not.

### Gaps

241 gaps of the stock set belong to this plugin set: one row per slot and effect type not built, plus the
approximations. "Changes the sound" is a classification of the gap, not a listening test: a stored value away from its default that is not carried counts as sound-affecting whether or not it is heard in a given Performance. Counts are Performances and parts of the stock set that carry the gap; parts switched off or muted are not counted against their other gaps. An effect-type row counts the converted parts and Performances that use that type in that
slot; the use counts in section 4 count every stored switched-on use, including those of parts that are muted or
switched off, so the two differ.

| Gap | Performances (stock) | Parts (stock) | Changes the sound | Status |
|---|---|---|---|---|
| reverb send: E-001 hall reverb (first family) played as Hall A, a first version whose decay is off for some settings (approximated) | 1572 | - | yes | open |
| reverb send: E-003 hall reverb (second family) not built | 388 | - | yes | open |
| variation send: parameter 3 approximated: the pitch-modulation depth law is a fit within one sample | 383 | - | yes | open |
| reverb send: E-004 hall reverb (fourth family) not built | 361 | - | yes | open |
| reverb send: E-002 hall reverb (third family) not built | 313 | - | yes | open |
| master effect: E-060 multiband compressor not built | 265 | - | yes | open |
| reverb send: E-005 room reverb (first family) not built | 217 | - | yes | open |
| insertion B: parameter 3 approximated: the pitch-modulation depth law is a fit within one sample | 206 | 261 | yes | open |
| insertion A: E-046 amp simulator (variant 1) not built | 186 | 204 | yes | open |
| reverb send: E-006 room reverb (third family) not built | 178 | - | yes | open |
| reverb send: E-009 plate reverb (third family) not built | 175 | - | yes | open |
| insertion A: parameter 3 approximated: the pitch-modulation depth law is a fit within one sample | 171 | 231 | yes | open |
| insertion B: E-046 amp simulator (variant 1) not built | 151 | 169 | yes | open |
| variation send: E-037 stereo phaser (modeled analog) not built | 120 | - | yes | open |
| insertion B: E-072 ring modulator not built | 118 | 133 | yes | open |
| insertion A: E-047 amp simulator (variant 2) not built | 113 | 120 | yes | open |
| reverb send: E-008 room reverb (fourth family) not built | 105 | - | yes | open |
| insertion B: E-100 early reflections not built | 102 | 139 | yes | open |
| insertion B: E-068 lo-fi not built | 93 | 104 | yes | open |
| insertion B: E-031 flanger (modeled analog) not built | 91 | 105 | yes | open |
| variation send: E-031 flanger (modeled analog) not built | 90 | - | yes | open |
| insertion B: E-099 pitch shifter not built | 87 | 94 | yes | open |
| reverb send: E-010 plate reverb (fourth family) not built | 83 | - | yes | open |
| insertion B: E-060 multiband compressor not built | 71 | 79 | yes | open |
| insertion B: E-047 amp simulator (variant 2) not built | 69 | 86 | yes | open |
| insertion B: E-098 piano damper resonance not built | 67 | 83 | yes | open |
| insertion B: E-049 compressor into distortion into delay not built | 67 | 69 | yes | open |
| insertion B: E-092 harmonic enhancer not built | 66 | 77 | yes | open |
| reverb send: E-012 room-shape reverb not built | 65 | - | yes | open |
| insertion A: E-092 harmonic enhancer not built | 62 | 64 | yes | open |
| reverb send: E-007 room reverb (second family) not built | 61 | - | yes | open |
| insertion B: E-039 tempo-synced phaser not built | 60 | 67 | yes | open |
| insertion B: E-003 hall reverb (second family) not built | 56 | 68 | yes | open |
| insertion A: E-003 hall reverb (second family) not built | 56 | 66 | yes | open |
| reverb send: E-015 shimmer reverb not built | 56 | - | yes | open |
| insertion A: E-037 stereo phaser (modeled analog) not built | 55 | 60 | yes | open |
| insertion A: E-068 lo-fi not built | 55 | 60 | yes | open |
| insertion A: E-072 ring modulator not built | 50 | 64 | yes | open |
| insertion A: E-031 flanger (modeled analog) not built | 47 | 54 | yes | open |
| reverb send: E-011 stage reverb (second family) not built | 46 | - | yes | open |
| insertion A: E-039 tempo-synced phaser not built | 45 | 48 | yes | open |
| insertion B: E-007 room reverb (second family) not built | 43 | 49 | yes | open |
| variation send: E-036 mono phaser (modeled analog) not built | 42 | - | yes | open |
| insertion A: E-060 multiband compressor not built | 40 | 50 | yes | open |
| insertion B: E-033 flanger (classic) not built | 40 | 45 | yes | open |
| insertion A: E-048 compressor into distortion not built | 40 | 43 | yes | open |
| insertion B: E-037 stereo phaser (modeled analog) not built | 39 | 44 | yes | open |
| variation send: E-033 flanger (classic) not built | 38 | - | yes | open |
| insertion A: E-099 pitch shifter not built | 37 | 43 | yes | open |
| variation send: E-023 analog-style delay (vintage) not built | 36 | - | yes | open |
| insertion A: E-033 flanger (classic) not built | 35 | 40 | yes | open |
| variation send: E-034 tempo-synced flanger not built | 32 | - | yes | open |
| insertion B: E-014 reverse reverb not built | 31 | 35 | yes | open |
| insertion A: E-049 compressor into distortion into delay not built | 31 | 31 | yes | open |
| variation send: E-100 early reflections not built | 30 | - | yes | open |
| insertion A: E-098 piano damper resonance not built | 29 | 38 | yes | open |
| variation send: E-099 pitch shifter not built | 29 | - | yes | open |
| insertion A: E-014 reverse reverb not built | 28 | 37 | yes | open |
| insertion A: E-034 tempo-synced flanger not built | 28 | 35 | yes | open |
| insertion A: E-071 bit crusher not built | 28 | 31 | yes | open |
| insertion B: E-048 compressor into distortion not built | 28 | 31 | yes | open |
| insertion A: E-095 wave folder not built | 26 | 37 | yes | open |
| variation send: E-024 analog-style delay (modern) not built | 26 | - | yes | open |
| insertion A: E-094 vowel formant modulator not built | 25 | 30 | yes | open |
| insertion B: E-071 bit crusher not built | 25 | 29 | yes | open |
| insertion A: E-066 envelope wah (modeled analog) not built | 25 | 25 | yes | open |
| insertion B: E-011 stage reverb (second family) not built | 24 | 31 | yes | open |
| master effect: E-049 compressor into distortion into delay not built | 24 | - | yes | open |
| insertion B: E-077 rhythmic filter and gate pattern not built | 23 | 27 | yes | open |
| insertion B: E-065 auto wah (modeled analog) not built | 23 | 23 | yes | open |
| insertion A: E-030 string-ensemble chorus with tremolo not built | 21 | 34 | yes | open |
| insertion B: E-034 tempo-synced flanger not built | 21 | 21 | yes | open |
| insertion A: E-096 compact filter (modeled analog) not built | 20 | 27 | yes | open |
| insertion B: E-084 modulation multi-effect not built | 20 | 23 | yes | open |
| insertion A: E-065 auto wah (modeled analog) not built | 20 | 20 | yes | open |
| insertion A: E-050 amp simulator: clean combo (variant 1) not built | 19 | 33 | yes | open |
| insertion B: E-013 gated reverb not built | 19 | 23 | yes | open |
| insertion B: E-074 envelope-following ring modulator not built | 19 | 22 | yes | open |
| insertion A: E-036 mono phaser (modeled analog) not built | 19 | 21 | yes | open |
| insertion A: E-061 downward compressor not built | 18 | 26 | yes | open |
| insertion A: E-011 stage reverb (second family) not built | 18 | 19 | yes | open |
| variation send: E-014 reverse reverb not built | 18 | - | yes | open |
| insertion B: E-044 rotary speaker (variant 2) not built | 17 | 31 | yes | open |
| insertion B: E-096 compact filter (modeled analog) not built | 17 | 24 | yes | open |
| insertion A: E-063 parallel compressor not built | 17 | 23 | yes | open |
| insertion B: E-020 delay with a controllable time not built | 17 | 22 | yes | open |
| insertion A: E-084 modulation multi-effect not built | 17 | 19 | yes | open |
| insertion B: parameter 11 approximated: the speaker's second biquad is a fit at one frequency (amp chain) | 17 | 19 | yes | open |
| insertion B: parameter 9 approximated: the insertion level is a fit (amp chain) | 17 | 19 | yes | open |
| insertion B: E-023 analog-style delay (vintage) not built | 17 | 18 | yes | open |
| insertion B: E-075 envelope-following filter not built | 16 | 22 | yes | open |
| insertion A: E-077 rhythmic filter and gate pattern not built | 16 | 17 | yes | open |
| insertion B: E-095 wave folder not built | 16 | 17 | yes | open |
| insertion A: E-076 filter with a controllable sweep not built | 15 | 18 | yes | open |
| insertion B: E-069 noise-adding lo-fi not built | 15 | 15 | yes | open |
| variation send: E-072 ring modulator not built | 15 | - | yes | open |
| insertion A: E-083 rhythmic slicer not built | 14 | 14 | yes | open |
| master effect: E-063 parallel compressor not built | 14 | - | yes | open |
| master effect: E-068 lo-fi not built | 14 | - | yes | open |
| variation send: E-039 tempo-synced phaser not built | 14 | - | yes | open |
| insertion B: E-076 filter with a controllable sweep not built | 13 | 16 | yes | open |
| insertion A: E-073 ring modulator (analog-synth style) not built | 13 | 14 | yes | open |
| insertion B: E-066 envelope wah (modeled analog) not built | 13 | 13 | yes | open |
| variation send: E-003 hall reverb (second family) not built | 13 | - | yes | open |
| variation send: E-007 room reverb (second family) not built | 13 | - | yes | open |
| insertion B: E-086 stutter repeat (straight) not built | 12 | 13 | yes | open |
| insertion A: E-082 band isolator not built | 12 | 12 | yes | open |
| insertion B: E-083 rhythmic slicer not built | 12 | 12 | yes | open |
| variation send: E-055 amp simulator: small stereo combo not built | 12 | - | yes | open |
| insertion B: E-036 mono phaser (modeled analog) not built | 11 | 12 | yes | open |
| insertion A: E-074 envelope-following ring modulator not built | 11 | 11 | yes | open |
| insertion A: E-093 headphone stereo optimizer not built | 11 | 11 | yes | open |
| insertion B: E-082 band isolator not built | 11 | 11 | yes | open |
| insertion B: E-093 headphone stereo optimizer not built | 11 | 11 | yes | open |
| variation send: E-086 stutter repeat (straight) not built | 11 | - | yes | open |
| insertion A: E-100 early reflections not built | 10 | 14 | yes | open |
| insertion B: E-094 vowel formant modulator not built | 10 | 13 | yes | open |
| insertion A: E-013 gated reverb not built | 10 | 10 | yes | open |
| variation send: E-071 bit crusher not built | 10 | - | yes | open |
| insertion A: E-051 amp simulator: clean combo (variant 2) not built | 9 | 14 | yes | open |
| insertion B: E-030 string-ensemble chorus with tremolo not built | 9 | 14 | yes | open |
| insertion A: E-007 room reverb (second family) not built | 9 | 11 | yes | open |
| insertion A: E-023 analog-style delay (vintage) not built | 9 | 11 | yes | open |
| insertion A: E-069 noise-adding lo-fi not built | 9 | 11 | yes | open |
| insertion B: E-040 envelope-following phaser not built | 9 | 11 | yes | open |
| insertion A: E-067 pedal wah (modeled analog) not built | 9 | 10 | yes | open |
| insertion A: E-086 stutter repeat (straight) not built | 9 | 9 | yes | open |
| variation send: E-020 delay with a controllable time not built | 9 | - | yes | open |
| insertion B: E-056 amp simulator: crunch combo not built | 8 | 13 | yes | open |
| insertion B: amp chain: a pre-EQ type not built | 8 | 12 | yes | open |
| insertion A: E-101 vocoder not built | 8 | 10 | yes | open |
| insertion A: E-070 turntable scratch not built | 8 | 9 | yes | open |
| variation send: E-077 rhythmic filter and gate pattern not built | 8 | - | yes | open |
| insertion A: E-056 amp simulator: crunch combo not built | 7 | 15 | yes | open |
| insertion B: E-050 amp simulator: clean combo (variant 1) not built | 7 | 10 | yes | open |
| insertion A: E-024 analog-style delay (modern) not built | 7 | 9 | yes | open |
| insertion A: E-088 stutter repeat (straight and triplet) not built | 7 | 9 | yes | open |
| insertion A: E-044 rotary speaker (variant 2) not built | 7 | 8 | yes | open |
| insertion B: E-070 turntable scratch not built | 7 | 8 | yes | open |
| insertion B: E-073 ring modulator (analog-synth style) not built | 7 | 7 | yes | open |
| insertion B: E-078 endless-rise spiral (variant 1) not built | 7 | 7 | yes | open |
| variation send: Compressor A on the variation send is not checked (approximated) | 7 | - | yes | open |
| variation send: E-046 amp simulator (variant 1) not built | 7 | - | yes | open |
| variation send: E-083 rhythmic slicer not built | 7 | - | yes | open |
| insertion A: E-020 delay with a controllable time not built | 6 | 10 | yes | open |
| insertion A: E-075 envelope-following filter not built | 6 | 9 | yes | open |
| insertion A: E-097 compact booster (modeled analog) not built | 6 | 6 | yes | open |
| insertion B: E-035 envelope-following flanger not built | 6 | 6 | yes | open |
| master effect: E-064 mid/side EQ and compressor not built | 6 | - | yes | open |
| master effect: E-085 record-stop slowdown not built | 6 | - | yes | open |
| master effect: E-096 compact filter (modeled analog) not built | 6 | - | yes | open |
| insertion B: E-024 analog-style delay (modern) not built | 5 | 14 | yes | open |
| insertion B: E-097 compact booster (modeled analog) not built | 5 | 7 | yes | open |
| insertion A: E-055 amp simulator: small stereo combo not built | 5 | 6 | yes | open |
| insertion B: E-051 amp simulator: clean combo (variant 2) not built | 5 | 6 | yes | open |
| insertion A: amp chain: a distortion type not built | 5 | 5 | yes | open |
| insertion A: E-080 endless-rise spiral (variant 2) not built | 5 | 5 | yes | open |
| insertion A: E-085 record-stop slowdown not built | 5 | 5 | yes | open |
| insertion B: E-064 mid/side EQ and compressor not built | 5 | 5 | yes | open |
| insertion B: E-080 endless-rise spiral (variant 2) not built | 5 | 5 | yes | open |
| insertion B: E-081 tempo-synced endless-rise spiral (variant 2) not built | 5 | 5 | yes | open |
| master effect: E-061 downward compressor not built | 5 | - | yes | open |
| master effect: E-091 presence enhancer not built | 5 | - | yes | open |
| variation send: E-095 wave folder not built | 5 | - | yes | open |
| insertion A: E-081 tempo-synced endless-rise spiral (variant 2) not built | 4 | 5 | yes | open |
| insertion B: E-061 downward compressor not built | 4 | 5 | yes | open |
| insertion A: E-040 envelope-following phaser not built | 4 | 4 | yes | open |
| insertion A: E-062 upward compressor not built | 4 | 4 | yes | open |
| insertion A: E-078 endless-rise spiral (variant 1) not built | 4 | 4 | yes | open |
| insertion B: E-087 stutter repeat (triplet) not built | 4 | 4 | yes | open |
| variation send: E-051 amp simulator: clean combo (variant 2) not built | 4 | - | yes | open |
| variation send: E-084 modulation multi-effect not built | 4 | - | yes | open |
| insertion A: E-032 flanger with a controllable sweep not built | 3 | 6 | yes | open |
| insertion B: E-053 amp simulator: lead stack (variant 1) not built | 3 | 4 | yes | open |
| insertion B: E-089 stutter repeat (free) not built | 3 | 4 | yes | open |
| insertion B: E-091 presence enhancer not built | 3 | 4 | yes | open |
| insertion A: E-102 noise gate, compressor and EQ not built | 3 | 3 | yes | open |
| insertion B: amp chain: a distortion type not built | 3 | 3 | yes | open |
| insertion B: E-088 stutter repeat (straight and triplet) not built | 3 | 3 | yes | open |
| master effect: E-062 upward compressor not built | 3 | - | yes | open |
| master effect: E-071 bit crusher not built | 3 | - | yes | open |
| master effect: E-075 envelope-following filter not built | 3 | - | yes | open |
| master effect: E-082 band isolator not built | 3 | - | yes | open |
| variation send: the amp chain on the variation send is not verified (dropped) | 3 | - | yes | open |
| variation send: E-013 gated reverb not built | 3 | - | yes | open |
| variation send: E-030 string-ensemble chorus with tremolo not built | 3 | - | yes | open |
| variation send: E-044 rotary speaker (variant 2) not built | 3 | - | yes | open |
| variation send: E-049 compressor into distortion into delay not built | 3 | - | yes | open |
| variation send: E-066 envelope wah (modeled analog) not built | 3 | - | yes | open |
| variation send: E-070 turntable scratch not built | 3 | - | yes | open |
| variation send: E-085 record-stop slowdown not built | 3 | - | yes | open |
| variation send: E-092 harmonic enhancer not built | 3 | - | yes | open |
| variation send: E-098 piano damper resonance not built | 3 | - | yes | open |
| insertion A: E-064 mid/side EQ and compressor not built | 2 | 4 | yes | open |
| insertion A: E-091 presence enhancer not built | 2 | 4 | yes | open |
| insertion B: E-052 amp simulator: high-gain stack not built | 2 | 4 | yes | open |
| insertion B: E-063 parallel compressor not built | 2 | 4 | yes | open |
| insertion B: E-085 record-stop slowdown not built | 2 | 4 | yes | open |
| insertion A: E-038 phaser with a controllable sweep not built | 2 | 3 | yes | open |
| insertion A: E-053 amp simulator: lead stack (variant 1) not built | 2 | 3 | yes | open |
| insertion B: E-101 vocoder not built | 2 | 3 | yes | open |
| insertion A: E-035 envelope-following flanger not built | 2 | 2 | yes | open |
| insertion B: E-067 pedal wah (modeled analog) not built | 2 | 2 | yes | open |
| master effect: E-087 stutter repeat (triplet) not built | 2 | - | yes | open |
| master effect: E-089 stutter repeat (free) not built | 2 | - | yes | open |
| master effect: E-095 wave folder not built | 2 | - | yes | open |
| variation send: E-011 stage reverb (second family) not built | 2 | - | yes | open |
| variation send: E-068 lo-fi not built | 2 | - | yes | open |
| variation send: E-076 filter with a controllable sweep not built | 2 | - | yes | open |
| variation send: E-080 endless-rise spiral (variant 2) not built | 2 | - | yes | open |
| variation send: E-081 tempo-synced endless-rise spiral (variant 2) not built | 2 | - | yes | open |
| variation send: E-082 band isolator not built | 2 | - | yes | open |
| variation send: E-089 stutter repeat (free) not built | 2 | - | yes | open |
| insertion B: E-079 tempo-synced endless-rise spiral (variant 1) not built | 1 | 3 | yes | open |
| insertion A: amp chain: a wah mode not built | 1 | 1 | yes | open |
| insertion A: E-052 amp simulator: high-gain stack not built | 1 | 1 | yes | open |
| insertion A: E-057 amp simulator: lead stack (variant 2) not built | 1 | 1 | yes | open |
| insertion A: E-087 stutter repeat (triplet) not built | 1 | 1 | yes | open |
| insertion A: E-089 stutter repeat (free) not built | 1 | 1 | yes | open |
| insertion B: amp chain: a speaker type not built | 1 | 1 | yes | open |
| insertion B: E-032 flanger with a controllable sweep not built | 1 | 1 | yes | open |
| insertion B: E-038 phaser with a controllable sweep not built | 1 | 1 | yes | open |
| insertion B: E-055 amp simulator: small stereo combo not built | 1 | 1 | yes | open |
| master effect: E-078 endless-rise spiral (variant 1) not built | 1 | - | yes | open |
| master effect: E-080 endless-rise spiral (variant 2) not built | 1 | - | yes | open |
| master effect: E-086 stutter repeat (straight) not built | 1 | - | yes | open |
| master effect: E-088 stutter repeat (straight and triplet) not built | 1 | - | yes | open |
| variation send: E-040 envelope-following phaser not built | 1 | - | yes | open |
| variation send: E-052 amp simulator: high-gain stack not built | 1 | - | yes | open |
| variation send: E-057 amp simulator: lead stack (variant 2) not built | 1 | - | yes | open |
| variation send: E-060 multiband compressor not built | 1 | - | yes | open |
| variation send: E-061 downward compressor not built | 1 | - | yes | open |
| variation send: E-064 mid/side EQ and compressor not built | 1 | - | yes | open |
| variation send: E-065 auto wah (modeled analog) not built | 1 | - | yes | open |
| variation send: E-074 envelope-following ring modulator not built | 1 | - | yes | open |
| variation send: E-087 stutter repeat (triplet) not built | 1 | - | yes | open |
| variation send: E-088 stutter repeat (straight and triplet) not built | 1 | - | yes | open |
| variation send: E-093 headphone stereo optimizer not built | 1 | - | yes | open |
| variation send: E-096 compact filter (modeled analog) not built | 1 | - | yes | open |
| variation send: E-097 compact booster (modeled analog) not built | 1 | - | yes | open |
| audio-input insertion A: effect type on the audio input (only for the audio input) | 1 | - | unknown | open |

Only in the personal library (not in the stock set):

| Gap | Performances (personal) | Parts (personal) | Changes the sound | Status |
|---|---|---|---|---|
| variation send: E-056 amp simulator: crunch combo not built | 4 | - | yes | open |
| audio-input insertion B: effect type on the audio input (only for the audio input) | 3 | - | unknown | open |

## 4. Port to Rust

Status: not started for any existing effect type.

A port counts as done when all of these hold: the test suite and the cost gates below are carried over and pass against the Rust build; a saved state of the C++ version opens and plays the same; the plugin id, the parameter ids and the sound format stay the same. For this plugin set: the test suite in `tests/` (including the stock render test and
the privacy check), the cost gate `scripts/cost-gate.sh` against
`scripts/cost-baseline.txt`, and the stock gate `scripts/stock-gate.sh` for the
library's own effects; the state text `oscairn-effects-1 <plugin id>` with the Mode parameter (id 1) and each mode's
parameter i as id mode << 8 | i; the same plugin ids and mode ids.

One row per effect type the plugin set has today. Cost: instructions per 128-frame block at 48 kHz with a signal, from
`scripts/cost-baseline.txt`. Fit: the reference types the converter maps onto the mode and how it
marks them, from the 2026-11-17 conversion run.

| Plugin / mode | Origin | Language | Cost per block | Fit to the reference | Decision |
|---|---|---|---|---|---|
| `org.oscairn.effects.reverb` / Reverb 1 | the vendored effects library's own algorithm, unchanged | C++ | not measured | no reference type maps to it | undecided |
| `org.oscairn.effects.reverb` / Reverb 2 | the vendored effects library's own algorithm, unchanged | C++ | 80,683 | no reference type maps to it | undecided |
| `org.oscairn.effects.reverb` / Hall A | own code on the vendored library's effect framework | C++ | 125,724 | E-001 hall reverb (first family): 1572 uses approximated (the converter's own marking) | undecided |
| `org.oscairn.effects.eq` / Five-band EQ | own code on the vendored library's effect framework | C++ | 6,488 | E-090 five-band EQ (modeled analog): 3146 uses exact (the converter's own marking) | undecided |
| `org.oscairn.effects.eq` / Two-band EQ | own code on the vendored library's effect framework | C++ | not measured | the part's two-band EQ (not an effect type); fit not in the inputs, unverified | undecided |
| `org.oscairn.effects.eq` / Three-band EQ | own code on the vendored library's effect framework | C++ | not measured | the part's three-band EQ (not an effect type); fit not in the inputs, unverified | undecided |
| `org.oscairn.effects.eq` / Master EQ | own code on the vendored library's effect framework | C++ | not measured | the master EQ (not an effect type); fit not in the inputs, unverified | undecided |
| `org.oscairn.effects.delay` / Tempo Cross Delay | own code on the vendored library's effect framework | C++ | 138,251 | E-017 tempo-synced cross delay: 1637 uses exact (the converter's own marking) | undecided |
| `org.oscairn.effects.delay` / Cross Delay | own code on the vendored library's effect framework | C++ | not measured | E-016 cross delay: 397 uses exact (the converter's own marking) | undecided |
| `org.oscairn.effects.delay` / Tempo Stereo Delay | own code on the vendored library's effect framework | C++ | not measured | E-019 tempo-synced stereo delay: 470 uses exact (the converter's own marking) | undecided |
| `org.oscairn.effects.delay` / L/R Delay | own code on the vendored library's effect framework | C++ | not measured | E-021 left/right delay: 289 uses exact (the converter's own marking) | undecided |
| `org.oscairn.effects.delay` / L/C/R Delay | own code on the vendored library's effect framework | C++ | not measured | E-022 left/center/right delay: 83 uses exact (the converter's own marking) | undecided |
| `org.oscairn.effects.delay` / Tempo Mono Delay | own code on the vendored library's effect framework | C++ | not measured | E-018 tempo-synced mono delay: 106 uses exact (the converter's own marking) | undecided |
| `org.oscairn.effects.dynamics` / Circuit Compressor | own code on the vendored library's effect framework | C++ | 92,830 | E-058 compressor (modeled analog): 1562 uses exact (the converter's own marking) | undecided |
| `org.oscairn.effects.dynamics` / Compressor A | own code on the vendored library's effect framework | C++ | 21,007 | E-059 compressor (classic): 7 uses approximated, 414 uses exact (the converter's own marking) | undecided |
| `org.oscairn.effects.tremolo-rotary` / Rotary A | own code on the vendored library's effect framework | C++ | not measured | E-043 rotary speaker (variant 1): 229 uses exact (the converter's own marking) | undecided |
| `org.oscairn.effects.tremolo-rotary` / Auto Pan | own code on the vendored library's effect framework | C++ | not measured | E-041 auto pan: 320 uses exact (the converter's own marking) | undecided |
| `org.oscairn.effects.tremolo-rotary` / Tremolo | own code on the vendored library's effect framework | C++ | not measured | E-042 tremolo: 232 uses exact (the converter's own marking) | undecided |
| `org.oscairn.effects.chorus` / Chorus A | own code on the vendored library's effect framework | C++ | 61,519 | E-025 chorus (first family): 901 uses approximated, 1 use exact (the converter's own marking) | undecided |
| `org.oscairn.effects.chorus` / Chorus B | own code on the vendored library's effect framework | C++ | not measured | E-027 chorus (second family): 273 uses exact (the converter's own marking) | undecided |
| `org.oscairn.effects.chorus` / Dual Modulator | own code on the vendored library's effect framework | C++ | not measured | E-026 dual modulator: 833 uses exact (the converter's own marking) | undecided |
| `org.oscairn.effects.chorus` / Chorus C | own code on the vendored library's effect framework | C++ | not measured | E-028 wide multi-voice chorus: 206 uses exact (the converter's own marking) | undecided |
| `org.oscairn.effects.chorus` / Dual Detune | own code on the vendored library's effect framework | C++ | 47,670 | E-029 dual detune: 493 uses exact (the converter's own marking) | undecided |
| `org.oscairn.effects.amp-chain` / Amp Chain A | own code on the vendored library's effect framework | C++ | not measured | E-054 amp and effects chain: 19 uses approximated (the converter's own marking) | undecided |

### Effect types still to build

The 83 effect types the stock set uses switched on that no mode plays yet, most used first (uses summed over the
slots of the effect-type table). Each is to be written in Rust.

| Key | Effect type | Uses (stock) | Slots | Language |
|---|---|---|---|---|
| E-003 | hall reverb (second family) | 552 | insertion A, insertion B, reverb send, variation send | to be written in Rust |
| E-046 | amp simulator (variant 1) | 470 | insertion A, insertion B, variation send | to be written in Rust |
| E-060 | multiband compressor | 428 | insertion A, insertion B, master effect, variation send | to be written in Rust |
| E-004 | hall reverb (fourth family) | 361 | reverb send | to be written in Rust |
| E-002 | hall reverb (third family) | 313 | reverb send | to be written in Rust |
| E-100 | early reflections | 263 | insertion A, insertion B, variation send | to be written in Rust |
| E-031 | flanger (modeled analog) | 259 | audio-input insertion A, insertion A, insertion B, variation send | to be written in Rust |
| E-072 | ring modulator | 243 | insertion A, insertion B, variation send | to be written in Rust |
| E-037 | stereo phaser (modeled analog) | 239 | insertion A, insertion B, variation send | to be written in Rust |
| E-068 | lo-fi | 237 | insertion A, insertion B, master effect, variation send | to be written in Rust |
| E-047 | amp simulator (variant 2) | 224 | insertion A, insertion B | to be written in Rust |
| E-005 | room reverb (first family) | 217 | reverb send | to be written in Rust |
| E-092 | harmonic enhancer | 214 | insertion A, insertion B, variation send | to be written in Rust |
| E-099 | pitch shifter | 190 | insertion A, insertion B, variation send | to be written in Rust |
| E-007 | room reverb (second family) | 180 | insertion A, insertion B, reverb send, variation send | to be written in Rust |
| E-006 | room reverb (third family) | 178 | reverb send | to be written in Rust |
| E-009 | plate reverb (third family) | 175 | reverb send | to be written in Rust |
| E-098 | piano damper resonance | 173 | insertion A, insertion B, variation send | to be written in Rust |
| E-039 | tempo-synced phaser | 147 | insertion A, insertion B, variation send | to be written in Rust |
| E-033 | flanger (classic) | 141 | insertion A, insertion B, variation send | to be written in Rust |
| E-049 | compressor into distortion into delay | 140 | insertion A, insertion B, master effect, variation send | to be written in Rust |
| E-014 | reverse reverb | 112 | insertion A, insertion B, variation send | to be written in Rust |
| E-008 | room reverb (fourth family) | 105 | reverb send | to be written in Rust |
| E-011 | stage reverb (second family) | 105 | insertion A, insertion B, reverb send, variation send | to be written in Rust |
| E-034 | tempo-synced flanger | 102 | insertion A, insertion B, variation send | to be written in Rust |
| E-071 | bit crusher | 85 | insertion A, insertion B, master effect, variation send | to be written in Rust |
| E-010 | plate reverb (fourth family) | 83 | reverb send | to be written in Rust |
| E-048 | compressor into distortion | 81 | insertion A, insertion B | to be written in Rust |
| E-036 | mono phaser (modeled analog) | 80 | insertion A, insertion B, variation send | to be written in Rust |
| E-023 | analog-style delay (vintage) | 70 | insertion A, insertion B, variation send | to be written in Rust |
| E-095 | wave folder | 67 | insertion A, insertion B, master effect, variation send | to be written in Rust |
| E-012 | room-shape reverb | 65 | reverb send | to be written in Rust |
| E-096 | compact filter (modeled analog) | 63 | insertion A, insertion B, master effect, variation send | to be written in Rust |
| E-084 | modulation multi-effect | 58 | insertion A, insertion B, variation send | to be written in Rust |
| E-077 | rhythmic filter and gate pattern | 57 | insertion A, insertion B, variation send | to be written in Rust |
| E-015 | shimmer reverb | 56 | reverb send | to be written in Rust |
| E-013 | gated reverb | 55 | insertion A, insertion B, variation send | to be written in Rust |
| E-030 | string-ensemble chorus with tremolo | 53 | insertion A, insertion B, variation send | to be written in Rust |
| E-074 | envelope-following ring modulator | 53 | insertion A, insertion B, variation send | to be written in Rust |
| E-065 | auto wah (modeled analog) | 51 | insertion A, insertion B, variation send | to be written in Rust |
| E-024 | analog-style delay (modern) | 49 | insertion A, insertion B, variation send | to be written in Rust |
| E-063 | parallel compressor | 49 | insertion A, insertion B, master effect | to be written in Rust |
| E-075 | envelope-following filter | 48 | insertion A, insertion B, master effect | to be written in Rust |
| E-076 | filter with a controllable sweep | 48 | insertion A, insertion B, variation send | to be written in Rust |
| E-050 | amp simulator: clean combo (variant 1) | 46 | insertion A, insertion B | to be written in Rust |
| E-020 | delay with a controllable time | 45 | insertion A, insertion B, variation send | to be written in Rust |
| E-066 | envelope wah (modeled analog) | 45 | insertion A, insertion B, variation send | to be written in Rust |
| E-094 | vowel formant modulator | 45 | insertion A, insertion B | to be written in Rust |
| E-044 | rotary speaker (variant 2) | 43 | insertion A, insertion B, variation send | to be written in Rust |
| E-055 | amp simulator: small stereo combo | 42 | insertion A, insertion B, variation send | to be written in Rust |
| E-061 | downward compressor | 41 | insertion A, insertion B, master effect, variation send | to be written in Rust |
| E-082 | band isolator | 38 | insertion A, insertion B, master effect, variation send | to be written in Rust |
| E-069 | noise-adding lo-fi | 37 | insertion A, insertion B | to be written in Rust |
| E-086 | stutter repeat (straight) | 36 | insertion A, insertion B, master effect, variation send | to be written in Rust |
| E-083 | rhythmic slicer | 35 | insertion A, insertion B, variation send | to be written in Rust |
| E-051 | amp simulator: clean combo (variant 2) | 32 | insertion A, insertion B, variation send | to be written in Rust |
| E-056 | amp simulator: crunch combo | 31 | insertion A, insertion B | to be written in Rust |
| E-045 | rotary speaker (modeled analog, on the rotary bus) | 30 | rotary bus | to be written in Rust |
| E-073 | ring modulator (analog-synth style) | 26 | insertion A, insertion B | to be written in Rust |
| E-070 | turntable scratch | 25 | insertion A, insertion B, variation send | to be written in Rust |
| E-093 | headphone stereo optimizer | 23 | insertion A, insertion B, variation send | to be written in Rust |
| E-064 | mid/side EQ and compressor | 22 | insertion A, insertion B, master effect, variation send | to be written in Rust |
| E-085 | record-stop slowdown | 20 | insertion A, insertion B, master effect, variation send | to be written in Rust |
| E-040 | envelope-following phaser | 18 | insertion A, insertion B, variation send | to be written in Rust |
| E-035 | envelope-following flanger | 16 | insertion A, insertion B | to be written in Rust |
| E-088 | stutter repeat (straight and triplet) | 16 | insertion A, insertion B, master effect, variation send | to be written in Rust |
| E-080 | endless-rise spiral (variant 2) | 15 | insertion A, insertion B, master effect, variation send | to be written in Rust |
| E-091 | presence enhancer | 14 | insertion A, insertion B, master effect | to be written in Rust |
| E-097 | compact booster (modeled analog) | 14 | insertion A, insertion B, variation send | to be written in Rust |
| E-081 | tempo-synced endless-rise spiral (variant 2) | 13 | insertion A, insertion B, variation send | to be written in Rust |
| E-101 | vocoder | 13 | insertion A, insertion B | to be written in Rust |
| E-067 | pedal wah (modeled analog) | 12 | insertion A, insertion B | to be written in Rust |
| E-078 | endless-rise spiral (variant 1) | 12 | insertion A, insertion B, master effect | to be written in Rust |
| E-089 | stutter repeat (free) | 10 | insertion A, insertion B, master effect, variation send | to be written in Rust |
| E-053 | amp simulator: lead stack (variant 1) | 8 | insertion A, insertion B | to be written in Rust |
| E-087 | stutter repeat (triplet) | 8 | insertion A, insertion B, master effect, variation send | to be written in Rust |
| E-032 | flanger with a controllable sweep | 7 | insertion A, insertion B | to be written in Rust |
| E-062 | upward compressor | 7 | insertion A, master effect | to be written in Rust |
| E-052 | amp simulator: high-gain stack | 6 | insertion A, insertion B, variation send | to be written in Rust |
| E-038 | phaser with a controllable sweep | 4 | insertion A, insertion B | to be written in Rust |
| E-102 | noise gate, compressor and EQ | 4 | insertion A | to be written in Rust |
| E-079 | tempo-synced endless-rise spiral (variant 1) | 3 | insertion B | to be written in Rust |
| E-057 | amp simulator: lead stack (variant 2) | 2 | insertion A, variation send | to be written in Rust |

Not in this list: the variants of E-054 amp and effects chain the amp chain does not map (28 uses), which extend an
existing C++ mode (decision undecided).

## 5. Next

In build order, from the cross-engine work packages (greedy: the package that brings the most stock Performances to
"no sound-affecting drop" first):

1. Reverb types on the reverb send (second of seventeen packages; 2059 stock Performances touched): E-003 hall reverb (second family)
   (388 Performances on the reverb send), E-004 hall reverb (fourth family) (361), E-002 hall reverb (third family) (313), E-005 room reverb (first family) (217),
   E-006 room reverb (third family) (178), E-009 plate reverb (third family) (175), E-008 room reverb (fourth family) (105), E-010 plate reverb (fourth family) (83). Also: the first version
   of E-001 hall reverb (first family) as Hall A, whose decay is off for some settings (1572 Performances, approximated). Three tank
   reverbs (Hall B, Stage A, Hall C) are written on a branch not yet merged; which reference types they cover is
   unverified here.
2. Insertion types (sixth; 1973 Performances): E-046 amp simulator (variant 1), E-047 amp simulator (variant 2),
   E-100 early reflections, E-031 flanger (modeled analog), E-072 ring modulator, E-068 lo-fi, E-037 stereo phaser (modeled analog),
   E-060 multiband compressor, and the rest of the list above by use.
3. Variation-send types (eighth; 700 Performances).
4. The master effect (E-060 multiband compressor: 265 Performances), the rotary bus and the audio-input insertion
   (sixteenth; 367 Performances).
