/*
 * Oscairn effects (doc/oscairn-plan.md). GPL-3.0-or-later, see ../OscairnEffects.cpp.
 *
 * A delay with one recirculating line and three output taps - "L/C/R Delay". Written for
 * Oscairn; follows the sst-effects effect contract. The parameter set has three delay times
 * (L, R and centre) and one feedback time: the input feeds one line, which answers on the left
 * Time L after it, on the right Time R after it, on both channels Time C after it at
 * (Level C / 127)^2, and feeds itself back through the damping every Feedback Time at
 * the feedback gain. Each channel's echo grid is {its time + k x Feedback Time} together with
 * {Time C + k x Feedback Time} (plan, section 15, "L/C/R Delay").
 *
 * The line laws are the delay family's (effects/TempoCrossDelay.h: Line, Damping,
 * dampingCoefficient, msLineSeconds) and the balance is the cross delay's
 * (msBalance). The line's feed is the mean of the two inputs: a left-only source echoes on
 * both channels 6.02 dB under a one-channel feed (delay-eq-2026-11-12 lcr_st). The EQ parameters
 * are the family's output EQ (OutputEq).
 */
#ifndef OSCAIRN_CLAP_EFFECTS_LCRDELAY_H
#define OSCAIRN_CLAP_EFFECTS_LCRDELAY_H

#include "effects/TempoCrossDelay.h"

namespace oscairn_fx::delay
{
template <typename FXConfig> struct LcrDelay : sst::effects::core::EffectTemplateBase<FXConfig>
{
    enum params
    {
        timeL,    // ms, the left tap
        timeR,    // ms, the right tap
        timeC,    // ms, the centre tap (both channels)
        fbTime,   // ms, the recirculation tap: the spacing every channel's grid repeats on
        feedback, // -63..+63, v x 516 / 32768 per loop
        levelC,   // 0..127, the centre tap's level, (raw / 127)^2
        damping,  // 0.1..1.0, the family's two-tap in the feedback path
        mix,      // -63..+63, D>W .. D<W
        eqLowFreq, // the output EQ (OutputEq)
        eqLowGain,
        eqHighFreq,
        eqHighGain,
        numParamsEnum
    };
    static constexpr int numParams{numParamsEnum};
    static constexpr const char *streamingName{"oscairn-lcr-delay"};
    static constexpr const char *displayName{"L/C/R Delay"};

    LcrDelay(typename FXConfig::GlobalStorage *s, typename FXConfig::EffectStorage *e, typename FXConfig::ValueStorage *p)
        : sst::effects::core::EffectTemplateBase<FXConfig>(s, e, p)
    {
        const int n = bufferSamples(this->sampleRate());
        line.resize(n);
        last.assign(numParams, NAN);
        eqOut.prepare(this->sampleRate(), s);
    }
    static constexpr int bufferSamples(double rate) { return (int)std::ceil(maxLineMs / 1000 * rate) + 2 * Damping::maxK + 8; }
    static size_t heapBytesAt(double rate) { return sizeof(float) * (size_t)bufferSamples(rate) + OutputEq::heapBytesAt(rate); }
    size_t heapBytes() const { return line.buf.size() * sizeof(float) + eqOut.heapBytes(); }
    // The longest silence between two echoes: the longest tap or the loop spacing, plus the
    // wrapper's 100 ms.
    double quietSeconds() const
    {
        return std::max({last[timeL], last[timeR], last[timeC], last[fbTime]}) / 1000 + 0.1;
    }

    void initialize()
    {
        line.clear();
        tapL.clear(), tapR.clear(), tapC.clear();
        damp.clear();
        eqOut.reset();
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
            const double oL = line.out(tapL), oR = line.out(tapR), oC = line.out(tapC);
            const double x = 0.5 * (l + r) + fb * damp.run(line.out()); // the loop at the line's own tap
            line.write((float)x);
            const double c = gainC * oC;
            L[i] = (float)(dry * l + wet * (oL + c));
            R[i] = (float)(dry * r + wet * (oR + c));
        }
        eqOut.process(L, R, FXConfig::blockSize);
    }

    sst::basic_blocks::params::ParamMetaData paramAt(int idx) const
    {
        using pmd = sst::basic_blocks::params::ParamMetaData;
        auto ms = [&](const char *n, float d) {
            return pmd().asFloat().withName(n).withRange(0.1f, 1638.3f).withDefault(d).withLinearScaleFormatting("ms").withDecimalPlaces(1);
        };
        switch (idx)
        {
        case timeL: return ms("Time L", 333.3f);
        case timeR: return ms("Time R", 166.7f);
        case timeC: return ms("Time C", 500.f);
        case fbTime: return ms("Feedback Time", 500.f);
        case feedback: return pmd().asInt().withName("Feedback").withRange(-63, 63).withDefault(10);
        case levelC: return pmd().asInt().withName("Level C").withRange(0, 127).withDefault(100);
        case damping:
            return pmd().asFloat().withName("High Damping").withRange(0.1f, 1.f).withDefault(1.f).withLinearScaleFormatting("").withDecimalPlaces(1);
        case mix:
        {
            std::unordered_map<int, std::string> m;
            for (int v = -63; v <= 63; ++v)
                m[v] = v < 0 ? "D" + std::to_string(-v) + ">W" : (v == 0 ? "D=W" : "D<W" + std::to_string(v));
            return pmd().asInt().withName("Mix").withRange(-63, 63).withDefault(-32).withUnorderedMapFormatting(m);
        }
        }
        if (idx >= eqLowFreq && idx <= eqHighGain)
            return eqParamAt(idx - eqLowFreq);
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
        fb = feedbackGain(last[feedback]);
        const double lc = std::lround(last[levelC]) / 127.0;
        gainC = lc * lc;
        const Balance bal = msBalance((double)std::lround(last[mix]));
        dry = bal.dry, wet = bal.wet;
        eqOut.set(last[eqLowFreq], last[eqLowGain], last[eqHighFreq], last[eqHighGain], rate);
        const double g = dampingCoefficient(last[damping]);
        if (!designed || g != damp.g)
        {
            designed = true;
            damp.design(g, rate);
        }
        // The taps in seconds kept at the rate; the feedback tap is shorter by the damping
        // filter's delay, so the output taps keep their time down to 0.1 ms at every rate.
        tapL.setLength(msLineSeconds(last[timeL]) * rate);
        tapR.setLength(msLineSeconds(last[timeR]) * rate);
        tapC.setLength(msLineSeconds(last[timeC]) * rate);
        line.setLength(msLineSeconds(last[fbTime]) * rate - damp.K);
    }

    Line line; // its own tap is the recirculation
    Tap tapL, tapR, tapC;
    Damping damp;
    OutputEq eqOut;
    bool designed{false};
    std::vector<float> last; // sized to numParams at construction
    double fb{0}, gainC{0}, dry{1}, wet{0};
};
} // namespace oscairn_fx::delay

#endif
