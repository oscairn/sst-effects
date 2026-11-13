/*
 * Oscairn effects (doc/oscairn-plan.md). GPL-3.0-or-later, see ../OscairnEffects.cpp.
 *
 * A delay with one line per channel - "L/R Delay". Written for Oscairn; follows the
 * sst-effects effect contract. The parameter set has four times - a delay time and a
 * feedback time per channel - and no input select, so the two lines are independent: the L
 * line is fed by input L and answers on the left Time L after it, its own output feeding
 * itself back through the damping and returning Time FB1 later (so its echoes sit
 * Time L + k x Feedback Time 1 apart, dropping feedback / 63.5 each time); the R line is the mirror
 * with Time R and Feedback Time 2. Nothing crosses to the other channel: with feedback at
 * its default, a left input answers on the left only. The pairing of the times to their own
 * channel follows the parameter set's own naming (the cross type names its times "L>R / R>L",
 * this one "Delay Time L / R", and has no input-select parameter to route anything), and the
 * reading was tested and holds: lr_pair and lr_hold_swap show each channel's echo grid moving
 * with its own two parameters (a cross-coupled reading, Time + feedback summed into cross
 * lines as in CrossCore, would put the left first echo at Time L + Feedback Time 2, and
 * lr_hold_swap's grids would not move), lr_cross shows nothing crossing at feedback 0, and
 * lr_tmax the series composition of the two taps. The right channel's line never answers on
 * the left and vice versa; both lines start at the same input event.
 *
 * Every law inside a line is the delay family's (effects/TempoCrossDelay.h, whose Line,
 * Damping and dampingCoefficient this mode uses unchanged): the line length
 * floor(ms x 10 x 18063 / 4096) samples at 44.1 kHz (msLineSeconds - each line stays under the
 * parameter's own 1,638.3 ms, so no ceiling differs from the cross modes), kept in seconds
 * at other rates by the first-order all-pass; feedback v / 63.5 per loop with inverted
 * polarity when negative; the two-tap damping g = 5 (1 - r) / 9 in the feedback path only,
 * its delay taken off the loop so the output taps keep their time; the cross delay's balance
 * (msBalance, measured here too; the tempo modes' equal-power pair is theirs).
 * The reference's four EQ parameters are not built: they act on the whole output, dry and
 * wet alike, not in the feedback path, and their shelf law is not measured yet (plan
 * decision 23, known difference 44).
 */
#ifndef OSCAIRN_CLAP_EFFECTS_LRDELAY_H
#define OSCAIRN_CLAP_EFFECTS_LRDELAY_H

#include "effects/TempoCrossDelay.h"

namespace oscairn_fx::delay
{
template <typename FXConfig> struct LRDelay : sst::effects::core::EffectTemplateBase<FXConfig>
{
    enum params
    {
        timeL,    // ms, the L line's delay time (its input answers on the left)
        timeR,    // ms, the R line's delay time
        fbTime1,  // ms, the L line's loop spacing
        fbTime2,  // ms, the R line's loop spacing
        feedback, // -63..+63, v / 63.5 per loop
        damping,  // 0.1..1.0, the family's two-tap in the feedback path
        mix,      // -63..+63, D>W .. D<W
        numParamsEnum
    };
    static constexpr int numParams{numParamsEnum};
    static constexpr const char *streamingName{"oscairn-lr-delay"};
    static constexpr const char *displayName{"L/R Delay"};

    LRDelay(typename FXConfig::GlobalStorage *s, typename FXConfig::EffectStorage *e, typename FXConfig::ValueStorage *p)
        : sst::effects::core::EffectTemplateBase<FXConfig>(s, e, p)
    {
        const int n = bufferSamples(this->sampleRate());
        lineL.resize(n), lineR.resize(n);
        last.assign(numParams, NAN);
    }
    static constexpr int bufferSamples(double rate) { return (int)std::ceil(maxLineMs / 1000 * rate) + 2 * Damping::maxK + 8; }
    static size_t heapBytesAt(double rate) { return 2 * sizeof(float) * (size_t)bufferSamples(rate); }
    size_t heapBytes() const { return (lineL.buf.size() + lineR.buf.size()) * sizeof(float); }
    // The longest silence between two echoes: the longest tap or loop spacing, plus the wrapper's 100 ms.
    double quietSeconds() const { return std::max({last[timeL], last[timeR], last[fbTime1], last[fbTime2]}) / 1000 + 0.1; }

    void initialize()
    {
        lineL.clear(), lineR.clear();
        outL.clear(), outR.clear();
        dampL.clear();
        dampR.clear();
        std::fill(last.begin(), last.end(), NAN);
        update();
    }
    void suspendProcessing() { initialize(); }
    int getRingoutDecay() const { return -1; }
    void onSampleRateChanged() { initialize(); }

    void processBlock(float *__restrict L, float *__restrict R)
    {
        update();
        for (int i = 0; i < FXConfig::blockSize; ++i)
        {
            const double l = L[i], r = R[i];
            const double oL = lineL.out(outL), oR = lineR.out(outR); // each line plays its own channel
            const double xL = l + fb * dampL.run(lineL.out()); // the loop at the line's own tap
            const double xR = r + fb * dampR.run(lineR.out());
            lineL.write((float)xL), lineR.write((float)xR);
            L[i] = (float)(dry * l + wet * oL);
            R[i] = (float)(dry * r + wet * oR);
        }
    }

    sst::basic_blocks::params::ParamMetaData paramAt(int idx) const
    {
        using pmd = sst::basic_blocks::params::ParamMetaData;
        auto ms = [&](const char *n) {
            return pmd().asFloat().withName(n).withRange(0.1f, 1638.3f).withDefault(400.f).withLinearScaleFormatting("ms").withDecimalPlaces(1);
        };
        switch (idx)
        {
        case timeL: return ms("Time L");
        case timeR: return ms("Time R");
        case fbTime1: return ms("Feedback Time 1");
        case fbTime2: return ms("Feedback Time 2");
        case feedback: return pmd().asInt().withName("Feedback").withRange(-63, 63).withDefault(18);
        case damping:
            return pmd().asFloat().withName("High Damping").withRange(0.1f, 1.f).withDefault(0.3f).withLinearScaleFormatting("").withDecimalPlaces(1);
        case mix:
        {
            std::unordered_map<int, std::string> m;
            for (int v = -63; v <= 63; ++v)
                m[v] = v < 0 ? "D" + std::to_string(-v) + ">W" : (v == 0 ? "D=W" : "D<W" + std::to_string(v));
            return pmd().asInt().withName("Mix").withRange(-63, 63).withDefault(-22).withUnorderedMapFormatting(m);
        }
        }
        return pmd().withName("Unused").withType(pmd::NONE);
    }

  private:
    void update()
    {
        bool changed = false;
        for (int i = 0; i < numParams; ++i)
        {
            const float v = this->floatValue(i);
            changed = changed || !(v == last[i]);
            last[i] = v;
        }
        if (!changed)
            return;
        const double rate = this->sampleRate();
        fb = std::lround(last[feedback]) / 63.5;
        // the cross delay's balance law (lr_hold_mix, lr_bal_m48: +7.04 / -24.94 dB wet/dry at v = +21 / -48)
        const Balance bal = msBalance((double)std::lround(last[mix]));
        dry = bal.dry, wet = bal.wet;
        const double g = dampingCoefficient(last[damping]);
        if (!designed || g != dampL.g)
        {
            designed = true;
            dampL.design(g, rate);
            dampR.design(g, rate);
        }
        // Each line's output tap and its loop, in seconds kept at the rate; the loop is shorter
        // by the damping filter's delay, so the output taps keep their time at every rate.
        outL.setLength(msLineSeconds(last[timeL]) * rate);
        lineL.setLength(msLineSeconds(last[fbTime1]) * rate - dampL.K);
        outR.setLength(msLineSeconds(last[timeR]) * rate);
        lineR.setLength(msLineSeconds(last[fbTime2]) * rate - dampR.K);
    }

    Line lineL, lineR; // each line's own tap is its loop
    Tap outL, outR;    // what each line plays on its channel
    Damping dampL, dampR;
    bool designed{false};
    std::vector<float> last; // sized to numParams at construction
    double fb{0}, dry{1}, wet{0};
};
} // namespace oscairn_fx::delay

#endif
