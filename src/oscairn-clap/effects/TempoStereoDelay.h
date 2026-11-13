/*
 * Oscairn effects (doc/oscairn-plan.md). GPL-3.0-or-later, see ../OscairnEffects.cpp.
 *
 * A tempo-synced stereo delay: the cross delay's signal path (effects/TempoCrossDelay.h,
 * CrossCore) with one note length for both lines, a Lag on both and an L/R Diffusion that
 * lengthens the line playing left and shortens the one playing right by the same ms, and a
 * fixed L&R input feed. Feedback v x 516 / 32768 per crossing, the two-tap feedback damping
 * g = 5 (1 - r) / 9 and the cosine/sine balance with the tempo family's linear position are
 * the family's laws, re-measured for this type (plan, section 15, "Tempo Stereo Delay"). The
 * time law is the family's, with this type's offsets (stereoLineSamples, stereoNote). The reference's four EQ parameters are left
 * out as in the neighbouring delay modes (decision 23, re-checked for this type).
 */
#ifndef OSCAIRN_CLAP_EFFECTS_TEMPOSTEREODELAY_H
#define OSCAIRN_CLAP_EFFECTS_TEMPOSTEREODELAY_H

#include "effects/TempoCrossDelay.h"

namespace oscairn_fx::delay
{
// A line's length in 44.1 kHz samples (kept in seconds at other rates): the note time truncated
// to 0.1 ms (the reference's displayed precision) plus the line's own offset in ms, by the
// family's time law (tenthsSamples). The note time halves while the longer line would reach
// lineLimit samples (halvedNote). Every echo time the 02 28 renders measured (1 to 131,062
// samples) fits this; round(ms * 44.1) - 1 misses most of them by a sample.
inline long stereoLineSamples(long noteTenths, double offsetMs)
{
    return tenthsSamples(noteTenths + std::lround(offsetMs * 10));
}
inline long stereoNote(int note, double tempo, double longestOffsetMs)
{
    return halvedNote(noteTenths(note, tempo), std::lround(longestOffsetMs * 10), lineLimit);
}

template <typename FXConfig> struct TempoStereoDelay : CrossCore<FXConfig, TempoStereoDelay>
{
    enum params
    {
        delayTime,
        feedback,
        damping,
        diffusion,
        lag,
        mix,
        numParamsEnum
    };
    static constexpr int numParams{numParamsEnum};
    static constexpr const char *streamingName{"oscairn-tempo-stereo-delay"};
    static constexpr const char *displayName{"Tempo Stereo Delay"};
    // No input-select parameter: input L feeds the line playing right, input R the one playing left.
    static constexpr int inputFeed{inLR};
    using CrossCore<FXConfig, TempoStereoDelay>::CrossCore;

    static int bufferSamples(double rate)
    {
        // every line stays under lineLimit samples
        return (int)std::ceil(lineLimit / designRate * rate) + 2 * Damping::maxK + 8;
    }
    // Both lines loop at the note time plus Lag; the diffusion moves only where they are read
    // for the output, the left later and the right earlier by its ms.
    static constexpr bool outputTaps{true};
    void setLines(double tempo, double)
    {
        const double lagMs = this->last[lag], diffMs = this->last[diffusion];
        const long note = stereoNote((int)std::lround(this->last[delayTime]), tempo, lagMs + std::abs(diffMs));
        this->secA = this->secB = (double)stereoLineSamples(note, lagMs) / designRate;
        this->outSecA = (double)stereoLineSamples(note, lagMs - diffMs) / designRate; // plays right
        this->outSecB = (double)stereoLineSamples(note, lagMs + diffMs) / designRate; // plays left
    }
    // The tempo family's balance (measured at -21, +21 and +48 for this type).
    static Balance balance(double v) { return tempoBalance(v); }

    sst::basic_blocks::params::ParamMetaData paramAt(int idx) const
    {
        using pmd = sst::basic_blocks::params::ParamMetaData;
        switch (idx)
        {
        case delayTime:
            return pmd().asInt().withName("Delay Time").withRange(0, 19).withDefault(9).withUnorderedMapFormatting(noteNames);
        case feedback: return pmd().asInt().withName("Feedback").withRange(-63, 63).withDefault(16);
        case damping:
            return pmd().asFloat().withName("High Damping").withRange(0.1f, 1.f).withDefault(1.f).withLinearScaleFormatting("").withDecimalPlaces(1);
        case diffusion:
            return pmd().asFloat().withName("L/R Diffusion").withRange(-63.f, 63.f).withDefault(0.f).withLinearScaleFormatting("ms").withDecimalPlaces(0);
        case lag: return pmd().asFloat().withName("Lag").withRange(-63.f, 63.f).withDefault(0.f).withLinearScaleFormatting("ms").withDecimalPlaces(0);
        case mix:
        {
            std::unordered_map<int, std::string> m;
            for (int v = -63; v <= 63; ++v)
                m[v] = v < 0 ? "D" + std::to_string(-v) + ">W" : (v == 0 ? "D=W" : "D<W" + std::to_string(v));
            return pmd().asInt().withName("Mix").withRange(-63, 63).withDefault(-52).withUnorderedMapFormatting(m);
        }
        }
        return pmd().withName("Unused").withType(pmd::NONE);
    }
};
} // namespace oscairn_fx::delay

#endif
