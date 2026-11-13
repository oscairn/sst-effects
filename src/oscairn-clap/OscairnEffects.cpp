/*
 * Oscairn effects: sst-effects as CLAP plugins for the Oscairn host (doc/oscairn-plan.md).
 *
 * Copyright 2026, Jacob Schooley and the sst-effects authors.
 *
 * Released under the GNU General Public Licence v3 or later (GPL-3.0-or-later), as the rest of
 * this repository; see LICENSE.md.
 *
 * One plugin per effect category. A "Mode" parameter chooses the algorithm; every mode's
 * parameters are listed, each under a module path named after its mode. Each plugin offers
 * Oscairn's native effect contexts (RFC 0007): independent instances sharing one engine.
 */
#include "OscairnEffects.h"
#include "OscairnConfig.h"
#include "OffThread.h"

#include <oscairn-clap/effect-contexts.h>
#include <oscairn-clap/memory.h>
#include <oscairn-clap/ui.h>
#include <clap/ext/render.h>

#include "sst/basic-blocks/simd/setup.h"
#include "sst/effects/Reverb1.h"
#include "sst/effects/Reverb2.h"
#include "effects/FiveBandEq.h"
#include "effects/TwoBandEq.h"
#include "effects/PartAndMasterEq.h"
#include "effects/TempoCrossDelay.h"
#include "effects/CrossDelay.h"
#include "effects/TempoStereoDelay.h"
#include "effects/LRDelay.h"
#include "effects/LcrDelay.h"
#include "effects/TempoMonoDelay.h"
#include "effects/HallReverb.h"
#include "effects/CircuitCompressor.h"
#include "effects/RotarySpeaker.h"
#include "effects/AutoPan.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <locale>
#include <sstream>
#include <memory>
#include <string>
#include <vector>

namespace
{
namespace pm = sst::basic_blocks::params;
using Cfg = oscairn_fx::Config;
constexpr int block = Cfg::blockSize;

// ---- effects behind a small virtual interface, so modes can be swapped at run time ----

struct Effect
{
    uint32_t mode{0};
    virtual ~Effect() = default;
    virtual float *params() = 0;
    virtual void initialize() = 0;
    // As constructed, without allocating (CLAP reset); the caller sets the parameters and
    // initializes it again, as after building it.
    virtual void reset() = 0;
    virtual void process(float *L, float *R) = 0;
    virtual int ringoutBlocks() const = 0;
    virtual size_t bytes() const = 0;
    virtual void setTempo(double bpm) = 0;
    // How long the output may stay silent while the effect still holds sound (a delay's longest
    // gap between echoes); the tail ends after that much silence.
    virtual double quietSeconds() const = 0;
};

// An effect's own heap allocations (made at construction), where it has any.
template <typename FX> size_t heapBytesAt(double rate)
{
    if constexpr (requires { FX::heapBytesAt(rate); })
        return FX::heapBytesAt(rate);
    return 0;
}

template <typename FX> struct EffectOf final : Effect
{
    Cfg::GS gs;
    Cfg::ES es;
    FX fx;
    EffectOf(double rate, oscairn_fx::Worker *worker, const std::atomic<bool> *offline)
        : gs(rate, worker, offline), fx(&gs, &es, nullptr)
    {
    }
    float *params() override { return fx.paramStorage; }
    void initialize() override { fx.initialize(); }
    void reset() override
    {
        // An effect without allocations is constructed again in place; one with allocations (the
        // Oscairn modes) clears everything in its initialize.
        if constexpr (!requires { fx.heapBytes(); })
        {
            std::destroy_at(&fx);
            std::construct_at(&fx, &gs, &es, nullptr);
        }
    }
    void process(float *L, float *R) override { fx.processBlock(L, R); }
    int ringoutBlocks() const override { return fx.getRingoutDecay(); }
    void setTempo(double bpm) override { gs.tempo = bpm; }
    double quietSeconds() const override
    {
        if constexpr (requires { fx.quietSeconds(); })
            return fx.quietSeconds();
        return 0.1;
    }
    size_t bytes() const override
    {
        if constexpr (requires { fx.heapBytes(); })
            return sizeof(*this) + fx.heapBytes();
        return sizeof(*this);
    }
};

struct Mode
{
    uint32_t id;      // stable: the Mode parameter's value and the high bits of parameter IDs
    const char *name; // generic, also the module path of the mode's parameters
    std::vector<pm::ParamMetaData> meta;
    std::unique_ptr<Effect> (*make)(double rate, oscairn_fx::Worker *, const std::atomic<bool> *offline);
    size_t (*bytes)(double rate); // what make(rate, ...) allocates
};

// An effect instance whose buffers are sized for the running rate (OscairnConfig.h).
template <template <typename> class FX>
std::unique_ptr<Effect> makeSized(double rate, oscairn_fx::Worker *w, const std::atomic<bool> *o)
{
    using namespace oscairn_fx;
    if (rate <= sizedRates[0])
        return std::make_unique<EffectOf<FX<SizedConfig<sizedRates[0]>>>>(rate, w, o);
    if (rate <= sizedRates[1])
        return std::make_unique<EffectOf<FX<SizedConfig<sizedRates[1]>>>>(rate, w, o);
    if (rate <= sizedRates[2])
        return std::make_unique<EffectOf<FX<SizedConfig<sizedRates[2]>>>>(rate, w, o);
    if (rate <= sizedRates[3])
        return std::make_unique<EffectOf<FX<SizedConfig<sizedRates[3]>>>>(rate, w, o);
    if (rate <= sizedRates[4])
        return std::make_unique<EffectOf<FX<SizedConfig<sizedRates[4]>>>>(rate, w, o);
    if (rate <= sizedRates[5])
        return std::make_unique<EffectOf<FX<SizedConfig<sizedRates[5]>>>>(rate, w, o);
    return std::make_unique<EffectOf<FX<Cfg>>>(rate, w, o);
}

template <template <typename> class FX> size_t sizedBytes(double rate)
{
    using namespace oscairn_fx;
    for (int i = 0; i < 6; ++i)
        if (rate <= sizedRates[i])
        {
            constexpr size_t sizes[] = {
                sizeof(EffectOf<FX<SizedConfig<sizedRates[0]>>>), sizeof(EffectOf<FX<SizedConfig<sizedRates[1]>>>),
                sizeof(EffectOf<FX<SizedConfig<sizedRates[2]>>>), sizeof(EffectOf<FX<SizedConfig<sizedRates[3]>>>),
                sizeof(EffectOf<FX<SizedConfig<sizedRates[4]>>>), sizeof(EffectOf<FX<SizedConfig<sizedRates[5]>>>)};
            return sizes[i] + heapBytesAt<FX<Cfg>>(rate);
        }
    return sizeof(EffectOf<FX<Cfg>>) + heapBytesAt<FX<Cfg>>(rate);
}

template <template <typename> class FX> Mode mode(uint32_t id, const char *name)
{
    Mode m{id, name, {}, makeSized<FX>, sizedBytes<FX>};
    static_assert(FX<Cfg>::numParams <= Cfg::BC::maxParamCount);
    // Metadata from the smallest instance; it does not depend on the buffer sizes.
    auto probe = std::make_unique<EffectOf<FX<oscairn_fx::SizedConfig<oscairn_fx::sizedRates[0]>>>>(48000.0, nullptr, nullptr);
    for (int i = 0; i < FX<Cfg>::numParams; ++i)
        m.meta.push_back(probe->fx.paramAt(i));
    return m;
}

struct Param
{
    clap_id id;
    int mode;  // index into Category::modes, -1 for the Mode parameter
    int index; // the effect's parameter index
};

struct Category
{
    clap_plugin_descriptor_t descriptor;
    std::vector<Mode> modes;
    uint32_t defaultMode;
    std::vector<Param> params; // [0] is Mode

    void finish()
    {
        params.push_back({1, -1, -1});
        for (int m = 0; m < (int)modes.size(); ++m)
            for (int i = 0; i < (int)modes[m].meta.size(); ++i)
                if (modes[m].meta[i].type != pm::ParamMetaData::NONE)
                    params.push_back({(clap_id)(modes[m].id << 8 | i), m, i});
    }
    int modeIndex(uint32_t id) const
    {
        for (int m = 0; m < (int)modes.size(); ++m)
            if (modes[m].id == id)
                return m;
        return -1;
    }
    int paramIndex(clap_id id) const
    {
        for (int p = 0; p < (int)params.size(); ++p)
            if (params[p].id == id)
                return p;
        return -1;
    }
    const pm::ParamMetaData &meta(const Param &p) const { return modes[p.mode].meta[p.index]; }
};

const char *const reverbFeatures[] = {CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, CLAP_PLUGIN_FEATURE_REVERB,
                                      CLAP_PLUGIN_FEATURE_STEREO, nullptr};

const char *const delayFeatures[] = {CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, CLAP_PLUGIN_FEATURE_DELAY,
                                     CLAP_PLUGIN_FEATURE_STEREO, nullptr};

const char *const eqFeatures[] = {CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, CLAP_PLUGIN_FEATURE_EQUALIZER,
                                  CLAP_PLUGIN_FEATURE_STEREO, nullptr};

const char *const dynamicsFeatures[] = {CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, CLAP_PLUGIN_FEATURE_COMPRESSOR,
                                        CLAP_PLUGIN_FEATURE_STEREO, nullptr};

const char *const rotaryFeatures[] = {CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, CLAP_PLUGIN_FEATURE_TREMOLO,
                                     CLAP_PLUGIN_FEATURE_STEREO, nullptr};

const std::vector<Category> &categories()
{
    static const std::vector<Category> all = [] {
        std::vector<Category> c;
        Category reverb{{CLAP_VERSION_INIT, "org.oscairn.effects.reverb", "Oscairn FX Reverb",
                         "Oscairn", "https://github.com/oscairn/sst-effects", "", "", "0.5.2",
                         "Reverbs, one mode per algorithm", reverbFeatures},
                        {},
                        2,
                        {}};
        // Stock sst-effects reverbs, unchanged. Mode IDs are never reused or renumbered.
        reverb.modes.push_back(mode<sst::effects::reverb1::Reverb1>(1, "Reverb 1"));
        reverb.modes.push_back(mode<sst::effects::reverb2::Reverb2>(2, "Reverb 2"));
        reverb.modes.push_back(mode<oscairn_fx::hall::HallReverb>(3, "Hall A"));
        reverb.finish();
        c.push_back(std::move(reverb));

        Category eq{{CLAP_VERSION_INIT, "org.oscairn.effects.eq", "Oscairn FX EQ", "Oscairn",
                     "https://github.com/oscairn/sst-effects", "", "", "0.1.2",
                     "Equalizers, one mode per design", eqFeatures},
                    {},
                    1,
                    {}};
        eq.modes.push_back(mode<oscairn_fx::eq::FiveBandEq>(1, "Five-band EQ"));
        eq.modes.push_back(mode<oscairn_fx::eq::TwoBandEq>(2, "Two-band EQ"));
        eq.modes.push_back(mode<oscairn_fx::eq::ThreeBandEq>(3, "Three-band EQ"));
        eq.modes.push_back(mode<oscairn_fx::eq::MasterEq>(4, "Master EQ"));
        eq.finish();
        c.push_back(std::move(eq));

        Category delay{{CLAP_VERSION_INIT, "org.oscairn.effects.delay", "Oscairn FX Delay", "Oscairn",
                        "https://github.com/oscairn/sst-effects", "", "", "0.2.0",
                        "Delays, one mode per design", delayFeatures},
                       {},
                       1,
                       {}};
        delay.modes.push_back(mode<oscairn_fx::delay::TempoCrossDelay>(1, "Tempo Cross Delay"));
        delay.modes.push_back(mode<oscairn_fx::delay::CrossDelay>(2, "Cross Delay"));
        delay.modes.push_back(mode<oscairn_fx::delay::TempoStereoDelay>(3, "Tempo Stereo Delay"));
        delay.modes.push_back(mode<oscairn_fx::delay::LRDelay>(4, "L/R Delay"));
        delay.modes.push_back(mode<oscairn_fx::delay::LcrDelay>(5, "L/C/R Delay"));
        delay.modes.push_back(mode<oscairn_fx::delay::TempoMonoDelay>(6, "Tempo Mono Delay"));
        delay.finish();
        c.push_back(std::move(delay));

        Category dynamics{{CLAP_VERSION_INIT, "org.oscairn.effects.dynamics", "Oscairn FX Dynamics",
                           "Oscairn", "https://github.com/oscairn/sst-effects", "", "", "0.2.2",
                           "Compressors and other dynamics, one mode per design", dynamicsFeatures},
                          {},
                          1,
                          {}};
        dynamics.modes.push_back(mode<oscairn_fx::dynamics::CircuitCompressor>(1, "Circuit Compressor"));
        dynamics.finish();
        c.push_back(std::move(dynamics));

        // Rotary speakers and the auto pan; later the tremolo and the other rotary types (plan, section 6).
        Category rotary{{CLAP_VERSION_INIT, "org.oscairn.effects.tremolo-rotary", "Oscairn FX Tremolo & Rotary",
                         "Oscairn", "https://github.com/oscairn/sst-effects", "", "", "0.2.0",
                         "Tremolos and rotary speakers, one mode per design", rotaryFeatures},
                        {},
                        1,
                        {}};
        rotary.modes.push_back(mode<oscairn_fx::rotary::RotarySpeaker>(1, "Rotary A"));
        rotary.modes.push_back(mode<oscairn_fx::rotary::AutoPan>(2, "Auto Pan"));
        rotary.finish();
        c.push_back(std::move(rotary));
        return c;
    }();
    return all;
}

// ---- the plugin ----

struct Engine
{
    double sampleRate;
    std::atomic<uint32_t> contexts{0};
    // Off-audio-thread work of the engine's contexts (the EQs' rate matching); its thread starts
    // with the first context that needs it.
    oscairn_fx::Worker worker;
    explicit Engine(double rate) : sampleRate(rate) {}
};

struct Context
{
    clap_plugin_t plugin{};
    const clap_host_t *host;
    const Category &cat;
    std::shared_ptr<Engine> engine;
    std::vector<std::atomic<float>> values; // by Category::params index; [0] unused
    std::atomic<uint32_t> wanted;           // Mode parameter
    std::unique_ptr<Effect> fx;             // audio thread while active
    std::atomic<Effect *> pending{nullptr}, retired{nullptr};
    std::atomic<bool> callbackRequested{false};
    uint32_t shownMode; // main thread: the mode whose parameters the host last saw unhidden
    std::atomic<size_t> fxBytes{0};
    std::atomic<uint32_t> fxMode{0}; // the mode of fx
    bool active{false};
    alignas(16) float inL[block]{}, inR[block]{}, outL[block]{}, outR[block]{};
    uint32_t pos{0};
    uint32_t quietBlocks{0}, quietLimit{1};
    std::atomic<bool> tail{false};
    // No block has run since activation or reset: a state loaded now gets an effect built for it,
    // which plays from the first block as on a fresh instance (an EQ's rate-matched coefficients
    // included, which a change otherwise brings from the worker later).
    std::atomic<bool> unheard{false};
    std::atomic<bool> offline{false}; // clap.render: the host renders offline
    bool renderFixed{false};          // main thread: the render mode was set, or the plugin activated

    Context(const clap_host_t *h, const Category &c, std::shared_ptr<Engine> e)
        : host(h), cat(c), engine(std::move(e)), values(c.params.size()), wanted(c.defaultMode),
          shownMode(c.defaultMode)
    {
        for (size_t p = 1; p < cat.params.size(); ++p)
            values[p].store(cat.meta(cat.params[p]).defaultVal);
        if (engine)
            ++engine->contexts;
    }
    ~Context()
    {
        delete pending.exchange(nullptr);
        delete retired.exchange(nullptr);
        if (engine)
            --engine->contexts;
    }
};

Context &self(const clap_plugin_t *p) { return *static_cast<Context *>(p->plugin_data); }

// The context's own storage, whatever its mode.
size_t contextFixedBytes(const Context &s) { return sizeof(Context) + s.values.size() * sizeof(std::atomic<float>); }

// The context's storage and every effect it holds: during a mode change, the new one and the old
// one until it is freed. Reported the same through oscairn.effect-contexts and oscairn.memory.
size_t contextHeldBytes(const Context &s)
{
    size_t held = contextFixedBytes(s) + s.fxBytes.load();
    if (auto *p = s.pending.load())
        held += p->bytes();
    if (auto *r = s.retired.load())
        held += r->bytes();
    return held;
}

// The parameter values of the effect's mode into its storage.
void loadValues(Context &s, Effect &fx)
{
    const int m = s.cat.modeIndex(fx.mode);
    float *store = fx.params();
    for (size_t p = 1; p < s.cat.params.size(); ++p)
        if (s.cat.params[p].mode == m)
            store[s.cat.params[p].index] = s.values[p].load(std::memory_order_relaxed);
}

std::unique_ptr<Effect> build(Context &s, uint32_t modeId)
{
    auto fx = s.cat.modes[s.cat.modeIndex(modeId)].make(s.engine->sampleRate, &s.engine->worker, &s.offline);
    fx->mode = modeId;
    loadValues(s, *fx);
    fx->initialize();
    return fx;
}

void requestCallback(Context &s)
{
    if (s.host && !s.callbackRequested.exchange(true))
        s.host->request_callback(s.host);
}

bool setValue(Context &s, clap_id id, double value)
{
    const int p = s.cat.paramIndex(id);
    if (p < 0 || !std::isfinite(value))
        return false;
    if (p == 0)
    {
        const auto m = (uint32_t)std::lround(value);
        if (s.cat.modeIndex(m) < 0)
            return false;
        s.wanted.store(m);
        return true;
    }
    const auto &md = s.cat.meta(s.cat.params[p]);
    s.values[p].store(std::clamp((float)value, md.minVal, md.maxVal), std::memory_order_relaxed);
    return true;
}

void apply(Context &s, const clap_event_header_t *h)
{
    if (!h || h->space_id != CLAP_CORE_EVENT_SPACE_ID || h->type != CLAP_EVENT_PARAM_VALUE ||
        h->size < sizeof(clap_event_param_value_t))
        return;
    const auto &e = *reinterpret_cast<const clap_event_param_value_t *>(h);
    if (e.note_id != -1 || e.port_index != -1 || e.channel != -1 || e.key != -1)
        return;
    setValue(s, e.param_id, e.value);
}

void runBlock(Context &s)
{
    if (auto *p = s.pending.load(std::memory_order_acquire);
        p && !s.retired.load(std::memory_order_acquire))
    {
        // Swap in the mode built on the main thread; the old effect is freed there. The new
        // mode is published before pending clears, so the main thread never sees no pending
        // effect beside the old mode and builds the new one again.
        s.fxBytes.store(p->bytes());
        s.fxMode.store(p->mode);
        s.pending.store(nullptr, std::memory_order_release);
        s.retired.store(s.fx.release(), std::memory_order_release);
        s.fx.reset(p);
        requestCallback(s);
    }
    s.quietLimit = std::max<uint32_t>(1, (uint32_t)std::ceil(s.fx->quietSeconds() * s.engine->sampleRate / block));
    if (s.fx->mode != s.wanted.load(std::memory_order_relaxed))
        requestCallback(s);

    s.unheard.store(false, std::memory_order_relaxed);

    float inPeak = 0, outPeak = 0;
    for (int i = 0; i < block; ++i)
        inPeak = std::max({inPeak, std::abs(s.inL[i]), std::abs(s.inR[i])});
    loadValues(s, *s.fx);
    std::memcpy(s.outL, s.inL, sizeof(s.outL));
    std::memcpy(s.outR, s.inR, sizeof(s.outR));
    s.fx->process(s.outL, s.outR);
    for (int i = 0; i < block; ++i)
        outPeak = std::max({outPeak, std::abs(s.outL[i]), std::abs(s.outR[i])});
    // Quiet: no input and output below -140 dBFS for 100 ms.
    if (inPeak > 0 || outPeak >= 1e-7f)
        s.quietBlocks = 0;
    else if (s.quietBlocks < s.quietLimit)
        ++s.quietBlocks;
    s.tail.store(s.quietBlocks < s.quietLimit, std::memory_order_relaxed);
}

clap_process_status process(const clap_plugin_t *plugin, const clap_process_t *pr)
{
    auto &s = self(plugin);
    if (!s.fx || pr->audio_outputs_count < 1 || pr->audio_outputs[0].channel_count < 2)
        return CLAP_PROCESS_ERROR;
    oscairn_fx::ScopedFlushDenormals ftz;
    // Tempo from the host's transport, 120 BPM without one (plan, section 7)
    const auto *tr = pr->transport;
    s.fx->setTempo(tr && (tr->flags & CLAP_TRANSPORT_HAS_TEMPO) && tr->tempo > 0 ? tr->tempo : 120.0);
    const bool hasIn = pr->audio_inputs_count > 0 && pr->audio_inputs[0].channel_count >= 2;
    const float *inL = hasIn ? pr->audio_inputs[0].data32[0] : nullptr;
    const float *inR = hasIn ? pr->audio_inputs[0].data32[1] : nullptr;
    float *outL = pr->audio_outputs[0].data32[0];
    float *outR = pr->audio_outputs[0].data32[1];
    const uint32_t count = pr->in_events ? pr->in_events->size(pr->in_events) : 0;
    uint32_t event = 0;
    bool silentIn = true;
    for (uint32_t i = 0; i < pr->frames_count; ++i)
    {
        while (event < count)
        {
            const auto *h = pr->in_events->get(pr->in_events, event);
            if (h && h->time > i)
                break;
            apply(s, h);
            ++event;
        }
        const float l = inL ? inL[i] : 0.f, r = inR ? inR[i] : 0.f;
        silentIn = silentIn && l == 0.f && r == 0.f;
        s.inL[s.pos] = l;
        s.inR[s.pos] = r;
        // Read before writing: in-place buffers are allowed.
        outL[i] = s.outL[s.pos];
        outR[i] = s.outR[s.pos];
        if (++s.pos == block)
        {
            s.pos = 0;
            runBlock(s);
        }
    }
    for (; event < count; ++event)
        apply(s, pr->in_events->get(pr->in_events, event));
    return silentIn && !s.tail.load(std::memory_order_relaxed) ? CLAP_PROCESS_SLEEP
                                                                : CLAP_PROCESS_CONTINUE;
}

void clear(Context &s)
{
    std::memset(s.inL, 0, sizeof(s.inL));
    std::memset(s.inR, 0, sizeof(s.inR));
    std::memset(s.outL, 0, sizeof(s.outL));
    std::memset(s.outR, 0, sizeof(s.outR));
    s.pos = 0;
    s.quietBlocks = s.quietLimit;
    s.tail.store(false);
}

// The parameters of modes other than the selected one are hidden (plugin-porting.md: a mode
// that changes which controls matter hides the others); tell the host when that changes.
void rescanIfModeChanged(Context &s)
{
    const uint32_t want = s.wanted.load();
    if (want == s.shownMode)
        return;
    s.shownMode = want;
    if (!s.host)
        return;
    if (auto *hp = static_cast<const clap_host_params_t *>(s.host->get_extension(s.host, CLAP_EXT_PARAMS)); hp && hp->rescan)
        hp->rescan(s.host, CLAP_PARAM_RESCAN_INFO);
}

// The main-thread work; false if the wanted mode could not be built, and the current effect stays.
// rebuild: build the effect again even in the current mode (a state load before the first block).
bool mainThreadWork(Context &s, bool rebuild = false)
{
    s.callbackRequested.store(false);
    delete s.retired.exchange(nullptr, std::memory_order_acq_rel);
    rescanIfModeChanged(s);
    if (!s.active)
        return true;
    const uint32_t want = s.wanted.load();
    auto *p = s.pending.load(std::memory_order_acquire);
    const uint32_t current = p ? p->mode : s.fxMode.load();
    if (current == want && !(rebuild && !p))
        return true;
    if (!p)
    {
        // Build only when nothing is pending, so no pointer is freed under the audio thread;
        // a change while one is pending is picked up when the audio thread asks again.
        std::unique_ptr<Effect> fx;
        try
        {
            fx = build(s, want);
        }
        catch (...)
        {
            return false; // out of memory or threads; the audio thread asks again
        }
        Effect *expected = nullptr;
        if (!s.pending.compare_exchange_strong(expected, fx.get(), std::memory_order_acq_rel))
            return true;
        fx.release();
    }
    return true;
}

void onMainThread(const clap_plugin_t *plugin) { mainThreadWork(self(plugin)); }

// ---- extensions ----

uint32_t portCount(const clap_plugin_t *, bool) { return 1; }
bool portGet(const clap_plugin_t *, uint32_t index, bool isInput, clap_audio_port_info_t *info)
{
    if (index != 0)
        return false;
    info->id = 0;
    info->flags = CLAP_AUDIO_PORT_IS_MAIN;
    info->channel_count = 2;
    info->port_type = CLAP_PORT_STEREO;
    info->in_place_pair = CLAP_INVALID_ID;
    std::snprintf(info->name, sizeof(info->name), "%s", isInput ? "Input" : "Output");
    return true;
}
const clap_plugin_audio_ports_t audioPorts{portCount, portGet};

const clap_plugin_latency_t latency{[](const clap_plugin_t *) -> uint32_t {
    return oscairn_fx::latencySamples;
}};

// Offline, effects do in process what they otherwise hand to the worker thread, so an offline
// render does not depend on timing. Oscairn sets the mode once, after init and before state load
// and activation; it then stays for the instance's lifetime, and a later set may only repeat it.
const clap_plugin_render_t renderExt{
    [](const clap_plugin_t *) { return false; },
    [](const clap_plugin_t *plugin, clap_plugin_render_mode mode) {
        if (mode != CLAP_RENDER_REALTIME && mode != CLAP_RENDER_OFFLINE)
            return false;
        auto &s = self(plugin);
        const bool off = mode == CLAP_RENDER_OFFLINE;
        if (s.renderFixed)
            return s.offline.load() == off;
        s.offline.store(off);
        s.renderFixed = true;
        return true;
    }};

const clap_plugin_tail_t tailExt{[](const clap_plugin_t *plugin) -> uint32_t {
    auto &s = self(plugin);
    if (!s.tail.load())
        return 0;
    return UINT32_MAX; // shortcut: "until tail_active ends"; a per-mode length when modes report one
}};

bool paramInfo(const clap_plugin_t *plugin, uint32_t i, clap_param_info_t *out)
{
    auto &s = self(plugin);
    const auto &cat = s.cat;
    if (i >= cat.params.size())
        return false;
    *out = {};
    const auto &p = cat.params[i];
    out->id = p.id;
    if (p.mode < 0)
    {
        out->flags = CLAP_PARAM_IS_STEPPED | CLAP_PARAM_IS_AUTOMATABLE;
        out->min_value = cat.modes.front().id;
        out->max_value = cat.modes.back().id;
        out->default_value = cat.defaultMode;
        std::snprintf(out->name, sizeof(out->name), "Mode");
        return true;
    }
    const auto &md = cat.meta(p);
    out->flags = CLAP_PARAM_IS_AUTOMATABLE;
    if (md.type == pm::ParamMetaData::INT || md.type == pm::ParamMetaData::BOOL)
        out->flags |= CLAP_PARAM_IS_STEPPED;
    if (cat.modes[p.mode].id != s.wanted.load())
        out->flags |= CLAP_PARAM_IS_HIDDEN;
    out->min_value = md.minVal;
    out->max_value = md.maxVal;
    out->default_value = std::clamp(md.defaultVal, md.minVal, md.maxVal);
    std::snprintf(out->name, sizeof(out->name), "%s", md.name.c_str());
    std::snprintf(out->module, sizeof(out->module), "%s", cat.modes[p.mode].name);
    return true;
}

bool paramValue(const clap_plugin_t *plugin, clap_id id, double *value)
{
    auto &s = self(plugin);
    const int p = s.cat.paramIndex(id);
    if (p < 0)
        return false;
    *value = p == 0 ? (double)s.wanted.load() : (double)s.values[p].load();
    return true;
}

bool valueToText(const clap_plugin_t *plugin, clap_id id, double value, char *text, uint32_t n)
{
    const auto &cat = self(plugin).cat;
    const int p = cat.paramIndex(id);
    if (p < 0 || !std::isfinite(value))
        return false;
    if (p == 0)
    {
        const int m = cat.modeIndex((uint32_t)std::lround(value));
        if (m < 0)
            return false;
        std::snprintf(text, n, "%s", cat.modes[m].name);
        return true;
    }
    auto t = cat.meta(cat.params[p]).valueToString((float)value);
    std::snprintf(text, n, "%s", t ? t->c_str() : std::to_string(value).c_str());
    return true;
}

bool textToValue(const clap_plugin_t *plugin, clap_id id, const char *text, double *value)
{
    const auto &cat = self(plugin).cat;
    const int p = cat.paramIndex(id);
    if (p < 0)
        return false;
    if (p == 0)
    {
        for (const auto &m : cat.modes)
            if (!std::strcmp(m.name, text))
                return *value = m.id, true;
        return false;
    }
    std::string err;
    auto v = cat.meta(cat.params[p]).valueFromString(text, err);
    if (!v)
        return false;
    *value = *v;
    return true;
}

const clap_plugin_params_t params{
    [](const clap_plugin_t *p) -> uint32_t { return (uint32_t)self(p).cat.params.size(); },
    paramInfo,
    paramValue,
    valueToText,
    textToValue,
    [](const clap_plugin_t *p, const clap_input_events_t *in, const clap_output_events_t *) {
        auto &s = self(p);
        for (uint32_t i = 0; in && i < in->size(in); ++i)
            apply(s, in->get(in, i));
        if (!s.active)
            rescanIfModeChanged(s); // inactive: flush is on the main thread
    }};

// State: text independent of byte order and locale. Unknown IDs are skipped, missing ones keep
// their defaults. History is not state.
constexpr char stateHeader[] = "oscairn-effects-1 ";

bool stateSave(const clap_plugin_t *plugin, const clap_ostream_t *stream)
{
    auto &s = self(plugin);
    std::string out = stateHeader;
    out += s.cat.descriptor.id;
    out += '\n';
    char buf[64];
    for (size_t p = 0; p < s.cat.params.size(); ++p)
    {
        const double v = p == 0 ? (double)s.wanted.load() : (double)s.values[p].load();
        auto r = std::to_chars(buf, buf + sizeof(buf), (uint32_t)s.cat.params[p].id);
        *r.ptr++ = '=';
        r = std::to_chars(r.ptr, buf + sizeof(buf), v, std::chars_format::general, 17);
        if (r.ec != std::errc{})
            return false;
        out.append(buf, r.ptr);
        out += '\n';
    }
    for (size_t at = 0; at < out.size();)
    {
        const auto w = stream->write(stream, out.data() + at, out.size() - at);
        if (w <= 0)
            return false;
        at += (size_t)w;
    }
    return true;
}

bool stateLoad(const clap_plugin_t *plugin, const clap_istream_t *stream)
{
    auto &s = self(plugin);
    std::string in;
    char buf[4096];
    for (;;)
    {
        const auto got = stream->read(stream, buf, sizeof(buf));
        if (got < 0)
            return false;
        if (got == 0)
            break;
        in.append(buf, (size_t)got);
        if (in.size() > (1u << 20))
            return false;
    }
    const std::string header = std::string(stateHeader) + s.cat.descriptor.id + "\n";
    if (in.compare(0, header.size(), header) != 0)
        return false;
    std::vector<std::pair<clap_id, double>> parsed;
    for (size_t at = header.size(); at < in.size();)
    {
        const size_t end = std::min(in.find('\n', at), in.size());
        const char *b = in.data() + at, *e = in.data() + end;
        const char *eq = std::find(b, e, '=');
        uint32_t id = 0;
        double v = 0;
        if (eq == e || std::from_chars(b, eq, id).ptr != eq)
            return false;
        std::istringstream num(std::string(eq + 1, e));
        num.imbue(std::locale::classic());
        if (!(num >> v) || !(num >> std::ws).eof() || !std::isfinite(v))
            return false;
        parsed.emplace_back(id, v);
        at = end + 1;
    }
    std::vector<float> was(s.values.size());
    for (size_t p = 1; p < s.cat.params.size(); ++p)
    {
        was[p] = s.values[p].load();
        s.values[p].store(s.cat.meta(s.cat.params[p]).defaultVal);
    }
    const uint32_t wasMode = s.wanted.exchange(s.cat.defaultMode);
    for (auto [id, v] : parsed)
        if (s.cat.paramIndex(id) >= 0)
            setValue(s, id, v);
    if (!s.active)
        rescanIfModeChanged(s);
    else if (!mainThreadWork(s, s.unheard.load())) // build the loaded mode now, so memory has settled when this returns
    {
        // The loaded mode could not be built: put back the state the host had.
        for (size_t p = 1; p < s.cat.params.size(); ++p)
            s.values[p].store(was[p]);
        s.wanted.store(wasMode);
        rescanIfModeChanged(s);
        return false;
    }
    return true;
}

const clap_plugin_state_t state{stateSave, stateLoad};

const clap_plugin_t *create(const clap_host_t *host, const Category &cat,
                            std::shared_ptr<Engine> engine);

// oscairn.memory: what activating would allocate for the loaded state (the mode it selects, at
// the rate), and what is held. Allocations do not depend on the maximum block size; no assets.
const oscairn_plugin_memory_t memory{
    [](const clap_plugin_t *p, double rate, uint32_t, oscairn_memory_t *out) -> bool {
        if (!out || !std::isfinite(rate) || rate < 8000 || rate > 768000)
            return false;
        auto &s = self(p);
        const int m = s.cat.modeIndex(s.wanted.load());
        *out = {sizeof(Engine), contextFixedBytes(s) + s.cat.modes[m].bytes(rate), 0};
        return true;
    },
    [](const clap_plugin_t *p, oscairn_memory_t *out) -> bool {
        auto &s = self(p);
        if (!out || !s.active || !s.engine)
            return false;
        *out = {sizeof(Engine), contextHeldBytes(s), 0};
        return true;
    }};

// oscairn.ui (draft 5): Mode first, then the selected mode's parameters in their order; the effect's
// main control (its knob in the Effects bank) is the parameter named "Mix", or "Output Level" in a
// mode without a Mix. Other modes' parameters get no hint. No rich views (algorithm) and no time
// information (seconds): these are effects, with no envelope or map to draw.
const oscairn_plugin_ui_t ui{[](const clap_plugin_t *plugin, clap_id id, oscairn_param_hint_t *h) -> bool {
    auto &s = self(plugin);
    const int p = s.cat.paramIndex(id);
    if (p < 0 || !h)
        return false;
    *h = {OSCAIRN_WIDGET_AUTO, 0, CLAP_INVALID_ID, 0, OSCAIRN_ROLE_NONE};
    if (p == 0)
        return h->priority = 1, true;
    const auto &par = s.cat.params[p];
    if (s.cat.modes[par.mode].id != s.wanted.load())
        return true;
    h->priority = 2 + (uint32_t)par.index;
    const auto &name = s.cat.meta(par).name;
    bool modeHasMix = false;
    for (size_t q = 1; q < s.cat.params.size(); ++q)
        modeHasMix |= s.cat.params[q].mode == par.mode && s.cat.meta(s.cat.params[q]).name == "Mix";
    if (name == "Mix" || (name == "Output Level" && !modeHasMix))
        h->role = OSCAIRN_ROLE_MIX;
    return true;
}, nullptr, nullptr};

const oscairn_plugin_effect_contexts_t contexts{
    [](const clap_plugin_t *p, const clap_host_t *host) -> const clap_plugin_t * {
        auto &s = self(p);
        if (!s.engine || s.engine->contexts.load() >= oscairn_fx::contextCapacity)
            return nullptr;
        try
        {
            return create(host, s.cat, s.engine);
        }
        catch (...)
        {
            return nullptr;
        }
    },
    [](const clap_plugin_t *p, oscairn_effect_resources_t *out) -> bool {
        auto &s = self(p);
        if (!out || !s.engine)
            return false;
        *out = {sizeof(Engine), contextHeldBytes(s),
                oscairn_fx::contextCapacity, s.engine->contexts.load(), s.tail.load()};
        return true;
    }};

bool activate(const clap_plugin_t *plugin, double rate, uint32_t, uint32_t)
{
    auto &s = self(plugin);
    if (!std::isfinite(rate) || rate < 8000 || rate > 768000)
        return false;
    s.renderFixed = true; // never set: real time from here on
    if (!s.engine || s.engine->sampleRate != rate)
    {
        if (s.engine && s.engine->contexts.load() > 1)
            return false; // contexts of one engine share its sample rate
        auto e = std::make_shared<Engine>(rate);
        ++e->contexts;
        if (s.engine)
            --s.engine->contexts;
        s.engine = std::move(e);
    }
    try
    {
        s.fx = build(s, s.wanted.load());
    }
    catch (...)
    {
        return false;
    }
    s.fxBytes.store(s.fx->bytes());
    s.fxMode.store(s.fx->mode);
    s.quietLimit = std::max<uint32_t>(1, (uint32_t)std::ceil(s.fx->quietSeconds() * rate / block));
    clear(s);
    s.unheard.store(true);
    s.active = true;
    return true;
}

void deactivate(const clap_plugin_t *plugin)
{
    auto &s = self(plugin);
    s.active = false;
    s.fx.reset();
    delete s.pending.exchange(nullptr);
    delete s.retired.exchange(nullptr);
    s.fxBytes.store(0);
}

const clap_plugin_t *create(const clap_host_t *host, const Category &cat,
                            std::shared_ptr<Engine> engine)
{
    auto *s = new Context(host, cat, std::move(engine));
    s->plugin.desc = &cat.descriptor;
    s->plugin.plugin_data = s;
    s->plugin.init = [](const clap_plugin_t *) { return true; };
    s->plugin.destroy = [](const clap_plugin_t *p) { delete &self(p); };
    s->plugin.activate = activate;
    s->plugin.deactivate = deactivate;
    s->plugin.start_processing = [](const clap_plugin_t *) { return true; };
    s->plugin.stop_processing = [](const clap_plugin_t *) {};
    // At once, as a fresh instance: the effect as built, the block buffers empty.
    s->plugin.reset = [](const clap_plugin_t *p) {
        auto &s = self(p);
        if (s.fx)
        {
            s.fx->reset();
            loadValues(s, *s.fx);
            s.fx->initialize();
        }
        clear(s);
        s.unheard.store(true);
    };
    s->plugin.process = process;
    s->plugin.get_extension = [](const clap_plugin_t *, const char *id) -> const void * {
        if (!std::strcmp(id, CLAP_EXT_AUDIO_PORTS))
            return &audioPorts;
        if (!std::strcmp(id, CLAP_EXT_PARAMS))
            return &params;
        if (!std::strcmp(id, CLAP_EXT_STATE))
            return &state;
        if (!std::strcmp(id, CLAP_EXT_LATENCY))
            return &latency;
        if (!std::strcmp(id, CLAP_EXT_TAIL))
            return &tailExt;
        if (!std::strcmp(id, CLAP_EXT_RENDER))
            return &renderExt;
        if (!std::strcmp(id, OSCAIRN_EXT_EFFECT_CONTEXTS))
            return &contexts;
        if (!std::strcmp(id, OSCAIRN_EXT_MEMORY))
            return &memory;
        if (!std::strcmp(id, OSCAIRN_EXT_UI))
            return &ui;
        return nullptr;
    };
    s->plugin.on_main_thread = onMainThread;
    return &s->plugin;
}

const clap_plugin_factory_t factory{
    [](const clap_plugin_factory_t *) -> uint32_t { return (uint32_t)categories().size(); },
    [](const clap_plugin_factory_t *, uint32_t i) -> const clap_plugin_descriptor_t * {
        return i < categories().size() ? &categories()[i].descriptor : nullptr;
    },
    [](const clap_plugin_factory_t *, const clap_host_t *host,
       const char *id) -> const clap_plugin_t * {
        for (const auto &c : categories())
            if (!std::strcmp(id, c.descriptor.id))
                return create(host, c, {});
        return nullptr;
    }};
} // namespace

const clap_plugin_entry_t oscairn_effects_entry{
    CLAP_VERSION_INIT, [](const char *) { return true; }, []() {},
    [](const char *id) -> const void * {
        return std::strcmp(id, CLAP_PLUGIN_FACTORY_ID) == 0 ? &factory : nullptr;
    }};
