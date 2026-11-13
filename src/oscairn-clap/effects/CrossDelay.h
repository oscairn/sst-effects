/*
 * Oscairn effects (doc/oscairn-plan.md). GPL-3.0-or-later, see ../OscairnEffects.cpp.
 *
 * The cross delay without tempo sync: the same signal path as the tempo cross delay
 * (effects/TempoCrossDelay.h, CrossCore), its two line times in ms (0.1 to 1638.3) instead of
 * note lengths, and no lag. Measured against the 02 00 renders (measurements/
 * qwen-cross-delay-2026-11-11/, qwen-night-2026-11-12/cross_delay_extra/ and
 * qwen-night-2026-11-12-review/cross_delay/): the lines are the family's time law
 * (msLineSeconds); the feedback is v x 516 / 32768 with inverted polarity when negative; the damping is
 * the two-tap g = 5(1 - r)/9 in the feedback path only; the input feeds are split L/R; the
 * balance turns the far side down by the square of its distance and keeps the near side at
 * unity (msBalance), not the tempo mode's equal-power pair. The EQ parameters are the family's
 * output EQ (OutputEq): two first-order shelves on the whole output, dry and wet alike.
 */
#ifndef OSCAIRN_CLAP_EFFECTS_CROSSDELAY_H
#define OSCAIRN_CLAP_EFFECTS_CROSSDELAY_H

#include "effects/TempoCrossDelay.h"

namespace oscairn_fx::delay
{
template <typename FXConfig> struct CrossDelay : CrossCore<FXConfig, CrossDelay>
{
    enum params
    {
        timeLR,
        timeRL,
        feedback,
        input,
        damping,
        mix,
        eqLowFreq,
        eqLowGain,
        eqHighFreq,
        eqHighGain,
        numParamsEnum
    };
    static constexpr int numParams{numParamsEnum};
    static constexpr const char *streamingName{"oscairn-cross-delay"};
    static constexpr const char *displayName{"Cross Delay"};
    using CrossCore<FXConfig, CrossDelay>::CrossCore;

    static int bufferSamples(double rate)
    {
        return (int)std::ceil(maxLineMs / 1000 * rate) + 2 * Damping::maxK + 8;
    }
    void setLines(double, double)
    {
        this->secA = msLineSeconds(this->last[timeLR]);
        this->secB = msLineSeconds(this->last[timeRL]);
    }
    static Balance balance(double v) { return msBalance(v); }

    sst::basic_blocks::params::ParamMetaData paramAt(int idx) const
    {
        using pmd = sst::basic_blocks::params::ParamMetaData;
        auto ms = [&](const char *n) {
            return pmd().asFloat().withName(n).withRange(0.1f, 1638.3f).withDefault(250.f).withLinearScaleFormatting("ms").withDecimalPlaces(1);
        };
        switch (idx)
        {
        case timeLR: return ms("Time L>R");
        case timeRL: return ms("Time R>L");
        case feedback: return pmd().asInt().withName("Feedback").withRange(-63, 63).withDefault(16);
        case input:
            return pmd().asInt().withName("Input").withRange(0, 4).withDefault(0).withUnorderedMapFormatting(
                {{inL, "L"}, {inR, "R"}, {inLR, "L&R"}, {inLMode2, "L Mode 2"}, {inRMode2, "R Mode 2"}});
        case damping:
            return pmd().asFloat().withName("High Damping").withRange(0.1f, 1.f).withDefault(0.5f).withLinearScaleFormatting("").withDecimalPlaces(1);
        case mix:
        {
            std::unordered_map<int, std::string> m;
            for (int v = -63; v <= 63; ++v)
                m[v] = v < 0 ? "D" + std::to_string(-v) + ">W" : (v == 0 ? "D=W" : "D<W" + std::to_string(v));
            return pmd().asInt().withName("Mix").withRange(-63, 63).withDefault(0).withUnorderedMapFormatting(m);
        }
        }
        if (idx >= eqLowFreq && idx <= eqHighGain)
            return eqParamAt(idx - eqLowFreq);
        return pmd().withName("Unused").withType(pmd::NONE);
    }
};
} // namespace oscairn_fx::delay

#endif
