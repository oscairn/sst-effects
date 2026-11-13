/*
 * Oscairn effects (doc/oscairn-plan.md). GPL-3.0-or-later, see ../OscairnEffects.cpp.
 *
 * A tempo-synced cross delay. Written for Oscairn; follows the sst-effects effect contract. Laws
 * fitted to reference renders at 44.1 kHz (plan, section 15, "Tempo cross delay"):
 *  - two lines: A takes the left input and plays on the right after its time (L>R), B takes the
 *    right input and plays on the left (R>L). Each line's output feeds the other line back
 *    through the feedback gain v x 516 / 32768 (v the displayed -63..+63; feedbackGain), so a
 *    left input ping-pongs;
 *  - input L feeds A only, R feeds B only, L&R feeds both. L Mode 2 feeds A only and sends A's
 *    output into B at unity and B's into A at fb |v| / 63 (mode2Gain; R Mode 2 the mirror), so each
 *    side repeats every two crossings and, at negative feedback, inverts once per repeat;
 *  - a line's length in samples at 44.1 kHz is the delay family's time law (tenthsSamples) of the
 *    note value at the tempo, cut to 0.1 ms, plus the lag (both lines); a line that would reach
 *    131,072 samples halves its note until it fits (halvedNote);
 *  - high damping is a two-tap filter (1 - g) x[n] + g x[n-1] in the feedback paths only (the
 *    first echo is unfiltered), g = 5 (1 - ratio) / 9;
 *  - the dry and wet levels are cos and sin of (pi / 2) x, x = (balance + 63) / 126 (tempoBalance).
 * At other rates the lines keep their length in seconds (an all-pass interpolator for the
 * fraction, so repeats keep their level) and the damping is a linear-phase filter fitted to the
 * 44.1 kHz filter's magnitude, its delay taken off the line.
 *
 * CrossCore is that signal path; a mode supplies its parameter table, the two line times and
 * how long its buffers must be (effects/CrossDelay.h is the other mode).
 */
#ifndef OSCAIRN_CLAP_EFFECTS_TEMPOCROSSDELAY_H
#define OSCAIRN_CLAP_EFFECTS_TEMPOCROSSDELAY_H

#include "sst/effects/EffectCore.h"
#include "sst/basic-blocks/params/ParamMetadata.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <unordered_map>
#include <vector>

namespace oscairn_fx::delay
{
constexpr double designRate = 44100;

// The longest line either delay mode reaches: the cross delay's 1638.3 ms; buffers size for it.
constexpr double maxLineMs{1638.3};

// The note values of the time parameters, in quarter notes.
constexpr double noteBeats[20] = {1. / 12, 3. / 32, 1. / 8, 1. / 6, 3. / 16, 1. / 4, 1. / 3, 3. / 8, 1. / 2, 2. / 3,
                                  3. / 4,  1.,      4. / 3, 3. / 2, 2.,      8. / 3, 3.,     4.,     5.,     6.};

// The note names of the time parameters' 20 values, in the reference's Data List order.
inline const std::unordered_map<int, std::string> noteNames = {
    {0, "1/32 triplet"}, {1, "1/64 dotted"}, {2, "1/32"},          {3, "1/16 triplet"}, {4, "1/32 dotted"},
    {5, "1/16"},         {6, "1/8 triplet"}, {7, "1/16 dotted"},   {8, "1/8"},          {9, "1/4 triplet"},
    {10, "1/8 dotted"},  {11, "1/4"},        {12, "1/2 triplet"},  {13, "1/4 dotted"},  {14, "1/2"},
    {15, "1/1 triplet"}, {16, "1/2 dotted"}, {17, "4 quarters"},   {18, "5 quarters"},  {19, "6 quarters"}};

// The delay family's one time law: a time in tenths of a millisecond is floor(t x 18063 / 4096)
// samples at 44.1 kHz (4.4099121 per 0.1 ms), at least one. The 02 40 and 02 50 renders put the
// factor between 4.40990 and 4.40994 (0.1 ms is 4 samples, 1,638.3 ms 72,247), the 02 28 and
// 02 20 renders between 4.409908 and 4.409915; 18063 / 4096 is the one 12-bit fixed-point factor
// inside both, and every time of every delay type's renders fits it (the 02 10 triplet and dotted
// lengths of fx16 and fx18 too, which round(T x 44100) - 1 missed by up to 2 samples).
inline long tenthsSamples(long tenths) { return tenths < 1 ? 1 : tenths * 18063 / 4096; }

// A line's length in seconds from a time in ms (0.1 to 1638.3, steps of 0.1): the family's law,
// kept in seconds at other rates.
inline double msLineSeconds(double ms)
{
    return (double)tenthsSamples(std::lround(std::clamp(ms, 0.1, maxLineMs) * 10)) / designRate;
}

// A note value at the tempo in tenths of a millisecond, cut to 0.1 ms (the 02 28 and 02 20
// renders: the 1/32 triplet at 120 BPM is 41.6 ms, 1,834 samples).
inline long noteTenths(int note, double tempo)
{
    return (long)std::floor(noteBeats[std::clamp(note, 0, 19)] * 600000 / std::clamp(tempo, 20.0, 999.0) + 1e-6);
}

// The tempo modes' long notes: while a line (the note plus its longest offset, in tenths of a ms)
// would reach `limit` samples, the note halves, as often as it takes. Each line of the cross and
// stereo types holds less than 131,072 samples: the tempo cross type's 4 and 5 quarters at
// 120 BPM play 88,198 and 110,247 samples and 6 quarters halves to 66,148 (tcd_t17/18/19), the
// stereo type's 6 quarters with lag -28 plays 131,062 and with lag -27 halves to 64,958
// (ts_t19_lag36/37). The mono type's one line holds less than 262,144 (monoLimit).
constexpr long lineLimit = 131072;
inline long halvedNote(long note, long offsetTenths, long limit)
{
    while (note > 0 && tenthsSamples(note + offsetTenths) >= limit)
        note /= 2;
    return note;
}

// Feedback v (-63..63) per crossing or loop: v x 516 / 32768, the 1/32768-grid step nearest
// 1/63.5 (1/63.504; the renders measure v / 63.5 within 0.01 dB, which cannot tell them apart).
inline double feedbackGain(double v) { return (double)std::lround(v) * 516 / 32768; }
// Mode 2's legs. The squared one is the feedback times |v| / 63 on the same grid, cut toward zero;
// the other is the grid's unity, 32767 / 32768. Their loop is 1.0078 x fb^2, which the cd_m2* and
// tcd_m2* renders measure at 1.0072-1.0073 (fb^2 itself missed by 0.063 dB per loop).
inline double mode2Gain(double v)
{
    const long n = std::lround(v);
    return (double)(std::abs(n) * n * 516 / 63) / 32768;
}
constexpr double mode2Unity = 32767.0 / 32768;

// High damping ratio r (0.1 to 1.0) to the two-tap coefficient: g = 5 (1 - r) / 9, so 0.1 puts
// a zero at Nyquist and 1.0 is off (fx16, fx17: within 0.001 of a free fit at every value).
inline double dampingCoefficient(double ratio) { return 5 * (1 - std::clamp(ratio, 0.1, 1.0)) / 9; }

// Dry/wet balance v (-63..63) to the two gains.
struct Balance
{
    double dry, wet;
};
// The ms types (02 00, 02 40, 02 50): the side being turned down falls with the square of its
// distance to the end, ((63 - |v|) / 63)^2, and the other stays at unity, so at D=W both are at
// unity. Wet/dry within 0.01 dB from v = -55 to +59 (cross_delay_extra, review cross_delay, the
// L/R and L/C/R batches); the steady saw of delay-eq-2026-11-12 puts the dry at D=W level with
// the dry-only render and the echo of the wet-only render level with it (0.000 / -0.002 dB).
inline Balance msBalance(double v)
{
    const double d = v > 0 ? (63 - v) / 63 : 1, w = v < 0 ? (63 + v) / 63 : 1;
    return {d * d, w * w};
}
// The tempo types (02 10, 02 28, 02 20): cosine and sine of (pi / 2)(v + 63) / 126, an equal-power
// pair (fx17; delay-eq-2026-11-12: the dry at D=W 3.01 dB under the dry-only render).
inline Balance tempoBalance(double v)
{
    const double x = std::clamp((v + 63) / 126, 0.0, 1.0);
    return {std::cos(M_PI / 2 * x), std::sin(M_PI / 2 * x)};
}

// A tempo cross line's length in seconds: the note at the tempo, halved while the line would
// reach lineLimit, plus the lag, by the family's time law (the 44.1 kHz sample count, kept in
// seconds at other rates).
inline double lineSeconds(int note, double tempo, double lagMs)
{
    const long lag = std::lround(lagMs * 10);
    return (double)tenthsSamples(halvedNote(noteTenths(note, tempo), lag, lineLimit) + lag) / designRate;
}

// A reader of a delay line at a fractional length: integer part from the buffer, the fraction
// (kept in [0.5, 1.5) samples) by a first-order all-pass, which leaves every repeat's level
// unchanged.
struct Tap
{
    int whole{1};
    double eta{0}, x1{0}, y1{0};
    bool frac{false};
    void clear() { x1 = y1 = 0; }
    void setLength(double samples)
    {
        samples = std::max(samples, 1.0); // below a sample the all-pass's pole would leave the unit circle
        if (std::abs(samples - std::round(samples)) < 1e-6) // whole samples (44.1 kHz) stay exact
            samples = std::round(samples);
        frac = samples != std::floor(samples);
        if (!frac)
        {
            whole = std::max(1, (int)samples);
            return;
        }
        whole = std::max(1, (int)std::floor(samples - 0.5));
        const double d = samples - whole;
        eta = (1 - d) / (1 + d);
    }
};

// A delay line with its own reader; further readers (Tap) can share its buffer.
struct Line
{
    std::vector<float> buf;
    int w{0};
    Tap tap;
    void resize(int n) { buf.assign((size_t)n, 0.f), w = 0; }
    void clear()
    {
        std::fill(buf.begin(), buf.end(), 0.f);
        tap.clear();
    }
    void setLength(double samples) { tap.setLength(samples); }
    inline float read(const Tap &t) const
    {
        int r = w - t.whole;
        if (r < 0)
            r += (int)buf.size();
        return buf[(size_t)r];
    }
    inline double out(Tap &t) const
    {
        const double x = read(t);
        if (!t.frac)
            return x;
        const double y = t.eta * (x - t.y1) + t.x1;
        t.x1 = x;
        t.y1 = y;
        return y;
    }
    inline double out() { return out(tap); }
    inline void write(float v)
    {
        buf[(size_t)w] = v;
        if (++w == (int)buf.size())
            w = 0;
    }
};

// The feedback damping: at 44.1 kHz the fitted two-tap filter; elsewhere a linear-phase filter
// of 65 taps fitted (least squares on the relative error, 0-20 kHz) to its magnitude, delay 32.
struct Damping
{
    static constexpr int maxK = 32;
    std::array<double, maxK + 1> h{};
    std::array<double, 2 * maxK + 1> hist{};
    int K{0}, at{0};
    double g{0};
    bool twoTap{true};
    void clear() { hist.fill(0), at = 0; }
    void design(double coefficient, double rate)
    {
        g = coefficient;
        twoTap = rate == designRate;
        if (twoTap)
        {
            K = 0;
            return;
        }
        K = maxK;
        // least squares on the relative error: H(w) = h0 + 2 sum h_k cos(k w) against
        // |1 - g + g e^-jw44|, which at ratio 0.1 falls to -16.6 dB at 20 kHz (a zero at 22.05 kHz)
        constexpr int nIn = 160, nOut = 24;
        auto target = [&](double f) {
            const double w44 = 2 * M_PI * std::min(f, 20000.0) / designRate;
            return std::hypot(1 - g + g * std::cos(w44), g * std::sin(w44));
        };
        const double edge = target(20000);
        double M[maxK + 1][maxK + 2]{};
        for (int i = 0; i < nIn + nOut; ++i)
        {
            const double f = i < nIn ? 20000.0 * i / (nIn - 1) : 20000 + (rate / 2 - 20000) * (i - nIn + 1) / nOut;
            const double t = target(f), wt = i < nIn ? 1 / std::max(t, 1e-3) : 0.02 / edge;
            double row[maxK + 1];
            row[0] = 1;
            for (int k = 1; k <= K; ++k)
                row[k] = 2 * std::cos(k * 2 * M_PI * f / rate);
            for (int r = 0; r <= K; ++r)
            {
                for (int c = 0; c <= K; ++c)
                    M[r][c] += wt * wt * row[r] * row[c];
                M[r][K + 1] += wt * wt * row[r] * t;
            }
        }
        for (int c = 0; c <= K; ++c) // Gaussian elimination, partial pivoting
        {
            int p = c;
            for (int r = c + 1; r <= K; ++r)
                if (std::abs(M[r][c]) > std::abs(M[p][c]))
                    p = r;
            for (int k = 0; k <= K + 1; ++k)
                std::swap(M[c][k], M[p][k]);
            for (int r = 0; r <= K; ++r)
                if (r != c)
                {
                    const double m = M[r][c] / M[c][c];
                    for (int k = c; k <= K + 1; ++k)
                        M[r][k] -= m * M[c][k];
                }
        }
        for (int k = 0; k <= K; ++k)
            h[k] = M[k][K + 1] / M[k][k];
    }
    inline double run(double x)
    {
        if (twoTap)
        {
            const double y = (1 - g) * x + g * hist[0];
            hist[0] = x;
            return y;
        }
        const int n = 2 * K + 1;
        hist[at] = x;
        double y = h[0] * hist[(at + n - K) % n];
        for (int k = 1; k <= K; ++k)
            y += h[k] * (hist[(at + n - K - k) % n] + hist[(at + n - K + k) % n]);
        if (++at == n)
            at = 0;
        return y;
    }
};

// The input select parameter's choices, shared by the delay modes.
enum Input
{
    inL,
    inR,
    inLR,
    inLMode2,
    inRMode2
};

// The cross delay's signal path, shared by every mode. A mode (the CRTP argument) supplies its
// parameter enum and table, `bufferSamples(rate)` and `setLines(tempo, rate)`, which sets secA
// and secB, the two lines' lengths in seconds. A mode with `outputTaps` also sets outA and outB,
// where each line is read for the output when that differs from its length in the loop; a mode
// with `parallelLines` does not cross its lines.
template <typename FXConfig, template <typename> class Mode>
struct CrossCore : sst::effects::core::EffectTemplateBase<FXConfig>
{
    using D = Mode<FXConfig>;
    CrossCore(typename FXConfig::GlobalStorage *s, typename FXConfig::EffectStorage *e,
              typename FXConfig::ValueStorage *p)
        : sst::effects::core::EffectTemplateBase<FXConfig>(s, e, p)
    {
        const int n = D::bufferSamples(this->sampleRate());
        a.resize(n);
        b.resize(n);
        last.assign(D::numParams, NAN);
    }
    static size_t heapBytesAt(double rate) { return 2 * sizeof(float) * (size_t)D::bufferSamples(rate); }
    size_t heapBytes() const { return (a.buf.size() + b.buf.size()) * sizeof(float); }
    // The longest silence between two echoes: the longer line, plus the wrapper's 100 ms.
    double quietSeconds() const { return std::max({secA, secB, outSecA, outSecB}) + 0.1; }

    void initialize()
    {
        a.clear();
        b.clear();
        tapA.clear();
        tapB.clear();
        dampA.clear();
        dampB.clear();
        inDelay.fill({0, 0});
        inAt = 0;
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
        // A mode without an input-select parameter names its fixed feed as D::inputFeed.
        const int mode = inputMode();
        for (int i = 0; i < FXConfig::blockSize; ++i)
        {
            const double l = L[i], r = R[i];
            const double oA = a.out(), oB = b.out(); // A plays right, B left
            double pA = oA, pB = oB; // what each line plays: its loop read, or its own output tap
            if constexpr (requires { D::outputTaps; })
                pA = a.out(tapA), pB = b.out(tapB);
            double fA, fB; // what feeds back into A and into B
            if constexpr (parallel())
                fA = dampA.run(oA), fB = dampB.run(oB); // each line into itself
            else
                fA = dampB.run(oB), fB = dampA.run(oA);
            const bool left = mode != inR && mode != inRMode2, right = mode == inR || mode == inLR || mode == inRMode2;
            double gA = fb, gB = fb; // into A from B, into B from A
            if (mode == inLMode2)
                gA = fb2, gB = mode2Unity;
            else if (mode == inRMode2)
                gA = mode2Unity, gB = fb2;
            double xA = left ? l : 0, xB = right ? r : 0;
            if (const int K = dampA.K)
            {
                // the damping filter delays the feedback by K samples and the lines are K shorter;
                // the input is delayed by K too, so the first echo keeps its time
                inDelay[(size_t)inAt] = {xA, xB};
                const auto &old = inDelay[(size_t)((inAt + 1) % (K + 1))];
                xA = old[0], xB = old[1];
                inAt = (inAt + 1) % (K + 1);
            }
            a.write((float)(xA + gA * fA));
            b.write((float)(xB + gB * fB));
            L[i] = (float)(dry * l + wet * (parallel() ? pA : pB));
            R[i] = (float)(dry * r + wet * (parallel() ? pB : pA));
        }
    }

  protected:
    // A mode with `parallelLines` runs its lines side by side: A loops into itself and plays left,
    // B into itself and plays right.
    static constexpr bool parallel() { return requires { D::parallelLines; }; }
    // The input feed: the input-select parameter's value, or the mode's fixed choice when it
    // has no such parameter.
    int inputMode() const
    {
        if constexpr (requires { D::input; })
            return (int)std::lround(last[D::input]);
        else
            return D::inputFeed;
    }

    double tempo() const
    {
        if constexpr (requires { this->globalStorage->tempo; })
            return this->globalStorage->tempo;
        return 120;
    }

    void update()
    {
        bool changed = !(tempo() == lastTempo);
        for (int i = 0; i < D::numParams; ++i)
        {
            const float v = this->floatValue(i);
            changed = changed || !(v == last[i]);
            last[i] = v;
        }
        if (!changed)
            return;
        lastTempo = tempo();
        const double rate = this->sampleRate();
        fb = feedbackGain(last[D::feedback]);
        fb2 = mode2Gain(last[D::feedback]);
        const Balance bal = D::balance((double)std::lround(last[D::mix]));
        dry = bal.dry, wet = bal.wet;
        const double g = dampingCoefficient(last[D::damping]);
        if (!designed || g != dampA.g)
        {
            designed = true;
            dampA.design(g, rate);
            dampB.design(g, rate);
        }
        static_cast<D *>(this)->setLines(lastTempo, rate);
        a.setLength(secA * rate - dampA.K);
        b.setLength(secB * rate - dampA.K);
        if constexpr (requires { D::outputTaps; })
        {
            tapA.setLength(outSecA * rate - dampA.K);
            tapB.setLength(outSecB * rate - dampA.K);
        }
    }

    Line a, b;
    Tap tapA, tapB; // the output reads of a mode with outputTaps
    Damping dampA, dampB;           // dampA filters A's output on its way back (into B, or A when parallel)
    std::array<std::array<double, 2>, Damping::maxK + 1> inDelay{};
    int inAt{0};
    bool designed{false};
    std::vector<float> last; // sized to D::numParams at construction
    double lastTempo{NAN};
    double fb{0}, fb2{0}, dry{1}, wet{0}, secA{0}, secB{0}, outSecA{0}, outSecB{0};
};

template <typename FXConfig> struct TempoCrossDelay : CrossCore<FXConfig, TempoCrossDelay>
{
    enum params
    {
        timeLR,
        timeRL,
        feedback,
        input,
        damping,
        lag,
        mix,
        numParamsEnum
    };
    static constexpr int numParams{numParamsEnum};
    static constexpr const char *streamingName{"oscairn-tempo-cross-delay"};
    static constexpr const char *displayName{"Tempo Cross Delay"};
    using CrossCore<FXConfig, TempoCrossDelay>::CrossCore;

    static int bufferSamples(double rate)
    {
        return (int)std::ceil(lineLimit / designRate * rate) + 2 * Damping::maxK + 8;
    }
    void setLines(double tempo, double)
    {
        this->secA = lineSeconds((int)std::lround(this->last[timeLR]), tempo, this->last[lag]);
        this->secB = lineSeconds((int)std::lround(this->last[timeRL]), tempo, this->last[lag]);
    }
    static Balance balance(double v) { return tempoBalance(v); }

    sst::basic_blocks::params::ParamMetaData paramAt(int idx) const
    {
        using pmd = sst::basic_blocks::params::ParamMetaData;
        auto time = [&](const char *n) { return pmd().asInt().withName(n).withRange(0, 19).withDefault(11).withUnorderedMapFormatting(noteNames); };
        switch (idx)
        {
        case timeLR: return time("Time L>R");
        case timeRL: return time("Time R>L");
        case feedback: return pmd().asInt().withName("Feedback").withRange(-63, 63).withDefault(16);
        case input:
            return pmd().asInt().withName("Input").withRange(0, 4).withDefault(0).withUnorderedMapFormatting(
                {{inL, "L"}, {inR, "R"}, {inLR, "L&R"}, {inLMode2, "L Mode 2"}, {inRMode2, "R Mode 2"}});
        case damping:
            return pmd().asFloat().withName("High Damping").withRange(0.1f, 1.f).withDefault(0.5f).withLinearScaleFormatting("").withDecimalPlaces(1);
        case lag: return pmd().asFloat().withName("Lag").withRange(-63.f, 63.f).withDefault(0.f).withLinearScaleFormatting("ms").withDecimalPlaces(0);
        case mix:
        {
            std::unordered_map<int, std::string> m;
            for (int v = -63; v <= 63; ++v)
                m[v] = v < 0 ? "D" + std::to_string(-v) + ">W" : (v == 0 ? "D=W" : "D<W" + std::to_string(v));
            return pmd().asInt().withName("Mix").withRange(-63, 63).withDefault(0).withUnorderedMapFormatting(m);
        }
        }
        return pmd().withName("Unused").withType(pmd::NONE);
    }
};
} // namespace oscairn_fx::delay

#endif
