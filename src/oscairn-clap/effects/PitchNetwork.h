/*
 * Oscairn effects (doc/oscairn-plan.md). GPL-3.0-or-later, see ../OscairnEffects.cpp.
 *
 * A two-voice pitch network and the Ensemble Detune mode on it (plan, section 15, "Ensemble
 * Detune"). Written for Oscairn; follows the sst-effects effect contract. Signal flow:
 *  - a first-order low shelf and a first-order high shelf on the input, dry and wet alike;
 *  - two lines, left and right, each fed its own channel, clipped to [-1, 1) and held as floats;
 *  - per line a voice: two taps swept by a saw ramp (the first at |d| + d x saw half-samples, the
 *    second at 2|d| + d x the saw half a turn on), so each tap's age moves by d / 65536 per sample
 *    and the voice plays 1 - d / 65536 times the pitch; each tap's gain a window that is zero where
 *    its ramp jumps (a folded saw), plus at one crossover a bump from a second folded saw;
 *  - the voices' sum clipped, spread across the outputs by two words, mixed with the dry input at
 *    the balance words and clipped.
 * Delays and the ramp's speed are held in samples and steps at 44.1 kHz and scaled to the running
 * rate, so every rate behaves as 44.1 kHz does (the taps read between samples there).
 */
#ifndef OSCAIRN_CLAP_EFFECTS_PITCHNETWORK_H
#define OSCAIRN_CLAP_EFFECTS_PITCHNETWORK_H

#include "ChorusFamily.h"

namespace oscairn_fx::chorus
{
// A saw in [-1, 1) moved by a phase and wrapped back into [-1, 1).
inline double wrapped(double x)
{
    const double t = x + 1;
    return t - 2 * std::floor(t * 0.5) - 1;
}
// A window from a folded saw: clip(2 (offset + |u|), 0, top), |u| = 1 read as top.
inline double window(double u, double offset)
{
    double a = std::abs(u);
    if (a == 1)
        a = top;
    const double v = (offset + a) + (offset + a);
    return v > top ? top : (v < 0 ? 0 : v);
}

// The ramp word for a voice playing `cents` off the input: trunc(65536 - 65536 r + 1/2) in single
// precision, r = 2^(cents / 1200) rounded to a float.
inline int rampWord(int cents)
{
    const float r = (float)std::pow(2.0, cents / 1200.0);
    return (int)(std::fmaf(-65536.f, r, 65536.f) + 0.5f);
}

template <typename FXConfig, int P, typename Derived> struct PitchNetwork : eq::SeriesEq<FXConfig, 2, P, Derived>
{
    using Base = eq::SeriesEq<FXConfig, 2, P, Derived>;
    PitchNetwork(typename FXConfig::GlobalStorage *s, typename FXConfig::EffectStorage *e, typename FXConfig::ValueStorage *p)
        : Base(s, e, p), line{std::vector<float>(bufferSamples(this->sampleRate())), std::vector<float>(bufferSamples(this->sampleRate()))}
    {
    }

    // The longest age at 44.1 kHz: a 50 ms initial delay and the second tap at three times the
    // widest half-ramp (1,920 samples at 50 cents).
    static constexpr int longest = 1 + 2205 + 2880 + 2;
    static int bufferSamples(double rate)
    {
        int n = 1;
        while (n < (int)std::ceil(longest * rate / rotary::designRate) + 4)
            n <<= 1;
        return n;
    }
    static size_t heapBytesAt(double rate) { return 2 * sizeof(float) * (size_t)bufferSamples(rate) + Base::heapBytesAt(rate); }
    size_t heapBytes() const { return (line[0].size() + line[1].size()) * sizeof(float) + Base::heapBytes(); }
    int getRingoutDecay() const { return -1; }
    double quietSeconds() const { return longest / rotary::designRate + 0.1; }

    void initialize()
    {
        for (auto &l : line)
            std::fill(l.begin(), l.end(), 0.f);
        w = 0;
        zeroRun = 0;
        for (auto &g : glides)
            g.now = NAN;
        for (auto &r : ramp)
            r.wave = Lfo::saw, r.bipolar = true, r.start(0);
        Base::initialize();
    }

    void processBlock(float *__restrict L, float *__restrict R)
    {
        Base::processBlock(L, R); // the EQ, and the settings when a parameter changed
        const double rate = this->sampleRate(), per = rotary::designRate / rate;
        for (auto &r : ramp)
            r.step = set.step * per;
        const int mask = (int)line[0].size() - 1;
        const double target[glideCount] = {set.depth[0], set.depth[1], set.bumpPhase[0], set.bumpPhase[1], set.bumpGain[0],
                                           set.bumpGain[1], set.near, set.far, set.wet, set.dry};
        bool settled = true;
        for (int k = 0; k < glideCount; ++k)
            settled = settled && glides[k].now == target[k];
        // Silence in, the lines empty and the glides settled: the block is silence out; only the
        // ramps and the write position move on.
        if (zeroRun > mask && settled && quiet(L, R))
        {
            for (auto &r : ramp)
                r.skip(FXConfig::blockSize);
            w = (w - FXConfig::blockSize) & mask;
            return;
        }
        for (int i = 0; i < FXConfig::blockSize; ++i)
        {
            w = (w - 1) & mask;
            if (!settled)
                for (int k = 0; k < glideCount; ++k)
                    glides[k].mix(target[k], per);
            double y[2];
            for (int v = 0; v < 2; ++v)
            {
                ramp[v].advance();
                const double saw = ramp[v].read(), d = glides[v].now, bump = glides[4 + v].now;
                const double b = window(wrapped(glides[2 + v].now + saw), -0.75), cross = window(wrapped(-1 + saw), -0.25);
                double g1 = cross + bump * b;
                if (v == 1) // the right voice's first gain is clipped, the left one's is not
                    g1 = clip(g1);
                const double g2 = (top - cross) + bump * b;
                const double x1 = tap(v, (std::abs(d) + d * saw) * 0.5 * 32768, rate, mask);
                const double x2 = tap(v, (std::abs(2 * d) + d * wrapped(-1 + saw)) * 0.5 * 32768, rate, mask);
                y[v] = clip(g1 * x1 + g2 * x2);
            }
            const double eL = L[i], eR = R[i], wet = glides[8].now, dry = glides[9].now, near = glides[6].now, far = glides[7].now;
            line[0][w] = (float)clip(eL);
            line[1][w] = (float)clip(eR);
            L[i] = (float)clip(4 * (wet * clip(2 * (near * y[0] + far * y[1])) + dry * eL));
            R[i] = (float)clip(4 * (wet * clip(2 * (near * y[1] + far * y[0])) + dry * eR));
            zeroRun = line[0][w] == 0 && line[1][w] == 0 && L[i] == 0 && R[i] == 0 ? zeroRun + 1 : 0;
        }
    }

  protected:
    // The settings, in the platform's words (x 1/32768) unless stated.
    struct Settings
    {
        double depth[2]{};      // each voice's ramp word (left, right)
        double bumpPhase[2]{};  // where its bump sits on the ramp
        double bumpGain[2]{};   // the bump's weight on both taps
        int age[2]{1, 1};       // both taps' nearest age at 44.1 kHz, before the ramp
        double near{0}, far{0}; // spread: a voice on its own side and on the other
        double wet{0}, dry{0};
        int step{256}; // the ramps' step at 44.1 kHz (2^-24 turns per sample)
    } set;

  private:
    // A tap `half` 44.1 kHz samples past the voice's age, read with linear interpolation.
    double tap(int v, double half, double rate, int mask) const
    {
        const double age = (set.age[v] + half) * rate / rotary::designRate;
        const int n = (int)age;
        const float *l = line[v].data();
        const double a = l[(w + n) & mask], b = l[(w + n + 1) & mask];
        return a + (age - n) * (b - a);
    }
    static bool quiet(const float *L, const float *R)
    {
        for (int i = 0; i < FXConfig::blockSize; ++i)
            if (L[i] != 0 || R[i] != 0)
                return false;
        return true;
    }
    static constexpr int glideCount = 10;
    std::vector<float> line[2];
    int w{0}, zeroRun{0};
    Lfo ramp[2];
    Glide glides[glideCount];
};

// Ensemble Detune: the left voice detuned down and the right up by the same cents, each line delayed
// by its initial delay, the voices spread by equal-power words from the dry/wet curve.
template <typename FXConfig> struct EnsembleDetune : PitchNetwork<FXConfig, 9, EnsembleDetune<FXConfig>>
{
    enum params
    {
        detune,
        delayLeft,
        delayRight,
        spread,
        lowFreq,
        lowGain,
        highFreq,
        highGain,
        mix,
        numParamsEnum
    };
    static constexpr int numParams{numParamsEnum};
    static constexpr const char *streamingName{"oscairn-ensemble-detune"};
    static constexpr const char *displayName{"Ensemble Detune"};
    using Base = PitchNetwork<FXConfig, 9, EnsembleDetune<FXConfig>>;
    using Base::Base;
    friend typename Base::Base;

    sst::basic_blocks::params::ParamMetaData paramAt(int idx) const
    {
        using pmd = sst::basic_blocks::params::ParamMetaData;
        auto ms = [](const char *n) {
            return pmd().asFloat().withName(n).withRange(0.f, 50.f).withDefault(0.f).withLinearScaleFormatting("ms").withDecimalPlaces(1);
        };
        switch (idx)
        {
        case detune: return pmd().asInt().withName("Detune").withRange(-50, 50).withDefault(10).withLinearScaleFormatting("cents");
        case delayLeft: return ms("Initial Delay L");
        case delayRight: return ms("Initial Delay R");
        case spread: return intParam("Spread", 0, 63, 32);
        case mix: return intParam("Mix", -63, 63, -63);
        default: return eqParam(idx - lowFreq);
        }
    }

  private:
    void design(double)
    {
        auto &l = this->last;
        auto &s = this->set;
        this->setActive(0, l[lowGain] != 0);
        this->setActive(1, l[highGain] != 0);
        // The insertion types' first-order shelves, held in 16-bit words with 2 and 4 integer bits.
        eq::firstOrderShelf(this->bands[0], l[lowFreq], l[lowGain], false);
        eq::firstOrderShelf(this->bands[1], l[highFreq], l[highGain], true);
        eq::roundToSteps(this->bands[0], 2, false);
        eq::roundToSteps(this->bands[1], 4, false);
        const int c = std::clamp((int)std::lround(l[detune]), -50, 50);
        s.depth[0] = rampWord(-c) * word; // left: down by c cents
        s.depth[1] = rampWord(c) * word;  // right: up
        for (int v = 0; v < 2; ++v)
        {
            s.bumpPhase[v] = s.depth[v] > 0 ? -0.5 : (s.depth[v] < 0 ? 0.5 : 0);
            s.bumpGain[v] = c != 0 ? 12288 * word : 0;
        }
        s.age[0] = 1 + tenthsToSamples(l[delayLeft]);
        s.age[1] = 1 + tenthsToSamples(l[delayRight]);
        const int p = std::clamp((int)std::lround(l[spread]), 0, 63);
        s.near = (balanceWord(64 + p) >> 1) * word;
        s.far = (balanceWord(64 - p) >> 1) * word;
        const int v = std::clamp((int)std::lround(l[mix]) + 64, 1, 127);
        s.wet = balanceGain(v, 0);
        s.dry = balanceGain(128 - v, 0);
    }
};
} // namespace oscairn_fx::chorus

#endif
