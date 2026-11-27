/*
 * Oscairn effects (doc/oscairn-plan.md). GPL-3.0-or-later, see ../OscairnEffects.cpp.
 *
 * Hall D, Room A and Plate A: the reference's newest reverb family (hall, room, plate), on the tank
 * reverbs' structure (TankReverb.h) with constants of their own, fitted to measured renders.
 *  - The input is mixed to mono (measured: left, right and center feeds give the same wet
 *    balance and decorrelation) and fed to the tank as there: a fixed first-order HPF, the
 *    initial delay, early reflections, then the loop through both sides. Diffusion, density and
 *    ER/Rev balance stay at the tank's defaults. Plate A's reflections sit at 0, 1.1, 2.4, 3.4, 4.7
 *    and 5.8 ms (measured: its first arrivals recur every 1.1-1.2 ms), and its output taps mix
 *    across the sides with a negative share, for its positive L/R correlation.
 *  - The loop's decay above its low shelf is the reverb time times a time scale (a knot table
 *    over the reverb time); the low shelf sets the decay at DC to a share of that (per room size
 *    or plate type).
 *  - High damp: the loop's first-order low-pass, its corner at a share of the high damp frequency
 *    held under a ceiling the high ratio raises (a knot table), so the extra decay above it is the
 *    same number of dB per second at every reverb time (measured: a one-pole roll-off, its corner
 *    at 0.63-0.82 of the frequency; the high ratio weakens it but never removes it, less so at low
 *    frequencies). The loop's delays are Hall B's scaled so one pass gives the measured strength.
 *  - The output passes a fixed first-order LPF, two second-order shelves (low and high, slope 1)
 *    at a share of their set frequencies, their gains as set (measured: corners at 0.85-1.13 times
 *    the set frequency, gains within 2 dB), and a level per room size or plate type.
 * Every gain and coefficient ramps over 5 ms. Nothing is modulated. Delays keep their 44.1 kHz
 * length in seconds at other rates; the shelves are designed at the running rate.
 * Approximations where the measurements say nothing: room size moves nothing in Hall D and Room A;
 * the high ratio's form (the ceiling) is fitted, not resolved; initial delays under 40 ms follow
 * the parameter as above it; the early reflections are fitted to the default renders' onset, not
 * to resolved taps; the level is the steady saw's (the reference's bursts read 2-5 dB louder).
 */
#ifndef OSCAIRN_CLAP_EFFECTS_MONOTANKREVERB_H
#define OSCAIRN_CLAP_EFFECTS_MONOTANKREVERB_H

#include "effects/TankReverb.h"

namespace oscairn_fx::monotank
{
using hall::interp;
using hall::Ramp;

constexpr int timeKnots = 9, ratioKnots = 5;

// A mode's laws; its structure is a tank::Fit (whose defaults, timeScale, dampA, dampB and
// shelfRatio this family does not use).
struct Fit
{
    const char *name, *streamingName, *sizeName;
    int sizeMax;
    // defaults: reverb time (s), room size or plate type, initial delay (ms), high damp frequency
    // (Hz), high ratio, EQ low frequency (Hz), EQ low gain (dB), EQ high frequency (Hz), EQ high gain (dB)
    float defaults[9];
    const tank::Fit *tank;
    // the loop's decay above its shelf / reverb time, at reverb times timeAt (s), held outside.
    // Fitted so the 250 Hz-1 kHz decay follows the measured one; measured 1.1-30 s (the reverb time
    // sweep at the defaults; under 1.1 s the measurement cannot resolve the decay).
    double timeAt[timeKnots], timeScale[timeKnots];
    // High damp: the loop low-pass's corner is 1 / (1 / (dampCorner x frequency) + 1 / ceiling),
    // the ceiling (Hz) by the high ratio at ratioAt. Fitted to the high damp sweep (1-20 kHz) and
    // the high ratio sweep (0-1) at the defaults and to two corners (5.5 s: 1 kHz with high ratio
    // 1, 20 kHz with high ratio 0).
    double dampCorner;
    double ratioAt[ratioKnots], ratioHz[ratioKnots];
    // per size or type: decay at DC / decay above the loop's shelf. Measured: the plate's types 0-2;
    // the hall's and room's default size only (the others' steps are not resolved)
    double shelfRatio[5];
    double levelDb[5];                // per size or type, added to the structure's level; measured as above
    double eqLowScale, eqHighScale;   // the shelves' corners / their set frequencies; measured over 10 EQ takes
    double hpfHz, lpfHz;              // the tank's input HPF and output LPF, fixed; fitted to the default band energies
};

// The structures. Delays as Hall B's scaled so a pass takes the measured damping strength.
inline constexpr tank::Fit hallDTank{
    "Hall D", "oscairn-tank-hall-d",
    {2.4f, 10, 37.9f, 20, 20000, 0.1f, 4, 0, 1},
    {{0, 7.3, 15.9, 23.1, 34.7, 46.2}, {2.88, 10.18, 18.78, 25.98, 37.58, 49.08}},
    {{0.1997, 0.6263, 0.9976, 0.4265, 0.00063, 0.308}, {0.00038, 0.00052, 0.00051, 0.1899, 0.441, 0.00034}},
    {{0, 0, 0, 0, 0, 0}, {1, 1, 1, 1, 1, 1}},
    {4.7, 5.3},
    29.68, 0,
    {3.1, 3.5},
    {5.04, 6.71}, {55.8, 52.57}, {21.48, 30.11}, {46.38, 39.73},
    0.62, 0.5, 0.65,
    {{2, 0, 1, 2}, {2, 0, 1, 2}},
    {{0, 0.37, 0.21, 0.63}, {0, 0.41, 0.27, 0.58}},
    {{0.2845, -0.8421, 2, 0.9135}, {0.3193, 0.2555, -1.382, -1.102}},
    0.1255, 1, 0, 0, 500, 1, 0.9203, 1.482, -12.3};

inline constexpr tank::Fit roomATank{
    "Room A", "oscairn-tank-room-a",
    {1.2f, 10, 6.4f, 20, 20000, 0.1f, 4, 0, 1},
    {{0, 7.3, 15.9, 23.1, 34.7, 46.2}, {2.88, 10.18, 18.78, 25.98, 37.58, 49.08}},
    {{0.1407, 0.006516, 0.00056, 1.342, 0.00056, 1.999}, {0.00041, 0.0005, 0.03168, 0.03661, 0.00056, 0.00046}},
    {{0, 0, 0, 0, 0, 0}, {1, 1, 1, 1, 1, 1}},
    {4.7, 5.3},
    38.1, 0,
    {3.1, 3.5},
    {6.5, 8.64}, {71.89, 67.74}, {27.67, 38.79}, {59.76, 51.19},
    0.62, 0.5, 0.65,
    {{2, 0, 1, 2}, {2, 0, 1, 2}},
    {{0, 0.37, 0.21, 0.63}, {0, 0.41, 0.27, 0.58}},
    {{-0.02936, 1.91, 2, 1.536}, {-0.1177, 0.7878, 2, -1.133}},
    0.2391, 1, 0, 0, 500, 1, 0.248, 1.536, -4.426};

inline constexpr tank::Fit plateATank{
    "Plate A", "oscairn-tank-plate-a",
    {2.4f, 10, 12.7f, 20, 20000, 0.1f, 4, 0, 1},
    {{0, 1.1, 2.4, 3.4, 4.7, 5.8}, {0, 1.1, 2.4, 3.4, 4.7, 5.8}},
    {{0.3938, 0.00041, 0.00046, 0.9493, 0.659, 0.8512}, {0.6525, 0.2162, 0.6091, 0.5906, 0.7328, 1.026}},
    {{0, 0, 0, 0, 0, 0}, {1, 1, 1, 1, 1, 1}},
    {4.7, 5.3},
    19.08, 0,
    {3.1, 3.5},
    {6.69, 8.9}, {74.04, 69.76}, {28.5, 39.95}, {61.55, 52.72},
    0.62, 0.5, 0.65,
    {{2, 0, 1, 2}, {2, 0, 1, 2}},
    {{0, 0.37, 0.21, 0.63}, {0, 0.41, 0.27, 0.58}},
    {{0.2531, -0.8353, -1.825, -0.2263}, {-0.09344, -0.5304, -1.66, 0.744}},
    -0.2926, 1, 0, 0, 200, 1, 0.626, 1.503, -11.25};

inline constexpr Fit hallD{
    "Hall D", "oscairn-tank-hall-d", "Room Size", 4,
    {2.4f, 2, 37.9f, 6300, 0.8f, 45, 1, 16000, 2},
    &hallDTank,
    {1.1, 1.9, 2.7, 3.5, 4.3, 5.5, 9.5, 17, 30},
    {0.964, 0.984, 0.978, 0.932, 0.886, 0.86, 0.825, 0.784, 0.686},
    0.839,
    {0, 0.2, 0.5, 0.8, 1},
    {7721, 10810, 30440, 200000, 200000},
    {1.18, 1.18, 1.18, 1.18, 1.18},
    {0, 0, 0, 0, 0},
    1.13, 0.87, 69.7, 11650};

inline constexpr Fit roomA{
    "Room A", "oscairn-tank-room-a", "Room Size", 4,
    {1.2f, 2, 6.4f, 6300, 0.6f, 40, 1, 18000, 2},
    &roomATank,
    {1.1, 1.9, 2.7, 3.5, 4.3, 5.5, 9.5, 17, 30},
    {0.946, 0.981, 1.015, 0.965, 0.912, 0.878, 0.749, 0.651, 0.572},
    0.539,
    {0, 0.2, 0.5, 0.6, 1},
    {3400, 3400, 5956, 9524, 180460},
    {0.895, 0.895, 0.895, 0.895, 0.895},
    {0, 0, 0, 0, 0},
    1.04, 1.03, 79.3, 16450};

inline constexpr Fit plateA{
    "Plate A", "oscairn-tank-plate-a", "Plate Type", 2,
    {2.4f, 1, 12.7f, 8000, 0.8f, 80, -1, 2500, 1},
    &plateATank,
    {1.1, 1.9, 2.7, 3.5, 4.3, 5.5, 9.5, 17, 30},
    {1.017, 1.072, 1.056, 1.035, 1.01, 1.014, 1.063, 1.062, 1.045},
    0.607,
    {0, 0.2, 0.5, 0.8, 1},
    {3710, 4993, 17346, 99400, 200000},
    {0.713, 0.861, 1.046, 1.046, 1.046},
    {-4.56, 0, 0.51, 0.51, 0.51},
    1.07, 0.85, 20, 9710};

struct Settings
{
    double time{2}, size{0}, initialDelayMs{10}, dampHz{8000}, highRatio{1}, eqLowHz{100}, eqLowDb{0}, eqHighHz{8000},
        eqHighDb{0};
    bool operator==(const Settings &) const = default;
};

// A second-order section whose coefficients ramp (direct form I: a ramp moves no stored state), states per side.
struct Shelf
{
    Ramp b0, b1, b2, a1, a2;
    double x1[2]{}, x2[2]{}, y1[2]{}, y2[2]{};
    // RBJ shelf, slope 1, gain gDb, corner hz, at the running rate
    void set(bool high, double hz, double gDb, double rate, int n, bool now)
    {
        const double A = std::pow(10.0, gDb / 40), w0 = 2 * M_PI * std::min(hz, 0.45 * rate) / rate;
        const double c = std::cos(w0), sA = 2 * std::sqrt(A) * std::sin(w0) / 2 * std::sqrt(2.0);
        const double s = high ? -1 : 1;
        const double c0 = A * ((A + 1) - s * (A - 1) * c + sA), c1 = s * 2 * A * ((A - 1) - s * (A + 1) * c),
                     c2 = A * ((A + 1) - s * (A - 1) * c - sA), d0 = (A + 1) + s * (A - 1) * c + sA,
                     d1 = -s * 2 * ((A - 1) + s * (A + 1) * c), d2 = (A + 1) + s * (A - 1) * c - sA;
        b0.to(c0 / d0, n, now), b1.to(c1 / d0, n, now), b2.to(c2 / d0, n, now), a1.to(d1 / d0, n, now), a2.to(d2 / d0, n, now);
    }
    template <bool ramp> inline void step()
    {
        if constexpr (ramp)
            b0.next(), b1.next(), b2.next(), a1.next(), a2.next();
    }
    inline double run(int s, double x)
    {
        const double y = b0.v * x + b1.v * x1[s] + b2.v * x2[s] - a1.v * y1[s] - a2.v * y2[s];
        x2[s] = x1[s], x1[s] = x, y2[s] = y1[s], y1[s] = y;
        return y;
    }
    void clear()
    {
        for (int s = 0; s < 2; ++s)
            x1[s] = x2[s] = y1[s] = y2[s] = 0;
    }
};

struct Core
{
    const Fit *fit{&hallD};
    tank::Core t;
    Shelf lo, hi;
    int ramping{0};

    void prepare(double r)
    {
        t.fit = fit->tank;
        t.prepare(r);
    }
    void clear()
    {
        t.clear();
        lo.clear();
        hi.clear();
    }
    void set(const Settings &q)
    {
        const Fit &f = *fit;
        const bool now = !t.have;
        tank::Settings ts;
        ts.time = q.time, ts.diffusion = 10, ts.initialDelayMs = q.initialDelayMs, ts.hpf = f.hpfHz, ts.lpf = f.lpfHz,
        ts.reverbDelayMs = 0.1, ts.density = 4, ts.balance = 0, ts.highDamp = 1, ts.mix = 63;
        t.set(ts);
        const int n = t.fadeLen;
        ramping = now ? 0 : n;
        const int size = (int)std::clamp(std::round(q.size), 0.0, (double)f.sizeMax);
        // the loop: as the tank's, but the time law, the shelf and the damping are this family's
        const double rate = t.rate;
        const double pass = 0.5 * (t.dApA[0] + t.dLine1[0] + t.dApB[0] + t.dLine2[0] + t.dApA[1] + t.dLine1[1] + t.dApB[1] + t.dLine2[1]) / rate;
        const double time = std::clamp(q.time, 0.3, 30.0);
        const double lg = -3 * pass / (time * interp(f.timeAt, f.timeScale, timeKnots, time));
        const double shelfDb = 20 * lg * (1 / f.shelfRatio[size] - 1);
        const double hz = 1 / (1 / (f.dampCorner * std::clamp(q.dampHz, 1000.0, 20000.0)) +
                               1 / interp(f.ratioAt, f.ratioHz, ratioKnots, std::clamp(q.highRatio, 0.0, 1.0)));
        const double g = std::min(std::pow(10.0, lg), 0.995 / std::max(1.0, std::pow(10.0, shelfDb / 20)));
        t.loopGain.to(g, n, now);
        for (int s = 0; s < 2; ++s)
        {
            t.damp[s].lowPass(hz, rate, n, now);
            t.shelf[s].lowShelf(f.tank->shelfHz, shelfDb, rate, n, now);
        }
        t.level.to(std::pow(10.0, (f.tank->levelDb + f.levelDb[size]) / 20), n, now);
        lo.set(false, f.eqLowScale * std::clamp(q.eqLowHz, 22.0, 1000.0), std::clamp(q.eqLowDb, -12.0, 12.0), rate, n, now);
        hi.set(true, f.eqHighScale * std::clamp(q.eqHighHz, 500.0, 18000.0), std::clamp(q.eqHighDb, -12.0, 12.0), rate, n, now);
    }

    void process(float *L, float *R, int frames)
    {
        // the input mixed to mono, as measured
        for (int i = 0; i < frames; ++i)
            L[i] = R[i] = 0.5f * (L[i] + R[i]);
        t.process(L, R, frames);
        const int k = std::min(ramping, frames);
        eq<true>(L, R, 0, k);
        eq<false>(L, R, k, frames);
        ramping -= k;
    }
    template <bool ramp> void eq(float *L, float *R, int from, int to)
    {
        for (int i = from; i < to; ++i)
        {
            lo.step<ramp>();
            hi.step<ramp>();
            L[i] = (float)hi.run(0, lo.run(0, L[i]));
            R[i] = (float)hi.run(1, lo.run(1, R[i]));
        }
    }
};

template <typename FXConfig, const Fit &F> struct MonoTankReverb : sst::effects::core::EffectTemplateBase<FXConfig>
{
    enum params
    {
        time,
        size,
        initialDelay,
        dampFreq,
        highRatio,
        eqLowFreq,
        eqLowGain,
        eqHighFreq,
        eqHighGain,
        numParamsEnum
    };
    static constexpr int numParams{numParamsEnum};
    static constexpr const char *streamingName{F.streamingName};
    static constexpr const char *displayName{F.name};

    MonoTankReverb(typename FXConfig::GlobalStorage *s, typename FXConfig::EffectStorage *e, typename FXConfig::ValueStorage *p)
        : sst::effects::core::EffectTemplateBase<FXConfig>(s, e, p)
    {
        core.fit = &F;
        core.prepare(this->sampleRate());
    }
    static size_t heapBytesAt(double rate) { return tank::Core::heapBytesAt(*F.tank, rate); }
    size_t heapBytes() const { return const_cast<tank::Core &>(core.t).heapBytes(); }
    double quietSeconds() const { return 0.1 + core.t.quiet; }

    void initialize()
    {
        core.clear();
        core.t.have = false;
        have = false;
        update();
    }
    void suspendProcessing() { initialize(); }
    int getRingoutDecay() const { return -1; }
    void onSampleRateChanged() { initialize(); }

    void processBlock(float *__restrict L, float *__restrict R)
    {
        update();
        core.process(L, R, FXConfig::blockSize);
    }

    sst::basic_blocks::params::ParamMetaData paramAt(int idx) const
    {
        using pmd = sst::basic_blocks::params::ParamMetaData;
        const float *d = F.defaults;
        switch (idx)
        {
        case time: return pmd().asFloat().withName("Reverb Time").withRange(0.3f, 30.f).withDefault(d[0]).withLinearScaleFormatting("s").withDecimalPlaces(1);
        case size: return pmd().asInt().withName(F.sizeName).withRange(0, F.sizeMax).withDefault(d[1]);
        case initialDelay: return pmd().asFloat().withName("Initial Delay").withRange(0.1f, 200.f).withDefault(d[2]).withLinearScaleFormatting("ms").withDecimalPlaces(1);
        case dampFreq: return pmd().asFloat().withName("High Damp Freq").withRange(1000.f, 20000.f).withDefault(d[3]).withLinearScaleFormatting("Hz").withDecimalPlaces(0);
        case highRatio: return pmd().asFloat().withName("High Ratio").withRange(0.f, 1.f).withDefault(d[4]).withLinearScaleFormatting("").withDecimalPlaces(1);
        case eqLowFreq: return pmd().asFloat().withName("EQ Low Freq").withRange(22.f, 1000.f).withDefault(d[5]).withLinearScaleFormatting("Hz").withDecimalPlaces(0);
        case eqLowGain: return pmd().asFloat().withName("EQ Low Gain").withRange(-12.f, 12.f).withDefault(d[6]).withLinearScaleFormatting("dB").withDecimalPlaces(1);
        case eqHighFreq: return pmd().asFloat().withName("EQ High Freq").withRange(500.f, 18000.f).withDefault(d[7]).withLinearScaleFormatting("Hz").withDecimalPlaces(0);
        case eqHighGain: return pmd().asFloat().withName("EQ High Gain").withRange(-12.f, 12.f).withDefault(d[8]).withLinearScaleFormatting("dB").withDecimalPlaces(1);
        }
        return pmd().withName("Unused").withType(pmd::NONE);
    }

    Core core;

  private:
    Settings last{};
    bool have{false};

    void update()
    {
        Settings q{this->floatValue(time),       std::round(this->floatValue(size)), this->floatValue(initialDelay),
                   this->floatValue(dampFreq),   this->floatValue(highRatio),        this->floatValue(eqLowFreq),
                   this->floatValue(eqLowGain),  this->floatValue(eqHighFreq),       this->floatValue(eqHighGain)};
        if (have && q == last)
            return;
        have = true;
        last = q;
        core.set(q);
    }
};

template <typename C> using HallD = MonoTankReverb<C, hallD>;
template <typename C> using RoomA = MonoTankReverb<C, roomA>;
template <typename C> using PlateA = MonoTankReverb<C, plateA>;
} // namespace oscairn_fx::monotank

#endif
