/*
 * Oscairn effects (doc/oscairn-plan.md). GPL-3.0-or-later, see ../OscairnEffects.cpp.
 *
 * A hall reverb whose decay statistics follow reference renders (plan, section 15, "Hall
 * reverb"). Written for Oscairn; follows the sst-effects effect contract. It matches measured
 * statistics, not the reference's structure:
 *  - per octave band (125 Hz to 8 kHz) the decay time is 1 / T = 1 / Tf(time, size) + eh(high
 *    ratio) + el(low ratio), with Tf, eh and el tables measured from renders (fx19);
 *  - the reverb starts at the initial delay plus a room-size dependent onset (fx17, fx19);
 *  - the HPF is first order, the LPF 0.4 + 0.6 times a first-order low-pass (fx19); 20 Hz and
 *    20 kHz switch them off;
 *  - the level follows an eight-line feedback delay network's own energy, corrected by measured
 *    offsets for the time, diffusion and decay parameters (fx19).
 * Each line's loop filter is a seven-band graphic EQ giving that band's decay time; everything is
 * designed in seconds and hertz, so the statistics are the same at every rate.
 */
#ifndef OSCAIRN_CLAP_EFFECTS_HALLREVERB_H
#define OSCAIRN_CLAP_EFFECTS_HALLREVERB_H

#include "sst/effects/EffectCore.h"
#include "sst/basic-blocks/params/ParamMetadata.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace oscairn_fx::hall
{
constexpr int bands = 7, lines = 8;
constexpr double bandHz[bands] = {125, 250, 500, 1000, 2000, 4000, 8000};

// Measured (fx19, 44.1 kHz renders of a sampled click, analysed with the measurement repository's
// fx/reverb.py): the decay time without high-frequency damping (s) at these displayed reverb
// times and room sizes.
constexpr double timeAt[10] = {0.3, 1.1, 1.9, 2.7, 3.5, 4.3, 5.5, 9.5, 13.5, 18.0};
constexpr double sizeAt[6] = {0, 8, 15, 22, 29, 31};
constexpr double flatDecay[10][6] = {
    {0.171, 0.356, 0.518, 0.710, 0.857, 0.904}, {0.182, 0.380, 0.558, 0.767, 0.929, 0.997},
    {0.419, 0.771, 1.077, 1.468, 1.775, 1.876}, {0.530, 1.023, 1.494, 2.038, 2.530, 2.618},
    {0.850, 1.307, 1.825, 2.410, 3.057, 3.138}, {1.266, 1.784, 2.430, 3.003, 3.616, 3.944},
    {1.400, 2.000, 2.720, 3.381, 4.160, 4.513}, {1.731, 2.399, 3.253, 4.009, 4.995, 5.371},
    {1.873, 2.683, 3.917, 4.951, 6.311, 6.725}, {2.914, 3.501, 5.313, 6.987, 9.023, 9.619}};
// Extra decay rate (1/s) per band for high ratio 0.1 ... 1.0 (the 125 Hz band takes 250 Hz's).
constexpr double highRate[10][bands] = {
    {0.030, 0.030, 0.042, 0.128, 0.237, 0.340, 0.363}, {0.027, 0.027, 0.027, 0.079, 0.170, 0.277, 0.366},
    {0.022, 0.022, 0.016, 0.053, 0.124, 0.222, 0.346}, {0.018, 0.018, 0.009, 0.037, 0.093, 0.182, 0.324},
    {0.021, 0.021, 0.008, 0.022, 0.064, 0.137, 0.285}, {0.021, 0.021, 0.006, 0.016, 0.050, 0.115, 0.257},
    {0.017, 0.017, 0.005, 0.012, 0.039, 0.097, 0.231}, {0.016, 0.016, 0.004, 0.009, 0.031, 0.083, 0.209},
    {0.015, 0.015, 0.000, -0.002, -0.003, 0.012, 0.055}, {0.016, 0.016, 0.000, -0.004, -0.009, -0.004, 0.002}};
// Extra decay rate per band for low ratio 0.1, 0.4, 0.7, 1.0 and 1.4, relative to 0.7.
constexpr double lowAt[5] = {0.1, 0.4, 0.7, 1.0, 1.4};
constexpr double lowRate[5][bands] = {{0.0088, 0.0088, 0.0132, 0.0183, 0.0152, 0.0103, 0.0047},
                                      {0.0081, 0.0081, 0.0094, 0.0101, 0.0068, 0.0042, 0.0018},
                                      {0, 0, 0, 0, 0, 0, 0},
                                      {-0.0124, -0.0124, -0.0154, -0.0108, -0.0049, -0.0025, -0.0011},
                                      {-0.0308, -0.0308, -0.0333, -0.0185, -0.0080, -0.0041, -0.0018}};
// The reverb's onset after the initial delay (ms), by room size.
constexpr double onsetMs[6] = {23.8, 31.2, 37.3, 44.0, 48.4, 52.7};
// Level corrections (dB) to the network's own energy: by reverb time, high and low ratio,
// diffusion and decay.
constexpr double timeLevel[10] = {3.28, 3.46, 1.25, 0.47, 0.21, -0.30, -0.36, -0.29, -0.16, -0.45};
constexpr double highLevel[10] = {-6.92, -4.77, -3.37, -2.35, -1.28, -0.76, -0.34, 0, 1.89, 2.32};
constexpr double lowLevel[5] = {-1.12, -0.43, 0, 0.25, 0.40};
constexpr double diffusionAt[5] = {0, 2, 5, 8, 10};
constexpr double diffusionLevel[5] = {-1.267, -1.906, -1.974, -0.988, 0};
constexpr double decayAt[5] = {0, 16, 32, 55, 63};
constexpr double decayLevel[5] = {-2.490, -2.043, -1.192, 0, 0.396};
constexpr double levelOffset = -9.82;
// The decay parameter moves the reverb's onset (ms, relative to its value at 55).
constexpr double decayOnset[5] = {-57.1, -44.5, -31.8, 0, 34.5};

// Line lengths (s) at room size 31; smaller rooms scale them down to 0.35 of these at size 0.
constexpr double lineSeconds[lines] = {0.0397, 0.0431, 0.0459, 0.0491, 0.0523, 0.0557, 0.0593, 0.0631};
constexpr double allpassSeconds[4] = {0.0047, 0.0036, 0.0127, 0.0093};
constexpr double maxPreSeconds = 0.2 + 0.0527 + 0.0345 + 0.001;

inline double interp(const double *x, const double *y, int n, double v)
{
    if (v <= x[0])
        return y[0];
    for (int i = 1; i < n; ++i)
        if (v <= x[i])
            return y[i - 1] + (y[i] - y[i - 1]) * (v - x[i - 1]) / (x[i] - x[i - 1]);
    return y[n - 1];
}

// Line lengths scale with room size: 0.35 of the longest at size 0, all of it at 31.
inline double lineScale(double size) { return 0.35 + 0.65 * size / 31; }
inline double sizeRateScale(double size) { return std::sqrt(lineScale(29) / lineScale(size)); }

// The decay time (s) per band for the parameters.
inline std::array<double, bands> decayTimes(double timeS, double size, double highRatio, double lowRatio)
{
    double col[10];
    for (int t = 0; t < 10; ++t)
        col[t] = interp(sizeAt, flatDecay[t], 6, size);
    const double tf = interp(timeAt, col, 10, timeS);
    const int h = std::clamp((int)std::lround(highRatio * 10), 1, 10) - 1;
    std::array<double, bands> out{};
    for (int b = 0; b < bands; ++b)
    {
        double lr[5];
        for (int k = 0; k < 5; ++k)
            lr[k] = lowRate[k][b];
        // the damping rates were measured at size 29; per second they grow as the lines shorten
        const double rate = 1 / tf + (highRate[h][b] + interp(lowAt, lr, 5, lowRatio)) * sizeRateScale(size);
        out[b] = 1 / std::max(rate, 1 / 60.0);
    }
    return out;
}

inline double onsetSeconds(double initialDelayMs, double size, double decay)
{
    return std::max(0.0, initialDelayMs + interp(sizeAt, onsetMs, 6, size) + interp(decayAt, decayOnset, 5, decay)) / 1000;
}

struct Section
{
    double b0{1}, b1{0}, b2{0}, a1{0}, a2{0}, z1{0}, z2{0};
    inline double run(double x)
    {
        const double y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
    // Response in dB at hz.
    double db(double hz, double rate) const
    {
        const double w = 2 * M_PI * hz / rate;
        const double cr = std::cos(w), ci = -std::sin(w), c2r = std::cos(2 * w), c2i = -std::sin(2 * w);
        const double nr = b0 + b1 * cr + b2 * c2r, ni = b1 * ci + b2 * c2i;
        const double dr = 1 + a1 * cr + a2 * c2r, di = a1 * ci + a2 * c2i;
        return 10 * std::log10((nr * nr + ni * ni) / (dr * dr + di * di));
    }
};

// Graphic EQ band k at gain g dB: a low shelf for 125 Hz, a high shelf for 8 kHz, peaks (Q 0.7)
// between (cookbook designs at the running rate).
inline void bandSection(Section &s, int k, double g, double rate)
{
    const double A = std::pow(10.0, g / 40);
    const double f = k == 0 ? bandHz[0] * M_SQRT2 : (k == bands - 1 ? bandHz[bands - 1] / M_SQRT2 : bandHz[k]);
    const double w = 2 * M_PI * std::min(f, 0.45 * rate) / rate, c = std::cos(w), sn = std::sin(w);
    double b[3], a[3];
    if (k == 0 || k == bands - 1)
    {
        const double sq = 2 * std::sqrt(A) * sn / M_SQRT2;
        const double sg = k == 0 ? -1 : 1;
        b[0] = A * ((A + 1) + sg * (A - 1) * c + sq);
        b[1] = -sg * 2 * A * ((A - 1) + sg * (A + 1) * c);
        b[2] = A * ((A + 1) + sg * (A - 1) * c - sq);
        a[0] = (A + 1) - sg * (A - 1) * c + sq;
        a[1] = sg * 2 * ((A - 1) - sg * (A + 1) * c);
        a[2] = (A + 1) - sg * (A - 1) * c - sq;
    }
    else
    {
        const double al = sn / (2 * 0.7);
        b[0] = 1 + al * A, b[1] = -2 * c, b[2] = 1 - al * A;
        a[0] = 1 + al / A, a[1] = -2 * c, a[2] = 1 - al / A;
    }
    s.b0 = b[0] / a[0], s.b1 = b[1] / a[0], s.b2 = b[2] / a[0], s.a1 = a[1] / a[0], s.a2 = a[2] / a[0];
}

template <typename FXConfig> struct HallReverb : sst::effects::core::EffectTemplateBase<FXConfig>
{
    enum params
    {
        time,
        diffusion,
        initialDelay,
        hpf,
        lpf,
        roomSize,
        highRatio,
        lowRatio,
        decay,
        numParamsEnum
    };
    static constexpr int numParams{numParamsEnum};
    static constexpr const char *streamingName{"oscairn-hall-reverb"};
    static constexpr const char *displayName{"Hall A"};

    HallReverb(typename FXConfig::GlobalStorage *s, typename FXConfig::EffectStorage *e,
               typename FXConfig::ValueStorage *p)
        : sst::effects::core::EffectTemplateBase<FXConfig>(s, e, p)
    {
        const double rate = this->sampleRate();
        pre.assign(samples(maxPreSeconds, rate) + 2, 0.f);
        for (int i = 0; i < lines; ++i)
            line[i].assign(samples(lineSeconds[i], rate) + 2, 0.f);
        for (int k = 0; k < 4; ++k)
            ap[k].assign(samples(allpassSeconds[k], rate) + 2, 0.f);
        // the graphic EQ's interaction matrix at this rate (response in dB per dB of gain)
        for (int k = 0; k < bands; ++k)
        {
            Section s;
            bandSection(s, k, -3, rate);
            for (int j = 0; j < bands; ++j)
                interaction[j][k] = s.db(bandHz[j], rate) / -3;
        }
    }
    static int samples(double s, double rate) { return (int)std::ceil(s * rate); }
    static size_t heapBytesAt(double rate)
    {
        size_t n = (size_t)samples(maxPreSeconds, rate) + 2;
        for (double l : lineSeconds)
            n += (size_t)samples(l, rate) + 2;
        for (double a : allpassSeconds)
            n += (size_t)samples(a, rate) + 2;
        return n * sizeof(float);
    }
    size_t heapBytes() const
    {
        size_t n = pre.size();
        for (auto &l : line)
            n += l.size();
        for (auto &a : ap)
            n += a.size();
        return n * sizeof(float);
    }
    double quietSeconds() const { return 0.1 + onset; }

    void initialize()
    {
        std::fill(pre.begin(), pre.end(), 0.f);
        for (auto &l : line)
            std::fill(l.begin(), l.end(), 0.f);
        for (auto &a : ap)
            std::fill(a.begin(), a.end(), 0.f);
        for (auto &l : eq)
            for (auto &s : l)
                s.z1 = s.z2 = 0;
        hpState = lpState = hpX = lpX = 0;
        last.fill(NAN);
        update();
    }
    void suspendProcessing() { initialize(); }
    int getRingoutDecay() const { return -1; }
    void onSampleRateChanged() { initialize(); }

    void processBlock(float *__restrict L, float *__restrict R)
    {
        update();
        const int preN = (int)pre.size();
        for (int i = 0; i < FXConfig::blockSize; ++i)
        {
            double x = 0.5 * ((double)L[i] + R[i]);
            if (hpOn)
            {
                const double y = hpA * (x - hpX) + hpR * hpState; // first-order high-pass, bilinear
                hpX = x;
                hpState = y;
                x = y;
            }
            if (lpOn)
            {
                const double y = lpB * (x + lpX) - lpA1 * lpState;
                lpX = x;
                lpState = y;
                x = 0.4 * x + 0.6 * y;
            }
            pre[(size_t)preW] = (float)x;
            auto tap = [&](int d) { int r = preW - d; return (double)pre[(size_t)(r < 0 ? r + preN : r)]; };
            const double early = tap(earlyDelay);
            double u = tap(netDelay);
            if (++preW == preN)
                preW = 0;
            for (int k = 0; k < 4; ++k)
            {
                auto &b = ap[k];
                int r = apW[k] - apLen[k];
                if (r < 0)
                    r += (int)b.size();
                const double d = b[(size_t)r], w = u + apG * d;
                b[(size_t)apW[k]] = (float)w;
                if (++apW[k] == (int)b.size())
                    apW[k] = 0;
                u = d - apG * w;
            }
            double v[lines];
            for (int j = 0; j < lines; ++j)
            {
                auto &b = line[j];
                int r = lineW[j] - lineLen[j];
                if (r < 0)
                    r += (int)b.size();
                double s = b[(size_t)r];
                for (auto &sec : eq[j])
                    s = sec.run(s);
                v[j] = s;
            }
            const double l = v[0] - v[2] + v[4] - v[6], r = v[1] - v[3] + v[5] - v[7];
            L[i] = (float)(gain * (l + early));
            R[i] = (float)(gain * (r + early));
            // eight-point Hadamard mixing, normalized
            double h[lines];
            for (int j = 0; j < lines; ++j)
                h[j] = v[j];
            for (int len = 1; len < lines; len <<= 1)
                for (int s0 = 0; s0 < lines; s0 += 2 * len)
                    for (int j = s0; j < s0 + len; ++j)
                    {
                        const double a = h[j], c = h[j + len];
                        h[j] = a + c;
                        h[j + len] = a - c;
                    }
            for (int j = 0; j < lines; ++j)
            {
                auto &b = line[j];
                b[(size_t)lineW[j]] = (float)(h[j] / std::sqrt((double)lines) + u);
                if (++lineW[j] == (int)b.size())
                    lineW[j] = 0;
            }
        }
    }

    sst::basic_blocks::params::ParamMetaData paramAt(int idx) const
    {
        using pmd = sst::basic_blocks::params::ParamMetaData;
        switch (idx)
        {
        case time: return pmd().asFloat().withName("Reverb Time").withRange(0.3f, 18.f).withDefault(3.5f).withLinearScaleFormatting("s").withDecimalPlaces(1);
        case diffusion: return pmd().asInt().withName("Diffusion").withRange(0, 10).withDefault(10);
        case initialDelay: return pmd().asFloat().withName("Initial Delay").withRange(0.1f, 200.f).withDefault(67.8f).withLinearScaleFormatting("ms").withDecimalPlaces(1);
        case hpf: return pmd().asFloat().withName("HPF Cutoff").withRange(20.f, 8000.f).withDefault(80.f).withLinearScaleFormatting("Hz").withDecimalPlaces(0);
        case lpf: return pmd().asFloat().withName("LPF Cutoff").withRange(1000.f, 20000.f).withDefault(5600.f).withLinearScaleFormatting("Hz").withDecimalPlaces(0);
        case roomSize: return pmd().asInt().withName("Room Size").withRange(0, 31).withDefault(29);
        case highRatio: return pmd().asFloat().withName("High Ratio").withRange(0.1f, 1.f).withDefault(0.8f).withLinearScaleFormatting("").withDecimalPlaces(1);
        case lowRatio: return pmd().asFloat().withName("Low Ratio").withRange(0.1f, 1.4f).withDefault(0.7f).withLinearScaleFormatting("").withDecimalPlaces(1);
        case decay: return pmd().asInt().withName("Decay").withRange(0, 63).withDefault(55);
        }
        return pmd().withName("Unused").withType(pmd::NONE);
    }

    // For the tests: the decay times the network is designed for.
    std::array<double, bands> designedDecay{};

  private:
    std::vector<float> pre, line[lines], ap[4];
    int preW{0}, lineW[lines]{}, apW[4]{}, lineLen[lines]{}, apLen[4]{};
    int earlyDelay{1}, netDelay{1};
    double interaction[bands][bands]{};
    std::array<std::array<Section, bands>, lines> eq{};
    double apG{0.5}, gain{0}, onset{0};
    bool hpOn{false}, lpOn{false};
    double hpA{0}, hpR{0}, hpState{0}, hpX{0}, lpB{0}, lpA1{0}, lpState{0}, lpX{0};
    std::array<float, numParamsEnum> last{};

    // Solves interaction * g = target (Gaussian elimination, partial pivoting).
    void solve(const double *target, double *g) const
    {
        double M[bands][bands + 1];
        for (int r = 0; r < bands; ++r)
        {
            for (int c = 0; c < bands; ++c)
                M[r][c] = interaction[r][c];
            M[r][bands] = target[r];
        }
        for (int c = 0; c < bands; ++c)
        {
            int p = c;
            for (int r = c + 1; r < bands; ++r)
                if (std::abs(M[r][c]) > std::abs(M[p][c]))
                    p = r;
            for (int k = 0; k <= bands; ++k)
                std::swap(M[c][k], M[p][k]);
            for (int r = 0; r < bands; ++r)
                if (r != c)
                {
                    const double m = M[r][c] / M[c][c];
                    for (int k = c; k <= bands; ++k)
                        M[r][k] -= m * M[c][k];
                }
        }
        for (int r = 0; r < bands; ++r)
            g[r] = M[r][bands] / M[r][r];
    }

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
        const double size = std::clamp((double)std::lround(last[roomSize]), 0.0, 31.0);
        designedDecay = decayTimes(last[time], size, last[highRatio], last[lowRatio]);
        const double scale = lineScale(size);
        for (int j = 0; j < lines; ++j)
        {
            lineLen[j] = std::max(1, (int)std::lround(lineSeconds[j] * scale * rate));
            const double sec = lineLen[j] / rate;
            double target[bands], g[bands], d[bands];
            for (int b = 0; b < bands; ++b)
                target[b] = -60 * sec / designedDecay[b];
            solve(target, g);
            for (int it = 0; it < 2; ++it) // correct for the bands' interaction at these gains
            {
                for (int b = 0; b < bands; ++b)
                    bandSection(eq[j][b], b, g[b], rate);
                for (int b = 0; b < bands; ++b)
                {
                    double sum = 0;
                    for (int k = 0; k < bands; ++k)
                        sum += eq[j][k].db(bandHz[b], rate);
                    d[b] = target[b] - sum;
                }
                double dg[bands];
                solve(d, dg);
                for (int b = 0; b < bands; ++b)
                    g[b] += dg[b];
            }
            for (int b = 0; b < bands; ++b)
                bandSection(eq[j][b], b, g[b], rate);
        }
        for (int k = 0; k < 4; ++k)
            apLen[k] = std::max(1, (int)std::lround(allpassSeconds[k] * rate));
        apG = 0.1 + 0.06 * std::clamp((double)std::lround(last[diffusion]), 0.0, 10.0);
        onset = onsetSeconds(last[initialDelay], size, std::lround(last[decay]));
        earlyDelay = std::clamp((int)std::lround(onset * rate), 1, (int)pre.size() - 1);
        netDelay = std::clamp(earlyDelay - lineLen[0], 1, (int)pre.size() - 1);
        const int h = std::clamp((int)std::lround(last[highRatio] * 10), 1, 10) - 1;
        const double levelDb = interp(timeAt, timeLevel, 10, last[time]) + highLevel[h] +
                               interp(lowAt, lowLevel, 5, last[lowRatio]) +
                               interp(diffusionAt, diffusionLevel, 5, std::lround(last[diffusion])) +
                               interp(decayAt, decayLevel, 5, std::lround(last[decay])) + levelOffset;
        gain = std::pow(10.0, levelDb / 20);
        hpOn = last[hpf] > 20.5f;
        lpOn = last[lpf] < 19999.5f;
        const double kh = std::tan(M_PI * std::min((double)last[hpf], 0.45 * rate) / rate);
        hpA = 1 / (1 + kh);
        hpR = (1 - kh) / (1 + kh);
        const double kl = std::tan(M_PI * std::min((double)last[lpf], 0.45 * rate) / rate);
        lpB = kl / (1 + kl);
        lpA1 = (kl - 1) / (kl + 1);
    }
};
} // namespace oscairn_fx::hall

#endif
