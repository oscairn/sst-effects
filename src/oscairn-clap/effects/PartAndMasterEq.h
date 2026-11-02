/*
 * Oscairn effects (doc/oscairn-plan.md). GPL-3.0-or-later, see ../OscairnEffects.cpp.
 *
 * Two more EQ designs, written for Oscairn and following the sst-effects effect contract; laws
 * fitted to reference renders at 44.1 kHz (plan, section 15, M1). Frequencies are in Hz; the
 * converter maps the platform's frequency indices to Hz.
 *
 * Three-band EQ: a low shelf, a peak and a high shelf in series. The shelves follow the
 * five-band EQ's law (slope 0.5, the set frequency as the corner on the flat side); the peak has
 * an effective Q of Q * 10^(-|gain| / 40), kept in octaves of bandwidth near Nyquist.
 *
 * Master EQ: five bands in series (not a network). Band 1 is a low shelf (the shared law) or a
 * peak, bands 2-4 peaks, band 5 a high shelf or a peak. Peaks have an effective Q of
 * Q * 10^(-|gain| / 40) with no widening; the Q has no effect on the shelves. The high shelf has
 * Q 0.5 at the set frequency divided by sqrt(2) in the prewarped domain.
 */
#ifndef OSCAIRN_CLAP_EFFECTS_PARTANDMASTEREQ_H
#define OSCAIRN_CLAP_EFFECTS_PARTANDMASTEREQ_H

#include "FiveBandEq.h"

namespace oscairn_fx::eq
{
// The three-band EQ's peak keeps its bandwidth in octaves near Nyquist (the cookbook's bandwidth
// form, measured: plan, section 15): the factor on its effective Q qc at hz, from the 44.1 kHz
// design.
inline double midWidening(double hz, double qc)
{
    const double w0 = 2 * M_PI * std::min(hz, 0.49 * designRate) / designRate;
    const double bw = 2 / std::log(2.0) * std::asinh(1 / (2 * qc));
    const double qd = 1 / (2 * std::sinh(std::log(2.0) / 2 * bw * w0 / std::sin(w0)));
    return qd / qc;
}

inline void masterHighShelf(Biquad &f, double rate, double hz, double gainDb)
{
    const double G = std::pow(10.0, gainDb / 40);
    // Designed at 44.1 kHz, its centre kept in Hz at other rates.
    const double centre = std::atan(std::tan(M_PI * std::min(hz, 0.49 * designRate) / designRate) / std::sqrt(2.0)) * designRate / M_PI;
    const double w0 = 2 * M_PI * std::min(centre, 0.49 * rate) / rate;
    const double q = 0.5, c = std::cos(w0), s2 = 2 * std::sqrt(G) * std::sin(w0) / (2 * q);
    f.set(G * ((G + 1) + (G - 1) * c + s2), -2 * G * ((G - 1) + (G + 1) * c),
          G * ((G + 1) + (G - 1) * c - s2), (G + 1) - (G - 1) * c + s2,
          2 * ((G - 1) - (G + 1) * c), (G + 1) - (G - 1) * c - s2);
}

// Common machinery: N sections in series, rebuilt when a parameter changes.
template <typename FXConfig, int N, int P, typename Derived>
struct SeriesEq : sst::effects::core::EffectTemplateBase<FXConfig>
{
    SeriesEq(typename FXConfig::GlobalStorage *s, typename FXConfig::EffectStorage *e,
             typename FXConfig::ValueStorage *p)
        : sst::effects::core::EffectTemplateBase<FXConfig>(s, e, p)
    {
        if constexpr (requires { s->worker; s->offline; })
            cascade.prepare(this->sampleRate(), s->worker, s->offline);
        else
            cascade.prepare(this->sampleRate(), nullptr, nullptr);
    }
    void initialize()
    {
        for (auto &b : bands)
            b.reset();
        cascade.reset();
        last.fill(NAN);
        update();
    }
    void suspendProcessing() { initialize(); }
    int getRingoutDecay() const { return 32; }
    void onSampleRateChanged() { initialize(); }
    void processBlock(float *__restrict L, float *__restrict R)
    {
        update();
        if (matched)
            return cascade.process(L, R, FXConfig::blockSize, gain);
        for (int i = 0; i < FXConfig::blockSize; ++i)
        {
            float l = L[i], r = R[i];
            for (int b = 0; b < N; ++b)
                if (active[b])
                {
                    l = bands[b].run(0, l);
                    r = bands[b].run(1, r);
                }
            L[i] = l * gain;
            R[i] = r * gain;
        }
    }

    static size_t heapBytesAt(double rate) { return decltype(cascade)::heapBytesAt(rate); }
    size_t heapBytes() const { return cascade.heapBytes(); }

  protected:
    std::array<Biquad, N> bands;
    std::array<bool, N> active{};
    std::array<float, P> last{};
    float gain{1};
    bool matched{false};         // at another rate than 44.1 kHz the cascade runs instead
    MatchedCascade<N> cascade;
    void update()
    {
        bool changed = false;
        for (int i = 0; i < P; ++i)
        {
            const float v = this->floatValue(i);
            changed = changed || !(v == last[i]);
            last[i] = v;
        }
        if (!changed)
            return;
        // Designed at 44.1 kHz; at another rate the active sections are matched to that design.
        static_cast<Derived *>(this)->design(designRate);
        matched = this->sampleRate() != designRate;
        if (matched)
        {
            const Biquad *design[N]{};
            for (int b = 0; b < N; ++b)
                design[b] = active[b] ? &bands[b] : nullptr;
            cascade.set(design);
        }
    }
    void setActive(int b, bool on)
    {
        if (on && !active[b])
            bands[b].reset();
        active[b] = on;
    }
};

template <typename FXConfig> struct ThreeBandEq : SeriesEq<FXConfig, 3, 7, ThreeBandEq<FXConfig>>
{
    enum params
    {
        lowFreq,
        lowGain,
        midFreq,
        midGain,
        midQ,
        highFreq,
        highGain,
        numParamsEnum
    };
    static constexpr int numParams{numParamsEnum};
    static constexpr const char *streamingName{"oscairn-three-band-eq"};
    static constexpr const char *displayName{"Three-band EQ"};
    using SeriesEq<FXConfig, 3, 7, ThreeBandEq<FXConfig>>::SeriesEq;
    static_assert(numParamsEnum == 7);

    friend struct SeriesEq<FXConfig, 3, 7, ThreeBandEq<FXConfig>>;
    sst::basic_blocks::params::ParamMetaData paramAt(int idx) const
    {
        using pmd = sst::basic_blocks::params::ParamMetaData;
        auto hz = [](const char *n, float lo, float hi, float d) {
            return pmd().asFloat().withName(n).withRange(lo, hi).withDefault(d).withLinearScaleFormatting("Hz").withDecimalPlaces(1);
        };
        auto db = [](const char *n) {
            return pmd().asFloat().withName(n).withRange(-12, 12).withDefault(0).withLinearScaleFormatting("dB").withDecimalPlaces(2);
        };
        switch (idx)
        {
        case lowFreq: return hz("Low Frequency", 20, 5000, 200);
        case lowGain: return db("Low Gain");
        case midFreq: return hz("Mid Frequency", 20, 20000, 1000);
        case midGain: return db("Mid Gain");
        case midQ:
            return pmd().asFloat().withName("Mid Q").withRange(0.5f, 12.f).withDefault(0.7f).withLinearScaleFormatting("").withDecimalPlaces(2);
        case highFreq: return hz("High Frequency", 500, 20000, 8000);
        case highGain: return db("High Gain");
        }
        return pmd().withName("Unused").withType(pmd::NONE);
    }
    void design(double rate)
    {
        auto &l = this->last;
        this->setActive(0, l[lowGain] != 0);
        this->setActive(1, l[midGain] != 0);
        this->setActive(2, l[highGain] != 0);
        shelf(this->bands[0], rate, l[lowFreq], l[lowGain], false);
        const double qc = l[midQ] * std::pow(10.0, -std::abs(l[midGain]) / 40);
        peaking(this->bands[1], rate, l[midFreq], l[midGain], l[midQ] * midWidening(l[midFreq], qc), false);
        shelf(this->bands[2], rate, l[highFreq], l[highGain], true);
        this->gain = 1;
    }
};

template <typename FXConfig> struct MasterEq : SeriesEq<FXConfig, 5, 17, MasterEq<FXConfig>>
{
    enum params
    {
        gain1, freq1, q1, shape1,
        gain2, freq2, q2,
        gain3, freq3, q3,
        gain4, freq4, q4,
        gain5, freq5, q5, shape5,
        numParamsEnum
    };
    static constexpr int numParams{numParamsEnum};
    static constexpr const char *streamingName{"oscairn-master-eq"};
    static constexpr const char *displayName{"Master EQ"};
    using SeriesEq<FXConfig, 5, 17, MasterEq<FXConfig>>::SeriesEq;
    static_assert(numParamsEnum == 17);

    sst::basic_blocks::params::ParamMetaData paramAt(int idx) const
    {
        using pmd = sst::basic_blocks::params::ParamMetaData;
        auto hz = [](std::string n, float lo, float hi, float d) {
            return pmd().asFloat().withName(n).withRange(lo, hi).withDefault(d).withLinearScaleFormatting("Hz").withDecimalPlaces(1);
        };
        auto db = [](std::string n) {
            return pmd().asFloat().withName(n).withRange(-12, 12).withDefault(0).withLinearScaleFormatting("dB").withDecimalPlaces(1);
        };
        auto q = [](std::string n) {
            return pmd().asFloat().withName(n).withRange(0.1f, 12.f).withDefault(0.7f).withLinearScaleFormatting("").withDecimalPlaces(2);
        };
        auto shape = [](std::string n) {
            return pmd().asInt().withName(n).withRange(0, 1).withDefault(0).withUnorderedMapFormatting({{0, "Shelf"}, {1, "Peak"}});
        };
        const float def[5] = {125, 400, 1000, 4000, 8000};
        const float lo[5] = {20, 20, 20, 20, 500}, hi[5] = {2000, 20000, 20000, 20000, 20000};
        const int band = idx < 4 ? 0 : (idx < 13 ? 1 + (idx - 4) / 3 : 4);
        const std::string b = "Band " + std::to_string(band + 1) + " ";
        if (idx == shape1 || idx == shape5)
            return shape(b + "Shape");
        const int k = idx < 4 ? idx : (idx < 13 ? (idx - 4) % 3 : idx - 13);
        if (k == 0)
            return db(b + "Gain");
        if (k == 1)
            return hz(b + "Frequency", lo[band], hi[band], def[band]);
        return q(b + "Q");
    }
    void design(double rate)
    {
        auto &l = this->last;
        const int g[5] = {gain1, gain2, gain3, gain4, gain5}, f[5] = {freq1, freq2, freq3, freq4, freq5},
                  qq[5] = {q1, q2, q3, q4, q5};
        for (int b = 0; b < 5; ++b)
        {
            const double gain = l[g[b]], fr = l[f[b]], q = l[qq[b]];
            this->setActive(b, gain != 0);
            const bool peak = (b != 0 && b != 4) || std::lround(l[b == 0 ? shape1 : shape5]) == 1;
            if (peak)
                peaking(this->bands[b], rate, fr, gain, q, false);
            else if (b == 0)
                shelf(this->bands[b], rate, fr, gain, false);
            else
                masterHighShelf(this->bands[b], rate, fr, gain);
        }
        this->gain = 1;
    }
};
} // namespace oscairn_fx::eq

#endif
