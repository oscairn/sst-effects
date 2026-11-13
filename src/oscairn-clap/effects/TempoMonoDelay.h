/*
 * Oscairn effects (doc/oscairn-plan.md). GPL-3.0-or-later, see ../OscairnEffects.cpp.
 *
 * A tempo-synced mono delay - "Tempo Mono Delay". Written for Oscairn; follows the
 * sst-effects effect contract. ONE recirculating line: the input (the mono mean of the two
 * inputs) feeds the line, which feeds itself back through the damping at the feedback gain every
 * note time plus Lag, and is read for the left channel L/R Diffusion later than that and for
 * the right channel L/R Diffusion earlier. Every channel's echoes therefore sit on the grid
 * {time + lag +- diffusion + k x (time + lag)} (tempo_mono and its review batch: the loop
 * spacing follows Lag, not Diffusion; a positive Diffusion puts the right echo first).
 *
 * Every law inside the line is the delay family's (effects/TempoCrossDelay.h, whose Line,
 * Tap, Damping and dampingCoefficient this mode uses unchanged): feedback v x 516 / 32768 per loop with
 * inverted polarity when negative; the two-tap damping g = 5 (1 - r) / 9 in the feedback
 * path only, its delay taken off the loop so the output taps keep their time; the cosine/sine balance with the tempo types' linear angle position (measured
 * for this type at +-21). The time law is the stereo type's (the note time cut to 0.1 ms, plus
 * the tap's Lag and Diffusion in ms, then the family's tenthsSamples at 44.1 kHz) with twice its
 * halving limit: 6 quarters at 120 BPM repeat after 3,000 ms. At other rates the lines keep their
 * length in seconds via the first-order all-pass. The EQ parameters are the family's output EQ
 * (OutputEq).
 */
#ifndef OSCAIRN_CLAP_EFFECTS_TEMPOMONODELAY_H
#define OSCAIRN_CLAP_EFFECTS_TEMPOMONODELAY_H

#include "effects/TempoCrossDelay.h"
#include "effects/TempoStereoDelay.h"

namespace oscairn_fx::delay
{
// This type's line holds twice the stereo type's: its note halves only when a tap would reach
// monoLimit samples (halvedNote), so the 6-quarter note plays its full 132,297 samples at
// 120 BPM (tmx_tmax), where the stereo type halves it. Only host tempos under about 62 BPM reach
// the limit, which the recorder cannot (known difference 45).
constexpr long monoLimit = 2 * lineLimit;
inline long monoNote(int note, double tempo, double longestOffsetMs)
{
    return halvedNote(noteTenths(note, tempo), std::lround(longestOffsetMs * 10), monoLimit);
}
inline double monoLineSeconds(long note, double offsetMs) { return (double)stereoLineSamples(note, offsetMs) / designRate; }
template <typename FXConfig> struct TempoMonoDelay : sst::effects::core::EffectTemplateBase<FXConfig>
{
    enum params
    {
        delayTime, // table #13 index, the note time of the loop and both taps
        feedback,  // -63..+63, v x 516 / 32768 per loop
        damping,   // 0.1..1.0, the family's two-tap in the feedback path
        diffusion, // -63..63 ms, the left tap that much later than the loop, the right earlier
        lag,       // -63..63 ms, on the loop and both taps
        mix,       // -63..+63, D>W .. D<W
        eqLowFreq, // the output EQ (OutputEq)
        eqLowGain,
        eqHighFreq,
        eqHighGain,
        numParamsEnum
    };
    static constexpr int numParams{numParamsEnum};
    static constexpr const char *streamingName{"oscairn-tempo-mono-delay"};
    static constexpr const char *displayName{"Tempo Mono Delay"};

    TempoMonoDelay(typename FXConfig::GlobalStorage *s, typename FXConfig::EffectStorage *e,
                   typename FXConfig::ValueStorage *p)
        : sst::effects::core::EffectTemplateBase<FXConfig>(s, e, p)
    {
        const int n = bufferSamples(this->sampleRate());
        line.resize(n);
        last.assign(numParams, NAN);
        eqOut.prepare(this->sampleRate(), s);
    }
    static constexpr int bufferSamples(double rate)
    {
        // every tap stays under monoLimit samples
        return (int)std::ceil(monoLimit / designRate * rate) + 2 * Damping::maxK + 8;
    }
    static size_t heapBytesAt(double rate) { return sizeof(float) * (size_t)bufferSamples(rate) + OutputEq::heapBytesAt(rate); }
    size_t heapBytes() const { return line.buf.size() * sizeof(float) + eqOut.heapBytes(); }
    // The longest silence between two echoes: the longer tap, plus the wrapper's 100 ms.
    double quietSeconds() const { return std::max(secL, secR) + 0.1; }

    void initialize()
    {
        line.clear();
        tapL.clear(), tapR.clear();
        damp.clear();
        eqOut.reset();
        std::fill(last.begin(), last.end(), NAN);
        lastTempo = NAN;
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
            const double oL = line.out(tapL), oR = line.out(tapR);
            // the mono mean (known difference 47) and the loop, read at the line's own tap
            const double x = 0.5 * (l + r) + fb * damp.run(line.out());
            line.write((float)x);
            L[i] = (float)(dry * l + wet * oL);
            R[i] = (float)(dry * r + wet * oR);
        }
        eqOut.process(L, R, FXConfig::blockSize);
    }


    sst::basic_blocks::params::ParamMetaData paramAt(int idx) const
    {
        using pmd = sst::basic_blocks::params::ParamMetaData;
        switch (idx)
        {
        case delayTime:
            return pmd().asInt().withName("Delay Time").withRange(0, 19).withDefault(11).withUnorderedMapFormatting(noteNames);
        case feedback: return pmd().asInt().withName("Feedback").withRange(-63, 63).withDefault(16);
        case damping:
            return pmd().asFloat().withName("High Damping").withRange(0.1f, 1.f).withDefault(1.f).withLinearScaleFormatting("").withDecimalPlaces(1);
        case diffusion:
            return pmd().asFloat().withName("L/R Diffusion").withRange(-63.f, 63.f).withDefault(8.f).withLinearScaleFormatting("ms").withDecimalPlaces(0);
        case lag: return pmd().asFloat().withName("Lag").withRange(-63.f, 63.f).withDefault(0.f).withLinearScaleFormatting("ms").withDecimalPlaces(0);
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

  private:
    void update()
    {
        bool changed = !(tempo() == lastTempo);
        for (int i = 0; i < numParams; ++i)
        {
            const float v = this->floatValue(i);
            changed = changed || !(v == last[i]);
            last[i] = v;
        }
        if (!changed)
            return;
        lastTempo = tempo();
        const double rate = this->sampleRate();
        fb = feedbackGain(last[feedback]);
        const Balance bal = tempoBalance((double)std::lround(last[mix]));
        dry = bal.dry, wet = bal.wet;
        eqOut.set(last[eqLowFreq], last[eqLowGain], last[eqHighFreq], last[eqHighGain], rate);
        const double g = dampingCoefficient(last[damping]);
        if (!designed || g != damp.g)
        {
            designed = true;
            damp.design(g, rate);
        }
        // The taps in seconds kept at the rate; the loop is shorter by the damping filter's
        // delay, so the output taps keep their time at every rate.
        const long note = monoNote((int)std::lround(last[delayTime]), lastTempo, last[lag] + std::abs(last[diffusion]));
        secL = monoLineSeconds(note, last[lag] + last[diffusion]);
        secR = monoLineSeconds(note, last[lag] - last[diffusion]);
        tapL.setLength(secL * rate);
        tapR.setLength(secR * rate);
        line.setLength(monoLineSeconds(note, last[lag]) * rate - damp.K);
    }

    double tempo() const
    {
        if constexpr (requires { this->globalStorage->tempo; })
            return this->globalStorage->tempo;
        return 120;
    }

    Line line; // its own tap is the loop
    Tap tapL, tapR;
    Damping damp;
    OutputEq eqOut;
    bool designed{false};
    std::vector<float> last; // sized to numParams at construction
    double lastTempo{NAN};
    double fb{0}, dry{1}, wet{0}, secL{0}, secR{0};
};
} // namespace oscairn_fx::delay

#endif
