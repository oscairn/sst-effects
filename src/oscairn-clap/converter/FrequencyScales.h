/*
 * Oscairn effects (doc/oscairn-plan.md). GPL-3.0-or-later, see ../OscairnEffects.cpp.
 *
 * Frequency scales of the platform's Part three-band EQ and Master EQ, for the converter
 * (decision 17): the index a
 * Performance stores, to the frequency in Hz the EQ modes take. Two versions, chosen at build
 * time with OSCAIRN_FREQUENCY_SCALE_TABLE (default: the table):
 *  - a table measured from reference renders (fx11a, every index 46-241: the mid band's centre,
 *    and below 83 half the low shelf's corner; 242-255, reached only by the high shelf, from the
 *    formula until measured). Not taken from any binary;
 *  - the nearest closed form: an octave split linearly into 25.5 steps from 55.113 Hz at index
 *    50.5, through a scaled tangent warp, f = 44100 / (pi k) tan(pi k F / 44100), k = 1.2059.
 *    Within 1.52% of the table (RMS 0.69%) over 46-241.
 */
#ifndef OSCAIRN_CLAP_CONVERTER_FREQUENCYSCALES_H
#define OSCAIRN_CLAP_CONVERTER_FREQUENCYSCALES_H

#include <cmath>

#ifndef OSCAIRN_FREQUENCY_SCALE_TABLE
#define OSCAIRN_FREQUENCY_SCALE_TABLE 1
#endif

namespace oscairn_fx::scales
{
constexpr int threeBandFirst = 46, threeBandLast = 255;

// Measured centres in Hz for indices 46-255 (196 measured, 14 from the formula).
constexpr float threeBandMeasuredHz[threeBandLast - threeBandFirst + 1] = {
    50.10f, 51.20f, 52.30f, 53.30f, 54.40f, 56.10f, 58.30f, 60.40f,
    62.50f, 64.70f, 66.80f, 69.00f, 71.10f, 73.30f, 75.40f, 77.49f,
    79.70f, 81.80f, 83.99f, 86.09f, 88.19f, 90.39f, 92.49f, 94.69f,
    96.79f, 98.99f, 101.09f, 103.19f, 105.39f, 107.49f, 109.69f, 113.99f,
    118.19f, 122.49f, 126.78f, 131.08f, 135.38f, 139.99f, 144.10f, 148.46f,
    152.81f, 157.13f, 161.43f, 165.67f, 169.90f, 174.26f, 178.64f, 182.92f,
    187.23f, 191.60f, 195.80f, 200.11f, 204.44f, 208.72f, 213.07f, 217.40f,
    223.42f, 232.03f, 240.63f, 249.28f, 257.86f, 266.50f, 275.18f, 283.71f,
    292.41f, 301.06f, 309.63f, 318.30f, 326.96f, 335.54f, 344.23f, 352.92f,
    361.48f, 370.18f, 378.89f, 387.47f, 396.14f, 404.86f, 413.52f, 422.13f,
    430.84f, 439.55f, 455.15f, 472.48f, 489.82f, 507.28f, 524.64f, 542.09f,
    559.48f, 576.89f, 594.41f, 611.82f, 629.23f, 646.77f, 664.23f, 681.68f,
    699.24f, 716.74f, 734.21f, 751.82f, 769.32f, 786.93f, 804.47f, 822.00f,
    839.65f, 857.20f, 874.78f, 895.94f, 931.22f, 966.44f, 1001.79f, 1037.19f,
    1072.52f, 1108.00f, 1143.52f, 1179.07f, 1214.56f, 1250.19f, 1285.87f, 1321.49f,
    1357.26f, 1393.06f, 1428.90f, 1464.69f, 1500.63f, 1536.61f, 1572.53f, 1608.60f,
    1644.70f, 1680.87f, 1716.97f, 1753.23f, 1789.53f, 1847.63f, 1920.42f, 1993.52f,
    2066.72f, 2140.22f, 2213.81f, 2287.74f, 2361.76f, 2436.01f, 2510.58f, 2585.27f,
    2660.29f, 2735.43f, 2810.93f, 2886.55f, 2962.40f, 3038.62f, 3114.98f, 3191.68f,
    3268.56f, 3345.69f, 3423.20f, 3500.85f, 3578.91f, 3657.11f, 3735.76f, 3893.59f,
    4052.83f, 4213.23f, 4374.83f, 4537.80f, 4702.14f, 4867.85f, 5035.01f, 5203.73f,
    5373.92f, 5545.74f, 5719.35f, 5894.61f, 6071.51f, 6250.40f, 6431.10f, 6614.05f,
    6798.84f, 6985.91f, 7175.27f, 7366.94f, 7561.12f, 7757.65f, 7957.07f, 8159.37f,
    8447.41f, 8869.69f, 9305.53f, 9757.18f, 10226.01f, 10714.28f, 11224.96f, 11761.16f,
    12327.18f, 12929.07f, 13574.35f, 14273.67f, 15128.78f, 15896.56f, 16715.86f, 17593.07f,
    18535.66f, 19552.40f, 20653.71f, 21852.00f, 23162.23f, 24602.55f, 26195.24f, 27967.94f,
    29955.39f, 33439.40f,
};

inline double threeBandFormulaHz(int i)
{
    const double u = (i - 50.5) / 25.5, e = std::floor(u);
    const double f = 55.113 * std::exp2(e) * (1 + (u - e)), k = 1.2059;
    return 44100 / (M_PI * k) * std::tan(M_PI * k * f / 44100);
}

inline double threeBandTableHz(int i)
{
    i = i < threeBandFirst ? threeBandFirst : (i > threeBandLast ? threeBandLast : i);
    return threeBandMeasuredHz[i - threeBandFirst];
}

// The Part three-band EQ's centre (mid) or half-corner (shelves) for index i.
inline double threeBandHz(int i)
{
#if OSCAIRN_FREQUENCY_SCALE_TABLE
    return threeBandTableHz(i);
#else
    return threeBandFormulaHz(i);
#endif
}
// The three-band EQ's high shelf uses its own mapping (measured on 17 indices, 131-249): the
// half-gain frequency is the linear-octave code (54.267 Hz at index 50.5, 25.5 indices per
// octave) compressed by the bilinear transform, 44100/pi atan(pi F / 44100), within 0.66%.
// No measured table: that needs a sweep of the 125 indices 131-255.
inline double threeBandHighShelfHalfGainHz(int i)
{
    const double u = (i - 50.5) / 25.5, e = std::floor(u);
    const double f = 54.267 * std::exp2(e) * (1 + (u - e));
    return 44100 / M_PI * std::atan(M_PI * f / 44100);
}
// The corner on the flat side the EQ modes take. It does not depend on the gain (as for the low
// shelf): it is the corner of the 12 dB shelf whose half-gain point is above.
inline double threeBandHighShelfCornerHz(int i)
{
    const double A = std::pow(10.0, 12.0 / 40);
    return 44100 / M_PI * std::atan(std::tan(M_PI * threeBandHighShelfHalfGainHz(i) / 44100) * A);
}

// The Master EQ's frequency index (0-60). Measured from renders (fx11b): every band follows the
// Data List's printed values, to 0.0% (low shelf), 0.8% (high shelf) and 0.4% (peaks from index
// 23; 14-22 unresolved, see the plan's known differences). The table is the printed one, which
// the measurements confirm; the nearest formula, 20 * 10^(i/20), is up to 5.16% off (at 1.2 and 12 kHz).
constexpr float masterPrintedHz[61] = {
    20, 22, 25, 28, 32, 36, 40, 45, 50, 56, 63, 70, 80, 90, 100, 110, 125, 140, 160, 180, 200,
    225, 250, 280, 315, 355, 400, 450, 500, 560, 630, 700, 800, 900, 1000, 1100, 1200, 1400, 1600, 1800, 2000,
    2200, 2500, 2800, 3200, 3600, 4000, 4500, 5000, 5600, 6300, 7000, 8000, 9000, 10000, 11000, 12000,
    14000, 16000, 18000, 20000};
inline double masterFormulaHz(int i) { return 20 * std::pow(10.0, i / 20.0); }
inline double masterTableHz(int i) { return masterPrintedHz[i < 0 ? 0 : (i > 60 ? 60 : i)]; }
inline double masterHz(int i)
{
#if OSCAIRN_FREQUENCY_SCALE_TABLE
    return masterTableHz(i);
#else
    return masterFormulaHz(i);
#endif
}
} // namespace oscairn_fx::scales

#endif
