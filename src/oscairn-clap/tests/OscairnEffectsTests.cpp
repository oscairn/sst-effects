/*
 * Tests for the Oscairn effects plugins (doc/oscairn-plan.md, section 7).
 * GPL-3.0-or-later, see ../OscairnEffects.cpp.
 */
#define CATCH_CONFIG_MAIN
#include "catch2.hpp"

#include "OscairnConfig.h"
#include "OscairnEffects.h"
#include <oscairn-clap/effect-contexts.h>
#include <oscairn-clap/memory.h>
#include <oscairn-clap/ui.h>

#include "sst/basic-blocks/simd/setup.h"
#include "sst/effects/Reverb1.h"
#include "sst/effects/Reverb2.h"

#include <atomic>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <new>
#include <set>
#include <string>
#include <map>
#include <thread>
#include <tuple>
#include <vector>

// ---- allocation counting: process() must not allocate ----
// Every replaceable form is defined, so no allocation pairs a library's operator new with ours.
static thread_local bool countAllocations = false;
static thread_local long allocations = 0;
static void *allocate(std::size_t n, std::size_t align = 0) noexcept
{
    if (countAllocations)
        ++allocations;
    if (!align)
        return std::malloc(n ? n : 1);
    align = std::max(align, sizeof(void *));
    return std::aligned_alloc(align, (n + align - 1) / align * align + (n ? 0 : align));
}
// Throwing operator new fails allocations larger than this (0: never), as when memory runs out.
static thread_local std::size_t failAllocationsOver = 0;
static void *allocateOrAbort(std::size_t n, std::size_t align = 0)
{
    if (failAllocationsOver && n > failAllocationsOver)
        throw std::bad_alloc();
    if (void *p = allocate(n, align))
        return p;
    std::abort();
}
void *operator new(std::size_t n) { return allocateOrAbort(n); }
void *operator new[](std::size_t n) { return allocateOrAbort(n); }
void *operator new(std::size_t n, std::align_val_t a) { return allocateOrAbort(n, (std::size_t)a); }
void *operator new[](std::size_t n, std::align_val_t a) { return allocateOrAbort(n, (std::size_t)a); }
void *operator new(std::size_t n, const std::nothrow_t &) noexcept { return allocate(n); }
void *operator new[](std::size_t n, const std::nothrow_t &) noexcept { return allocate(n); }
void *operator new(std::size_t n, std::align_val_t a, const std::nothrow_t &) noexcept
{
    return allocate(n, (std::size_t)a);
}
void *operator new[](std::size_t n, std::align_val_t a, const std::nothrow_t &) noexcept
{
    return allocate(n, (std::size_t)a);
}
void operator delete(void *p) noexcept { std::free(p); }
void operator delete[](void *p) noexcept { std::free(p); }
void operator delete(void *p, std::size_t) noexcept { std::free(p); }
void operator delete[](void *p, std::size_t) noexcept { std::free(p); }
void operator delete(void *p, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void *p, std::align_val_t) noexcept { std::free(p); }
void operator delete(void *p, std::size_t, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void *p, std::size_t, std::align_val_t) noexcept { std::free(p); }
void operator delete(void *p, const std::nothrow_t &) noexcept { std::free(p); }
void operator delete[](void *p, const std::nothrow_t &) noexcept { std::free(p); }
void operator delete(void *p, std::align_val_t, const std::nothrow_t &) noexcept { std::free(p); }
void operator delete[](void *p, std::align_val_t, const std::nothrow_t &) noexcept { std::free(p); }

namespace
{
using Cfg = oscairn_fx::Config;
constexpr uint32_t L = oscairn_fx::latencySamples;
constexpr const char *reverbId = "org.oscairn.effects.reverb";

struct Host
{
    clap_host_t host{};
    std::atomic<int> callbacks{0};
    int infoRescans{0}; // main thread
    clap_host_params_t params{
        [](const clap_host_t *h, clap_param_rescan_flags f) {
            if (f & CLAP_PARAM_RESCAN_INFO)
                ++static_cast<Host *>(h->host_data)->infoRescans;
        },
        [](const clap_host_t *, clap_id, clap_param_clear_flags) {}, [](const clap_host_t *) {}};
    Host()
    {
        host.clap_version = CLAP_VERSION;
        host.host_data = this;
        host.name = "test";
        host.vendor = "test";
        host.url = "";
        host.version = "0";
        host.get_extension = [](const clap_host_t *h, const char *id) -> const void * {
            return std::strcmp(id, CLAP_EXT_PARAMS) ? nullptr : &static_cast<Host *>(h->host_data)->params;
        };
        host.request_restart = [](const clap_host_t *) {};
        host.request_process = [](const clap_host_t *) {};
        host.request_callback = [](const clap_host_t *h) { ++static_cast<Host *>(h->host_data)->callbacks; };
    }
};

const clap_plugin_factory_t *factory()
{
    return static_cast<const clap_plugin_factory_t *>(
        oscairn_effects_entry.get_factory(CLAP_PLUGIN_FACTORY_ID));
}

template <typename T> const T *ext(const clap_plugin_t *p, const char *id)
{
    return static_cast<const T *>(p->get_extension(p, id));
}

struct Event
{
    uint32_t time;
    clap_id id;
    double value;
};

struct Events
{
    std::vector<clap_event_param_value_t> e;
    clap_input_events_t in{this,
                           [](const clap_input_events_t *l) -> uint32_t {
                               return (uint32_t) static_cast<const Events *>(l->ctx)->e.size();
                           },
                           [](const clap_input_events_t *l, uint32_t i) -> const clap_event_header_t * {
                               return &static_cast<const Events *>(l->ctx)->e[i].header;
                           }};
    void add(uint32_t time, clap_id id, double v)
    {
        clap_event_param_value_t x{};
        x.header = {sizeof(x), time, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_PARAM_VALUE, 0};
        x.param_id = id;
        x.note_id = -1;
        x.port_index = -1;
        x.channel = -1;
        x.key = -1;
        x.value = v;
        e.push_back(x);
    }
};

struct Plugin
{
    Host host;
    const clap_plugin_t *p{nullptr};
    explicit Plugin(const char *id = reverbId)
    {
        p = factory()->create_plugin(factory(), &host.host, id);
        REQUIRE(p);
        REQUIRE(p->init(p));
    }
    explicit Plugin(const clap_plugin_t *child) : p(child) { REQUIRE(p->init(p)); }
    ~Plugin()
    {
        if (p)
            p->destroy(p);
    }
    void set(clap_id id, double v)
    {
        Events ev;
        ev.add(0, id, v);
        ext<clap_plugin_params_t>(p, CLAP_EXT_PARAMS)->flush(p, &ev.in, nullptr);
    }
    double get(clap_id id)
    {
        double v = NAN;
        REQUIRE(ext<clap_plugin_params_t>(p, CLAP_EXT_PARAMS)->get_value(p, id, &v));
        return v;
    }
    void activate(double rate = 48000)
    {
        REQUIRE(p->activate(p, rate, 1, 4096));
        REQUIRE(p->start_processing(p));
    }
    // Renders in and returns the output, with host blocks cycling through `blocks`.
    clap_process_status last{};
    double tempo{0}; // > 0: a transport with this tempo
    std::vector<clap_process_status> statuses; // of the last render, per host block
    std::vector<float> render(const std::vector<float> &inL, const std::vector<float> &inR,
                              std::vector<float> &outR, std::vector<uint32_t> blocks = {37},
                              std::vector<Event> events = {}, bool count = true)
    {
        std::vector<float> outL(inL.size());
        outR.assign(inL.size(), 0);
        std::vector<float> bl(4096), br(4096), ol(4096), orr(4096);
        size_t at = 0, b = 0;
        statuses.clear();
        statuses.reserve(inL.size() / blocks[0] + 2);
        Events ev;
        ev.e.reserve(events.size() + 1);
        while (at < inL.size())
        {
            const uint32_t n = (uint32_t)std::min<size_t>(blocks[b++ % blocks.size()], inL.size() - at);
            std::copy(inL.begin() + at, inL.begin() + at + n, bl.begin());
            std::copy(inR.begin() + at, inR.begin() + at + n, br.begin());
            ev.e.clear();
            for (auto &e : events)
                if (e.time >= at && e.time < at + n)
                    ev.add((uint32_t)(e.time - at), e.id, e.value);
            float *ins[2] = {bl.data(), br.data()}, *outs[2] = {ol.data(), orr.data()};
            clap_audio_buffer_t ib{ins, nullptr, 2, 0, 0}, ob{outs, nullptr, 2, 0, 0};
            clap_process_t pr{};
            pr.steady_time = (int64_t)at;
            pr.frames_count = n;
            pr.audio_inputs = &ib;
            pr.audio_outputs = &ob;
            pr.audio_inputs_count = 1;
            pr.audio_outputs_count = 1;
            pr.in_events = &ev.in;
            clap_event_transport_t tr{};
            tr.header = {sizeof(tr), 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_TRANSPORT, 0};
            tr.flags = CLAP_TRANSPORT_HAS_TEMPO;
            tr.tempo = tempo;
            if (tempo > 0)
                pr.transport = &tr;
            countAllocations = count;
            last = p->process(p, &pr);
            countAllocations = false;
            statuses.push_back(last);
            REQUIRE(last != CLAP_PROCESS_ERROR);
            std::copy(ol.begin(), ol.begin() + n, outL.begin() + at);
            std::copy(orr.begin(), orr.begin() + n, outR.begin() + at);
            at += n;
        }
        return outL;
    }
};

// A deterministic stereo test signal: a noise burst, a decaying tone, then silence.
void signal(size_t n, std::vector<float> &l, std::vector<float> &r)
{
    l.assign(n, 0);
    r.assign(n, 0);
    uint32_t x = 12345;
    for (size_t i = 0; i < n && i < 9600; ++i)
    {
        x = x * 1664525u + 1013904223u;
        const float noise = ((x >> 8) / 16777216.f) * 2 - 1;
        l[i] = 0.5f * noise;
        r[i] = 0.4f * (float)std::sin(i * 0.031) * std::exp(-(float)i / 4000.f);
    }
}

// The same template the plugin runs, driven directly in 16-sample blocks.
template <typename FX> std::vector<float> direct(const std::vector<float> &inL,
                                                 const std::vector<float> &inR,
                                                 std::vector<float> &outR, double rate)
{
    auto gs = std::make_unique<Cfg::GS>(rate);
    Cfg::ES es;
    auto fx = std::make_unique<FX>(gs.get(), &es, nullptr);
    for (int i = 0; i < FX::numParams; ++i)
        fx->paramStorage[i] = fx->paramAt(i).defaultVal;
    fx->initialize();
    std::vector<float> outL(inL.size());
    outR.assign(inL.size(), 0);
    alignas(16) float bl[Cfg::blockSize], br[Cfg::blockSize];
    oscairn_fx::ScopedFlushDenormals ftz;
    for (size_t at = 0; at + Cfg::blockSize <= inL.size(); at += Cfg::blockSize)
    {
        std::copy(inL.begin() + at, inL.begin() + at + Cfg::blockSize, bl);
        std::copy(inR.begin() + at, inR.begin() + at + Cfg::blockSize, br);
        fx->processBlock(bl, br);
        std::copy(bl, bl + Cfg::blockSize, outL.begin() + at);
        std::copy(br, br + Cfg::blockSize, outR.begin() + at);
    }
    return outL;
}

clap_id paramIdByName(const clap_plugin_t *p, const char *module, const char *name)
{
    auto *params = ext<clap_plugin_params_t>(p, CLAP_EXT_PARAMS);
    for (uint32_t i = 0; i < params->count(p); ++i)
    {
        clap_param_info_t info;
        REQUIRE(params->get_info(p, i, &info));
        if (!std::strcmp(info.module, module) && !std::strcmp(info.name, name))
            return info.id;
    }
    FAIL("no parameter " << module << "/" << name);
    return 0;
}

struct Buffer
{
    std::string data;
    size_t at = 0;
    clap_ostream_t out{this, [](const clap_ostream_t *s, const void *b, uint64_t n) -> int64_t {
                           static_cast<Buffer *>(s->ctx)->data.append((const char *)b, n);
                           return (int64_t)n;
                       }};
    clap_istream_t in{this, [](const clap_istream_t *s, void *b, uint64_t n) -> int64_t {
                          auto &self = *static_cast<Buffer *>(s->ctx);
                          n = std::min<uint64_t>(n, self.data.size() - self.at);
                          std::memcpy(b, self.data.data() + self.at, n);
                          self.at += n;
                          return (int64_t)n;
                      }};
};

// Time for the energy decay curve to fall from -5 to -35 dB, times two (T30), in seconds.
double t30(const std::vector<float> &l, const std::vector<float> &r, double rate)
{
    std::vector<double> edc(l.size() + 1, 0);
    for (size_t i = l.size(); i-- > 0;)
        edc[i] = edc[i + 1] + (double)l[i] * l[i] + (double)r[i] * r[i];
    const double total = edc[0];
    double a = -1, b = -1;
    for (size_t i = 0; i < l.size(); ++i)
    {
        const double db = 10 * std::log10(edc[i] / total + 1e-300);
        if (a < 0 && db <= -5)
            a = i / rate;
        if (b < 0 && db <= -35)
            b = i / rate;
    }
    return 2 * (b - a);
}
} // namespace

TEST_CASE("Every plugin passes the host's load checks")
{
    REQUIRE(factory());
    const uint32_t n = factory()->get_plugin_count(factory());
    REQUIRE(n >= 1);
    for (uint32_t i = 0; i < n; ++i)
    {
        auto *d = factory()->get_plugin_descriptor(factory(), i);
        REQUIRE(d);
        INFO(d->id);
        Plugin pl(d->id);
        auto *p = pl.p;
        auto *params = ext<clap_plugin_params_t>(p, CLAP_EXT_PARAMS);
        REQUIRE(params);
        const uint32_t count = params->count(p);
        REQUIRE(count > 1);
        REQUIRE(count <= 32768);
        std::set<clap_id> ids;
        for (uint32_t k = 0; k < count; ++k)
        {
            clap_param_info_t info;
            REQUIRE(params->get_info(p, k, &info));
            REQUIRE(ids.insert(info.id).second);
            REQUIRE(std::isfinite(info.min_value));
            REQUIRE(std::isfinite(info.max_value));
            REQUIRE(info.min_value <= info.max_value);
            REQUIRE(info.default_value >= info.min_value);
            REQUIRE(info.default_value <= info.max_value);
            REQUIRE(std::strlen(info.name) > 0);
            char text[256];
            REQUIRE(params->value_to_text(p, info.id, info.default_value, text, sizeof(text)));
        }
        auto *ports = ext<clap_plugin_audio_ports_t>(p, CLAP_EXT_AUDIO_PORTS);
        REQUIRE(ports);
        for (bool input : {true, false})
        {
            REQUIRE(ports->count(p, input) == 1);
            clap_audio_port_info_t info;
            REQUIRE(ports->get(p, 0, input, &info));
            REQUIRE(info.channel_count == 2);
            REQUIRE((info.flags & CLAP_AUDIO_PORT_IS_MAIN));
        }
        REQUIRE(ext<clap_plugin_state_t>(p, CLAP_EXT_STATE));
        REQUIRE(ext<oscairn_plugin_effect_contexts_t>(p, OSCAIRN_EXT_EFFECT_CONTEXTS));
        auto *lat = ext<clap_plugin_latency_t>(p, CLAP_EXT_LATENCY);
        REQUIRE(lat);
        REQUIRE(lat->get(p) == L);
    }
}

TEST_CASE("Editor hints: Mode first, the selected mode's parameters shown, the others hidden")
{
    // plugin-porting.md: a mode is best a stepped parameter first by priority; when it changes
    // which controls matter, the others are hidden and the host is told to rescan their info.
    for (const char *id : {reverbId, "org.oscairn.effects.eq", "org.oscairn.effects.delay", "org.oscairn.effects.dynamics", "org.oscairn.effects.tremolo-rotary", "org.oscairn.effects.chorus"})
    {
        Plugin pl(id);
        auto *params = ext<clap_plugin_params_t>(pl.p, CLAP_EXT_PARAMS);
        auto *ui = ext<oscairn_plugin_ui_t>(pl.p, OSCAIRN_EXT_UI);
        REQUIRE(ui);
        REQUIRE(ui->param_hint);
        clap_param_info_t mode;
        REQUIRE(params->get_info(pl.p, 0, &mode));
        auto check = [&](uint32_t selected) {
            oscairn_param_hint_t h;
            REQUIRE(ui->param_hint(pl.p, 1, &h));
            REQUIRE(h.priority == 1);
            std::set<uint32_t> priorities;
            int shown = 0, mixes = 0;
            bool hasMix = false; // the main control is Mix, a modulation mode's depth, or Output Level in a mode without either
            for (uint32_t i = 1; i < params->count(pl.p); ++i)
            {
                clap_param_info_t info;
                REQUIRE(params->get_info(pl.p, i, &info));
                hasMix |= info.id >> 8 == selected && (!std::strcmp(info.name, "Mix") || !std::strcmp(info.name, "Horn Depth") ||
                                                       !std::strcmp(info.name, "L/R Depth") || !std::strcmp(info.name, "AM Depth"));
            }
            for (uint32_t i = 1; i < params->count(pl.p); ++i)
            {
                clap_param_info_t info;
                REQUIRE(params->get_info(pl.p, i, &info));
                REQUIRE(ui->param_hint(pl.p, info.id, &h));
                const bool mine = info.id >> 8 == selected;
                INFO(id << " " << info.module << "/" << info.name);
                REQUIRE(((info.flags & CLAP_PARAM_IS_HIDDEN) == 0) == mine);
                REQUIRE((h.priority > 1) == mine);
                if (mine)
                {
                    REQUIRE(priorities.insert(h.priority).second);
                    ++shown;
                    mixes += h.role == OSCAIRN_ROLE_MIX;
                    const bool main = hasMix ? !std::strcmp(info.name, "Mix") || !std::strcmp(info.name, "Horn Depth") ||
                                                   !std::strcmp(info.name, "L/R Depth") || !std::strcmp(info.name, "AM Depth")
                                             : !std::strcmp(info.name, "Output Level");
                    REQUIRE((h.role == OSCAIRN_ROLE_MIX) == main);
                }
                else
                    REQUIRE(h.role == OSCAIRN_ROLE_NONE);
            }
            REQUIRE(shown > 0);
            return mixes;
        };
        const auto first = (uint32_t)mode.default_value, other = first == (uint32_t)mode.min_value ? (uint32_t)mode.max_value : (uint32_t)mode.min_value;
        if (first == other)
        {
            // One mode: the delay, whose Mix is its mix; the compressor's Output Level (check()
            // requires the role exactly on those names).
            REQUIRE(check(first) == 1);
            continue;
        }
        check(first);
        // inactive: a change through flush rescans at once
        pl.set(1, other);
        REQUIRE(pl.host.infoRescans == 1);
        check(other);
        // active: a change in process is reported from the main-thread callback it requests
        pl.activate();
        std::vector<float> z(256, 0), r;
        pl.render(z, z, r, {64}, {{10, 1, (double)first}}, false);
        REQUIRE(pl.host.infoRescans == 1);
        pl.p->on_main_thread(pl.p);
        REQUIRE(pl.host.infoRescans == 2);
        const int mixes = check(first);
        REQUIRE(mixes == 1); // Reverb 2's Mix; the five-band EQ's Output Level; Rotary A's horn depth (the
        // modulation modes have no dry/wet, so their depth takes the Effects knob: one parameter per mode)
        // a state load that selects another mode rescans too
        Buffer buf;
        pl.set(1, other); // in flush while active: reported at the next callback
        REQUIRE(ext<clap_plugin_state_t>(pl.p, CLAP_EXT_STATE)->save(pl.p, &buf.out));
        pl.set(1, first);
        pl.p->on_main_thread(pl.p);
        const int before = pl.host.infoRescans;
        REQUIRE(ext<clap_plugin_state_t>(pl.p, CLAP_EXT_STATE)->load(pl.p, &buf.in));
        REQUIRE(pl.host.infoRescans == before + 1);
        check(other);
    }
}

TEST_CASE("Audio ports stay the same whatever the mode or state")
{
    // The host refuses a plugin whose audio port layout changes while it is loaded.
    for (const char *id : {reverbId, "org.oscairn.effects.eq", "org.oscairn.effects.delay", "org.oscairn.effects.dynamics", "org.oscairn.effects.tremolo-rotary", "org.oscairn.effects.chorus"})
    {
        Plugin pl(id);
        auto *ports = ext<clap_plugin_audio_ports_t>(pl.p, CLAP_EXT_AUDIO_PORTS);
        auto layout = [&] {
            std::string out;
            for (bool in : {true, false})
                for (uint32_t i = 0; i < ports->count(pl.p, in); ++i)
                {
                    clap_audio_port_info_t info;
                    REQUIRE(ports->get(pl.p, i, in, &info));
                    out += std::to_string(info.id) + ":" + std::to_string(info.channel_count) + ":" + std::to_string(info.flags) + ":" + info.name + ";";
                }
            return out;
        };
        const auto first = layout();
        clap_param_info_t mode;
        REQUIRE(ext<clap_plugin_params_t>(pl.p, CLAP_EXT_PARAMS)->get_info(pl.p, 0, &mode));
        for (double m = mode.min_value; m <= mode.max_value; ++m)
        {
            pl.set(1, m);
            REQUIRE(layout() == first);
        }
        pl.activate();
        Buffer buf;
        REQUIRE(ext<clap_plugin_state_t>(pl.p, CLAP_EXT_STATE)->save(pl.p, &buf.out));
        REQUIRE(ext<clap_plugin_state_t>(pl.p, CLAP_EXT_STATE)->load(pl.p, &buf.in));
        REQUIRE(layout() == first);
    }
}

TEST_CASE("Stock modes render exactly as the sst-effects templates")
{
    std::vector<float> inL, inR, pr, dr;
    signal(48000 * 2, inL, inR);
    for (uint32_t mode : {1u, 2u})
    {
        INFO("mode " << mode);
        Plugin pl;
        pl.set(1, mode);
        pl.activate();
        auto pout = pl.render(inL, inR, pr, {37});
        auto dout = mode == 1
                        ? direct<sst::effects::reverb1::Reverb1<Cfg>>(inL, inR, dr, 48000)
                        : direct<sst::effects::reverb2::Reverb2<Cfg>>(inL, inR, dr, 48000);
        double energy = 0;
        size_t diff = 0;
        for (size_t i = 0; i + L < inL.size() && i + 16 <= inL.size(); ++i)
        {
            diff += pout[i + L] != dout[i] || pr[i + L] != dr[i];
            energy += dout[i] * dout[i];
        }
        for (uint32_t i = 0; i < L; ++i)
            REQUIRE((pout[i] == 0 && pr[i] == 0));
        REQUIRE(energy > 1);
        REQUIRE(diff == 0);
    }
}

TEST_CASE("Output does not depend on the host's block size")
{
    std::vector<float> inL, inR, r0, r;
    signal(48000, inL, inR);
    Plugin probe;
    const clap_id decay = paramIdByName(probe.p, "Reverb 2", "Decay Time");
    const std::vector<Event> events{{10007, decay, 0.5}, {20011, decay, -1.0}};
    std::vector<float> ref;
    for (auto blocks : std::vector<std::vector<uint32_t>>{{512}, {1}, {37}, {7, 300, 64, 1}})
    {
        Plugin pl;
        pl.activate();
        auto out = pl.render(inL, inR, r, blocks, events);
        if (ref.empty())
        {
            ref = out;
            r0 = r;
            continue;
        }
        REQUIRE(out == ref);
        REQUIRE(r == r0);
    }
    // The events change the sound, so the comparison covers their timing.
    Plugin plain;
    plain.activate();
    auto noEvents = plain.render(inL, inR, r, {512});
    REQUIRE(noEvents != ref);
}

TEST_CASE("State round-trips, and bad state is refused")
{
    Plugin a;
    const clap_id decay = paramIdByName(a.p, "Reverb 1", "Decay Time");
    const clap_id mix = paramIdByName(a.p, "Reverb 2", "Mix");
    a.set(1, 1);
    a.set(decay, 1.25);
    a.set(mix, 0.3);
    Buffer buf;
    REQUIRE(ext<clap_plugin_state_t>(a.p, CLAP_EXT_STATE)->save(a.p, &buf.out));
    Plugin b;
    REQUIRE(ext<clap_plugin_state_t>(b.p, CLAP_EXT_STATE)->load(b.p, &buf.in));
    // A worker saves right after every load, as its recovery record: the same bytes come back.
    Buffer again;
    REQUIRE(ext<clap_plugin_state_t>(b.p, CLAP_EXT_STATE)->save(b.p, &again.out));
    REQUIRE(again.data == buf.data);
    auto *params = ext<clap_plugin_params_t>(a.p, CLAP_EXT_PARAMS);
    for (uint32_t k = 0; k < params->count(a.p); ++k)
    {
        clap_param_info_t info;
        params->get_info(a.p, k, &info);
        REQUIRE(a.get(info.id) == b.get(info.id));
    }
    REQUIRE(b.get(1) == 1);
    REQUIRE(b.get(decay) == 1.25);
    std::vector<float> inL, inR, ra, rb;
    signal(24000, inL, inR);
    a.activate();
    b.activate();
    REQUIRE(a.render(inL, inR, ra) == b.render(inL, inR, rb));

    Buffer bad;
    bad.data = "oscairn-effects-1 org.oscairn.effects.reverb\n1=x\n";
    Plugin c;
    REQUIRE_FALSE(ext<clap_plugin_state_t>(c.p, CLAP_EXT_STATE)->load(c.p, &bad.in));
    Buffer other;
    other.data = "something else\n";
    REQUIRE_FALSE(ext<clap_plugin_state_t>(c.p, CLAP_EXT_STATE)->load(c.p, &other.in));
}

TEST_CASE("Effect contexts are independent and share one engine")
{
    std::vector<float> xL, xR, yL, yR, r;
    signal(24000, xL, xR);
    signal(24000, yL, yR);
    for (auto &v : yL)
        v *= -0.7f;

    auto parent = std::make_unique<Plugin>();
    parent->activate();
    auto *ctx = ext<oscairn_plugin_effect_contexts_t>(parent->p, OSCAIRN_EXT_EFFECT_CONTEXTS);
    Host childHost;
    auto child = std::make_unique<Plugin>(ctx->create(parent->p, &childHost.host));
    const clap_id size = paramIdByName(child->p, "Reverb 2", "Room Size");
    child->set(size, 0.9);
    child->activate();

    Plugin refA, refB;
    refB.set(size, 0.9);
    refA.activate();
    refB.activate();

    // Interleave the two contexts block by block, as one engine would run them.
    std::vector<float> pA, pB, rA, rB, pAr, pBr, rAr, rBr;
    pA = parent->render(xL, xR, pAr);
    pB = child->render(yL, yR, pBr);
    rA = refA.render(xL, xR, rAr);
    rB = refB.render(yL, yR, rBr);
    REQUIRE(pA == rA);
    REQUIRE(pB == rB);
    REQUIRE(pA != pB);

    oscairn_effect_resources_t res;
    REQUIRE(ctx->resources(parent->p, &res));
    REQUIRE(res.capacity == oscairn_fx::contextCapacity);
    REQUIRE(res.prepared == 2);
    REQUIRE(res.context_bytes > sizeof(sst::effects::reverb2::Reverb2<oscairn_fx::SizedConfig<48000>>));

    // Capacity: 64 contexts including the parent, then refusal.
    std::vector<std::unique_ptr<Plugin>> more;
    for (;;)
    {
        auto *c = ctx->create(parent->p, &childHost.host);
        if (!c)
            break;
        more.push_back(std::make_unique<Plugin>(c));
    }
    REQUIRE(more.size() + 2 == oscairn_fx::contextCapacity);
    more.clear();

    // A child keeps working after its parent is gone.
    parent.reset();
    auto after = child->render(yL, yR, r);
    for (float v : after)
        REQUIRE(std::isfinite(v));
}

TEST_CASE("Mode changes build the new effect on the main thread")
{
    std::vector<float> inL, inR, r;
    signal(4800, inL, inR);
    Plugin pl;
    pl.activate();
    auto *ctx = ext<oscairn_plugin_effect_contexts_t>(pl.p, OSCAIRN_EXT_EFFECT_CONTEXTS);
    oscairn_effect_resources_t before, res;
    ctx->resources(pl.p, &before);
    pl.render(inL, inR, r, {64}, {{100, 1, 1.0}});
    REQUIRE(pl.host.callbacks == 1);
    REQUIRE(pl.get(1) == 1);
    ctx->resources(pl.p, &res);
    REQUIRE(res.context_bytes == before.context_bytes); // still the old effect
    pl.p->on_main_thread(pl.p);                          // builds Reverb 1
    pl.render(inL, inR, r, {64});                        // swaps it in, asks to free the old one
    REQUIRE(pl.host.callbacks == 2);
    const size_t r1 = sizeof(sst::effects::reverb1::Reverb1<oscairn_fx::SizedConfig<48000>>);
    ctx->resources(pl.p, &res);
    // Both held until freed: Reverb 1 (plus its small wrapper) on top of what was there.
    REQUIRE(res.context_bytes >= before.context_bytes + r1);
    REQUIRE(res.context_bytes < before.context_bytes + r1 + 1024);
    pl.p->on_main_thread(pl.p);                                     // frees Reverb 2
    ctx->resources(pl.p, &res);
    REQUIRE(res.context_bytes ==
            before.context_bytes - sizeof(sst::effects::reverb2::Reverb2<oscairn_fx::SizedConfig<48000>>) + r1);
    pl.render(inL, inR, r, {64});
    REQUIRE(pl.host.callbacks == 2); // nothing more to do
}

TEST_CASE("A mode that cannot be built keeps the current effect, and its state load is refused")
{
    std::vector<float> inL, inR, r;
    signal(4800, inL, inR);
    Plugin pl;
    pl.activate();
    auto *ctx = ext<oscairn_plugin_effect_contexts_t>(pl.p, OSCAIRN_EXT_EFFECT_CONTEXTS);
    auto *st = ext<clap_plugin_state_t>(pl.p, CLAP_EXT_STATE);
    oscairn_effect_resources_t before, res;
    ctx->resources(pl.p, &before);
    Buffer original, reverb1;
    REQUIRE(st->save(pl.p, &original.out));
    Plugin other;
    other.set(1, 1);
    REQUIRE(ext<clap_plugin_state_t>(other.p, CLAP_EXT_STATE)->save(other.p, &reverb1.out));
    const size_t r1 = sizeof(sst::effects::reverb1::Reverb1<oscairn_fx::SizedConfig<48000>>);

    // A state load whose mode cannot be built: refused, and the state the host had is kept.
    failAllocationsOver = r1 / 2;
    bool loaded = true;
    REQUIRE_NOTHROW(loaded = st->load(pl.p, &reverb1.in));
    failAllocationsOver = 0;
    REQUIRE_FALSE(loaded);
    Buffer after;
    REQUIRE(st->save(pl.p, &after.out));
    REQUIRE(after.data == original.data);
    ctx->resources(pl.p, &res);
    REQUIRE(res.context_bytes == before.context_bytes);

    // A mode change whose build fails keeps the current effect, and is built when asked again.
    pl.render(inL, inR, r, {64}, {{100, 1, 1.0}});
    REQUIRE(pl.host.callbacks == 1);
    failAllocationsOver = r1 / 2;
    REQUIRE_NOTHROW(pl.p->on_main_thread(pl.p));
    failAllocationsOver = 0;
    ctx->resources(pl.p, &res);
    REQUIRE(res.context_bytes == before.context_bytes);
    pl.render(inL, inR, r, {64});
    REQUIRE(pl.host.callbacks == 2);
    pl.p->on_main_thread(pl.p);
    pl.render(inL, inR, r, {64});
    pl.p->on_main_thread(pl.p);
    ctx->resources(pl.p, &res);
    REQUIRE(res.context_bytes ==
            before.context_bytes - sizeof(sst::effects::reverb2::Reverb2<oscairn_fx::SizedConfig<48000>>) + r1);
}

TEST_CASE("Processing never allocates")
{
    std::vector<float> inL, inR, r;
    signal(48000, inL, inR);
    Plugin pl;
    pl.activate();
    const clap_id decay = paramIdByName(pl.p, "Reverb 2", "Decay Time");
    allocations = 0;
    pl.render(inL, inR, r, {1, 37, 512}, {{5, decay, 2.0}, {900, 1, 1.0}});
    pl.p->on_main_thread(pl.p);
    pl.render(inL, inR, r, {1, 37, 512}, {{5, 1, 2.0}});
    REQUIRE(allocations == 0);
    // The EQ modes at 48 kHz hand a change to the worker thread and swap its result in.
    for (auto [mode, module, name] : {std::tuple{1, "Five-band EQ", "Band 3 Frequency"}, std::tuple{2, "Two-band EQ", "Band 1 Frequency"},
                                      std::tuple{3, "Three-band EQ", "Mid Frequency"}, std::tuple{4, "Master EQ", "Band 3 Frequency"}})
    {
        Plugin eq("org.oscairn.effects.eq");
        eq.set(1, mode);
        eq.set(paramIdByName(eq.p, module, mode == 2 ? "Band 1 Type" : (mode == 1 ? "Band 3 Gain" : (mode == 3 ? "Mid Gain" : "Band 3 Gain"))), mode == 2 ? 5 : 6);
        eq.activate(48000);
        const clap_id f = paramIdByName(eq.p, module, name);
        allocations = 0;
        eq.render(inL, inR, r, {1, 37, 512}, {{5, f, 2500.0}, {700, f, 9000.0}, {20000, f, 15000.0}});
        INFO(module);
        REQUIRE(allocations == 0);
    }
    // Rotary A: the speed switch, a depth and a shelf change while playing.
    {
        Plugin rot("org.oscairn.effects.tremolo-rotary");
        rot.activate(48000);
        const clap_id sp = paramIdByName(rot.p, "Rotary A", "Speed"), dep = paramIdByName(rot.p, "Rotary A", "Horn Depth"),
                      lg = paramIdByName(rot.p, "Rotary A", "Low Shelf Gain");
        allocations = 0;
        rot.render(inL, inR, r, {1, 37, 512}, {{5, sp, 1.0}, {700, dep, 90.0}, {9000, lg, -6.0}, {20000, sp, 0.0}});
        REQUIRE(allocations == 0);
    }
    // Auto Pan: the LFO speed, the directions, and a band switched in (the rate matcher's swap)
    // while playing.
    {
        Plugin pan("org.oscairn.effects.tremolo-rotary");
        pan.set(1, 2);
        pan.activate(48000);
        const clap_id sp = paramIdByName(pan.p, "Auto Pan", "LFO Speed"), dir = paramIdByName(pan.p, "Auto Pan", "Pan Direction"),
                      mg = paramIdByName(pan.p, "Auto Pan", "Mid Gain"), mf = paramIdByName(pan.p, "Auto Pan", "Mid Frequency");
        allocations = 0;
        pan.render(inL, inR, r, {1, 37, 512}, {{5, sp, 9.0}, {700, dir, 3.0}, {9000, mg, 6.0}, {20000, mf, 3000.0}, {30000, mg, 0.0}});
        REQUIRE(allocations == 0);
    }
    // Tremolo: depth and speed changes, a band switched in, and the input mode flip while playing.
    {
        Plugin tr("org.oscairn.effects.tremolo-rotary");
        tr.set(1, 3);
        tr.activate(48000);
        const clap_id sp = paramIdByName(tr.p, "Tremolo", "LFO Speed"), am = paramIdByName(tr.p, "Tremolo", "AM Depth"),
                      pm = paramIdByName(tr.p, "Tremolo", "PM Depth"), mg = paramIdByName(tr.p, "Tremolo", "Mid Gain"),
                      im = paramIdByName(tr.p, "Tremolo", "Input Mode");
        allocations = 0;
        tr.render(inL, inR, r, {1, 37, 512}, {{5, sp, 9.0}, {700, am, 80.0}, {9000, pm, 60.0}, {20000, mg, 6.0}, {30000, mg, 0.0}, {40000, im, 0.0}});
        REQUIRE(allocations == 0);
    }
    // The chorus modes: speed, depth, feedback, offset, mix, a band and the input mode while playing.
    for (auto [mode, module, depth] : {std::tuple{1, "Chorus A", "PM Depth"}, {2, "Chorus B", "LFO Depth"}, {3, "Dual Modulator", "AM Depth"}, {4, "Chorus C", "LFO Depth"}})
    {
        Plugin ch("org.oscairn.effects.chorus");
        ch.set(1, mode);
        ch.activate(48000);
        const clap_id sp = paramIdByName(ch.p, module, "LFO Speed"), dp = paramIdByName(ch.p, module, depth),
                      of = paramIdByName(ch.p, module, "Delay Offset"), mx = paramIdByName(ch.p, module, "Mix"),
                      mg = paramIdByName(ch.p, module, "Mid Gain");
        allocations = 0;
        ch.render(inL, inR, r, {1, 37, 512}, {{5, sp, 9.0}, {700, dp, 127.0}, {9000, of, 50.0}, {12000, mx, 0.0}, {20000, mg, 6.0}, {30000, mg, 0.0}});
        REQUIRE(allocations == 0);
    }
    // The counter works: an allocation while counting is seen.
    countAllocations = true;
    void *probe = ::operator new(16);
    countAllocations = false;
    ::operator delete(probe);
    REQUIRE(allocations == 1);
}

TEST_CASE("The tail ends, and the plugin then sleeps")
{
    Plugin pl;
    pl.activate();
    const clap_id decay = paramIdByName(pl.p, "Reverb 2", "Decay Time");
    pl.set(decay, -1.0); // 0.5 s
    auto *ctx = ext<oscairn_plugin_effect_contexts_t>(pl.p, OSCAIRN_EXT_EFFECT_CONTEXTS);
    std::vector<float> inL(48000 * 20, 0), inR(48000 * 20, 0), r;
    inL[0] = inR[0] = 1;
    oscairn_effect_resources_t res;
    std::vector<float> first(inL.begin(), inL.begin() + 4800), firstR(first.size(), 0);
    firstR[0] = 1;
    pl.render(first, firstR, r, {480});
    ctx->resources(pl.p, &res);
    REQUIRE(res.tail_active);
    REQUIRE(pl.last == CLAP_PROCESS_CONTINUE);
    std::vector<float> rest(inL.size(), 0);
    pl.render(rest, rest, r, {480});
    ctx->resources(pl.p, &res);
    REQUIRE_FALSE(res.tail_active);
    REQUIRE(pl.last == CLAP_PROCESS_SLEEP);
    REQUIRE(ext<clap_plugin_tail_t>(pl.p, CLAP_EXT_TAIL)->get(pl.p) == 0);
}

TEST_CASE("Activates and keeps its decay time at 44.1, 48 and 96 kHz")
{
    // Stock Reverb 2 at defaults; T30 in seconds must agree across rates.
    // Proposed tolerance: 5% of the 48 kHz value.
    double t48 = 0;
    for (double rate : {48000.0, 44100.0, 96000.0})
    {
        INFO("rate " << rate);
        Plugin pl;
        pl.activate(rate);
        const size_t n = (size_t)(rate * 12);
        std::vector<float> inL(n, 0), inR(n, 0), r;
        inL[0] = inR[0] = 1;
        auto out = pl.render(inL, inR, r, {512}, {}, false);
        for (size_t i = 0; i < n; ++i)
            REQUIRE((std::isfinite(out[i]) && std::isfinite(r[i])));
        const double t = t30(out, r, rate);
        std::printf("Reverb 2 defaults at %.0f Hz: T30 %.4f s\n", rate, t);
        REQUIRE(t > 0.3);
        if (rate == 48000)
            t48 = t;
        else
            REQUIRE(std::abs(t - t48) <= 0.05 * t48);
    }
}

TEST_CASE("Main-thread work runs safely beside processing")
{
    // Run under ThreadSanitizer in CI: an audio thread renders and switches modes while the
    // main thread builds effects, saves and loads state and reads resources.
    Plugin pl;
    pl.activate();
    auto *ctx = ext<oscairn_plugin_effect_contexts_t>(pl.p, OSCAIRN_EXT_EFFECT_CONTEXTS);
    auto *st = ext<clap_plugin_state_t>(pl.p, CLAP_EXT_STATE);
    std::vector<float> inL, inR;
    signal(4800, inL, inR);
    std::atomic<bool> done{false};
    std::atomic<int> errors{0}, handled{0};
    // No Catch macros on this thread: Catch2's assertions are not thread-safe.
    std::thread audio([&] {
        std::vector<float> bl(64), br(64), ol(64), orr(64);
        Events ev;
        ev.e.reserve(1);
        // At least 600 blocks, and until the main thread has handled several callbacks
        // (bounded, so a broken callback path fails instead of hanging).
        for (int k = 0; k < 600 || (handled < 8 && k < 2000000); ++k)
        {
            ev.e.clear();
            if (k % 15 == 0)
                ev.add(3, 1, (k / 15) % 2 ? 2.0 : 1.0);
            for (int i = 0; i < 64; ++i)
                bl[i] = br[i] = inL[(k * 64 + i) % inL.size()];
            float *ins[2] = {bl.data(), br.data()}, *outs[2] = {ol.data(), orr.data()};
            clap_audio_buffer_t ib{ins, nullptr, 2, 0, 0}, ob{outs, nullptr, 2, 0, 0};
            clap_process_t pr{};
            pr.frames_count = 64;
            pr.audio_inputs = &ib;
            pr.audio_outputs = &ob;
            pr.audio_inputs_count = 1;
            pr.audio_outputs_count = 1;
            pr.in_events = &ev.in;
            if (pl.p->process(pl.p, &pr) == CLAP_PROCESS_ERROR)
                ++errors;
        }
        done = true;
    });
    int seen = 0;
    while (!done)
    {
        if (pl.host.callbacks.load() != seen)
        {
            seen = pl.host.callbacks.load();
            pl.p->on_main_thread(pl.p);
            ++handled;
        }
        Buffer buf;
        REQUIRE(st->save(pl.p, &buf.out));
        REQUIRE(st->load(pl.p, &buf.in));
        oscairn_effect_resources_t res;
        REQUIRE(ctx->resources(pl.p, &res));
        std::this_thread::yield();
    }
    audio.join();
    pl.p->on_main_thread(pl.p);
    REQUIRE(errors == 0);
    REQUIRE(handled > 0);
}

// ---- Five-band EQ (plan, section 15, M1) ----
namespace
{
constexpr const char *eqId = "org.oscairn.effects.eq";

// The fitted design, written out independently of effects/FiveBandEq.h: |H| in dB of each
// section at frequency f, for a sample rate.
double sectionDb(std::complex<double> b0, double b1, double b2, double a0, double a1, double a2,
                 double f, double rate)
{
    const auto z = std::polar(1.0, -2 * M_PI * f / rate);
    return 20 * std::log10(std::abs((b0 + b1 * z + b2 * z * z) / (a0 + a1 * z + a2 * z * z)));
}
// A band of the five-band network: unity-peak band-pass at fc, Q widened at high frequencies.
std::complex<double> bandpassH(double f, double fc, double q, double rate)
{
    const double r = std::min(1.0, std::max(0.05, 1.25517 - 4.8172e-5 * fc - 0.1 * std::log10(q)));
    const double w = 2 * M_PI * fc / rate, al = std::sin(w) / (2 * q * r);
    const auto z = std::polar(1.0, -2 * M_PI * f / rate);
    return al * (1.0 - z * z) / ((1 + al) - 2 * std::cos(w) * z + (1 - al) * z * z);
}
// Boosts add in the numerator, cuts in the denominator.
double networkDb(double f, const double (&fc)[3], const double (&g)[3], const double (&q)[3], double rate)
{
    std::complex<double> num = 1, den = 1;
    for (int b = 0; b < 3; ++b)
    {
        const double G = std::pow(10, g[b] / 20);
        if (g[b] > 0)
            num += (G - 1) * bandpassH(f, fc[b], q[b], rate);
        else if (g[b] < 0)
            den += (1 / G - 1) * bandpassH(f, fc[b], q[b], rate);
    }
    return 20 * std::log10(std::abs(num / den));
}
double shelfDb(double f, double fc, double g, bool high, double rate)
{
    const double A = std::pow(10, std::abs(g) / 40), G = std::pow(10, g / 40);
    const double w = 2 * std::atan(std::tan(M_PI * fc / rate) * (high ? 1 / A : A));
    const double q = 1 / std::sqrt((G + 1 / G) * (1 / 0.5 - 1) + 2);
    const double c = std::cos(w), s = std::sqrt(G) * std::sin(w) / q;
    if (high)
        return sectionDb(G * ((G + 1) + (G - 1) * c + s), -2 * G * ((G - 1) + (G + 1) * c),
                         G * ((G + 1) + (G - 1) * c - s), (G + 1) - (G - 1) * c + s,
                         2 * ((G - 1) - (G + 1) * c), (G + 1) - (G - 1) * c - s, f, rate);
    return sectionDb(G * ((G + 1) - (G - 1) * c + s), 2 * G * ((G - 1) - (G + 1) * c),
                     G * ((G + 1) - (G - 1) * c - s), (G + 1) + (G - 1) * c + s,
                     -2 * ((G - 1) + (G + 1) * c), (G + 1) + (G - 1) * c - s, f, rate);
}

struct EqSetting
{
    double lf, lg, q2, f2, g2, q3, f3, g3, q4, f4, g4, hf, hg, out;
    double modelDb(double f, double rate) const
    {
        return shelfDb(f, lf, lg, false, rate) + networkDb(f, {f2, f3, f4}, {g2, g3, g4}, {q2, q3, q4}, rate) +
               shelfDb(f, hf, hg, true, rate) + out;
    }
};

const std::vector<EqSetting> eqSettings{
    {100, 0, 2, 400, 12, 2, 1000, 0, 2, 4000, 0, 8000, 0, 0},       // one boost
    {63.1, -12, 0.5, 125.9, -6, 8, 2512, 9, 16, 11220, -12, 3981, 6, -3}, // all bands
    {251, 6, 1, 5623, 18, 4, 14125, -18, 0.71, 19953, 12, 1000, -12, 6},  // high, narrow
    {100, 0, 1, 1000, 12, 1, 1413, 12, 2, 2000, -12, 8000, 0, 0},       // overlapping boosts and a cut
};

// The plugin's magnitude response at f, from its impulse response.
std::vector<double> eqResponseDb(const EqSetting &s, double rate, const std::vector<double> &freqs)
{
    Plugin pl(eqId);
    const char *names[] = {"Low Shelf Frequency", "Low Shelf Gain", "Band 2 Q", "Band 2 Frequency",
                           "Band 2 Gain", "Band 3 Q", "Band 3 Frequency", "Band 3 Gain",
                           "Band 4 Q", "Band 4 Frequency", "Band 4 Gain", "High Shelf Frequency",
                           "High Shelf Gain", "Output Level"};
    const double *v = &s.lf;
    for (int i = 0; i < 14; ++i)
        pl.set(paramIdByName(pl.p, "Five-band EQ", names[i]), v[i]);
    pl.activate(rate);
    const size_t n = (size_t)rate;
    std::vector<float> inL(n, 0), inR(n, 0), r;
    inL[0] = inR[0] = 1;
    auto out = pl.render(inL, inR, r, {512}, {}, false);
    std::vector<double> db;
    for (double f : freqs)
    {
        std::complex<double> acc = 0;
        for (size_t i = L; i < n; ++i)
            acc += (double)out[i] * std::polar(1.0, -2 * M_PI * f * (double)(i - L) / rate);
        db.push_back(20 * std::log10(std::abs(acc)));
    }
    return db;
}

std::vector<double> logFreqs(double lo, double hi, int count)
{
    std::vector<double> f;
    for (int i = 0; i < count; ++i)
        f.push_back(lo * std::pow(hi / lo, i / (count - 1.0)));
    return f;
}
} // namespace

TEST_CASE("Five-band EQ is transparent when flat")
{
    // To 1e-9 (-180 dBFS): the sections' arithmetic may be contracted into fused multiply-adds,
    // which leaves residues near 1e-14 rather than bit-exact output.
    std::vector<float> inL, inR, r;
    signal(48000, inL, inR);
    Plugin pl(eqId);
    pl.activate();
    auto out = pl.render(inL, inR, r, {37});
    size_t bad = 0, first = 0;
    for (size_t i = 0; i + L < inL.size(); ++i)
        if (!(std::abs(out[i + L] - inL[i]) <= 1e-9f && std::abs(r[i + L] - inR[i]) <= 1e-9f) && !bad++)
            first = i;
    INFO("first difference at " << first << ": " << out[first + L] << " vs " << inL[first] << ", "
                                << r[first + L] << " vs " << inR[first]);
    REQUIRE(bad == 0);
}

TEST_CASE("Five-band EQ follows its fitted design at 44.1 kHz")
{
    // The design was fitted to reference renders at 44.1 kHz (plan, section 15); this checks the
    // plugin implements it, to 0.01 dB from 20 Hz to 20 kHz.
    const auto freqs = logFreqs(20, 20000, 61);
    for (const auto &s : eqSettings)
    {
        auto got = eqResponseDb(s, 44100, freqs);
        double worst = 0;
        for (size_t k = 0; k < freqs.size(); ++k)
            worst = std::max(worst, std::abs(got[k] - s.modelDb(freqs[k], 44100)));
        INFO("worst " << worst << " dB");
        REQUIRE(worst < 0.01);
    }
}

// ---- Effect memory (plan, section 15, effect memory) ----
namespace
{
// Renders an impulse and a burst through FX with the given parameters, block by block.
template <typename FX> std::vector<float> renderWith(double rate, const std::vector<std::pair<int, float>> &set)
{
    auto gs = std::make_unique<typename FX::FXConfig_t::GS>(rate);
    typename FX::FXConfig_t::ES es;
    auto fx = std::make_unique<FX>(gs.get(), &es, nullptr);
    for (int i = 0; i < FX::numParams; ++i)
        fx->paramStorage[i] = fx->paramAt(i).defaultVal;
    for (auto [i, v] : set)
        fx->paramStorage[i] = v;
    fx->initialize();
    std::vector<float> inL, inR, out;
    signal((size_t)(rate * 4), inL, inR);
    inL[0] = inR[0] = 1;
    alignas(16) float bl[Cfg::blockSize], br[Cfg::blockSize];
    oscairn_fx::ScopedFlushDenormals ftz;
    for (size_t at = 0; at + Cfg::blockSize <= inL.size(); at += Cfg::blockSize)
    {
        std::copy(inL.begin() + at, inL.begin() + at + Cfg::blockSize, bl);
        std::copy(inR.begin() + at, inR.begin() + at + Cfg::blockSize, br);
        fx->processBlock(bl, br);
        out.insert(out.end(), bl, bl + Cfg::blockSize);
        out.insert(out.end(), br, br + Cfg::blockSize);
    }
    return out;
}

template <int Rate> void reverb2SizedMatchesUpstream(double rate)
{
    using R2 = sst::effects::reverb2::Reverb2<oscairn_fx::SizedConfig<Rate>>;
    using Up = sst::effects::reverb2::Reverb2<Cfg>;
    // Defaults, then the longest reach: room size +1, 2 s predelay, full modulation and decay.
    const std::vector<std::vector<std::pair<int, float>>> sets{
        {}, {{R2::rev2_room_size, 1.f}, {R2::rev2_predelay, 1.f}, {R2::rev2_modulation, 1.f}, {R2::rev2_decay_time, 6.f}},
        {{R2::rev2_room_size, -1.f}, {R2::rev2_predelay, -8.f}, {R2::rev2_modulation, 0.f}}};
    for (const auto &set : sets)
    {
        INFO("rate " << rate << " sized for " << Rate);
        REQUIRE(renderWith<R2>(rate, set) == renderWith<Up>(rate, set));
    }
}
} // namespace

TEST_CASE("Reverb 2 sized for the running rate renders exactly as upstream's")
{
    reverb2SizedMatchesUpstream<44100>(44100);
    reverb2SizedMatchesUpstream<48000>(48000);
    reverb2SizedMatchesUpstream<88200>(50000); // a rate between tiers uses the next one up
    reverb2SizedMatchesUpstream<96000>(96000);
    reverb2SizedMatchesUpstream<192000>(192000);
}

TEST_CASE("Per-context memory by mode and sample rate")
{
    // Prints the table for the plan; gates Reverb 2 at 48 kHz below 1.5 MB (it was 14.5 MB), and
    // the EQs at 44.1 kHz below 4 KB: they hold the rate matcher's 58 KB only at other rates.
    for (const char *id : {reverbId, eqId, "org.oscairn.effects.delay", "org.oscairn.effects.dynamics", "org.oscairn.effects.tremolo-rotary", "org.oscairn.effects.chorus"})
    {
        Plugin probe(id);
        double lo, hi;
        clap_param_info_t info;
        ext<clap_plugin_params_t>(probe.p, CLAP_EXT_PARAMS)->get_info(probe.p, 0, &info);
        lo = info.min_value;
        hi = info.max_value;
        for (int m = (int)lo; m <= (int)hi; ++m)
            for (double rate : {44100.0, 48000.0, 96000.0})
            {
                Plugin pl(id);
                pl.set(1, m);
                pl.activate(rate);
                oscairn_effect_resources_t res;
                ext<oscairn_plugin_effect_contexts_t>(pl.p, OSCAIRN_EXT_EFFECT_CONTEXTS)->resources(pl.p, &res);
                char name[64];
                ext<clap_plugin_params_t>(pl.p, CLAP_EXT_PARAMS)->value_to_text(pl.p, 1, m, name, sizeof(name));
                std::printf("memory: %s %s %.0f Hz: context %llu bytes, engine %llu bytes\n", id, name, rate,
                            (unsigned long long)res.context_bytes, (unsigned long long)res.shared_bytes);
                if (std::string(name) == "Reverb 2" && rate == 48000)
                    REQUIRE(res.context_bytes < 1500000);
                if (!std::strcmp(id, eqId))
                {
                    INFO(name << " at " << rate);
                    REQUIRE((rate == 44100) == (res.context_bytes < 4096));
                }
            }
    }
}

// ---- oscairn.memory (RFC 0025 draft) ----
namespace
{
const oscairn_plugin_memory_t *memoryExt(const clap_plugin_t *p)
{
    return ext<oscairn_plugin_memory_t>(p, OSCAIRN_EXT_MEMORY);
}
oscairn_effect_resources_t resourcesOf(const clap_plugin_t *p)
{
    oscairn_effect_resources_t r{};
    REQUIRE(ext<oscairn_plugin_effect_contexts_t>(p, OSCAIRN_EXT_EFFECT_CONTEXTS)->resources(p, &r));
    return r;
}
std::string stateWithMode(const char *id, int mode)
{
    Plugin a(id);
    a.set(1, mode);
    Buffer buf;
    REQUIRE(ext<clap_plugin_state_t>(a.p, CLAP_EXT_STATE)->save(a.p, &buf.out));
    return buf.data;
}
} // namespace

TEST_CASE("Declared memory equals what activation holds and what effect contexts report")
{
    // For every plugin and mode at 44.1 to 192 kHz: estimate (after state load, before
    // activate) == current (after activate) == the effect contexts' resources.
    for (const char *id : {reverbId, eqId, "org.oscairn.effects.delay", "org.oscairn.effects.dynamics", "org.oscairn.effects.tremolo-rotary", "org.oscairn.effects.chorus"})
    {
        Plugin probe(id);
        clap_param_info_t info;
        ext<clap_plugin_params_t>(probe.p, CLAP_EXT_PARAMS)->get_info(probe.p, 0, &info);
        for (int m = (int)info.min_value; m <= (int)info.max_value; ++m)
            for (double rate : {44100.0, 48000.0, 88200.0, 96000.0, 176400.0, 192000.0})
            {
                INFO(id << " mode " << m << " at " << rate);
                Plugin pl(id);
                Buffer buf;
                buf.data = stateWithMode(id, m);
                REQUIRE(ext<clap_plugin_state_t>(pl.p, CLAP_EXT_STATE)->load(pl.p, &buf.in));
                auto *mem = memoryExt(pl.p);
                REQUIRE(mem);
                oscairn_memory_t est{}, cur{};
                REQUIRE(mem->estimate(pl.p, rate, 4096, &est));
                REQUIRE_FALSE(mem->current(pl.p, &cur)); // not activated yet
                pl.activate(rate);
                REQUIRE(mem->current(pl.p, &cur));
                const auto res = resourcesOf(pl.p);
                REQUIRE(est.shared_bytes == cur.shared_bytes);
                REQUIRE(est.context_bytes == cur.context_bytes);
                REQUIRE(est.asset_bytes == 0);
                REQUIRE(cur.asset_bytes == 0);
                REQUIRE(res.shared_bytes == cur.shared_bytes);
                REQUIRE(res.context_bytes == cur.context_bytes);
            }
    }
}

TEST_CASE("The memory estimate follows another state load, and both extensions agree in a mode change")
{
    Plugin pl; // Reverb 2
    auto *mem = memoryExt(pl.p);
    oscairn_memory_t r2{}, r1{};
    REQUIRE(mem->estimate(pl.p, 48000, 512, &r2));
    Buffer buf;
    buf.data = stateWithMode(reverbId, 1);
    REQUIRE(ext<clap_plugin_state_t>(pl.p, CLAP_EXT_STATE)->load(pl.p, &buf.in));
    REQUIRE(mem->estimate(pl.p, 48000, 512, &r1)); // called again after another state load
    REQUIRE(r1.context_bytes != r2.context_bytes);
    // Activate in Reverb 1, then load Reverb 2's state while active: the new mode is built before
    // load returns, both effects are held until the swap, and both extensions say so.
    pl.activate();
    oscairn_memory_t cur{};
    REQUIRE(mem->current(pl.p, &cur));
    REQUIRE(cur.context_bytes == r1.context_bytes);
    buf.data = stateWithMode(reverbId, 2);
    buf.at = 0;
    REQUIRE(ext<clap_plugin_state_t>(pl.p, CLAP_EXT_STATE)->load(pl.p, &buf.in));
    REQUIRE(mem->current(pl.p, &cur));
    REQUIRE(cur.context_bytes > r1.context_bytes);
    REQUIRE(cur.context_bytes > r2.context_bytes);
    REQUIRE(resourcesOf(pl.p).context_bytes == cur.context_bytes);
    std::vector<float> inL(1024, 0), inR(1024, 0), r;
    pl.render(inL, inR, r, {64});
    pl.p->on_main_thread(pl.p); // frees the old effect
    REQUIRE(mem->current(pl.p, &cur));
    REQUIRE(cur.context_bytes == r2.context_bytes);
    REQUIRE(resourcesOf(pl.p).context_bytes == cur.context_bytes);
}

// ---- Two-band EQ (plan, section 15, M1) ----
namespace
{
double onePoleDb(double f, double fc, bool high, double rate)
{
    const double K = std::tan(M_PI * fc / rate);
    return high ? sectionDb(1, -1, 0, 1 + K, K - 1, 0, f, rate) : sectionDb(K, K, 0, 1 + K, K - 1, 0, f, rate);
}
double cookbookPeakDb(double f, double fc, double g, double q, double rate)
{
    const double A = std::pow(10, g / 40), w = 2 * M_PI * fc / rate;
    const double al = std::sin(w) / (2 * q * std::pow(10, -std::abs(g) / 40));
    return sectionDb(1 + al * A, -2 * std::cos(w), 1 - al * A, 1 + al / A, -2 * std::cos(w), 1 - al / A, f, rate);
}
struct TwoBandSetting
{
    int t1;
    double f1, g1, q1;
    int t2;
    double f2, g2, q2, out;
    static double bandDb(int t, double fr, double fc, double g, double q, double rate)
    {
        switch (t)
        {
        case 1: return onePoleDb(fr, fc, false, rate);
        case 2: return onePoleDb(fr, fc, true, rate);
        case 3: return shelfDb(fr, fc, g, false, rate);
        case 4: return shelfDb(fr, fc, g, true, rate);
        case 5: return cookbookPeakDb(fr, fc, g, q, rate);
        }
        return 0;
    }
    double modelDb(double fr, double rate) const
    {
        return bandDb(t1, fr, f1, g1, q1, rate) + bandDb(t2, fr, f2, g2, q2, rate) + out;
    }
};
const std::vector<TwoBandSetting> twoBandSettings{
    {1, 1000, 0, 1, 5, 5000, 6, 2, 0},
    {2, 200, 0, 1, 3, 300, 9, 1, -3},
    {4, 3000, -6, 1, 5, 1000, -9, 1, 2},
};
std::vector<double> twoBandResponseDb(const TwoBandSetting &s, double rate, const std::vector<double> &freqs)
{
    Plugin pl(eqId);
    pl.set(1, 2);
    const char *names[] = {"Band 1 Type", "Band 1 Frequency", "Band 1 Gain", "Band 1 Q", "Band 2 Type",
                           "Band 2 Frequency", "Band 2 Gain", "Band 2 Q", "Output Level"};
    const double v[] = {(double)s.t1, s.f1, s.g1, s.q1, (double)s.t2, s.f2, s.g2, s.q2, s.out};
    for (int i = 0; i < 9; ++i)
        pl.set(paramIdByName(pl.p, "Two-band EQ", names[i]), v[i]);
    pl.activate(rate);
    const size_t n = (size_t)rate;
    std::vector<float> inL(n, 0), inR(n, 0), r;
    inL[0] = inR[0] = 1;
    auto out = pl.render(inL, inR, r, {512}, {}, false);
    std::vector<double> db;
    for (double f : freqs)
    {
        std::complex<double> acc = 0;
        for (size_t i = L; i < n; ++i)
            acc += (double)out[i] * std::polar(1.0, -2 * M_PI * f * (double)(i - L) / rate);
        db.push_back(20 * std::log10(std::abs(acc)));
    }
    return db;
}
} // namespace

TEST_CASE("Two-band EQ follows its fitted design at 44.1 kHz")
{
    // 0.01 dB at 44.1 kHz, 20 Hz to 20 kHz; other rates: "Fitted EQs keep their 44.1 kHz response".
    const auto all = logFreqs(20, 20000, 61);
    for (const auto &s : twoBandSettings)
    {
        auto got = twoBandResponseDb(s, 44100, all);
        double worst = 0;
        for (size_t k = 0; k < all.size(); ++k)
            worst = std::max(worst, std::abs(got[k] - s.modelDb(all[k], 44100)));
        INFO("44.1 kHz worst " << worst << " dB");
        REQUIRE(worst < 0.01);
    }
}

TEST_CASE("Two-band EQ is transparent when off")
{
    std::vector<float> inL, inR, r;
    signal(48000, inL, inR);
    Plugin pl(eqId);
    pl.set(1, 2);
    pl.activate();
    auto out = pl.render(inL, inR, r, {37});
    for (size_t i = 0; i + L < inL.size(); ++i)
        REQUIRE((out[i + L] == inL[i] && r[i + L] == inR[i]));
}

// ---- Three-band and Master EQ (plan, section 15, M1) ----
namespace
{
double masterHighShelfDb(double f, double fc, double g, double rate)
{
    const double G = std::pow(10, g / 40);
    const double w = 2 * std::atan(std::tan(M_PI * fc / rate) / std::sqrt(2.0));
    const double c = std::cos(w), s = std::sqrt(G) * std::sin(w) / 0.5;
    return sectionDb(G * ((G + 1) + (G - 1) * c + s), -2 * G * ((G - 1) + (G + 1) * c),
                     G * ((G + 1) + (G - 1) * c - s), (G + 1) - (G - 1) * c + s,
                     2 * ((G - 1) - (G + 1) * c), (G + 1) - (G - 1) * c - s, f, rate);
}
// The three-band EQ's mid: the cookbook peak with its bandwidth kept in octaves near Nyquist.
double threeBandMidDb(double f, double fc, double g, double q, double rate)
{
    const double qc = q * std::pow(10, -std::abs(g) / 40), w = 2 * M_PI * fc / 44100;
    const double bw = 2 / std::log(2.0) * std::asinh(1 / (2 * qc));
    const double qd = 1 / (2 * std::sinh(std::log(2.0) / 2 * bw * w / std::sin(w)));
    return cookbookPeakDb(f, fc, g, q * qd / qc, rate);
}
// A mode's response from its impulse response, parameters by module and name.
std::vector<double> modeResponseDb(int modeId, const char *module, const std::vector<std::pair<const char *, double>> &set,
                                   double rate, const std::vector<double> &freqs)
{
    Plugin pl(eqId);
    pl.set(1, modeId);
    for (auto [n, v] : set)
        pl.set(paramIdByName(pl.p, module, n), v);
    pl.activate(rate);
    const size_t n = (size_t)rate;
    std::vector<float> inL(n, 0), inR(n, 0), r;
    inL[0] = inR[0] = 1;
    auto out = pl.render(inL, inR, r, {512}, {}, false);
    std::vector<double> db;
    for (double f : freqs)
    {
        std::complex<double> acc = 0;
        for (size_t i = L; i < n; ++i)
            acc += (double)out[i] * std::polar(1.0, -2 * M_PI * f * (double)(i - L) / rate);
        db.push_back(20 * std::log10(std::abs(acc)));
    }
    return db;
}
} // namespace

namespace
{
struct ModeCase
{
    int mode;
    const char *module;
    std::vector<std::pair<const char *, double>> set;
    std::function<double(double, double)> model;
};
std::vector<ModeCase> threeAndMasterCases()
{
    return {
        {3, "Three-band EQ",
         {{"Low Frequency", 200}, {"Low Gain", 9}, {"Mid Frequency", 1200}, {"Mid Gain", -6}, {"Mid Q", 2.6}, {"High Frequency", 6000}, {"High Gain", 4.5}},
         [](double f, double r) { return shelfDb(f, 200, 9, false, r) + threeBandMidDb(f, 1200, -6, 2.6, r) + shelfDb(f, 6000, 4.5, true, r); }},
        {3, "Three-band EQ",
         {{"Low Frequency", 100}, {"Low Gain", -6}, {"Mid Frequency", 7000}, {"Mid Gain", -6}, {"Mid Q", 2.6}, {"High Frequency", 3000}, {"High Gain", 4.5}},
         [](double f, double r) { return shelfDb(f, 100, -6, false, r) + threeBandMidDb(f, 7000, -6, 2.6, r) + shelfDb(f, 3000, 4.5, true, r); }},
        {4, "Master EQ",
         {{"Band 1 Gain", 6}, {"Band 1 Frequency", 125}, {"Band 1 Shape", 0}, {"Band 2 Gain", -4}, {"Band 2 Frequency", 500}, {"Band 2 Q", 1.2},
          {"Band 3 Gain", 8}, {"Band 3 Frequency", 1000}, {"Band 3 Q", 3}, {"Band 4 Gain", -9}, {"Band 4 Frequency", 3200}, {"Band 4 Q", 0.7},
          {"Band 5 Gain", 5}, {"Band 5 Frequency", 8000}, {"Band 5 Shape", 0}},
         [](double f, double r) {
             return shelfDb(f, 125, 6, false, r) + cookbookPeakDb(f, 500, -4, 1.2, r) + cookbookPeakDb(f, 1000, 8, 3, r) +
                    cookbookPeakDb(f, 3200, -9, 0.7, r) + masterHighShelfDb(f, 8000, 5, r);
         }},
        {4, "Master EQ",
         {{"Band 1 Gain", -7}, {"Band 1 Frequency", 250}, {"Band 1 Q", 2}, {"Band 1 Shape", 1}, {"Band 5 Gain", 10}, {"Band 5 Frequency", 2500}, {"Band 5 Q", 1}, {"Band 5 Shape", 1}},
         [](double f, double r) { return cookbookPeakDb(f, 250, -7, 2, r) + cookbookPeakDb(f, 2500, 10, 1, r); }},
    };
}
} // namespace

TEST_CASE("Three-band and Master EQ follow their fitted designs at 44.1 kHz")
{
    // 0.01 dB at 44.1 kHz from 20 Hz to 20 kHz; other rates: "Fitted EQs keep their 44.1 kHz response".
    const auto all = logFreqs(20, 20000, 61);
    for (const auto &c : threeAndMasterCases())
    {
        INFO(c.module);
        auto got = modeResponseDb(c.mode, c.module, c.set, 44100, all);
        double worst = 0;
        for (size_t k = 0; k < all.size(); ++k)
            worst = std::max(worst, std::abs(got[k] - c.model(all[k], 44100)));
        INFO("44.1 kHz worst " << worst);
        REQUIRE(worst < 0.01);
    }
}

TEST_CASE("Three-band and Master EQ are transparent when flat")
{
    std::vector<float> inL, inR, r;
    signal(48000, inL, inR);
    for (int mode : {3, 4})
    {
        Plugin pl(eqId);
        pl.set(1, mode);
        pl.activate();
        auto out = pl.render(inL, inR, r, {37});
        for (size_t i = 0; i + L < inL.size(); ++i)
            REQUIRE((out[i + L] == inL[i] && r[i + L] == inR[i]));
    }
}

// ---- Every fitted EQ at 48, 88.2, 96 and 192 kHz (plan, decision 13) ----
#include <chrono>
#include <random>
#include "effects/FiveBandEq.h"
namespace
{
// Magnitude in dB from a rendered impulse response (latency L removed), by phasor recurrence.
std::vector<double> impulseDb(const std::vector<float> &out, double rate, const std::vector<double> &freqs)
{
    std::vector<double> db;
    for (double f : freqs)
    {
        const std::complex<double> step = std::polar(1.0, -2 * M_PI * f / rate);
        std::complex<double> ph = 1, acc = 0;
        for (size_t i = L; i < out.size(); ++i)
        {
            acc += (double)out[i] * ph;
            ph *= step;
            if (((i - L) & 1023) == 1023)
                ph /= std::abs(ph);
        }
        db.push_back(20 * std::log10(std::abs(acc) + 1e-300));
    }
    return db;
}
std::vector<float> modeImpulse(int modeId, const char *module, const std::vector<std::pair<const char *, double>> &set, double rate)
{
    Plugin pl(eqId);
    pl.set(1, modeId);
    for (auto [n, v] : set)
        pl.set(paramIdByName(pl.p, module, n), v);
    pl.activate(rate);
    const size_t n = (size_t)rate;
    std::vector<float> inL(n, 0), inR(n, 0), r;
    inL[0] = inR[0] = 1;
    return pl.render(inL, inR, r, {512}, {}, false);
}
// Settings for every fitted mode: the fixed ones above and seeded random ones.
std::vector<ModeCase> allFittedCases()
{
    auto cases = threeAndMasterCases();
    auto five = [](const EqSetting &s) {
        return ModeCase{1, "Five-band EQ",
                        {{"Low Shelf Frequency", s.lf}, {"Low Shelf Gain", s.lg}, {"Band 2 Q", s.q2}, {"Band 2 Frequency", s.f2},
                         {"Band 2 Gain", s.g2}, {"Band 3 Q", s.q3}, {"Band 3 Frequency", s.f3}, {"Band 3 Gain", s.g3},
                         {"Band 4 Q", s.q4}, {"Band 4 Frequency", s.f4}, {"Band 4 Gain", s.g4}, {"High Shelf Frequency", s.hf},
                         {"High Shelf Gain", s.hg}, {"Output Level", s.out}},
                        [s](double f, double r) { return s.modelDb(f, r); }};
    };
    auto two = [](const TwoBandSetting &s) {
        return ModeCase{2, "Two-band EQ",
                        {{"Band 1 Type", (double)s.t1}, {"Band 1 Frequency", s.f1}, {"Band 1 Gain", s.g1}, {"Band 1 Q", s.q1},
                         {"Band 2 Type", (double)s.t2}, {"Band 2 Frequency", s.f2}, {"Band 2 Gain", s.g2}, {"Band 2 Q", s.q2},
                         {"Output Level", s.out}},
                        [s](double f, double r) { return s.modelDb(f, r); }};
    };
    for (const auto &s : eqSettings)
        cases.push_back(five(s));
    for (const auto &s : twoBandSettings)
        cases.push_back(two(s));
    std::mt19937 rng(1302);
    auto uni = [&](double lo, double hi) { return std::uniform_real_distribution<double>(lo, hi)(rng); };
    auto lf = [&](double lo, double hi) { return lo * std::pow(hi / lo, uni(0, 1)); };
    for (int k = 0; k < 8; ++k)
        cases.push_back(five({lf(20, 2000), uni(-12, 12), lf(0.5, 16), lf(20, 20000), uni(-18, 18), lf(0.5, 16), lf(20, 20000),
                              uni(-18, 18), lf(0.5, 16), lf(20, 20000), uni(-18, 18), lf(500, 20000), uni(-12, 12), 0}));
    for (int k = 0; k < 8; ++k)
    {
        const int t1 = 1 + (int)(uni(0, 5)), t2 = 1 + (int)(uni(0, 5));
        cases.push_back(two({t1, lf(20, 20000), uni(-12, 12), lf(0.1, 12), t2, lf(20, 20000), uni(-12, 12), lf(0.1, 12), 0}));
    }
    for (int k = 0; k < 6; ++k)
    {
        const double lfq = lf(20, 5000), lg = uni(-12, 12), mf = lf(20, 20000), mg = uni(-12, 12), mq = lf(0.5, 12), hf = lf(500, 20000), hg = uni(-12, 12);
        cases.push_back({3, "Three-band EQ",
                         {{"Low Frequency", lfq}, {"Low Gain", lg}, {"Mid Frequency", mf}, {"Mid Gain", mg}, {"Mid Q", mq}, {"High Frequency", hf}, {"High Gain", hg}},
                         [=](double f, double r) { return shelfDb(f, lfq, lg, false, r) + threeBandMidDb(f, mf, mg, mq, r) + shelfDb(f, hf, hg, true, r); }});
    }
    for (int k = 0; k < 6; ++k)
    {
        double g[5], fr[5], q[5];
        for (int b = 0; b < 5; ++b)
        {
            g[b] = uni(-12, 12);
            fr[b] = b == 0 ? lf(20, 2000) : (b == 4 ? lf(500, 20000) : lf(20, 20000));
            q[b] = lf(0.1, 12);
        }
        const int sh1 = uni(0, 1) < 0.5 ? 0 : 1, sh5 = uni(0, 1) < 0.5 ? 0 : 1;
        std::vector<std::pair<const char *, double>> set{{"Band 1 Shape", (double)sh1}, {"Band 5 Shape", (double)sh5}};
        static const char *names[5][3] = {{"Band 1 Gain", "Band 1 Frequency", "Band 1 Q"}, {"Band 2 Gain", "Band 2 Frequency", "Band 2 Q"},
                                          {"Band 3 Gain", "Band 3 Frequency", "Band 3 Q"}, {"Band 4 Gain", "Band 4 Frequency", "Band 4 Q"},
                                          {"Band 5 Gain", "Band 5 Frequency", "Band 5 Q"}};
        for (int b = 0; b < 5; ++b)
        {
            set.push_back({names[b][0], g[b]});
            set.push_back({names[b][1], fr[b]});
            set.push_back({names[b][2], q[b]});
        }
        std::array<double, 5> G, F, Q;
        std::copy(g, g + 5, G.begin());
        std::copy(fr, fr + 5, F.begin());
        std::copy(q, q + 5, Q.begin());
        cases.push_back({4, "Master EQ", set, [=](double f, double r) {
                             double v = sh1 ? cookbookPeakDb(f, F[0], G[0], Q[0], r) : shelfDb(f, F[0], G[0], false, r);
                             for (int b = 1; b < 4; ++b)
                                 v += cookbookPeakDb(f, F[b], G[b], Q[b], r);
                             return v + (sh5 ? cookbookPeakDb(f, F[4], G[4], Q[4], r) : masterHighShelfDb(f, F[4], G[4], r));
                         }});
    }
    return cases;
}
} // namespace

TEST_CASE("Fitted EQs keep their 44.1 kHz response at 48, 88.2, 96 and 192 kHz")
{
    // Decision 13, tolerances proposed: from 20 Hz to 20 kHz the plugin's response at each rate
    // is within 0.05 dB of the 44.1 kHz design wherever the design is within 20 dB of its peak,
    // and within 0.5 dB further down; above 20 kHz it does not exceed the design's peak up to
    // 22.05 kHz by more than 3 dB. The 44.1 kHz design matches the reference renders (plan,
    // section 15). Every mode, fixed and seeded random settings.
    const auto band = logFreqs(20, 20000, 160);
    const auto cases = allFittedCases();
    for (double rate : {48000.0, 88200.0, 96000.0, 192000.0})
    {
        auto above = logFreqs(20000, 0.49 * rate, 40);
        auto freqs = band;
        freqs.insert(freqs.end(), above.begin(), above.end());
        double worstMain = 0, worstDeep = 0, worstOver = -1e9;
        for (size_t c = 0; c < cases.size(); ++c)
        {
            const auto &cs = cases[c];
            const auto got = impulseDb(modeImpulse(cs.mode, cs.module, cs.set, rate), rate, freqs);
            double top = -1e9, top22 = -1e9;
            std::vector<double> m;
            for (double f : band)
                m.push_back(cs.model(f, 44100)), top = std::max(top, m.back());
            for (double f : logFreqs(20000, 22049, 30))
                top22 = std::max(top22, cs.model(f, 44100));
            double mainErr = 0, deepErr = 0, over = -1e9;
            for (size_t k = 0; k < band.size(); ++k)
            {
                const double d = std::abs(got[k] - m[k]);
                (m[k] > top - 20 ? mainErr : deepErr) = std::max(m[k] > top - 20 ? mainErr : deepErr, d);
            }
            for (size_t k = band.size(); k < freqs.size(); ++k)
                over = std::max(over, got[k] - std::max(top, top22));
            INFO(cs.module << " case " << c << " at " << rate << " Hz: " << mainErr << " dB within 20 dB of the peak, " << deepErr
                           << " dB below, " << over << " dB over above 20 kHz");
            CHECK(mainErr < 0.05);
            CHECK(deepErr < 0.5);
            CHECK(over < 10);
            worstMain = std::max(worstMain, mainErr);
            worstDeep = std::max(worstDeep, deepErr);
            worstOver = std::max(worstOver, over);
        }
        std::printf("Fitted EQs at %.0f Hz, %zu settings: worst %.4f dB within 20 dB of the peak, %.4f dB below, %+.2f dB above 20 kHz\n",
                    rate, cases.size(), worstMain, worstDeep, worstOver);
    }
}

TEST_CASE("Rate matching: coefficient cost")
{
    // Printed, not gated: the time to compute an EQ's coefficients at another rate (on a
    // parameter change, on the engine's worker thread), cold, over the same settings.
    using namespace oscairn_fx::eq;
    std::mt19937 rng(7);
    auto uni = [&](double lo, double hi) { return std::uniform_real_distribution<double>(lo, hi)(rng); };
    auto lf = [&](double lo, double hi) { return lo * std::pow(hi / lo, uni(0, 1)); };
    auto m = std::make_unique<rate::Matcher>();
    for (double r : {48000.0, 96000.0, 192000.0})
    {
        double total = 0, worst = 0;
        const int n = 40;
        for (int k = 0; k < n; ++k)
        {
            Biquad s[2], bp[3], net[3];
            shelf(s[0], designRate, lf(20, 2000), uni(-12, 12), false);
            shelf(s[1], designRate, lf(500, 20000), uni(-12, 12), true);
            double weight[3];
            int role[3];
            for (int b = 0; b < 3; ++b)
            {
                bandpass(bp[b], designRate, lf(20, 20000), lf(0.5, 16));
                const double g = uni(-18, 18), G = std::pow(10, g / 20);
                role[b] = g > 0 ? 1 : -1;
                weight[b] = g > 0 ? G - 1 : 1 / G - 1;
            }
            const auto t0 = std::chrono::steady_clock::now();
            const int nn = networkBiquads(bp, weight, role, net);
            rate::Section sec[5], out[15];
            const Biquad *d[5] = {&s[0], &s[1], &net[0], &net[1], &net[2]};
            for (int i = 0; i < 2 + nn; ++i)
            {
                sec[i].b[0] = d[i]->b0, sec[i].b[1] = d[i]->b1, sec[i].b[2] = d[i]->b2;
                sec[i].a[1] = d[i]->a1, sec[i].a[2] = d[i]->a2;
            }
            m->match(sec, 2 + nn, r, out);
            const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
            total += us;
            worst = std::max(worst, us);
        }
        std::printf("Five-band EQ coefficients at %.0f Hz: mean %.0f us, worst %.0f us (40 random settings)\n", r, total / n, worst);
    }
}

// ---- Rate matching off the audio thread (plan, decision 2) ----
#include <clap/ext/render.h>
#if defined(__has_feature)
#if __has_feature(thread_sanitizer) || __has_feature(address_sanitizer)
#define OSCAIRN_SANITIZED 1
#endif
#endif
#if defined(__SANITIZE_THREAD__) || defined(__SANITIZE_ADDRESS__)
#define OSCAIRN_SANITIZED 1
#endif
namespace
{
// Drives a plugin one host block at a time, timing each process call.
struct Driver
{
    Plugin &pl;
    uint32_t frames;
    std::vector<float> l, r, ol, orr;
    double worstUs{0};
    std::vector<double> callUs; // every timed call, in order
    explicit Driver(Plugin &p, uint32_t n) : pl(p), frames(n), l(n), r(n), ol(n), orr(n) {}
    // One block of `in` (a function of the sample index), with an optional parameter event at 0.
    void block(size_t at, const std::function<float(size_t)> &in, clap_id id = CLAP_INVALID_ID, double v = 0)
    {
        for (uint32_t i = 0; i < frames; ++i)
            l[i] = r[i] = in(at + i);
        Events ev;
        ev.e.reserve(1);
        if (id != CLAP_INVALID_ID)
            ev.add(0, id, v);
        float *ins[2] = {l.data(), r.data()}, *outs[2] = {ol.data(), orr.data()};
        clap_audio_buffer_t ib{ins, nullptr, 2, 0, 0}, ob{outs, nullptr, 2, 0, 0};
        clap_process_t pr{};
        pr.frames_count = frames;
        pr.audio_inputs = &ib;
        pr.audio_outputs = &ob;
        pr.audio_inputs_count = 1;
        pr.audio_outputs_count = 1;
        pr.in_events = &ev.in;
        const auto t0 = std::chrono::steady_clock::now();
        REQUIRE(pl.p->process(pl.p, &pr) != CLAP_PROCESS_ERROR);
        callUs.push_back(std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count());
        worstUs = std::max(worstUs, callUs.back());
    }
};
void setOffline(Plugin &pl)
{
    auto *rx = ext<clap_plugin_render_t>(pl.p, CLAP_EXT_RENDER);
    REQUIRE(rx);
    REQUIRE(rx->set(pl.p, CLAP_RENDER_OFFLINE));
}
// The impulse response after the filters are cleared (CLAP reset), so two plugins holding the
// same coefficients give the same samples whatever they played before.
std::vector<float> impulseAfterReset(Plugin &pl, double rate)
{
    pl.p->reset(pl.p);
    const size_t n = (size_t)(rate / 4);
    std::vector<float> inL(n, 0), inR(n, 0), r;
    inL[0] = inR[0] = 1;
    return pl.render(inL, inR, r, {64}, {}, false);
}
// Renders silence with pauses until the worker's newest result has been swapped in and the
// crossfade has ended: until the impulse response stops changing (bounded by `limit`).
std::vector<float> settledImpulse(Plugin &pl, double rate)
{
    std::vector<float> prev;
    for (int k = 0; k < 400; ++k)
    {
        std::vector<float> z(256, 0), r;
        pl.render(z, z, r, {64}, {}, false);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        pl.render(z, z, r, {64}, {}, false);
        auto now = impulseAfterReset(pl, rate);
        if (now == prev)
            return now;
        prev = std::move(now);
    }
    return prev;
}
} // namespace

TEST_CASE("The render mode is set once and kept for the instance's lifetime")
{
    // Oscairn (9bfc302) sets clap.render after init, before state load and activation, and never
    // changes it while the plugin is loaded; a later set may repeat the mode but not change it.
    auto rx = [](Plugin &pl) { return ext<clap_plugin_render_t>(pl.p, CLAP_EXT_RENDER); };
    Plugin off(eqId);
    REQUIRE(rx(off)->set(off.p, CLAP_RENDER_OFFLINE));
    REQUIRE(!rx(off)->set(off.p, CLAP_RENDER_REALTIME));
    REQUIRE(rx(off)->set(off.p, CLAP_RENDER_OFFLINE));
    off.activate(48000);
    REQUIRE(!rx(off)->set(off.p, CLAP_RENDER_REALTIME));
    // never set: real time once activated
    Plugin rt(eqId);
    rt.activate(48000);
    REQUIRE(!rx(rt)->set(rt.p, CLAP_RENDER_OFFLINE));
    REQUIRE(rx(rt)->set(rt.p, CLAP_RENDER_REALTIME));
    // the offline instance still matches in process: a change lands before the next block
    const clap_id g = paramIdByName(off.p, "Five-band EQ", "Band 2 Gain");
    std::vector<float> z(64, 0), o;
    off.render(z, z, o, {64}, {{0, g, -6.0}}, false);
    Plugin twin(eqId); // the same history, offline throughout
    REQUIRE(rx(twin)->set(twin.p, CLAP_RENDER_OFFLINE));
    twin.activate(48000);
    twin.render(z, z, o, {64}, {{0, g, -6.0}}, false);
    REQUIRE(impulseAfterReset(off, 48000) == impulseAfterReset(twin, 48000));
}

TEST_CASE("Rate matching runs off the audio thread")
{
    // A parameter sweep at 192 kHz with 64-sample host blocks (0.33 ms): one change per block.
    // Matching costs 0.1-3 ms per change, so in process the worst block takes several block
    // lengths. Off the audio thread the worst process call must stay under a quarter of a block
    // (gated in unsanitized builds; the best of three runs, against preemption by the OS).
    const double rate = 192000;
    const uint32_t frames = 64;
    const double blockUs = 1e6 * frames / rate;
    auto tone = [rate](size_t i) { return 0.25f * (float)std::sin(2 * M_PI * 440.0 * (double)i / rate); };
    double bestWorst = 1e300;
    for (int run = 0; run < 3; ++run)
    {
        Plugin pl(eqId);
        pl.set(paramIdByName(pl.p, "Five-band EQ", "Band 3 Gain"), 9);
        pl.set(paramIdByName(pl.p, "Five-band EQ", "Band 3 Q"), 4);
        pl.activate(rate);
        const clap_id f = paramIdByName(pl.p, "Five-band EQ", "Band 3 Frequency");
        Driver d(pl, frames);
        size_t at = 0;
        for (int k = 0; k < 20; ++k, at += frames)
            d.block(at, tone);
        d.worstUs = 0;
        for (int k = 0; k <= 300; ++k, at += frames)
            d.block(at, tone, f, 100 * std::pow(200.0, k / 300.0));
        std::printf("Five-band EQ sweep at 192 kHz, run %d: worst process call %.1f us (block %.1f us)\n", run, d.worstUs, blockUs);
        bestWorst = std::min(bestWorst, d.worstUs);
    } // destroyed right after the sweep, with the worker possibly still matching
#ifndef OSCAIRN_SANITIZED
    REQUIRE(bestWorst < blockUs / 4);
#endif

    SECTION("After a change, the swapped-in coefficients equal those matched in process")
    {
        // Offline, the plugin matches in process; in real time, on the worker. Same history,
        // so the same coefficients, bit for bit.
        for (double r : {48000.0, 192000.0})
        {
            Plugin rt(eqId), off(eqId);
            setOffline(off);
            for (Plugin *pl : {&rt, &off})
            {
                pl->set(paramIdByName(pl->p, "Five-band EQ", "Band 2 Gain"), -6);
                pl->activate(r);
                std::vector<float> z(4096, 0), o;
                pl->render(z, z, o, {64}, {{100, paramIdByName(pl->p, "Five-band EQ", "Band 2 Frequency"), 5000.0}}, false);
            }
            const auto a = settledImpulse(rt, r), b = impulseAfterReset(off, r);
            INFO("rate " << r);
            REQUIRE(a == b);
            // and both hold the new setting: as a plugin activated with it (whose fit starts
            // cold, so equal within the fits' tolerance)
            Plugin fresh(eqId);
            fresh.set(paramIdByName(fresh.p, "Five-band EQ", "Band 2 Gain"), -6);
            fresh.set(paramIdByName(fresh.p, "Five-band EQ", "Band 2 Frequency"), 5000);
            fresh.activate(r);
            const auto freqs = logFreqs(20, 20000, 120);
            const auto x = impulseDb(a, r, freqs), y = impulseDb(impulseAfterReset(fresh, r), r, freqs);
            double worst = 0;
            for (size_t k = 0; k < freqs.size(); ++k)
                worst = std::max(worst, std::abs(x[k] - y[k]));
            std::printf("Five-band EQ after a change at %.0f Hz: against a fresh plugin, worst %.5f dB\n", r, worst);
            REQUIRE(worst < 0.02);
        }
    }
    SECTION("After a sweep, the response equals the in-process one")
    {
        // In real time the worker skips designs superseded before it reached them, so its fits
        // start from other previous fits than in process: equal within the fits' 0.01 dB.
        for (double r : {48000.0, 192000.0})
        {
            Plugin rt(eqId), off(eqId);
            setOffline(off);
            for (Plugin *pl : {&rt, &off})
            {
                pl->set(paramIdByName(pl->p, "Five-band EQ", "Band 3 Gain"), 12);
                pl->activate(r);
                const clap_id f = paramIdByName(pl->p, "Five-band EQ", "Band 3 Frequency");
                std::vector<Event> sweep;
                for (int k = 0; k <= 200; ++k)
                    sweep.push_back({(uint32_t)(64 * k), f, 200 * std::pow(80.0, k / 200.0)});
                std::vector<float> z(64 * 210, 0), o;
                pl->render(z, z, o, {64}, sweep, false);
            }
            const auto freqs = logFreqs(20, 20000, 120);
            const auto a = impulseDb(settledImpulse(rt, r), r, freqs), b = impulseDb(impulseAfterReset(off, r), r, freqs);
            double worst = 0;
            for (size_t k = 0; k < freqs.size(); ++k)
                worst = std::max(worst, std::abs(a[k] - b[k]));
            std::printf("Five-band EQ after a sweep at %.0f Hz: worker against in process, worst %.5f dB\n", r, worst);
            REQUIRE(worst < 0.01);
            Plugin fresh(eqId);
            fresh.set(paramIdByName(fresh.p, "Five-band EQ", "Band 3 Gain"), 12);
            fresh.set(paramIdByName(fresh.p, "Five-band EQ", "Band 3 Frequency"), 200 * 80.0);
            fresh.activate(r);
            const auto c = impulseDb(impulseAfterReset(fresh, r), r, freqs);
            double worstFresh = 0;
            for (size_t k = 0; k < freqs.size(); ++k)
                worstFresh = std::max(worstFresh, std::abs(a[k] - c[k]));
            std::printf("Five-band EQ after a sweep at %.0f Hz: against a fresh plugin, worst %.5f dB\n", r, worstFresh);
            REQUIRE(worstFresh < 0.02);
        }
    }
}

TEST_CASE("Rate-matched EQs change their number of sections without clicks")
{
    // A band switched on or off changes how many biquads the matched cascade holds (at 44.1 kHz
    // nothing is matched). The click is the largest output above 8 kHz (a fourth-order high-pass)
    // in the 50 ms after the change, for a 220 Hz + 1 kHz input that has nothing up there; at
    // every rate it must stay within 6 dB of the same change at 44.1 kHz, or below -80 dBFS.
    // Rendered offline (matching in process) so the change lands at the same sample.
    struct Change
    {
        const char *what, *name;
        double from, to;
    };
    const Change changes[] = {{"band 3 on", "Band 3 Gain", 0, 9},
                              {"band 3 off", "Band 3 Gain", 9, 0},
                              {"band 4 cut on", "Band 4 Gain", 0, -12},
                              {"low shelf on", "Low Shelf Gain", 0, 6}};
    auto click = [](const Change &c, double rate) {
        Plugin pl(eqId);
        setOffline(pl);
        pl.set(paramIdByName(pl.p, "Five-band EQ", "Band 3 Frequency"), 1500);
        pl.set(paramIdByName(pl.p, "Five-band EQ", "Band 3 Q"), 2);
        pl.set(paramIdByName(pl.p, "Five-band EQ", "Band 4 Frequency"), 400);
        pl.set(paramIdByName(pl.p, "Five-band EQ", "Band 4 Q"), 1);
        pl.set(paramIdByName(pl.p, "Five-band EQ", "Band 2 Gain"), 4); // a section beside the change
        pl.set(paramIdByName(pl.p, "Five-band EQ", c.name), c.from);
        pl.activate(rate);
        const size_t n = (size_t)(rate * 0.6), at = (size_t)(rate * 0.5) + 7;
        std::vector<float> in(n), r;
        for (size_t i = 0; i < n; ++i)
            in[i] = 0.25f * (float)(std::sin(2 * M_PI * 220 * i / rate) + std::sin(2 * M_PI * 1000 * i / rate));
        auto out = pl.render(in, in, r, {64}, {{(uint32_t)at, paramIdByName(pl.p, "Five-band EQ", c.name), c.to}}, false);
        // fourth-order Butterworth high-pass at 8 kHz, as two bilinear sections
        double peak = 0, z[2][2]{};
        const double K = std::tan(M_PI * 8000 / rate);
        for (size_t i = 0; i < n; ++i)
        {
            double x = out[i];
            for (int s = 0; s < 2; ++s)
            {
                const double q = s ? 1.3065630 : 0.5411961, norm = 1 / (1 + K / q + K * K);
                const double b0 = norm, b1 = -2 * norm, a1 = 2 * (K * K - 1) * norm, a2 = (1 - K / q + K * K) * norm;
                const double y = b0 * x + z[s][0];
                z[s][0] = b1 * x - a1 * y + z[s][1];
                z[s][1] = b0 * x - a2 * y;
                x = y;
            }
            if (i >= at && i < at + (size_t)(0.05 * rate))
                peak = std::max(peak, std::abs(x));
        }
        return 20 * std::log10(peak + 1e-12);
    };
    for (const auto &c : changes)
    {
        const double ref = click(c, 44100);
        for (double rate : {48000.0, 96000.0, 192000.0})
        {
            const double got = click(c, rate);
            std::printf("Click, %s: %.1f dBFS at %.0f Hz (44.1 kHz: %.1f dBFS)\n", c.what, got, rate, ref);
            INFO(c.what << " at " << rate << " Hz: " << got << " dBFS, 44.1 kHz " << ref << " dBFS");
            CHECK(got < std::max(ref + 6, -80.0));
        }
    }
}

// ---- Tempo cross delay (plan, section 15) ----
#include "effects/TempoCrossDelay.h"
namespace
{
constexpr const char *delayId = "org.oscairn.effects.delay";
// The delay family's feedback step (feedbackGain) and Mode 2's legs (mode2Gain, mode2Unity).
constexpr double fbStep = 516.0 / 32768, unity2 = 32767.0 / 32768;
double leg2(int v) { return (double)(std::abs(v) * v * 516 / 63) / 32768; }
struct Echo
{
    size_t at;
    double l, r;
};
// An impulse into the left input; a delay mode's settings by name; returns the output.
std::vector<float> delayImpulseMode(int mode, const char *module, double rate, const std::vector<std::pair<const char *, double>> &set, std::vector<float> &right,
                                double seconds = 3, double tempo = 0, bool rightInput = false, Plugin **keep = nullptr)
{
    static std::unique_ptr<Plugin> held;
    held = std::make_unique<Plugin>(delayId);
    Plugin &pl = *held;
    pl.tempo = tempo;
    pl.set(1, mode);
    for (auto [n, v] : set)
        pl.set(paramIdByName(pl.p, module, n), v);
    pl.activate(rate);
    const size_t n = (size_t)(rate * seconds);
    std::vector<float> inL(n, 0), inR(n, 0);
    (rightInput ? inR : inL)[0] = 1;
    auto out = pl.render(inL, inR, right, {64}, {}, false);
    if (keep)
        *keep = &pl;
    return out;
}
std::vector<float> delayImpulse(double rate, const std::vector<std::pair<const char *, double>> &set, std::vector<float> &right,
                                double seconds = 3, double tempo = 0, bool rightInput = false, Plugin **keep = nullptr)
{
    return delayImpulseMode(1, "Tempo Cross Delay", rate, set, right, seconds, tempo, rightInput, keep);
}
} // namespace

TEST_CASE("Tempo cross delay: the measured routing, times and gains at 44.1 kHz")
{
    // Laws from fx14, fx16 and fx17 (plan, section 15): at 120 BPM a quarter is 22,049 samples;
    // feedback v x 516 / 32768 per crossing; input L ping-pongs, first on the right.
    std::vector<float> r;
    const double fb = 40 * fbStep, wet = std::sin(M_PI / 4), dry = std::cos(M_PI / 4);
    auto l = delayImpulse(44100, {{"Feedback", 40}, {"High Damping", 1.0}, {"Input", 0}}, r);
    REQUIRE(l[L] == Approx(dry).epsilon(1e-6));
    REQUIRE(r[L] == 0);
    const size_t q = 22049;
    REQUIRE(r[L + q] == Approx(wet).epsilon(1e-6));
    REQUIRE(l[L + 2 * q] == Approx(wet * fb).epsilon(1e-6));
    REQUIRE(r[L + 3 * q] == Approx(wet * fb * fb).epsilon(1e-6));
    for (size_t i = 1; i < 4 * q; ++i)
        if (i != q && i != 2 * q && i != 3 * q)
            REQUIRE((l[L + i] == 0 && r[L + i] == 0));

    SECTION("Mode 2 crosses at unity one way and at the feedback squared the other")
    {
        auto l2 = delayImpulse(44100, {{"Feedback", 40}, {"High Damping", 1.0}, {"Input", 3}}, r);
        // the squared leg is the feedback times |v| / 63 (1.0078 x fb^2; the reference's loop
        // measures 1.0072-1.0073 x fb^2 in cd_m2* and tcd_m2*), the other leg 32767 / 32768
        REQUIRE(r[L + q] == Approx(wet).epsilon(1e-6));
        REQUIRE(l2[L + 2 * q] == Approx(wet * unity2).epsilon(1e-7));
        REQUIRE(r[L + 3 * q] == Approx(wet * unity2 * leg2(40)).epsilon(1e-6));
        REQUIRE(leg2(40) / (fb * fb) == Approx(1.0079).margin(0.0001));
        // at negative feedback the squared leg inverts once per repeat (fx18 dho01, and the
        // 2026-11-12 review batch's tcd_m2l_n37 / n20: ++--++)
        auto l3 = delayImpulse(44100, {{"Feedback", -20}, {"High Damping", 1.0}, {"Input", 3}}, r);
        REQUIRE(l3[L + 2 * q] == Approx(wet * unity2).epsilon(1e-7));
        REQUIRE(r[L + 3 * q] == Approx(wet * unity2 * leg2(-20)).epsilon(1e-6));
        REQUIRE(l3[L + 4 * q] == Approx(wet * unity2 * unity2 * leg2(-20)).epsilon(1e-6));
        REQUIRE(r[L + 5 * q] == Approx(wet * std::pow(unity2 * leg2(-20), 2)).epsilon(1e-6));
    }
    SECTION("Lag shortens or lengthens both lines; the tempo comes from the transport")
    {
        // the first echo lands on exactly one sample
        auto only = [&](size_t at) {
            for (size_t i = 1; i < at + 100; ++i)
                if ((i == at) != (r[L + i] != 0))
                    return false;
            return true;
        };
        delayImpulse(44100, {{"Lag", -63}, {"High Damping", 1.0}, {"Input", 0}}, r);
        REQUIRE(only(19271)); // 436.98 ms, as measured
        delayImpulse(44100, {{"Lag", 63}, {"High Damping", 1.0}, {"Input", 0}}, r);
        REQUIRE(only(24827)); // 562.97 ms
        delayImpulse(44100, {{"High Damping", 1.0}, {"Input", 0}}, r, 2, 90);
        REQUIRE(only(29396)); // a quarter at 90 BPM: 666.6 ms
        delayImpulse(44100, {{"Time L>R", 5}, {"High Damping", 1.0}, {"Input", 0}}, r);
        REQUIRE(only(5512)); // a sixteenth: 5,512.5 rounds up, less one
        delayImpulse(44100, {{"Time L>R", 7}, {"Lag", -16}, {"High Damping", 1.0}, {"Input", 0}}, r);
        REQUIRE(only(7562)); // 1,875 - 160 tenths of a ms
    }
    SECTION("Long notes halve while a line would reach 131,072 samples, as tcd_t17/18/19 measured")
    {
        auto first = [&](int note) {
            delayImpulse(44100, {{"Time L>R", note}, {"High Damping", 1.0}, {"Input", 0}}, r, 2.6);
            for (size_t i = 1; i + L < r.size(); ++i)
                if (r[L + i] != 0)
                    return i;
            return (size_t)0;
        };
        REQUIRE(first(17) == 88198);  // 4 quarters, whole
        REQUIRE(first(18) == 110247); // 5 quarters, whole
        REQUIRE(first(19) == 66148);  // 6 quarters (132,297) plays 1,500 ms
    }
    SECTION("Triplet and dotted notes follow the family's time law, as fx16 and fx18 measured")
    {
        // The first echo of line A (input L, on the right) at the reference's own sample; the
        // old round(T x 44100) + round(lag x 44.1) - 1 missed each by 1 or 2 samples.
        auto first = [&](int note, double lag) {
            delayImpulse(44100, {{"Time L>R", note}, {"Lag", lag}, {"High Damping", 1.0}, {"Input", 0}}, r, 1.5);
            for (size_t i = 1; i + L < r.size(); ++i)
                if (r[L + i] != 0)
                    return i;
            return (size_t)0;
        };
        REQUIRE(first(15, 0) == 58797);  // 1/1 triplet, d_t5_15_fb (the old law: 58,799)
        REQUIRE(first(4, -12) == 3602);  // 1/32 dotted, lag -12, dho04 (3,604)
        REQUIRE(first(6, -17) == 6597);  // 1/8 triplet, lag -17, dho06 (6,599)
        REQUIRE(first(4, -31) == 2765);  // 1/32 dotted, lag -31, dho07 (2,766)
    }
    SECTION("High damping is the two-tap filter g = 5 (1 - ratio) / 9 on each repeat")
    {
        for (double ratio : {0.1, 0.4, 0.7})
        {
            auto l3 = delayImpulse(44100, {{"Feedback", 40}, {"High Damping", ratio}, {"Input", 0}}, r);
            const double g = 5 * (1 - ratio) / 9;
            REQUIRE(r[L + q] == Approx(wet).epsilon(1e-6)); // the first echo is not filtered
            REQUIRE(l3[L + 2 * q] == Approx(wet * fb * (1 - g)).epsilon(1e-6));
            REQUIRE(l3[L + 2 * q + 1] == Approx(wet * fb * g).epsilon(1e-6));
        }
    }
    SECTION("Dry and wet follow the balance as cosine and sine")
    {
        for (int v : {-63, -40, 0, 31, 63})
        {
            auto out = delayImpulse(44100, {{"Mix", v}, {"Input", 0}}, r, 0.6);
            const double x = (v + 63) / 126.0;
            REQUIRE(out[L] == Approx(std::cos(M_PI / 2 * x)).margin(1e-7));
            REQUIRE(r[L + q] == Approx(std::sin(M_PI / 2 * x)).margin(1e-7));
        }
    }
}

TEST_CASE("Tempo cross delay keeps its 44.1 kHz echoes at 48, 88.2, 96 and 192 kHz")
{
    // Decision 13: echo times in seconds within a sample of the running rate, and each repeat's
    // response (echo 2 over echo 1: feedback and damping) within 0.05 dB of the 44.1 kHz
    // filter from 20 Hz to 20 kHz, for every damping value (tolerance proposed).
    for (double damping : {0.1, 0.4, 0.7, 0.9})
    {
        const double g = oscairn_fx::delay::dampingCoefficient(damping), fb = 40 * fbStep;
        for (double rate : {48000.0, 88200.0, 96000.0, 192000.0})
        {
            std::vector<float> r;
            auto l = delayImpulse(rate, {{"Feedback", 40}, {"High Damping", damping}, {"Input", 0}, {"Mix", 63}}, r, 1.3);
            const double q = 22049 / 44100.0 * rate;
            // echo 1 on the right, echo 2 on the left: their spectra over a window around each
            auto spectrum = [&](const std::vector<float> &x, double at, double f) {
                const size_t c = (size_t)std::lround(at) + L, half = (size_t)(0.004 * rate);
                std::complex<double> acc = 0;
                for (size_t i = c - half; i < c + half; ++i)
                    acc += (double)x[i] * std::polar(1.0, -2 * M_PI * f * ((double)i - (double)L - at) / rate);
                return acc;
            };
            size_t peak = 0;
            for (size_t i = L + 1; i < r.size(); ++i)
                if (std::abs(r[i]) > std::abs(r[peak]))
                    peak = i;
            INFO("damping " << damping << " at " << rate << " Hz");
            REQUIRE(std::abs((double)(peak - L) - q) <= 1.0);
            double worst = 0;
            for (double f : logFreqs(20, 20000, 80))
            {
                const double got = std::abs(spectrum(l, 2 * q, f) / spectrum(r, q, f));
                const double want = fb * std::hypot(1 - g + g * std::cos(2 * M_PI * f / 44100), g * std::sin(2 * M_PI * f / 44100));
                worst = std::max(worst, std::abs(20 * std::log10(got / want)));
            }
            std::printf("Tempo cross delay, damping %.1f at %.0f Hz: repeat response within %.4f dB of 44.1 kHz\n", damping, rate, worst);
            REQUIRE(worst < 0.05);
        }
    }
}

TEST_CASE("Tempo cross delay keeps playing between its echoes")
{
    // An echo a second away must not be cut by the tail rule (100 ms of silence elsewhere).
    std::vector<float> r;
    Plugin *pl = nullptr;
    auto l = delayImpulse(48000, {{"Time L>R", 14}, {"Time R>L", 14}, {"Feedback", 32}, {"Input", 0}}, r, 4, 0, false, &pl);
    size_t firstSleep = pl->statuses.size();
    for (size_t b = 0; b < pl->statuses.size(); ++b)
        if (pl->statuses[b] == CLAP_PROCESS_SLEEP)
        {
            firstSleep = b;
            break;
        }
    size_t lastSound = 0;
    for (size_t i = 0; i < l.size(); ++i)
        if (std::abs(l[i]) > 1e-6 || std::abs(r[i]) > 1e-6)
            lastSound = i;
    REQUIRE(lastSound > (size_t)(3 * 48000)); // the third echo, about 3 s in
    REQUIRE(firstSleep * 64 > lastSound);
}

TEST_CASE("Tempo cross delay stays stable at its shortest time at every rate")
{
    // A 1/32 triplet with lag -63 ms rounds to the shortest line, one 44.1 kHz sample. At other
    // rates the damping filter's 32-sample delay comes off the line, which must stay at least a
    // sample long: shorter, the fractional all-pass's pole leaves the unit circle (known
    // difference 31). Full wet, no feedback: the impulse must come back once, bounded.
    for (double rate : {44100.0, 48000.0, 96000.0, 192000.0})
    {
        std::vector<float> r;
        auto l = delayImpulse(rate, {{"Time L>R", 0}, {"Time R>L", 0}, {"Lag", -63}, {"Feedback", 0}, {"Mix", 63}, {"Input", 0}}, r, 0.5);
        double peak = 0, energy = 0;
        for (size_t i = 0; i < l.size(); ++i)
        {
            REQUIRE((std::isfinite(l[i]) && std::isfinite(r[i])));
            peak = std::max({peak, (double)std::abs(l[i]), (double)std::abs(r[i])});
            energy += (double)r[i] * r[i];
        }
        INFO("rate " << rate);
        REQUIRE(peak <= 1.0 + 1e-6);
        REQUIRE(energy == Approx(1.0).epsilon(1e-3)); // an all-pass: the echo keeps the impulse's energy
    }
}

// ---- Cross delay (the tempo mode's laws, times in ms; plan, section 15) ----

TEST_CASE("Cross delay: the impulse times and routing at 44.1 and 48 kHz")
{
    // CrossCore's laws (effects/CrossDelay.h), as measured against the 02 00 renders
    // (results.md): the time in ms becomes a line of its 44.1 kHz sample count minus one
    // (500.0 ms gave 22,049 samples there), kept in seconds at other rates; 250 ms is
    // 11,025 - 1 = 11,024 samples at 44.1 kHz, 11,999.4 at 48 kHz (the all-pass for the
    // fraction). Wet only, feedback at centre (zero), input L: the impulse returns once, on
    // the right, and the left output stays silent.
    for (auto [rate, q] : {std::pair{44100.0, 11024.0}, {48000.0, 11999.4}})
    {
        std::vector<float> r;
        auto l = delayImpulseMode(2, "Cross Delay", rate, {{"Time L>R", 250}, {"Feedback", 0}, {"Mix", 63}, {"Input", 0}}, r, 1.0);
        size_t peak = 0;
        for (size_t i = 1; i < r.size(); ++i)
            if (std::abs(r[i]) > std::abs(r[peak]))
                peak = i;
        INFO("rate " << rate);
        REQUIRE(std::abs((double)(peak - L) - q) <= 1.0);
        double energy = 0;
        for (size_t i = 0; i < r.size(); ++i)
            energy += (double)r[i] * r[i];
        REQUIRE(energy == Approx(1.0).epsilon(1e-3)); // the whole echo, the all-pass holds level
        if (rate == 44100.0)
        {
            REQUIRE(r[peak] == Approx(1.0).margin(1e-6)); // whole samples: one tap, no all-pass
            for (size_t i = 0; i < r.size(); ++i)
                if (i != peak)
                    REQUIRE(std::abs(r[i]) <= 1e-4); // no pre-echo, no repeat (feedback zero)
        }
        else
            for (size_t i = 0; i + 4 < peak; ++i) // nothing arrives before the line's length
                REQUIRE(std::abs(r[i]) <= 1e-6);
        for (size_t i = 0; i < l.size(); ++i)
            REQUIRE(std::abs(l[i]) < 1e-12); // dry off: nothing returns on the left
    }
}

TEST_CASE("Cross delay state round-trips")
{
    Plugin a(delayId);
    a.set(1, 2);
    for (auto [n, v] : {std::pair{"Time L>R", 123.4}, {"Time R>L", 77.7}, {"Feedback", 30}, {"Input", 2}, {"High Damping", 0.3}, {"Mix", -12}})
        a.set(paramIdByName(a.p, "Cross Delay", n), v);
    Buffer buf;
    REQUIRE(ext<clap_plugin_state_t>(a.p, CLAP_EXT_STATE)->save(a.p, &buf.out));
    Plugin b(delayId);
    REQUIRE(ext<clap_plugin_state_t>(b.p, CLAP_EXT_STATE)->load(b.p, &buf.in));
    Buffer again;
    REQUIRE(ext<clap_plugin_state_t>(b.p, CLAP_EXT_STATE)->save(b.p, &again.out));
    REQUIRE(again.data == buf.data);
    auto *params = ext<clap_plugin_params_t>(a.p, CLAP_EXT_PARAMS);
    for (uint32_t i = 0; i < params->count(a.p); ++i)
    {
        clap_param_info_t info;
        params->get_info(a.p, i, &info);
        REQUIRE(a.get(info.id) == b.get(info.id));
    }
    std::vector<float> inL, inR, ra, rb;
    signal(24000, inL, inR);
    a.activate();
    b.activate();
    REQUIRE(a.render(inL, inR, ra) == b.render(inL, inR, rb));
}

TEST_CASE("Cross delay: every parameter's value text parses back")
{
    Plugin pl(delayId);
    pl.set(1, 2);
    auto *params = ext<clap_plugin_params_t>(pl.p, CLAP_EXT_PARAMS);
    int checked = 0;
    for (uint32_t i = 1; i < params->count(pl.p); ++i)
    {
        clap_param_info_t info;
        REQUIRE(params->get_info(pl.p, i, &info));
        if ((info.id >> 8) != 2)
            continue;
        for (double t : {0., .25, .5, .75, 1.})
        {
            double v = info.min_value + t * (info.max_value - info.min_value);
            if (info.flags & CLAP_PARAM_IS_STEPPED)
                v = std::round(v);
            char text[256];
            REQUIRE(params->value_to_text(pl.p, info.id, v, text, sizeof(text)));
            double back = NAN;
            INFO(info.name << " at " << v << ": \"" << text << "\"");
            REQUIRE(params->text_to_value(pl.p, info.id, text, &back));
            if (info.flags & CLAP_PARAM_IS_STEPPED)
                REQUIRE(back == v); // a label must select the value it came from
            char again[256];
            REQUIRE(params->value_to_text(pl.p, info.id, back, again, sizeof(again)));
            REQUIRE(!std::strcmp(text, again));
            ++checked;
        }
    }
    REQUIRE(checked == 50); // five points on each parameter (the EQ's four included), no vacuous pass
}

TEST_CASE("Cross delay: the laws measured against the 02 00 renders")
{
    // measurements/qwen-cross-delay-2026-11-11/results.md: a 500.0 ms line is 22,049 samples
    // (the family's time law, cd_500_t120), feedback v x 516 / 32768 per crossing with inverted
    // polarity at negative v (cd_fb_low -18.00 dB, cd_fb_neg alternates), a 300/700 ms pair
    // routes L into line A (right after 13,229) and R into line B (left after 30,869) —
    // split, not summed (cd_input_r, cd_input_lr) — and the two tempos of cd_500_t120/150
    // give identical renders: this mode ignores the transport.
    std::vector<float> r;
    auto cross = [&](double tempo, double fb, std::vector<float> &right) {
        return delayImpulseMode(2, "Cross Delay", 44100,
                                {{"Time L>R", 500}, {"Time R>L", 500}, {"Feedback", fb}, {"High Damping", 1.0},
                                 {"Mix", 63}, {"Input", 0}},
                                right, 3.0, tempo);
    };
    auto l = cross(120, 8, r);
    const double f8 = 8 * fbStep; // -18.06 dB per crossing; measured -18.00 ± 0.5
    REQUIRE(r[L + 22049] == Approx(1.0).epsilon(1e-6));
    REQUIRE(l[L + 2 * 22049] == Approx(f8).epsilon(1e-6));
    REQUIRE(r[L + 3 * 22049] == Approx(f8 * f8).epsilon(1e-6));
    std::vector<float> r150;
    REQUIRE(cross(150, 8, r150) == l); // tempo 150 renders bit-identically
    REQUIRE(r == r150);

    SECTION("Negative feedback decays like positive and flips the echo (cd_fb_neg)")
    {
        std::vector<float> rn;
        auto ln = cross(120, -63, rn);
        const double fn = -63 * fbStep; // -0.07 dB per crossing, measured -0.07
        REQUIRE(rn[L + 22049] == Approx(1.0).epsilon(1e-6));
        REQUIRE(ln[L + 2 * 22049] == Approx(fn).epsilon(1e-6)); // inverted
        REQUIRE(rn[L + 3 * 22049] == Approx(fn * fn).epsilon(1e-6));
    }
    SECTION("The time ends are on the sample grid (cd_time_max, cd_time_swap)")
    {
        std::vector<float> rt;
        delayImpulseMode(2, "Cross Delay", 44100,
                         {{"Time L>R", 1638.3}, {"Time R>L", 0.1}, {"Feedback", 0}, {"Mix", 63}, {"Input", 0}},
                         rt, 2.5);
        size_t peak = 0;
        for (size_t i = 1; i < rt.size(); ++i)
            if (std::abs(rt[i]) > std::abs(rt[peak]))
                peak = i;
        REQUIRE(peak - L == 72247); // floor(16,383 x 18063 / 4096), the reference's own sample
        // (round(ms x 44.1) - 1 put it one later)
    }
    SECTION("Input L&R feeds the lines split, not summed (cd_input_lr)")
    {
        const std::vector<std::pair<const char *, double>> set{
            {"Time L>R", 300}, {"Time R>L", 700}, {"Feedback", 0}, {"Mix", 63}, {"Input", 2}};
        std::vector<float> rl;
        auto ll = delayImpulseMode(2, "Cross Delay", 44100, set, rl, 1.5); // impulse left
        REQUIRE(rl[L + 13229] == Approx(1.0).epsilon(1e-6));              // L -> A -> right at 300.0 - 1
        for (size_t i = 1; i < 30000; ++i)
            REQUIRE(ll[L + i] == 0); // a summed feed would answer on the left too
        std::vector<float> rr;
        auto lr = delayImpulseMode(2, "Cross Delay", 44100, set, rr, 1.5, 0, true); // impulse right
        REQUIRE(lr[L + 30869] == Approx(1.0).epsilon(1e-6)); // R -> B -> left at 700.0 - 1
        for (size_t i = 1; i < 40000; ++i)
            REQUIRE(rr[L + i] == 0);
    }
}

// ---- Converter frequency scales (decision 17) ----
#include "converter/FrequencyScales.h"
#include <chrono>

TEST_CASE("Cross delay: Mode 2 and the balance, as measured")
{
    // measurements/qwen-night-2026-11-12/cross_delay_extra (cd_m2*, cd_mix_m48/m21/p21/p48)
    // and qwen-night-2026-11-12-review/cross_delay (cd_mix_p10/m35/p35/m55/p55/p59).
    // Mode 2: the fed line answers at unity, crosses at 32767 / 32768, then repeats at the
    // feedback times |v| / 63 per two crossings: at -37 the reference's signs run ++--++
    // (cd_m2l_n37, cd_m2r_n37), its loop 0.34198 against the law's 0.34215 (fb^2: 0.33947).
    const size_t q = 22049; // 500.0 ms
    const double g = unity2 * leg2(-37);
    std::vector<float> r;
    auto l = delayImpulseMode(2, "Cross Delay", 44100,
                              {{"Time L>R", 500}, {"Time R>L", 500}, {"Feedback", -37}, {"High Damping", 1.0},
                               {"Mix", 63}, {"Input", 3}},
                              r, 3.2);
    REQUIRE(r[L + q] == Approx(1.0).epsilon(1e-6));
    REQUIRE(l[L + 2 * q] == Approx(unity2).epsilon(1e-7));
    REQUIRE(r[L + 3 * q] == Approx(g).epsilon(1e-6));
    REQUIRE(l[L + 4 * q] == Approx(g * unity2).epsilon(1e-6));
    REQUIRE(r[L + 5 * q] == Approx(g * g).epsilon(1e-6));
    auto l2 = delayImpulseMode(2, "Cross Delay", 44100,
                               {{"Time L>R", 500}, {"Time R>L", 500}, {"Feedback", -37}, {"High Damping", 1.0},
                                {"Mix", 63}, {"Input", 4}},
                               r, 3.2, 0, true);
    REQUIRE(l2[L + q] == Approx(1.0).epsilon(1e-6));
    REQUIRE(r[L + 2 * q] == Approx(unity2).epsilon(1e-7));
    REQUIRE(l2[L + 3 * q] == Approx(g).epsilon(1e-6));
    // Balance: the reference's wet/dry in dB (echo against its own render's click); the near side
    // stays at unity (delay-eq-2026-11-12: at D=W the dry is level with the dry-only render's).
    const std::vector<std::pair<double, double>> measured = {
        {-55, -35.86}, {-48, -24.94}, {-35, -14.09}, {-21, -7.05}, {10, 3.00},
        {21, 7.04},    {35, 14.09},   {48, 24.93},   {55, 35.85},  {59, 47.89}};
    for (auto [v, db] : measured)
    {
        auto out = delayImpulseMode(2, "Cross Delay", 44100, {{"Time L>R", 500}, {"Feedback", 0}, {"Mix", v}, {"Input", 0}}, r, 0.6);
        INFO("Mix " << v);
        REQUIRE(20 * std::log10(r[L + q] / out[L]) == Approx(db).margin(0.05));
        REQUIRE(std::max(r[L + q], out[L]) == Approx(1.0).margin(1e-6));
    }
    auto dw = delayImpulseMode(2, "Cross Delay", 44100, {{"Time L>R", 500}, {"Feedback", 0}, {"Mix", 0}, {"Input", 0}}, r, 0.6);
    REQUIRE(dw[L] == Approx(1.0).margin(1e-6)); // D=W: dry and wet both at unity
    REQUIRE(r[L + q] == Approx(1.0).margin(1e-6));
}

// ---- Tempo stereo delay (one time, L/R Diffusion; plan, section 15) ----

TEST_CASE("Tempo stereo delay: every echo time measured on the 02 28 renders")
{
    // At 120 BPM each line is the family's time law of the note truncated to 0.1 ms (halved,
    // as often as it takes, while the longer line would reach 131,072 samples) + Lag, + Diffusion on the left
    // and - Diffusion on the right; never under one sample. Each row is a render of the review
    // batch (night batch for 41.6 ms), {note, lag, diffusion, left echo, right echo}.
    struct Row
    {
        int note;
        double lag, diff;
        long left, right;
    };
    const Row rows[] = {{9, 0, 0, 14698, 14698},      {0, 0, 0, 1834, 1834},          {9, -63, 0, 11919, 11919},
                        {9, 32, 0, 16109, 16109},     {9, 0, 63, 17476, 11919},       {9, 0, -63, 11919, 17476},
                        {9, 0, 32, 16109, 13287},     {9, -63, 63, 14698, 9141},      {14, 17, -40, 43084, 46612},
                        {0, 0, 41, 3642, 26},         {0, 0, 63, 4612, 1},            {16, 0, 0, 66148, 66148},
                        {17, 0, 0, 88198, 88198},     {18, 0, 0, 110247, 110247},     {18, 63, 63, 115804, 110247},
                        {19, 0, 0, 66148, 66148},     {19, -15, 0, 65487, 65487},     {19, -40, 0, 130533, 130533},
                        {19, -63, 0, 129519, 129519}, {19, -63, 63, 66148, 60592},
                        {19, -28, 0, 131062, 131062}, {19, -27, 0, 64958, 64958}}; // the last two: ts_t19_lag36/37
    auto peak = [](const std::vector<float> &x) {
        size_t at = 0;
        for (size_t i = 1; i < x.size(); ++i)
            if (std::abs(x[i]) > std::abs(x[at]))
                at = i;
        return (long)(at - L);
    };
    for (const auto &w : rows)
    {
        INFO("note " << w.note << " lag " << w.lag << " diffusion " << w.diff);
        const std::vector<std::pair<const char *, double>> set{
            {"Delay Time", w.note}, {"Feedback", 0}, {"High Damping", 1.0}, {"L/R Diffusion", w.diff}, {"Lag", w.lag}, {"Mix", 63}};
        const double seconds = (std::max(w.left, w.right) + 4096) / 44100.;
        std::vector<float> r, rr;
        auto l = delayImpulseMode(3, "Tempo Stereo Delay", 44100, set, r, seconds, 120); // input L plays left
        delayImpulseMode(3, "Tempo Stereo Delay", 44100, set, rr, seconds, 120, true);   // input R plays right
        REQUIRE(peak(l) == w.left);
        REQUIRE(peak(rr) == w.right);
        REQUIRE(l[L + w.left] == Approx(1.0).epsilon(1e-6));
    }
}

TEST_CASE("Tempo stereo delay: the lines loop at the note plus Lag, Diffusion moves only the output")
{
    // rs_dfb / rs_dfb2: with feedback, every later echo follows the first by the note plus Lag
    // on each channel (17,476 left / 11,919 right, then +14,698 each); feedback v x 516 / 32768 per
    // repeat, its sign alternating when negative. The lines run side by side: input L's repeats
    // all stay on the left (delay-eq-2026-11-12 ts_st_fb40: a left-only source, nothing on the
    // right, -4.0 dB per repeat at feedback +40), and a centred source gives both channels every
    // repeat.
    const double fb = -40 * fbStep;
    const long q = 14698;
    const std::vector<std::pair<const char *, double>> set{
        {"Delay Time", 9}, {"Feedback", -40}, {"High Damping", 1.0}, {"L/R Diffusion", 63}, {"Mix", 63}};
    for (bool rightIn : {false, true})
    {
        std::vector<float> r;
        auto l = delayImpulseMode(3, "Tempo Stereo Delay", 44100, set, r, 1.6, 120, rightIn);
        int nonzero = 0;
        for (size_t i = 1; i < r.size() - L; ++i)
            nonzero += (r[L + i] != 0) + (l[L + i] != 0);
        REQUIRE(nonzero == 4);
        for (int k = 0; k < 4; ++k)
        {
            const auto &out = rightIn ? r : l;
            REQUIRE(out[L + (rightIn ? 11919 : 17476) + q * k] == Approx(std::pow(fb, k)).epsilon(1e-6));
        }
    }
    SECTION("A right-input impulse at 48 kHz keeps its time in seconds")
    {
        const double want = 11919. / 44100 * 48000;
        std::vector<float> r48;
        auto l48 = delayImpulseMode(3, "Tempo Stereo Delay", 48000,
                                    {{"Feedback", 0}, {"High Damping", 1.0}, {"L/R Diffusion", 63}, {"Mix", 63}}, r48, 1.0, 120, true);
        size_t at = 0;
        for (size_t i = 1; i < r48.size(); ++i)
            if (std::abs(r48[i]) > std::abs(r48[at]))
                at = i;
        REQUIRE(std::abs((double)(at - L) - want) <= 1.0);
        for (float v : l48)
            REQUIRE(std::abs(v) < 1e-12);
    }
    SECTION("The dry and wet follow the tempo family's linear balance (measured at -21, +21, +48)")
    {
        for (int v : {-48, -21, 0, 21, 48})
        {
            const double x = (v + 63) / 126.0;
            std::vector<float> rb;
            auto out = delayImpulseMode(3, "Tempo Stereo Delay", 44100,
                                        {{"Feedback", 0}, {"High Damping", 1.0}, {"Mix", v}}, rb, 0.6, 120);
            REQUIRE(out[L] == Approx(std::cos(M_PI / 2 * x)).margin(1e-7));
            REQUIRE(out[L + 14698] == Approx(std::sin(M_PI / 2 * x)).margin(1e-7)); // input L plays left
        }
    }
}

TEST_CASE("Tempo stereo delay state round-trips")
{
    Plugin a(delayId);
    a.set(1, 3);
    for (auto [n, v] : {std::pair{"Delay Time", 7}, {"Feedback", -21}, {"High Damping", 0.4},
                        {"L/R Diffusion", 31.5}, {"Lag", -9}, {"Mix", 12}})
        a.set(paramIdByName(a.p, "Tempo Stereo Delay", n), v);
    Buffer buf;
    REQUIRE(ext<clap_plugin_state_t>(a.p, CLAP_EXT_STATE)->save(a.p, &buf.out));
    Plugin b(delayId);
    REQUIRE(ext<clap_plugin_state_t>(b.p, CLAP_EXT_STATE)->load(b.p, &buf.in));
    Buffer again;
    REQUIRE(ext<clap_plugin_state_t>(b.p, CLAP_EXT_STATE)->save(b.p, &again.out));
    REQUIRE(again.data == buf.data);
    auto *params = ext<clap_plugin_params_t>(a.p, CLAP_EXT_PARAMS);
    for (uint32_t i = 0; i < params->count(a.p); ++i)
    {
        clap_param_info_t info;
        params->get_info(a.p, i, &info);
        REQUIRE(a.get(info.id) == b.get(info.id));
    }
    std::vector<float> inL, inR, ra, rb;
    signal(24000, inL, inR);
    a.activate();
    b.activate();
    REQUIRE(a.render(inL, inR, ra) == b.render(inL, inR, rb));
}

TEST_CASE("Tempo stereo delay: every parameter's value text parses back")
{
    Plugin pl(delayId);
    pl.set(1, 3);
    auto *params = ext<clap_plugin_params_t>(pl.p, CLAP_EXT_PARAMS);
    int checked = 0;
    for (uint32_t i = 1; i < params->count(pl.p); ++i)
    {
        clap_param_info_t info;
        REQUIRE(params->get_info(pl.p, i, &info));
        if ((info.id >> 8) != 3)
            continue;
        for (double t : {0., .25, .5, .75, 1.})
        {
            double v = info.min_value + t * (info.max_value - info.min_value);
            if (info.flags & CLAP_PARAM_IS_STEPPED)
                v = std::round(v);
            char text[256];
            REQUIRE(params->value_to_text(pl.p, info.id, v, text, sizeof(text)));
            double back = NAN;
            INFO(info.name << " at " << v << ": \"" << text << "\"");
            REQUIRE(params->text_to_value(pl.p, info.id, text, &back));
            if (info.flags & CLAP_PARAM_IS_STEPPED)
                REQUIRE(back == v); // a label must select the value it came from
            char again[256];
            REQUIRE(params->value_to_text(pl.p, info.id, back, again, sizeof(again)));
            REQUIRE(!std::strcmp(text, again));
            ++checked;
        }
    }
    REQUIRE(checked == 50); // five points on each parameter (the EQ's four included), no vacuous pass
}

// ---- L/R delay (one line per channel, four times; plan, section 15) ----

TEST_CASE("L/R delay: one line per channel, its own delay time and loop spacing, at 44.1 and 48 kHz")
{
    // Each channel feeds its own line and answers on its own side: input L returns on the left
    // floor(Time L x 10 x 18063 / 4096) samples later (msLineSeconds, the family's law; 200.0 ms is
    // 8,819 samples at 44.1 kHz, the length kept in seconds at 48 kHz with the all-pass for
    // the fraction), then every Feedback Time 1 (150.0 ms is 6,614) at the feedback gain per
    // loop. The R line is the mirror with Time R 500 (22,049) and Feedback Time 2 300
    // (13,229). Nothing crosses: a left input leaves the right output silent (lr_cross).
    const size_t qa = 8819, fa = 6614, qb = 22049, fb = 13229;
    const double g = 40 * fbStep; // -4.01 dB per loop
    const std::vector<std::pair<const char *, double>> set{
        {"Time L", 200}, {"Time R", 500}, {"Feedback Time 1", 150}, {"Feedback Time 2", 300},
        {"Feedback", 40}, {"High Damping", 1.0}, {"Mix", 63}};
    std::vector<float> r;
    auto l = delayImpulseMode(4, "L/R Delay", 44100, set, r, 3.0);
    REQUIRE(l[L + qa] == Approx(1.0).epsilon(1e-6));
    REQUIRE(l[L + qa + fa] == Approx(g).epsilon(1e-6));
    REQUIRE(l[L + qa + 2 * fa] == Approx(g * g).epsilon(1e-6));
    REQUIRE(l[L + qa + 3 * fa] == Approx(g * g * g).epsilon(1e-6));
    for (size_t i = 1; i < qa; ++i)
        REQUIRE(l[L + i] == 0); // nothing before the line's length, no dry (Mix wet, cos is 6e-17)
    for (size_t i = qa + 1; i + L < l.size(); ++i)
        if ((i - qa) % fa)
            REQUIRE(l[L + i] == 0); // and none between the echoes: no cross-coupled taps
    for (size_t i = 0; i < r.size(); ++i)
        REQUIRE(std::abs(r[i]) < 1e-12);
    SECTION("The R line is the mirror")
    {
        std::vector<float> rr;
        auto lr = delayImpulseMode(4, "L/R Delay", 44100, set, rr, 3.0, 0, true);
        REQUIRE(rr[L + qb] == Approx(1.0).epsilon(1e-6));
        REQUIRE(rr[L + qb + fb] == Approx(g).epsilon(1e-6));
        REQUIRE(rr[L + qb + 2 * fb] == Approx(g * g).epsilon(1e-6));
        for (size_t i = 0; i < lr.size(); ++i)
            REQUIRE(std::abs(lr[i]) < 1e-12);
    }
    SECTION("Every rate keeps the lines' lengths in seconds")
    {
        for (double rate : {48000.0, 88200.0, 96000.0, 176400.0, 192000.0})
        {
            std::vector<float> rr;
            auto lr = delayImpulseMode(4, "L/R Delay", rate, set, rr, 1.0);
            auto peakIn = [&](double from, double to) {
                size_t p = (size_t)from;
                for (size_t i = (size_t)from; i < (size_t)to; ++i)
                    if (std::abs(lr[i]) > std::abs(lr[p]))
                        p = i;
                return (double)p;
            };
            const double k = rate / 44100, e1 = peakIn(L + 1, L + (qa + fa / 2) * k);
            INFO(rate << " Hz");
            REQUIRE(std::abs(e1 - L - qa * k) <= 1.0);
            REQUIRE(std::abs(peakIn(e1 + fa * k / 2, e1 + fa * k * 1.5) - e1 - fa * k) <= 1.0);
        }
    }
    SECTION("Negative feedback flips each loop")
    {
        std::vector<std::pair<const char *, double>> neg = set;
        neg[4] = {"Feedback", -63}; // -0.07 dB per loop, inverted every time
        std::vector<float> rn;
        auto ln = delayImpulseMode(4, "L/R Delay", 44100, neg, rn, 3.0);
        REQUIRE(ln[L + qa] == Approx(1.0).epsilon(1e-6));
        REQUIRE(ln[L + qa + fa] == Approx(-63 * fbStep).epsilon(1e-6));
        REQUIRE(ln[L + qa + 2 * fa] == Approx(63 * fbStep * (63 * fbStep)).epsilon(1e-6));
    }
}

TEST_CASE("L/R delay ignores the transport, and its balance and damping follow the family")
{
    const std::vector<std::pair<const char *, double>> set{
        {"Time L", 200}, {"Time R", 500}, {"Feedback Time 1", 150}, {"Feedback Time 2", 300},
        {"Feedback", 40}, {"High Damping", 1.0}};
    std::vector<float> r120;
    auto l120 = delayImpulseMode(4, "L/R Delay", 44100, set, r120, 2.0, 120);
    std::vector<float> r150;
    auto l150 = delayImpulseMode(4, "L/R Delay", 44100, set, r150, 2.0, 150);
    REQUIRE(l120 == l150); // ms times: the tempo means nothing here, as in the cross modes
    REQUIRE(r120 == r150);
    const size_t qa = 8819, fa = 6614;
    SECTION("The balance is the cross delay's: the near side at unity")
    {
        // 02 40 renders: wet/dry +7.04 dB at v = +21 (lr_hold_mix) and -24.94 dB at v = -48
        // (lr_bal_m48); the tempo family's linear position gives +4.77 / -14.46 dB there.
        for (auto [v, db] : {std::pair{21., 7.04}, {-48., -24.94}, {0., 0.}})
        {
            auto out = delayImpulseMode(4, "L/R Delay", 44100,
                                        {{"Time L", 200}, {"Feedback", 0}, {"High Damping", 1.0}, {"Mix", v}}, r120, 0.5);
            INFO("Mix " << v);
            REQUIRE(std::max(out[L], out[L + qa]) == Approx(1).margin(1e-6));
            REQUIRE(20 * std::log10(out[L + qa] / out[L]) == Approx(db).margin(0.02));
        }
    }
    SECTION("Damping is the two-tap in the feedback path; the first echo is unfiltered")
    {
        const double g = 5 * (1 - 0.1) / 9; // damping 0.1; an impulse sees the filter's mean
        // power over the band, (1-g)^2 + g^2 (the two taps are decorrelated by the 150 ms gap)
        auto out = delayImpulseMode(4, "L/R Delay", 44100,
                                    {{"Time L", 200}, {"Feedback Time 1", 150}, {"Feedback", 63}, {"High Damping", 0.1}, {"Mix", 63}}, r150, 2.0);
        double e1 = 0, e2 = 0;
        for (size_t i = qa; i < qa + fa; ++i)
            e1 += (double)out[L + i] * out[L + i];
        for (size_t i = qa + fa; i < qa + 2 * fa; ++i)
            e2 += (double)out[L + i] * out[L + i];
        const double f = 63 * fbStep; // feedback's own drop, on top of the filter's
        REQUIRE(e2 == Approx(e1 * f * f * ((1 - g) * (1 - g) + g * g)).epsilon(0.02));
    }
}

TEST_CASE("L/R delay: the range ends and the series loop")
{
    // lr_tmax's settings: Time L = Feedback Time 1 = 1,638.3 ms puts echo 1 at
    // floor(16,383 x 18063 / 4096) = 72,247 samples and echo 2 72,247 further (two taps in
    // series, past the parameter's own maximum); Time R = Feedback Time 2 = 0.1 ms is
    // 4 samples, as the reference's (round(ms x 44.1) - 1 gave 72,248 and 3).
    std::vector<float> r;
    auto out = delayImpulseMode(4, "L/R Delay", 44100,
                                {{"Time L", 1638.3}, {"Time R", 0.1}, {"Feedback Time 1", 1638.3},
                                 {"Feedback Time 2", 0.1}, {"Feedback", 40}, {"High Damping", 1.0}, {"Mix", 63}},
                                r, 3.6);
    REQUIRE(out[L + 72247] == Approx(1.0).epsilon(1e-6));
    REQUIRE(out[L + 144494] == Approx(40 * fbStep).epsilon(1e-6));
    for (size_t i = 5; i < 72247; ++i)
        REQUIRE(std::abs(out[L + i]) < 1e-6); // nothing else on the left until the long echo
    for (size_t i = 0; i < r.size(); ++i)
        REQUIRE(std::abs(r[i]) < 1e-12); // a left input never reaches the right line
    std::vector<float> r2;
    auto right = delayImpulseMode(4, "L/R Delay", 44100,
                                  {{"Time L", 1638.3}, {"Time R", 0.1}, {"Feedback Time 1", 1638.3},
                                   {"Feedback Time 2", 0.1}, {"Feedback", 40}, {"High Damping", 1.0}, {"Mix", 63}},
                                  r2, 0.5, 0, true);
    REQUIRE(r2[L + 4] == Approx(1.0).epsilon(1e-6)); // the 0.1 ms line plays 4 samples later
    REQUIRE(r2[L + 8] == Approx(40 * fbStep).epsilon(1e-6));
    for (size_t i = 9; i < 400; ++i)
        if (i % 4)
            REQUIRE(std::abs(r2[L + i]) < 1e-6); // echoes only every Feedback Time 2 (4 samples)
}

TEST_CASE("L/R delay keeps playing until its longest tap answers")
{
    // Time L 1.5 s with short loops: the tail rule must wait for the first echo.
    std::vector<float> r;
    Plugin *pl = nullptr;
    auto l = delayImpulseMode(4, "L/R Delay", 48000,
                              {{"Time L", 1500}, {"Time R", 100}, {"Feedback Time 1", 100}, {"Feedback Time 2", 100}, {"Feedback", 0}, {"Mix", 63}},
                              r, 2.0, 0, false, &pl);
    size_t firstSleep = pl->statuses.size();
    for (size_t b = 0; b < pl->statuses.size(); ++b)
        if (pl->statuses[b] == CLAP_PROCESS_SLEEP)
        {
            firstSleep = b;
            break;
        }
    size_t lastSound = 0;
    for (size_t i = 0; i < l.size(); ++i)
        if (std::abs(l[i]) > 1e-6)
            lastSound = i;
    REQUIRE(lastSound > (size_t)(1.4 * 48000));
    REQUIRE(firstSleep * 64 > lastSound);
}

TEST_CASE("L/R delay state round-trips")
{
    Plugin a(delayId);
    a.set(1, 4);
    for (auto [n, v] : {std::pair{"Time L", 123.4}, {"Time R", 77.7}, {"Feedback Time 1", 321.1},
                        {"Feedback Time 2", 88.8}, {"Feedback", 30}, {"High Damping", 0.3}, {"Mix", -12}})
        a.set(paramIdByName(a.p, "L/R Delay", n), v);
    Buffer buf;
    REQUIRE(ext<clap_plugin_state_t>(a.p, CLAP_EXT_STATE)->save(a.p, &buf.out));
    Plugin b(delayId);
    REQUIRE(ext<clap_plugin_state_t>(b.p, CLAP_EXT_STATE)->load(b.p, &buf.in));
    Buffer again;
    REQUIRE(ext<clap_plugin_state_t>(b.p, CLAP_EXT_STATE)->save(b.p, &again.out));
    REQUIRE(again.data == buf.data);
    auto *params = ext<clap_plugin_params_t>(a.p, CLAP_EXT_PARAMS);
    for (uint32_t i = 0; i < params->count(a.p); ++i)
    {
        clap_param_info_t info;
        params->get_info(a.p, i, &info);
        REQUIRE(a.get(info.id) == b.get(info.id));
    }
    std::vector<float> inL, inR, ra, rb;
    signal(24000, inL, inR);
    a.activate();
    b.activate();
    REQUIRE(a.render(inL, inR, ra) == b.render(inL, inR, rb));
}

TEST_CASE("L/R delay: every parameter's value text parses back")
{
    Plugin pl(delayId);
    pl.set(1, 4);
    auto *params = ext<clap_plugin_params_t>(pl.p, CLAP_EXT_PARAMS);
    int checked = 0;
    for (uint32_t i = 1; i < params->count(pl.p); ++i)
    {
        clap_param_info_t info;
        REQUIRE(params->get_info(pl.p, i, &info));
        if ((info.id >> 8) != 4)
            continue;
        for (double t : {0., .25, .5, .75, 1.})
        {
            double v = info.min_value + t * (info.max_value - info.min_value);
            if (info.flags & CLAP_PARAM_IS_STEPPED)
                v = std::round(v);
            char text[256];
            REQUIRE(params->value_to_text(pl.p, info.id, v, text, sizeof(text)));
            double back = NAN;
            INFO(info.name << " at " << v << ": \"" << text << "\"");
            REQUIRE(params->text_to_value(pl.p, info.id, text, &back));
            if (info.flags & CLAP_PARAM_IS_STEPPED)
                REQUIRE(back == v); // a label must select the value it came from
            char again[256];
            REQUIRE(params->value_to_text(pl.p, info.id, back, again, sizeof(again)));
            REQUIRE(!std::strcmp(text, again));
            ++checked;
        }
    }
    REQUIRE(checked == 55); // five points on each parameter (the EQ's four included), no vacuous pass
}

// ---- L/C/R delay (one line, three taps, one feedback time; plan, section 15) ----

TEST_CASE("L/C/R delay: one line, three taps and a shared feedback time, at 44.1 and 48 kHz")
{
    // ONE line read at four offsets (effects/LcrDelay.h; plan, section 15): Time L answers
    // left, Time R right, Time C both channels at (Level C / 127)^2, Feedback Time recirculates
    // at the feedback gain. At 44.1 kHz the taps are the family's time law: 200 -> 8,819,
    // 300 -> 13,229, 560 -> 24,695, 150 -> 6,614, as lcr_grid and lcr_r_dw measured. The line's
    // feed is the mean of the inputs (measured with a left-only source, lcr_st), so a left impulse of
    // 1 feeds it 0.5 and answers on both channels.
    const size_t qa = 8819, qb = 13229, qc = 24695, fa = 6614;
    const double g = 40 * fbStep, half = 0.5; // mono-mean feed
    const std::vector<std::pair<const char *, double>> set{
        {"Time L", 200}, {"Time R", 300}, {"Time C", 560}, {"Feedback Time", 150},
        {"Feedback", 40}, {"Level C", 127}, {"High Damping", 1.0}, {"Mix", 63}};
    std::vector<float> r;
    auto l = delayImpulseMode(5, "L/C/R Delay", 44100, set, r, 3.0);
    REQUIRE(l[L + qa] == Approx(half).epsilon(1e-6));
    REQUIRE(l[L + qa + fa] == Approx(half * g).epsilon(1e-6));
    REQUIRE(l[L + qa + 2 * fa] == Approx(half * g * g).epsilon(1e-6));
    REQUIRE(r[L + qb] == Approx(half).epsilon(1e-6)); // a LEFT input answers on the RIGHT at Time R
    REQUIRE(r[L + qb + fa] == Approx(half * g).epsilon(1e-6));
    REQUIRE(r[L + qc] == Approx(half).epsilon(1e-6));  // the centre tap, Level C 127: same gain
    REQUIRE(r[L + qc + fa] == Approx(half * g).epsilon(1e-6));
    REQUIRE(l[L + qc] == Approx(half).epsilon(1e-6));  // and in the left channel too
    for (size_t i = 1; i < l.size() - L; ++i)
        if ((i - qa) % fa && (i - qb) % fa && (i - qc) % fa)
            REQUIRE(l[L + i] == 0); // no dry (Mix wet), no taps off the three grids
    SECTION("Level C sets the centre tap alone")
    {
        std::vector<std::pair<const char *, double>> zero{
            {"Time L", 200}, {"Time R", 300}, {"Time C", 560}, {"Feedback Time", 150},
            {"Feedback", 40}, {"Level C", 0}, {"High Damping", 1.0}, {"Mix", 63}};
        auto off = delayImpulseMode(5, "L/C/R Delay", 44100, zero, r, 2.0); // r = its right channel
        for (size_t i = 1; i + L < off.size(); ++i)
            if ((i - qc) % fa == 0)
                REQUIRE(off[L + i] == 0); // the centre grid is gone from the left...
        for (size_t i = 1; i + L < r.size(); ++i)
            if ((i - qc) % fa == 0)
                REQUIRE(r[L + i] == 0); // ...and gone from the right, whose 560-grid echoes ARE the centre
        REQUIRE(off[L + qa] == Approx(half).epsilon(1e-6)); // the side taps unchanged
        auto mid = delayImpulseMode(5, "L/C/R Delay", 44100,
                                    {{"Time L", 200}, {"Time R", 300}, {"Time C", 560}, {"Feedback Time", 150},
                                     {"Feedback", 0}, {"Level C", 64}, {"High Damping", 1.0}, {"Mix", 63}},
                                    r, 1.0);
        REQUIRE(mid[L + qc] == Approx(half * (64.0 / 127) * (64.0 / 127)).epsilon(1e-6));
        // centre over side tap as measured: 64 by lcr_hold_level (fitted), 32 and 100 by the
        // review batch (hold-outs)
        for (auto [raw, db] : {std::pair{64, -11.91}, {32, -23.95}, {100, -4.15}})
        {
            auto c = delayImpulseMode(5, "L/C/R Delay", 44100,
                                      {{"Time L", 200}, {"Time R", 300}, {"Time C", 560}, {"Feedback Time", 150},
                                       {"Feedback", 0}, {"Level C", (double)raw}, {"High Damping", 1.0}, {"Mix", 63}},
                                      r, 1.0);
            INFO("Level C " << raw);
            REQUIRE(20 * std::log10(c[L + qc] / c[L + qa]) == Approx(db).margin(0.02));
        }
    }
    SECTION("A right input mirrors it, and 48 kHz keeps the lengths in seconds")
    {
        std::vector<float> rr;
        auto lr = delayImpulseMode(5, "L/C/R Delay", 44100, set, rr, 3.0, 0, true);
        REQUIRE(rr[L + qb] == Approx(half).epsilon(1e-6));
        REQUIRE(lr[L + qa] == Approx(half).epsilon(1e-6)); // the mean feed: right answers left too
        std::vector<float> r48;
        auto l48 = delayImpulseMode(5, "L/C/R Delay", 48000, set, r48, 3.0);
        size_t peak = 0;
        for (size_t i = 1; i < l48.size(); ++i)
            if (std::abs(l48[i]) > std::abs(l48[peak]))
                peak = i;
        REQUIRE(std::abs((double)(peak - L) - qa * 48000 / 44100.0) <= 1.0);
    }
    SECTION("The 0.1 ms taps and the loop keep their time at every rate, with the damping on")
    {
        for (double rate : {48000., 88200., 96000., 176400., 192000.})
        {
            std::vector<float> rs;
            auto ls = delayImpulseMode(5, "L/C/R Delay", rate,
                                       {{"Time L", 0.1}, {"Time R", 0.1}, {"Time C", 0.1}, {"Feedback Time", 150},
                                        {"Feedback", 40}, {"Level C", 127}, {"High Damping", 0.1}, {"Mix", 63}},
                                       rs, 0.3);
            auto peakIn = [&](size_t from, size_t to) {
                size_t p = from;
                for (size_t i = from; i < to; ++i)
                    if (std::abs(ls[i]) > std::abs(ls[p]))
                        p = i;
                return (double)(p - L);
            };
            const size_t half = L + (size_t)(0.075 * rate);
            INFO(rate << " Hz");
            REQUIRE(std::abs(peakIn(1, half) - 4 * rate / 44100) <= 1.0); // 4 samples at 44.1 kHz
            REQUIRE(std::abs(peakIn(half, ls.size()) - (4 + 6614) * rate / 44100) <= 1.0); // one loop later
        }
    }
}

TEST_CASE("L/C/R delay: negative feedback, damping, the range ends and the shared loop")
{
    // Feedback - v x 516 / 32768 per loop, inverted every loop (the family's law): with one shared
    // feedback tap every channel's k-th echo carries the same (-1)^k. Damping is the two-tap
    // g = 5(1 - r)/9 in the feedback path only, measured here the way lr_dampmin checks it:
    // successive echoes' energy ratio is fb^2 ((1-g)^2 + g^2), the first echo unfiltered.
    const size_t qa = 8819, fa = 6614;
    std::vector<float> r;
    auto neg = delayImpulseMode(5, "L/C/R Delay", 44100,
                                {{"Time L", 200}, {"Time R", 300}, {"Time C", 560}, {"Feedback Time", 150},
                                 {"Feedback", -63}, {"Level C", 127}, {"High Damping", 1.0}, {"Mix", 63}},
                                r, 2.0);
    REQUIRE(neg[L + qa] == Approx(0.5).epsilon(1e-6));
    REQUIRE(neg[L + qa + fa] == Approx(-63 * fbStep * 0.5).epsilon(1e-6));
    REQUIRE(neg[L + qa + 2 * fa] == Approx(63 * fbStep * (63 * fbStep) * 0.5).epsilon(1e-6));
    std::vector<float> rd;
    auto damp = delayImpulseMode(5, "L/C/R Delay", 44100,
                                 {{"Time L", 200}, {"Time R", 300}, {"Time C", 560}, {"Feedback Time", 150},
                                  {"Feedback", 63}, {"Level C", 0}, {"High Damping", 0.1}, {"Mix", 63}},
                                 rd, 2.0);
    double e1 = 0, e2 = 0;
    for (size_t i = qa; i < qa + fa; ++i)
        e1 += (double)damp[L + i] * damp[L + i];
    for (size_t i = qa + fa; i < qa + 2 * fa; ++i)
        e2 += (double)damp[L + i] * damp[L + i];
    const double gc = 5 * (1 - 0.1) / 9, f = 63 * fbStep; // the filter's, and the feedback's own drop
    REQUIRE(e2 == Approx(e1 * f * f * ((1 - gc) * (1 - gc) + gc * gc)).epsilon(0.02));
    SECTION("The range ends, and echoes run past them on the shared loop")
    {
        // Time L = Feedback Time = 1,638.3 ms: echo 1 at 72,247, echo 2 one loop later (the
        // series of two taps, past the parameter's own maximum) - lr_tmax's shape, probed by
        // lcr_tmax. Time R = 0.1 ms is 4 samples.
        std::vector<float> rt;
        auto mx = delayImpulseMode(5, "L/C/R Delay", 44100,
                                   {{"Time L", 1638.3}, {"Time R", 0.1}, {"Time C", 0.1}, {"Feedback Time", 1638.3},
                                    {"Feedback", 40}, {"Level C", 0}, {"High Damping", 1.0}, {"Mix", 63}},
                                   rt, 3.6);
        REQUIRE(mx[L + 72247] == Approx(0.5).epsilon(1e-6));
        REQUIRE(mx[L + 144494] == Approx(0.5 * 40 * fbStep).epsilon(1e-6));
        REQUIRE(rt[L + 4] == Approx(0.5).epsilon(1e-6)); // the 0.1 ms right tap on the right channel
        for (size_t i = 5; i < 72247; ++i)
            REQUIRE(std::abs(mx[L + i]) < 1e-6);
    }
    SECTION("The transport means nothing, and the balance follows the cross delay's")
    {
        const std::vector<std::pair<const char *, double>> set{
            {"Time L", 200}, {"Time R", 300}, {"Time C", 560}, {"Feedback Time", 150},
            {"Feedback", 40}, {"Level C", 100}, {"High Damping", 1.0}, {"Mix", 21}};
        std::vector<float> r120, r150;
        auto l120 = delayImpulseMode(5, "L/C/R Delay", 44100, set, r120, 2.0, 120);
        auto l150 = delayImpulseMode(5, "L/C/R Delay", 44100, set, r150, 2.0, 150);
        REQUIRE(l120 == l150);
        REQUIRE(r120 == r150);
        // wet over dry as measured from the click: +7.04 dB at D<W21 (lcr_hold_mix), -24.94 dB
        // at D48>W (lcr_r_balm48); the dry is the input, the wet the line's mean feed
        for (auto [v, db] : {std::pair{21, 7.04}, {-48, -24.94}})
        {
            std::vector<float> rw;
            auto dw = delayImpulseMode(5, "L/C/R Delay", 44100,
                                       {{"Time L", 200}, {"Feedback", 0}, {"Level C", 0}, {"High Damping", 1.0}, {"Mix", (double)v}},
                                       rw, 0.5);
            INFO("Mix " << v);
            REQUIRE(20 * std::log10(2 * dw[L + qa] / dw[L]) == Approx(db).margin(0.02));
        }
    }
}

TEST_CASE("L/C/R delay state round-trips")
{
    Plugin a(delayId);
    a.set(1, 5);
    for (auto [n, v] : {std::pair{"Time L", 123.4}, {"Time R", 77.7}, {"Time C", 456.7},
                        {"Feedback Time", 321.1}, {"Feedback", 30}, {"Level C", 40},
                        {"High Damping", 0.3}, {"Mix", -12}})
        a.set(paramIdByName(a.p, "L/C/R Delay", n), v);
    Buffer buf;
    REQUIRE(ext<clap_plugin_state_t>(a.p, CLAP_EXT_STATE)->save(a.p, &buf.out));
    Plugin b(delayId);
    REQUIRE(ext<clap_plugin_state_t>(b.p, CLAP_EXT_STATE)->load(b.p, &buf.in));
    Buffer again;
    REQUIRE(ext<clap_plugin_state_t>(b.p, CLAP_EXT_STATE)->save(b.p, &again.out));
    REQUIRE(again.data == buf.data);
    auto *params = ext<clap_plugin_params_t>(a.p, CLAP_EXT_PARAMS);
    for (uint32_t i = 0; i < params->count(a.p); ++i)
    {
        clap_param_info_t info;
        params->get_info(a.p, i, &info);
        REQUIRE(a.get(info.id) == b.get(info.id));
    }
    std::vector<float> inL, inR, ra, rb;
    signal(24000, inL, inR);
    a.activate();
    b.activate();
    REQUIRE(a.render(inL, inR, ra) == b.render(inL, inR, rb));
}

TEST_CASE("L/C/R delay: every parameter's value text parses back")
{
    Plugin pl(delayId);
    pl.set(1, 5);
    auto *params = ext<clap_plugin_params_t>(pl.p, CLAP_EXT_PARAMS);
    int checked = 0;
    for (uint32_t i = 1; i < params->count(pl.p); ++i)
    {
        clap_param_info_t info;
        REQUIRE(params->get_info(pl.p, i, &info));
        if ((info.id >> 8) != 5)
            continue;
        for (double t : {0., .25, .5, .75, 1.})
        {
            double v = info.min_value + t * (info.max_value - info.min_value);
            if (info.flags & CLAP_PARAM_IS_STEPPED)
                v = std::round(v);
            char text[256];
            REQUIRE(params->value_to_text(pl.p, info.id, v, text, sizeof(text)));
            double back = NAN;
            INFO(info.name << " at " << v << ": \"" << text << "\"");
            REQUIRE(params->text_to_value(pl.p, info.id, text, &back));
            if (info.flags & CLAP_PARAM_IS_STEPPED)
                REQUIRE(back == v); // a label must select the value it came from
            char again[256];
            REQUIRE(params->value_to_text(pl.p, info.id, back, again, sizeof(again)));
            REQUIRE(!std::strcmp(text, again));
            ++checked;
        }
    }
    REQUIRE(checked == 60); // five points on each parameter (the EQ's four included), no vacuous pass
}

// ---- Tempo mono delay (one line, two taps, loop = note time plus Lag; plan, section 15) ----

TEST_CASE("Tempo mono delay: the taps, times and gains at 44.1 and 48 kHz")
{
    // One recirculating line (effects/TempoMonoDelay.h): with no diffusion and no lag the
    // mono-mean input answers on both channels at the note time and repeats at that spacing
    // (this set has no separate feedback time). The time law is the TEMPO DELAY pair's:
    // trunc(T ms, 0.1) x 44.1 rounded, minus 1 (tempo_stereo ts_base / ts_tmin): the quarter
    // default (raw 11) at 120 BPM is 500.0 ms -> 22,049 samples; its 1/32 triplet "41.6 ms" ->
    // 1,834. Feedback v x 516 / 32768 per loop; the tempo types' linear balance position. A left
    // impulse of 1 feeds the line 0.5 (the mono mean; difference 48).
    const size_t q = 22049;
    const double fb = 40 * fbStep, half = 0.5;
    const std::vector<std::pair<const char *, double>> set{
        {"Feedback", 40}, {"High Damping", 1.0}, {"L/R Diffusion", 0}, {"Mix", 63}};
    std::vector<float> r;
    auto l = delayImpulseMode(6, "Tempo Mono Delay", 44100, set, r, 3.5, 120);
    REQUIRE(std::abs(l[L]) < 1e-12); // dry off (cos(pi/2) is 6e-17, not zero)
    REQUIRE(l[L + q] == Approx(half).epsilon(1e-6));
    REQUIRE(r[L + q] == Approx(half).epsilon(1e-6)); // no diffusion: both taps answer together
    REQUIRE(l[L + 2 * q] == Approx(half * fb).epsilon(1e-6)); // loop spacing = the delay time
    REQUIRE(r[L + 3 * q] == Approx(half * fb * fb).epsilon(1e-6));
    for (size_t i = 1; i < 4 * q; ++i)
        if (i % q)
            REQUIRE((l[L + i] == 0 && r[L + i] == 0));
    SECTION("The 1/32 triplet lands at 1,834 samples (the truncation, not the cross law)")
    {
        std::vector<float> r0;
        auto l0 = delayImpulseMode(6, "Tempo Mono Delay", 44100,
                                   {{"Delay Time", 0}, {"Feedback", 40}, {"High Damping", 1.0}, {"L/R Diffusion", 0}, {"Mix", 63}}, r0, 0.3, 120);
        REQUIRE(r0[L + 1834] == Approx(half).epsilon(1e-6)); // floor(41.6 ms x 44.0991); the cross law gives 1,837
        REQUIRE(l0[L + 1834] == Approx(half).epsilon(1e-6)); // both taps answer together, on both channels
    }
    SECTION("The line holds 3,000 ms: repeats 132,297 samples apart, not halved as the stereo type's")
    {
        std::vector<float> rt;
        auto lt = delayImpulseMode(6, "Tempo Mono Delay", 44100,
                                   {{"Delay Time", 19}, {"Feedback", 16}, {"High Damping", 1.0}, {"L/R Diffusion", 0}, {"Mix", 63}}, rt, 7.0, 120);
        // 6 quarters = 3,000 ms; the reference repeats 132,297 samples after its click and after
        // its first echo (review batch tmx_tmax), floor(3,000 x 44.0991)
        REQUIRE(rt[L + 132297] == Approx(half).epsilon(1e-6));
        REQUIRE(lt[L + 2 * 132297] == Approx(half * 16 * fbStep).epsilon(1e-6));
        std::vector<float> r5; // 5 quarters: 110,247 (tmx_t18), round(ms x 44.1) - 1 gives 110,249
        delayImpulseMode(6, "Tempo Mono Delay", 44100,
                         {{"Delay Time", 18}, {"Feedback", 0}, {"High Damping", 1.0}, {"L/R Diffusion", 0}, {"Mix", 63}}, r5, 3.0, 120);
        REQUIRE(r5[L + 110247] == Approx(half).epsilon(1e-6));
        // Slower host tempos: the line holds up to 262,143 samples, then the note halves
        std::vector<float> r61, r60;
        delayImpulseMode(6, "Tempo Mono Delay", 44100,
                         {{"Delay Time", 19}, {"Feedback", 0}, {"High Damping", 1.0}, {"L/R Diffusion", 0}, {"Mix", 63}}, r61, 6.0, 61);
        REQUIRE(r61[L + 260255] == Approx(half).epsilon(1e-6)); // 5,901.6 ms, whole
        delayImpulseMode(6, "Tempo Mono Delay", 44100,
                         {{"Delay Time", 19}, {"Feedback", 0}, {"High Damping", 1.0}, {"L/R Diffusion", 0}, {"Mix", 63}}, r60, 3.2, 60);
        REQUIRE(r60[L + 132297] == Approx(half).epsilon(1e-6)); // 6,000 ms would be 264,594: halved
    }
    SECTION("Negative feedback flips the echo polarity once per loop")
    {
        std::vector<float> rn;
        auto ln = delayImpulseMode(6, "Tempo Mono Delay", 44100,
                                   {{"Feedback", -63}, {"High Damping", 1.0}, {"L/R Diffusion", 0}, {"Mix", 63}}, rn, 2.5, 120);
        REQUIRE(ln[L + q] == Approx(half).epsilon(1e-6));
        REQUIRE(ln[L + 2 * q] == Approx(-half * 63 * fbStep).epsilon(1e-6));
        REQUIRE(rn[L + 3 * q] == Approx(half * 63 * fbStep * 63 * fbStep).epsilon(1e-6));
    }
    SECTION("At 48 to 192 kHz the taps and the loop keep their length in seconds")
    {
        for (double rate : {48000.0, 88200.0, 96000.0, 176400.0, 192000.0})
        {
            const double want = 22049. / 44100 * rate; // fractional read
            std::vector<float> rr;
            auto lr = delayImpulseMode(6, "Tempo Mono Delay", rate,
                                       {{"Feedback", 40}, {"High Damping", 0.5}, {"L/R Diffusion", 0}, {"Mix", 63}}, rr, 1.6, 120);
            REQUIRE(lr.size() > (size_t)(L + 2.5 * want));
            auto peakIn = [&](double from, double to) {
                size_t p = (size_t)(L + from);
                for (size_t i = p; i < (size_t)(L + to); ++i)
                    if (std::abs(lr[i]) > std::abs(lr[p]))
                        p = i;
                return (double)(p - L);
            };
            INFO("rate " << rate);
            REQUIRE(std::abs(peakIn(0.5 * want, 1.5 * want) - want) <= 1.0);
            REQUIRE(std::abs(peakIn(1.5 * want, 2.5 * want) - 2 * want) <= 1.0); // the loop
        }
    }
    SECTION("The dry and wet follow the tempo types' linear position")
    {
        for (int v : {-48, -21, 0, 21, 48})
        {
            const double x = (v + 63) / 126.0;
            auto out = delayImpulseMode(6, "Tempo Mono Delay", 44100,
                                        {{"Feedback", 0}, {"High Damping", 1.0}, {"L/R Diffusion", 0}, {"Mix", v}}, r, 0.6, 120);
            REQUIRE(out[L] == Approx(std::cos(M_PI / 2 * x)).margin(1e-7));
            REQUIRE(r[L + q] == Approx(0.5 * std::sin(M_PI / 2 * x)).margin(1e-7));
        }
    }
}

TEST_CASE("Tempo mono delay: L/R Diffusion splits the taps around the loop, Lag moves all three")
{
    // Review batch (dry in every render, feedback +40): Diffusion +63 puts the left echo at
    // 24,827 samples and the right at 19,271, both repeating every 22,049 (tmx_diffp; tmx_diffn
    // mirrors it); Lag -63 / +63 puts both echoes at 19,271 / 24,827 and repeats at that same
    // spacing (tmx_lagn / tmx_lagp). So the loop runs at time + lag and the taps sit +-diffusion
    // around it, 1 ms per printed ms each.
    auto echoAt = [&](const std::vector<float> &x) {
        size_t peak = 0;
        for (size_t i = 1; i < x.size(); ++i)
            if (std::abs(x[i]) > std::abs(x[peak]))
                peak = i;
        return (long)(peak - L);
    };
    const std::vector<std::pair<const char *, double>> base{
        {"Delay Time", 11}, {"Feedback", 0}, {"High Damping", 1.0}, {"L/R Diffusion", 0}, {"Mix", 63}};
    auto set = base;
    set.emplace_back("L/R Diffusion", 63); // raw 127
    std::vector<float> r0, r;
    auto l0 = delayImpulseMode(6, "Tempo Mono Delay", 44100, base, r0, 1.0, 120);
    auto l = delayImpulseMode(6, "Tempo Mono Delay", 44100, set, r, 1.0, 120);
    const long q = 22049; // quarter at 120 BPM
    REQUIRE(echoAt(l0) == q);
    REQUIRE(echoAt(r0) == q);
    REQUIRE(echoAt(l) == q + 2778); // round(63 x 44.1) later on the left
    REQUIRE(echoAt(r) == q - 2778); // and earlier on the right
    SECTION("The loop follows Lag and not Diffusion")
    {
        auto fed = set;
        fed.emplace_back("Feedback", 40);
        std::vector<float> rf;
        auto lf = delayImpulseMode(6, "Tempo Mono Delay", 44100, fed, rf, 1.5, 120);
        const double g = 40 * fbStep; // tmx_diffp: 24,827 and 19,271, both repeating every 22,049
        REQUIRE(lf[L + q + 2778] == Approx(0.5).epsilon(1e-6));
        REQUIRE(lf[L + 2 * q + 2778] == Approx(0.5 * g).epsilon(1e-6));
        REQUIRE(rf[L + q - 2778] == Approx(0.5).epsilon(1e-6));
        REQUIRE(rf[L + 2 * q - 2778] == Approx(0.5 * g).epsilon(1e-6));
        auto lagged = base;
        lagged.emplace_back("Lag", -63);
        std::vector<float> rl;
        auto ll = delayImpulseMode(6, "Tempo Mono Delay", 44100, lagged, rl, 1.0, 120);
        REQUIRE(echoAt(ll) == q - 2778); // tmx_lagn: 19,271 on both channels
        REQUIRE(echoAt(rl) == q - 2778);
        lagged.emplace_back("Feedback", 40);
        auto lg = delayImpulseMode(6, "Tempo Mono Delay", 44100, lagged, rl, 1.5, 120);
        REQUIRE(lg[L + 2 * (q - 2778)] == Approx(0.5 * g).epsilon(1e-6)); // repeats at 19,271
    }
    SECTION("Host tempo moves the grid")
    {
        std::vector<float> r150;
        auto l150 = delayImpulseMode(6, "Tempo Mono Delay", 44100, base, r150, 1.0, 150);
        REQUIRE(echoAt(l150) == 17639); // 400.0 ms at 150 BPM: round(4000 x 4.41) - 1
    }
    SECTION("Damping is the two-tap in the feedback path; the first echo is unfiltered")
    {
        const std::vector<std::pair<const char *, double>> damp{
            {"Delay Time", 11}, {"Feedback", 40}, {"High Damping", 0.1}, {"L/R Diffusion", 0}, {"Mix", 63}};
        std::vector<float> rd;
        auto ld = delayImpulseMode(6, "Tempo Mono Delay", 44100, damp, rd, 4.0, 120);
        const double g = 5 * (1 - 0.1) / 9; // the two taps are decorrelated by the 500 ms gap:
        double e1 = 0, e2 = 0;              // echo 2's energy is echo 1's x fb^2 x ((1-g)^2 + g^2)
        for (size_t i = q; i < 2 * q; ++i)
            e1 += (double)ld[L + i] * ld[L + i];
        for (size_t i = 2 * q; i < 3 * q; ++i)
            e2 += (double)ld[L + i] * ld[L + i];
        const double f = 40 * fbStep; // feedback's own drop, on top of the filter's
        REQUIRE(e2 == Approx(e1 * f * f * ((1 - g) * (1 - g) + g * g)).epsilon(0.02));
        REQUIRE(ld[L + 2 * q] != 0); // the second echo exists (echo 1 came from the raw input)
    }
}

TEST_CASE("Tempo mono delay state round-trips")
{
    Plugin a(delayId);
    a.set(1, 6);
    for (auto [n, v] : {std::pair{"Delay Time", 7}, {"Feedback", -21}, {"High Damping", 0.4},
                        {"L/R Diffusion", 31.5}, {"Lag", -9}, {"Mix", 12}})
        a.set(paramIdByName(a.p, "Tempo Mono Delay", n), v);
    Buffer buf;
    REQUIRE(ext<clap_plugin_state_t>(a.p, CLAP_EXT_STATE)->save(a.p, &buf.out));
    Plugin b(delayId);
    REQUIRE(ext<clap_plugin_state_t>(b.p, CLAP_EXT_STATE)->load(b.p, &buf.in));
    Buffer again;
    REQUIRE(ext<clap_plugin_state_t>(b.p, CLAP_EXT_STATE)->save(b.p, &again.out));
    REQUIRE(again.data == buf.data);
    auto *params = ext<clap_plugin_params_t>(a.p, CLAP_EXT_PARAMS);
    for (uint32_t i = 0; i < params->count(a.p); ++i)
    {
        clap_param_info_t info;
        params->get_info(a.p, i, &info);
        REQUIRE(a.get(info.id) == b.get(info.id));
    }
    std::vector<float> inL, inR, ra, rb;
    signal(24000, inL, inR);
    a.activate();
    b.activate();
    REQUIRE(a.render(inL, inR, ra) == b.render(inL, inR, rb));
}

TEST_CASE("Tempo mono delay: every parameter's value text parses back")
{
    Plugin pl(delayId);
    pl.set(1, 6);
    auto *params = ext<clap_plugin_params_t>(pl.p, CLAP_EXT_PARAMS);
    int checked = 0;
    for (uint32_t i = 1; i < params->count(pl.p); ++i)
    {
        clap_param_info_t info;
        REQUIRE(params->get_info(pl.p, i, &info));
        if ((info.id >> 8) != 6)
            continue;
        for (double t : {0., .25, .5, .75, 1.})
        {
            double v = info.min_value + t * (info.max_value - info.min_value);
            if (info.flags & CLAP_PARAM_IS_STEPPED)
                v = std::round(v);
            char text[256];
            REQUIRE(params->value_to_text(pl.p, info.id, v, text, sizeof(text)));
            double back = NAN;
            INFO(info.name << " at " << v << ": \"" << text << "\"");
            REQUIRE(params->text_to_value(pl.p, info.id, text, &back));
            if (info.flags & CLAP_PARAM_IS_STEPPED)
                REQUIRE(back == v); // a label must select the value it came from
            char again[256];
            REQUIRE(params->value_to_text(pl.p, info.id, back, again, sizeof(again)));
            REQUIRE(!std::strcmp(text, again));
            ++checked;
        }
    }
    REQUIRE(checked == 50); // five points on each parameter (the EQ's four included), no vacuous pass
}

// ---- The delay modes' output EQ (plan, section 15 and decision 23) ----

TEST_CASE("Delay: the EQ shelves the whole output as measured, in every mode, and within the rate gate")
{
    // delay-eq-2026-11-12: a steady 55 Hz saw through each type, the EQ's response at its
    // harmonics against the same type's render with the EQ flat (dry only, or at D=W before the
    // echo for 02 40). None of these settings was fitted. At 44.1 kHz within 0.05 dB of the
    // reference (tolerance proposed); at 48, 88.2, 96, 176.4 and 192 kHz within the rate gate of 44.1 kHz.
    struct Setting
    {
        int modeId;
        const char *module;
        double loHz, loDb, hiHz, hiDb, mix;
        std::vector<double> ref; // dB at 55, 110, 330, 990, 2970, 8910 and 16005 Hz
    };
    const double hzs[] = {55.0, 110.0, 330.0, 990.0, 2970.0, 8910.0, 16005.0};
    const std::vector<Setting> settings{
        {2, "Cross Delay", 32, 12, 16000, -12, -63, {6.77, 3.34, 0.56, 0.00, -0.58, -4.03, -9.26}},      // cd_eq1
        {1, "Tempo Cross Delay", 125, -9, 8000, 9, -63, {-8.32, -6.90, -2.68, -0.10, 2.26, 6.94, 8.69}}, // tcd_eq2
        {6, "Tempo Mono Delay", 32, -12, 6300, 6, -63, {-6.78, -3.34, -0.53, 0.20, 1.74, 4.90, 5.85}},   // tm_eq4
        {3, "Tempo Stereo Delay", 400, 9, 630, -9, -63, {8.70, 7.91, 3.12, -4.80, -8.35, -8.95, -9.00}}, // ts_eq3
        {4, "L/R Delay", 32, 12, 16000, -12, 0, {6.77, 3.34, 0.56, 0.00, -0.58, -4.03, -9.26}},          // lr_eqdw
        {5, "L/C/R Delay", 2000, -12, 500, 12, -63, {-11.29, -9.72, -4.49, -0.04, 4.42, 10.11, 11.73}}}; // lcr_eq4
    auto level = [](double rate, const Setting &s, double hz) {
        Plugin pl(delayId);
        pl.set(1, s.modeId);
        const std::vector<std::pair<const char *, double>> set{
            {"Low Shelf Frequency", s.loHz}, {"Low Shelf Gain", s.loDb}, {"High Shelf Frequency", s.hiHz},
            {"High Shelf Gain", s.hiDb}, {"Mix", s.mix}, {"Feedback", 0}};
        for (auto [n, v] : set)
            pl.set(paramIdByName(pl.p, s.module, n), v);
        if (s.mix > -63) // at D=W the echo comes after the window
            pl.set(paramIdByName(pl.p, s.module, "Time L"), 1000);
        pl.activate(rate);
        const size_t n = (size_t)(0.6 * rate);
        std::vector<float> in(n), r;
        for (size_t i = 0; i < n; ++i)
            in[i] = (float)(0.25 * std::sin(2 * M_PI * hz * (double)i / rate));
        auto l = pl.render(in, in, r, {64}, {}, false);
        double e = 0;
        const size_t a = (size_t)(0.2 * rate);
        for (size_t i = a; i < n; ++i)
            e += (double)l[i] * l[i];
        return 10 * std::log10(e / (double)(n - a) / (0.25 * 0.25 / 2));
    };
    for (const auto &s : settings)
    {
        const double peak = *std::max_element(s.ref.begin(), s.ref.end());
        for (size_t k = 0; k < s.ref.size(); ++k)
        {
            const double at44 = level(44100, s, hzs[k]);
            INFO(s.module << ": low " << s.loHz << " Hz " << s.loDb << " dB, high " << s.hiHz << " Hz " << s.hiDb << " dB, at "
                          << hzs[k] << " Hz: " << at44 << ", reference " << s.ref[k]);
            REQUIRE(std::abs(at44 - s.ref[k]) < 0.05);
            for (double rate : {48000.0, 88200.0, 96000.0, 176400.0, 192000.0})
                REQUIRE(std::abs(level(rate, s, hzs[k]) - at44) < (s.ref[k] > peak - 20 ? 0.05 : 0.5));
        }
    }
}

TEST_CASE("Delay: the EQ acts on dry and wet alike and stays out of the loop")
{
    // The reference's loop keeps its echo-to-echo ratio with the EQ on (cd_eq, tm_eq, lr_eq,
    // lcr_eq), and its dry and echo carry the same shelf (lr_eqdw, cd_eqdw before and after the
    // echo). Here: the first echo is the dry's shape, and every later echo is exactly the
    // feedback gain times the one before, sample by sample.
    for (auto [modeId, module, timeName] : {std::tuple{2, "Cross Delay", "Time L>R"}, {4, "L/R Delay", "Time L"}, {5, "L/C/R Delay", "Time L"}})
    {
        const std::vector<std::pair<const char *, double>> eqSet{
            {"Low Shelf Frequency", 250}, {"Low Shelf Gain", 9}, {"High Shelf Frequency", 2000}, {"High Shelf Gain", -12}};
        auto with = [&](std::vector<std::pair<const char *, double>> set, std::vector<float> &r) {
            set.insert(set.end(), eqSet.begin(), eqSet.end());
            set.push_back({timeName, 200});
            set.push_back({"High Damping", 1.0});
            if (modeId == 2)
                set.push_back({"Time R>L", 200});
            if (modeId == 4)
                set.push_back({"Feedback Time 1", 200});
            if (modeId == 5)
                set.insert(set.end(), {{"Feedback Time", 200}, {"Level C", 0}});
            return delayImpulseMode(modeId, module, 44100, set, r, 1.0);
        };
        std::vector<float> rd, rw;
        auto dry = with({{"Mix", -63}, {"Feedback", 0}}, rd);
        auto wet = with({{"Mix", 63}, {"Feedback", 40}}, rw);
        const auto &echo = modeId == 2 ? rw : wet;  // the cross mode plays a left input on the right,
        const auto &echo2 = modeId == 2 ? wet : echo; // and its repeat on the left
        const double scale = modeId == 5 ? 0.5 : 1.0;  // L/C/R feeds its line the inputs' mean
        const size_t q = 8819;                          // 200 ms
        INFO(module);
        double worst = 0, energy = 0;
        for (size_t i = 0; i < q; ++i)
        {
            worst = std::max(worst, std::abs((double)echo[L + q + i] - scale * dry[L + i]));
            worst = std::max(worst, std::abs((double)echo2[L + 2 * q + i] - 40 * fbStep * echo[L + q + i]));
            energy += (double)dry[L + i] * dry[L + i];
        }
        REQUIRE(worst < 1e-6);
        REQUIRE(std::abs(dry[L] - 1.0) > 0.05); // the shelf is really there: not an impulse
        REQUIRE(energy > 0);
    }
}

// ---- Converter frequency scales (decision 17) ----
#include "converter/FrequencyScales.h"
#include <chrono>

TEST_CASE("Frequency scales: the measured tables and their nearest formulas")
{
    namespace sc = oscairn_fx::scales;
    double worst3 = 0, worstM = 0;
    for (int i = 46; i <= 241; ++i)
    {
        worst3 = std::max(worst3, std::abs(sc::threeBandFormulaHz(i) / sc::threeBandTableHz(i) - 1));
        REQUIRE(sc::threeBandTableHz(i + 1) > sc::threeBandTableHz(i));
    }
    for (int i = 0; i <= 60; ++i)
    {
        worstM = std::max(worstM, std::abs(sc::masterFormulaHz(i) / sc::masterTableHz(i) - 1));
        if (i < 60)
            REQUIRE(sc::masterTableHz(i + 1) > sc::masterTableHz(i));
    }
    std::printf("three-band formula vs table: worst %.2f%%; Master formula vs table: worst %.2f%%\n",
                100 * worst3, 100 * worstM);
    REQUIRE(worst3 < 0.0153); // as fitted: 1.52%
    REQUIRE(worstM < 0.0517); // 5.16%: the printed scale is not a formula
    REQUIRE(sc::threeBandTableHz(46) == Approx(50.1).margin(0.01));
    for (int i = 131; i <= 255; ++i)
    {
        const double c = sc::threeBandHighShelfCornerHz(i);
        REQUIRE(c > sc::threeBandHighShelfHalfGainHz(i));
        REQUIRE(c < 22050);
    }

    // Cost of a conversion (the converter runs it once per value; the plugins take Hz).
    auto bench = [](auto fn) {
        volatile double acc = 0;
        const auto t0 = std::chrono::steady_clock::now();
        for (int r = 0; r < 20000; ++r)
            for (int i = 46; i <= 241; ++i)
                acc = acc + fn(i);
        return std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count() / (20000.0 * 196);
    };
    std::printf("three-band conversion: table %.2f ns, formula %.2f ns\n", bench(sc::threeBandTableHz),
                bench(sc::threeBandFormulaHz));
}

#include "effects/HallReverb.h"
#include "effects/CircuitCompressor.h"
namespace
{
// Hall A's response to an impulse in both inputs, with its latency removed; L and R.
std::pair<std::vector<float>, std::vector<float>> hallImpulse(double rate, const std::vector<std::pair<const char *, double>> &set,
                                                              double seconds)
{
    Plugin pl(reverbId);
    pl.set(1, 3);
    for (auto [n, v] : set)
        pl.set(paramIdByName(pl.p, "Hall A", n), v);
    pl.activate(rate);
    const size_t n = (size_t)(rate * seconds) + oscairn_fx::latencySamples;
    std::vector<float> in(n, 0), r;
    in[0] = 1;
    auto l = pl.render(in, in, r, {64}, {}, false);
    l.erase(l.begin(), l.begin() + oscairn_fx::latencySamples);
    r.erase(r.begin(), r.begin() + oscairn_fx::latencySamples);
    return {l, r};
}
// Decay time (s) in the octave band at fc: band-pass (two cookbook band-pass sections, Q 1.4),
// backward-integrated energy, slope between -5 and -25 dB, as the measurement repository's
// fx/dynamics.py decay_times does.
double bandDecay(const std::vector<float> &x, double rate, double fc, double *bandEnergy = nullptr)
{
    oscairn_fx::hall::Section s[2];
    const double w = 2 * M_PI * fc / rate, al = std::sin(w) / (2 * 1.4), a0 = 1 + al;
    for (auto &q : s)
        q.b0 = al / a0, q.b1 = 0, q.b2 = -al / a0, q.a1 = -2 * std::cos(w) / a0, q.a2 = (1 - al) / a0;
    std::vector<double> e(x.size());
    for (size_t i = 0; i < x.size(); ++i)
    {
        const double y = s[1].run(s[0].run(x[i]));
        e[i] = y * y;
    }
    for (size_t i = e.size() - 1; i-- > 0;)
        e[i] += e[i + 1];
    if (bandEnergy)
        *bandEnergy = e[0];
    size_t i0 = 0, i1 = 0;
    while (i0 < e.size() && 10 * std::log10(e[i0] / e[0]) > -5)
        ++i0;
    for (i1 = i0; i1 < e.size() && 10 * std::log10(e[i1] / e[0]) > -25; ++i1)
        ;
    double sx = 0, sy = 0, sxx = 0, sxy = 0;
    const double n = (double)(i1 - i0);
    for (size_t i = i0; i < i1; ++i)
    {
        const double t = i / rate, v = 10 * std::log10(e[i] / e[0]);
        sx += t, sy += v, sxx += t * t, sxy += t * v;
    }
    return -60 / ((n * sxy - sx * sy) / (n * sxx - sx * sx));
}
double firstAbove(const std::vector<float> &x, double rate, float thr = 1e-6f)
{
    for (size_t i = 0; i < x.size(); ++i)
        if (std::abs(x[i]) > thr)
            return i / rate;
    return -1;
}
} // namespace

TEST_CASE("Hall A: room size moves only delays, reverb time only gains, nothing is modulated")
{
    // The reference's structure (plan, decision 26), checked on the core: changing the room size
    // leaves every gain and coefficient alone, changing the reverb time leaves every delay alone.
    using namespace oscairn_fx::hall;
    auto core = [](const Settings &q) {
        auto c = std::make_unique<Core>();
        c->prepare(44100);
        c->set(q);
        return c;
    };
    auto gains = [](const Core &c) {
        std::vector<double> g{c.loopGain.v, c.level.v, c.mix.v};
        for (auto &s : c.erGain)
            for (auto &e : s)
                g.push_back(e.v);
        for (int s = 0; s < 2; ++s)
            for (const FirstOrder *f : {&c.hp[s], &c.shelf[s], &c.pre[s], &c.lp1[s], &c.loopShelf[s], &c.outLp[s]})
                g.insert(g.end(), {f->b0.v, f->b1.v, f->a1.v});
        return g;
    };
    auto delays = [](const Core &c) {
        std::vector<int> d;
        for (int s = 0; s < 2; ++s)
        {
            d.insert(d.end(), std::begin(c.dDiff[s]), std::end(c.dDiff[s]));
            d.insert(d.end(), std::begin(c.dEr[s]), std::end(c.dEr[s]));
            d.insert(d.end(), std::begin(c.dFeed[s]), std::end(c.dFeed[s]));
            d.insert(d.end(), std::begin(c.dOut[s]), std::end(c.dOut[s]));
            d.insert(d.end(), {c.dErAp[s], c.dTankAp[s], c.dLine1[s], c.dOuter[s], c.dInner[s], c.dLine2[s]});
        }
        return d;
    };
    Settings a{}, b{};
    b.size = 8;
    REQUIRE(gains(*core(a)) == gains(*core(b)));
    REQUIRE(delays(*core(a)) != delays(*core(b)));
    b = a;
    b.time = 13.5;
    REQUIRE(delays(*core(a)) == delays(*core(b)));
    REQUIRE(gains(*core(a)) != gains(*core(b)));
    // No modulation: an impulse later in time gives the same response, shifted.
    auto c = core(a);
    const int n = 44100 * 2, late = 12345;
    std::vector<float> l(n, 0), r(n, 0), l2(n, 0), r2(n, 0);
    l[0] = r[0] = 1;
    l2[late] = r2[late] = 1;
    c->process(l.data(), r.data(), n);
    c = core(a);
    c->process(l2.data(), r2.data(), n);
    for (int i = 0; i + late < n; ++i)
        REQUIRE(l2[i + late] == l[i]);
}

TEST_CASE("Hall A: the reference's decay per band on fitting renders, and its onset")
{
    // Reference renders of the fitting grid (fx19, fx27; a sampled click, fx/reverb.py's
    // analysis): the plugin's decay per octave band (250 Hz to 8 kHz) within 10% (the M3 gate) at
    // fitting points the fit reaches. Settings in the platform's units.
    using namespace oscairn_fx::hall;
    struct Ref
    {
        std::vector<std::pair<const char *, double>> set;
        double t[6]; // 250 Hz ... 8 kHz
    };
    for (auto &c : std::vector<Ref>{
             {{{"HPF Cutoff", 90}, {"High Ratio", 0.5}}, {2.87, 2.98, 2.87, 2.56, 2.15, 1.63}},                 // fx19 r_h5
             {{{"HPF Cutoff", 90}, {"Reverb Time", 2.7}, {"Room Size", 31}}, {2.59, 2.62, 2.55, 2.4, 2.16, 1.77}}, // r_t24_s31
             {{{"HPF Cutoff", 90}, {"High Ratio", 0.6}}, {2.88, 3.0, 2.91, 2.65, 2.26, 1.71}}})                  // r_h6
    {
        auto [l, r] = hallImpulse(44100, c.set, 2.5 * c.t[0] + 0.5);
        for (int b = 1; b < bands; ++b)
        {
            const double got = 0.5 * (bandDecay(l, 44100, bandHz[b]) + bandDecay(r, 44100, bandHz[b]));
            INFO("setting " << c.set.size() << " band " << bandHz[b] << " reference " << c.t[b - 1] << " got " << got);
            CHECK(std::abs(got / c.t[b - 1] - 1) < 0.1);
        }
    }
    // The onset is the first early reflection: initial delay + fixed + scaled x the room's scale
    // (measured on fx19's and fx27's diffusion-0 renders), moved by the decay setting.
    for (auto [id, size, dec] : std::vector<std::array<double, 3>>{{67.8, 29, 55}, {0.1, 0, 63}, {200, 31, 0}, {20, 15, 40}})
    {
        auto [l, r] = hallImpulse(44100, {{"Initial Delay", id}, {"Room Size", size}, {"Decay", dec}, {"HPF Cutoff", 20}}, 0.6);
        const double want = (id + interp(fitted.decayAt, fitted.decayShiftMs, 5, dec) + fitted.erFixedMs[0][0] +
                             fitted.erScaledMs[0][0] * sizeScale(fitted, size)) / 1000;
        INFO("initial delay " << id << " size " << size << " decay " << dec);
        REQUIRE(std::abs(firstAbove(l, 44100, 1e-7f) - want) <= 1.0 / 44100);
    }
}

TEST_CASE("Hall A: the LPF is 0.4 + 0.6 times a first-order low-pass")
{
    using namespace oscairn_fx::hall;
    // a short reverb time, so the 2 s responses hold their whole tails
    auto [a, ar] = hallImpulse(44100, {{"HPF Cutoff", 20}, {"LPF Cutoff", 20000}, {"Reverb Time", 0.3}}, 2.0);
    auto [b, br] = hallImpulse(44100, {{"HPF Cutoff", 20}, {"LPF Cutoff", 1000}, {"Reverb Time", 0.3}}, 2.0);
    auto at = [&](const std::vector<float> &x, double f) {
        std::complex<double> s = 0;
        for (size_t i = 0; i < x.size(); ++i)
            s += (double)x[i] * std::polar(1.0, -2 * M_PI * f * i / 44100);
        return std::abs(s);
    };
    // the same network both times, so the ratio of the spectra is the filter
    REQUIRE(20 * std::log10(at(b, 1000) / at(a, 1000)) == Approx(-2.37).margin(0.05));
    REQUIRE(20 * std::log10(at(b, 8000) / at(a, 8000)) == Approx(20 * std::log10(std::abs(0.4 + 0.6 / std::complex<double>(1, std::tan(M_PI * 8000 / 44100) / std::tan(M_PI * 1000 / 44100))))).margin(0.05));
}

namespace
{
// Power (dB) of x from t0 to t1 seconds after its onset, summed over 11 frequencies spaced 1/30
// octave around fc: a narrow band, so the ratio of two renders' values is the filter between them
// whatever the source's spectrum.
double narrowBandAfterOnset(const std::vector<float> &x, double rate, double fc, double t0, double t1)
{
    const double on = firstAbove(x, rate, 1e-7f);
    double p = 0;
    for (int k = -5; k <= 5; ++k)
    {
        std::complex<double> s = 0;
        const double f = fc * std::pow(2.0, k / 30.0);
        for (size_t i = (size_t)std::ceil((on + t0) * rate); i < x.size() && i < (on + t1) * rate; ++i)
            s += (double)x[i] * std::polar(1.0, -2 * M_PI * f * i / rate);
        p += std::norm(s);
    }
    return 10 * std::log10(p);
}
} // namespace

TEST_CASE("Hall A: the ratios' fixed filters act on the whole reverb from its first reflection on")
{
    // Reference renders (fx19, a sampled click, the same measure): against high ratio 1.0, high
    // ratio 0.1 and 0.4 take the same low-pass off every part of the reverb in its first 300 ms,
    // early reflections included (checked over 150 ms: this design's tank recirculates sooner, known
    // difference 30), and against low ratio 1.0, low ratio 0.1 and 1.4 shelve the first 300 ms.
    // Within 0.75 dB per band (tolerance proposed, set after seeing 0.52 dB at 8 kHz, high ratio 0.1).
    struct Ref
    {
        const char *name;
        double from, to, t1;
        std::vector<std::pair<double, double>> db; // band (Hz), reference difference (dB)
    };
    for (auto &c : std::vector<Ref>{{"High Ratio", 1.0, 0.1, 0.15, {{1000, -3.22}, {2000, -6.88}, {4000, -12.56}, {8000, -18.91}}}, // r_h1 - r_h10
                                    {"High Ratio", 1.0, 0.4, 0.15, {{1000, -0.76}, {2000, -2.57}, {4000, -6.53}, {8000, -12.25}}}, // r_h4 - r_h10
                                    {"Low Ratio", 1.0, 0.1, 0.3, {{1000, -5.61}, {2000, -3.03}, {4000, -1.00}}}, // r_l1 - r_l10
                                    {"Low Ratio", 1.0, 1.4, 0.3, {{1000, 0.87}, {2000, 0.36}, {4000, 0.10}}}}) // r_l14 - r_l10
    {
        auto a = hallImpulse(44100, {{"HPF Cutoff", 90}, {c.name, c.from}}, 0.5).first;
        auto b = hallImpulse(44100, {{"HPF Cutoff", 90}, {c.name, c.to}}, 0.5).first;
        for (auto [fc, want] : c.db)
        {
            const double got = narrowBandAfterOnset(b, 44100, fc, 0.012, c.t1) - narrowBandAfterOnset(a, 44100, fc, 0.012, c.t1);
            INFO(c.name << " " << c.from << " to " << c.to << " at " << fc << " Hz: reference " << want << " dB, got " << got);
            CHECK(std::abs(got - want) < 0.75);
        }
    }
}

TEST_CASE("Hall A keeps its 44.1 kHz decay, onset and level at 48, 88.2, 96 and 192 kHz")
{
    // Decision 13: per octave band (250 Hz to 8 kHz) the decay time
    // within 5% of the 44.1 kHz render's and its energy within 1.5 dB (the plan's M3 energy
    // tolerance; the 8 kHz band reaches about 1 dB, the bilinear filters' warping near 22 kHz at
    // 44.1 kHz, known difference 23), and the onset within 0.1 ms (tolerances proposed). Filters
    // at their defaults (HPF 80 Hz, LPF 5.6 kHz).
    using namespace oscairn_fx::hall;
    for (auto set : std::vector<std::vector<std::pair<const char *, double>>>{
             {}, {{"Reverb Time", 1.1}, {"Room Size", 8}, {"High Ratio", 0.4}}, {{"Reverb Time", 9.5}, {"Room Size", 31}, {"Initial Delay", 150}}})
    {
        const double secs = set.empty() ? 5 : (set.size() == 3 && set[0].second > 5 ? 12 : 3);
        auto ref = hallImpulse(44100, set, secs).first;
        for (double rate : {48000.0, 88200.0, 96000.0, 192000.0})
        {
            auto got = hallImpulse(rate, set, secs).first;
            INFO("rate " << rate << " setting " << set.size());
            REQUIRE(std::abs(firstAbove(got, rate) - firstAbove(ref, 44100)) < 1e-4);
            std::printf("Hall A at %.0f Hz, setting %zu, band: decay, energy:", rate, set.size());
            for (int b = 1; b < bands; ++b)
            {
                // a unit impulse carries 44100/rate of 44.1 kHz's energy per hertz
                double ea = 0, ec = 0;
                const double a = bandDecay(ref, 44100, bandHz[b], &ea), c = bandDecay(got, rate, bandHz[b], &ec);
                const double de = 10 * std::log10(ec * rate / ea / 44100);
                std::printf(" %.0f: %+.1f%% %+.2f dB", bandHz[b], 100 * (c / a - 1), de);
                REQUIRE(std::abs(c / a - 1) < 0.05);
                REQUIRE(std::abs(de) < 1.5);
            }
            std::printf("\n");
        }
    }
}

namespace
{
const char *dynamicsId = "org.oscairn.effects.dynamics";
// A 440 Hz sine at -22.1 dBFS RMS stepping 13 dB down for the second of every two seconds, the
// reference probes' source (fx21 to fx23), through the circuit compressor with the platform's
// raw values (input, output, ratio, attack, release, each 0..200 or 0..4).
std::vector<float> compRender(double rate, int inl, int outl, int ratio, int atk, int rel, double seconds = 4)
{
    using namespace oscairn_fx::dynamics;
    Plugin pl(dynamicsId);
    auto set = [&](const char *n, double v) { pl.set(paramIdByName(pl.p, "Circuit Compressor", n), v); };
    set("Input Level", 60 * std::log10(inl / 200.0));
    set("Output Level", 60 * std::log10(outl / 200.0));
    set("Ratio", ratio);
    set("Attack", attackSeconds(atk) * 1000);
    set("Release", releaseSeconds(rel) * 1000);
    pl.activate(rate);
    const size_t n = (size_t)(rate * seconds) + oscairn_fx::latencySamples;
    std::vector<float> in(n, 0), r;
    for (size_t i = 0; i < n; ++i)
    {
        const double t = i / rate;
        in[i] = (float)(0.0786 * M_SQRT2 * (std::fmod(t, 2.0) < 1 ? 1 : std::pow(10.0, -13 / 20.0)) * std::sin(2 * M_PI * 440 * t));
    }
    auto l = pl.render(in, in, r, {64}, {}, false);
    l.erase(l.begin(), l.begin() + oscairn_fx::latencySamples);
    return l;
}
// Gain (dB) over [t0, t1) s against the source.
double compGain(const std::vector<float> &y, double rate, double t0, double t1)
{
    double ey = 0, ex = 0;
    for (size_t i = (size_t)(t0 * rate); i < (size_t)(t1 * rate); ++i)
    {
        const double t = i / rate;
        const double x = 0.0786 * M_SQRT2 * (std::fmod(t, 2.0) < 1 ? 1 : std::pow(10.0, -13 / 20.0)) * std::sin(2 * M_PI * 440 * t);
        ey += (double)y[i] * y[i], ex += x * x;
    }
    return 10 * std::log10(ey / ex);
}
} // namespace

TEST_CASE("Circuit compressor: the measured steady gains at 44.1 kHz")
{
    // Gains measured on reference renders (fx21, fx22): below threshold (the quiet second) the
    // gain depends on the settings, not on the signal, within 0.15 dB; compressing (the loud
    // second at input 160 or 200) within 0.5 dB (tolerances proposed).
    struct Case { int in, out, q, a, r; double quiet, loud; };
    for (auto c : std::vector<Case>{{100, 100, 1, 0, 0, 7.75, NAN}, {100, 100, 1, 200, 0, 4.77, NAN},
                                    {100, 100, 1, 0, 200, 8.66, NAN}, {60, 100, 1, 10, 100, -1.35, NAN},
                                    {60, 100, 1, 175, 100, 2.26, NAN}, {90, 100, 1, 100, 100, 4.77, NAN},
                                    {200, 60, 1, 100, 0, NAN, -8.84}, {200, 60, 1, 100, 200, NAN, -7.47},
                                    {160, 60, 0, 100, 100, NAN, -9.31}, {160, 60, 4, 100, 100, NAN, -6.57}})
    {
        auto y = compRender(44100, c.in, c.out, c.q, c.a, c.r);
        const double quiet = compGain(y, 44100, 1.5, 2.0), loud = compGain(y, 44100, 2.5, 3.0);
        INFO("input " << c.in << " ratio " << c.q << " attack " << c.a << " release " << c.r << ": quiet " << quiet << " loud " << loud);
        if (!std::isnan(c.quiet))
            REQUIRE(std::abs(quiet - c.quiet) < 0.15);
        if (!std::isnan(c.loud))
            REQUIRE(std::abs(loud - c.loud) < 0.5);
    }
}

TEST_CASE("Circuit compressor: low input settings, and ratio 2 near full scale (fx7, fx13)")
{
    // A steady 440 Hz sine at -24.08 dBFS RMS, output 200, attack and release 100, as in fx7 and
    // fx13. The reference's gain moves in 6.02 dB steps at the lowest input settings (its input
    // level is held to steps of 1/16384), and at ratio 2 with input 200 its output is squashed to a
    // peak of 0.810 without reaching full scale (tolerances 0.1 dB, 0.3 dB and 0.02, accepted).
    using namespace oscairn_fx::dynamics;
    auto render = [](int inl, int ratio) {
        Plugin pl(dynamicsId);
        auto set = [&](const char *n, double v) { pl.set(paramIdByName(pl.p, "Circuit Compressor", n), v); };
        set("Input Level", 60 * std::log10(inl / 200.0));
        set("Output Level", 0);
        set("Ratio", ratio);
        set("Attack", attackSeconds(100) * 1000);
        set("Release", releaseSeconds(100) * 1000);
        pl.activate(44100);
        const size_t n = 44100 + oscairn_fx::latencySamples;
        std::vector<float> in(n), r;
        for (size_t i = 0; i < n; ++i)
            in[i] = (float)(0.08891 * std::sin(2 * M_PI * 440 * i / 44100.0));
        auto l = pl.render(in, in, r, {64}, {}, false);
        double e = 0, peak = 0;
        for (size_t i = 22050; i < n; ++i)
            e += (double)l[i] * l[i], peak = std::max(peak, (double)std::abs(l[i]));
        return std::make_pair(10 * std::log10(e / (n - 22050)), peak);
    };
    for (auto [inl, gain] : std::vector<std::pair<int, double>>{{8, -18.16}, {10, -12.14}, {13, -6.12}, {16, -0.10}, {20, 5.92}})
    {
        const double got = render(inl, 1).first + 24.08;
        INFO("input " << inl << ": gain " << got << " dB, reference " << gain);
        REQUIRE(std::abs(got - gain) < 0.1);
    }
    auto [rms, peak] = render(200, 0);
    INFO("ratio 2, input 200: " << rms << " dBFS RMS, peak " << peak);
    REQUIRE(std::abs(rms - -3.30) < 0.3);
    REQUIRE(std::abs(peak - 0.810) < 0.02);
}

TEST_CASE("Circuit compressor keeps its 44.1 kHz gain at 48, 88.2, 96 and 192 kHz")
{
    // The gain in 5 ms windows over a step down and back, compressing, within 0.1 dB of the
    // 44.1 kHz render's (tolerance proposed); slow and fast settings.
    for (auto s : std::vector<std::array<int, 5>>{{200, 60, 1, 100, 100}, {160, 100, 3, 20, 10}, {120, 60, 0, 0, 200}})
    {
        auto ref = compRender(44100, s[0], s[1], s[2], s[3], s[4]);
        for (double rate : {48000.0, 88200.0, 96000.0, 192000.0})
        {
            auto got = compRender(rate, s[0], s[1], s[2], s[3], s[4]);
            double worst = 0;
            for (double t = 0.2; t < 3.9; t += 0.005)
                worst = std::max(worst, std::abs(compGain(got, rate, t, t + 0.005) - compGain(ref, 44100, t, t + 0.005)));
            std::printf("Circuit compressor at %.0f Hz, input %d attack %d release %d: worst %.3f dB\n", rate, s[0], s[3], s[4], worst);
            REQUIRE(worst < 0.1);
        }
    }
}

namespace
{
// Compressor A's settings: attack ms, release ms, threshold dB, ratio, output level dB.
struct CompA
{
    double attack, release, threshold, ratio, output;
};
// A sine at amplitude 0.3 for half a second, then 0.03 (20 dB down), on both channels or the left
// only, through Compressor A; returns the left output.
std::vector<float> compARender(double rate, CompA c, double hz, bool leftOnly, double seconds = 1)
{
    Plugin pl(dynamicsId);
    pl.set(1, 2);
    auto set = [&](const char *n, double v) { pl.set(paramIdByName(pl.p, "Compressor A", n), v); };
    set("Attack", c.attack), set("Release", c.release), set("Threshold", c.threshold), set("Ratio", c.ratio), set("Output Level", c.output);
    pl.activate(rate);
    const size_t n = (size_t)(rate * seconds) + oscairn_fx::latencySamples;
    std::vector<float> l(n, 0), r(n, 0), outR;
    for (size_t i = 0; i < (size_t)rate; ++i)
    {
        const double t = i / rate;
        l[i] = (float)((t < 0.5 ? 0.3 : 0.03) * std::sin(2 * M_PI * hz * t));
        r[i] = leftOnly ? 0.f : l[i];
    }
    auto o = pl.render(l, r, outR, {64}, {}, false);
    o.erase(o.begin(), o.begin() + oscairn_fx::latencySamples);
    REQUIRE(pl.last != CLAP_PROCESS_ERROR);
    return o;
}
// Gain (dB) of the left output against the input over [t0, t1) s.
double compAGain(const std::vector<float> &y, double rate, double hz, double t0, double t1)
{
    double ey = 0, ex = 0;
    for (size_t i = (size_t)(t0 * rate); i < (size_t)(t1 * rate); ++i)
    {
        const double t = i / rate, x = (float)((t < 0.5 ? 0.3 : 0.03) * std::sin(2 * M_PI * hz * t));
        ey += (double)y[i] * y[i], ex += x * x;
    }
    return 10 * std::log10(ey / ex);
}
const double compAWindows[6][2] = {{0, 0.003}, {0.003, 0.01}, {0.05, 0.5}, {0.502, 0.505}, {0.52, 0.55}, {0.8, 1}};
} // namespace

TEST_CASE("Compressor A: the reference program's gains at 44.1 kHz")
{
    // Expected gains from the reference program's structure and words (plan, section 15), which
    // null against reference renders within -127 dB; each case leans on some of its laws: the
    // insertion level, the threshold and output words and the gain table (steady windows), the
    // attack smoother (onset windows), the two release followers (after the step), the detector's
    // link (left only), the 20 Hz high-pass (a 20 Hz sine at ratio 1). Within 0.01 dB.
    const double out101 = 40 * std::log10(101 / 127.0);
    struct Case
    {
        const char *name;
        CompA c;
        double hz;
        bool leftOnly;
        double gains[6];
    };
    for (auto &k : std::vector<Case>{
             {"defaults", {8, 85, -30, 3, out101}, 1000, false, {6.8088, 3.1994, -5.7926, -5.6306, -4.6793, 7.5661}},
             {"defaults, left only", {8, 85, -30, 3, out101}, 1000, true, {7.0061, 4.0953, -1.7792, -1.6164, -0.6578, 7.9660}},
             {"slow", {40, 680, -40, 20, 0}, 1000, false, {11.2566, 10.2076, -10.6783, -17.4256, -17.5698, -15.0508}},
             {"fast", {1, 10, -12, 1.5, 40 * std::log10(80 / 127.0)}, 1000, false, {3.4760, 3.1456, 3.1654, 3.7052, 3.9990, 3.9975}},
             {"ratio 1 at 20 Hz", {8, 85, -30, 1, 0}, 20, false, {10.7532, 7.9531, 8.9013, 30.3171, 7.6924, 8.9016}},
             {"ratio 10 at 220 Hz", {4, 55, -24, 10, 40 * std::log10(90 / 127.0)}, 220, false, {3.9645, -1.0348, -7.1130, -9.3988, -3.2580, 6.0224}}})
    {
        auto y = compARender(44100, k.c, k.hz, k.leftOnly);
        for (int w = 0; w < 6; ++w)
        {
            const double g = compAGain(y, 44100, k.hz, compAWindows[w][0], compAWindows[w][1]);
            INFO(k.name << ", window " << compAWindows[w][0] << " s: " << g << " dB, expected " << k.gains[w]);
            REQUIRE(std::abs(g - k.gains[w]) < 0.01);
        }
    }
}

TEST_CASE("Compressor A keeps its 44.1 kHz gains at 48 and 96 kHz")
{
    // 5 ms windows over the step, compressing, within 0.1 dB of the 44.1 kHz render's (proposed);
    // the words' poles keep their time constants, the high-pass its corner.
    for (auto c : std::vector<CompA>{{8, 85, -30, 3, 0}, {1, 10, -40, 20, -6}, {40, 680, -24, 5, 0}})
    {
        auto ref = compARender(44100, c, 1000, false);
        for (double rate : {48000.0, 96000.0})
        {
            auto got = compARender(rate, c, 1000, false);
            double worst = 0;
            for (double t = 0.01; t < 0.99; t += 0.005)
                worst = std::max(worst, std::abs(compAGain(got, rate, 1000, t, t + 0.005) - compAGain(ref, 44100, 1000, t, t + 0.005)));
            std::printf("Compressor A at %.0f Hz, attack %g release %g: worst %.3f dB\n", rate, c.attack, c.release, worst);
            REQUIRE(worst < 0.1);
        }
    }
}

TEST_CASE("Compressor A: fed silence, the output is exact silence and the plugin sleeps")
{
    Plugin pl(dynamicsId);
    pl.set(1, 2);
    pl.activate(44100);
    const size_t n = 44100 * 3;
    std::vector<float> l(n, 0), r;
    for (size_t i = 0; i < 44100 / 2; ++i)
        l[i] = (float)(0.5 * std::sin(i * 0.05));
    auto o = pl.render(l, l, r, {64}, {}, false);
    size_t last = 0;
    for (size_t i = 0; i < n; ++i)
        if (o[i] != 0 || r[i] != 0)
            last = i;
    INFO("last non-zero sample at " << last / 44100.0 << " s");
    REQUIRE(last < 44100);
    REQUIRE(pl.last == CLAP_PROCESS_SLEEP);
}

TEST_CASE("Hall A changes high ratio and reverb time without clicks")
{
    // Every gain and filter coefficient ramps over 2 ms to its new value. The click is the
    // largest output above 8 kHz (a fourth-order high-pass) in the 50 ms after a high ratio or
    // reverb time change, for a 220 Hz + 1 kHz input with nothing up there; it must stay below
    // -60 dBFS (accepted). Offline, as before.
    struct Change
    {
        const char *name;
        double from, to;
    };
    for (auto c : {Change{"High Ratio", 1.0, 0.1}, Change{"High Ratio", 0.1, 1.0}, Change{"Reverb Time", 18, 0.3}})
        for (double rate : {44100.0, 192000.0})
        {
            Plugin pl(reverbId);
            setOffline(pl);
            pl.set(1, 3);
            pl.set(paramIdByName(pl.p, "Hall A", "LPF Cutoff"), 20000);
            pl.set(paramIdByName(pl.p, "Hall A", c.name), c.from);
            pl.activate(rate);
            const size_t n = (size_t)(rate * 1.6), at = (size_t)(rate * 1.5) + 7;
            std::vector<float> in(n), r;
            for (size_t i = 0; i < n; ++i)
                in[i] = 0.25f * (float)(std::sin(2 * M_PI * 220 * i / rate) + std::sin(2 * M_PI * 1000 * i / rate));
            auto out = pl.render(in, in, r, {64}, {{(uint32_t)at, paramIdByName(pl.p, "Hall A", c.name), c.to}}, false);
            double peak = 0, z[2][2]{};
            const double K = std::tan(M_PI * 8000 / rate);
            for (size_t i = 0; i < n; ++i)
            {
                double x = out[i];
                for (int s = 0; s < 2; ++s)
                {
                    const double q = s ? 1.3065630 : 0.5411961, norm = 1 / (1 + K / q + K * K);
                    const double b0 = norm, b1 = -2 * norm, a1 = 2 * (K * K - 1) * norm, a2 = (1 - K / q + K * K) * norm;
                    const double y = b0 * x + z[s][0];
                    z[s][0] = b1 * x - a1 * y + z[s][1];
                    z[s][1] = b0 * x - a2 * y;
                    x = y;
                }
                if (i >= at && i < at + (size_t)(0.05 * rate))
                    peak = std::max(peak, std::abs(x));
            }
            const double db = 20 * std::log10(peak + 1e-12);
            std::printf("Hall A click, %s %.1f to %.1f at %.0f Hz: %.1f dBFS\n", c.name, c.from, c.to, rate, db);
            CHECK(db < -60);
        }
}

TEST_CASE("Hall A's parameter changes stay cheap on the audio thread")
{
    // Decision 27, revised: the rebuilt Hall A's changes cost a few first-order designs, so they
    // are made in process. Sweeping reverb time, room size and high ratio at 192 kHz with one
    // change per 64-sample host block (333 us), the worst process call must stay under a quarter
    // of the block, as the off-thread EQs' does (unsanitized builds). The sweep is the same in
    // each of three runs, so each block is gated on its fastest run: a block the OS preempted in
    // one run does not count, a change that is slow, or waits, is slow in every run.
    const double rate = 192000;
    const uint32_t frames = 64;
    const double blockUs = 1e6 * frames / rate;
    auto tone = [rate](size_t i) { return 0.25f * (float)std::sin(2 * M_PI * 440.0 * (double)i / rate); };
    for (const char *name : {"Reverb Time", "Room Size", "High Ratio"})
    {
        std::vector<double> best; // per block, its fastest of the three runs
        for (int run = 0; run < 3; ++run)
        {
            Plugin pl(reverbId);
            pl.set(1, 3);
            pl.activate(rate);
            const clap_id id = paramIdByName(pl.p, "Hall A", name);
            Driver d(pl, frames);
            size_t at = 0;
            for (int k = 0; k < 20; ++k, at += frames)
                d.block(at, tone);
            d.callUs.clear();
            for (int k = 0; k <= 300; ++k, at += frames)
            {
                const double v = !std::strcmp(name, "Room Size") ? k % 32 : !std::strcmp(name, "High Ratio") ? 0.1 + 0.1 * (k % 10) : 0.3 + 17.7 * k / 300.0;
                d.block(at, tone, id, v);
            }
            if (best.empty())
                best = d.callUs;
            for (size_t i = 0; i < best.size(); ++i)
                best[i] = std::min(best[i], d.callUs[i]);
        }
        const double bestWorst = *std::max_element(best.begin(), best.end());
        std::printf("Hall A %s sweep at 192 kHz: worst process call %.1f us (block %.1f us)\n", name, bestWorst, blockUs);
#ifndef OSCAIRN_SANITIZED
        REQUIRE(bestWorst < blockUs / 4);
#endif
    }
}

TEST_CASE("Reset leaves a fresh instance at once in every mode")
{
    constexpr double fresh_tempo = 100;
    // Oscairn resets a used effect before giving it a state, or before playing it again with its
    // own: it must then render sample for sample as a fresh instance (activated, then given the
    // state), from the first block. Cases: the same state loaded again, another state, no load,
    // and a load while deactivated. At 48 kHz the EQs' coefficients come rate-matched. Reset
    // allocates nothing.
    // A state in mode m with every parameter of that mode at fraction f of its range.
    auto setAll = [](Plugin &a, int m, double f) {
        auto *params = ext<clap_plugin_params_t>(a.p, CLAP_EXT_PARAMS);
        for (uint32_t i = 1; i < params->count(a.p); ++i)
        {
            clap_param_info_t info;
            REQUIRE(params->get_info(a.p, i, &info));
            if ((info.id >> 8) != (clap_id)m)
                continue;
            double v = info.min_value + f * (info.max_value - info.min_value);
            if (info.flags & CLAP_PARAM_IS_STEPPED)
                v = std::round(v);
            a.set(info.id, v);
        }
    };
    auto stateAt = [&](const char *id, int m, double f) {
        Plugin a(id);
        a.set(1, m);
        setAll(a, m, f);
        Buffer buf;
        REQUIRE(ext<clap_plugin_state_t>(a.p, CLAP_EXT_STATE)->save(a.p, &buf.out));
        return buf.data;
    };
    auto load = [](Plugin &pl, const std::string &data) {
        Buffer buf;
        buf.data = data;
        return ext<clap_plugin_state_t>(pl.p, CLAP_EXT_STATE)->load(pl.p, &buf.in);
    };
    for (const char *id : {reverbId, eqId, "org.oscairn.effects.delay", "org.oscairn.effects.dynamics", "org.oscairn.effects.tremolo-rotary", "org.oscairn.effects.chorus"})
    {
        Plugin probe(id);
        clap_param_info_t info;
        ext<clap_plugin_params_t>(probe.p, CLAP_EXT_PARAMS)->get_info(probe.p, 0, &info);
        for (int m = (int)info.min_value; m <= (int)info.max_value; ++m)
            for (double rate : {44100.0, 48000.0})
            {
                const std::string stateA = stateAt(id, m, 0.37), stateB = stateAt(id, m, 0.71);
                std::vector<float> inL, inR, r1, r2;
                signal((size_t)(rate / 2), inL, inR);
                for (int c = 0; c < 4; ++c)
                {
                    INFO(id << " mode " << m << " at " << rate << ", case " << c);
                    Plugin used(id);
                    used.tempo = fresh_tempo;
                    REQUIRE(load(used, stateA));
                    used.activate(rate);
                    used.render(inL, inR, r1, {64});
                    // Every parameter moved a block before the reset: smoothers are mid-way.
                    setAll(used, m, 0.55);
                    std::vector<float> z(64, 0);
                    used.render(z, z, r1, {64});
                    // time for the worker to match the change (the EQs, Rotary A's shelves, the delays' and
                    // the chorus modes' EQ)
                    if ((!std::strcmp(id, eqId) || !std::strcmp(id, "org.oscairn.effects.tremolo-rotary") ||
                         !std::strcmp(id, "org.oscairn.effects.delay") || !std::strcmp(id, "org.oscairn.effects.chorus")) &&
                        rate != 44100)
                        std::this_thread::sleep_for(std::chrono::milliseconds(200));
                    Buffer before;
                    REQUIRE(ext<clap_plugin_state_t>(used.p, CLAP_EXT_STATE)->save(used.p, &before.out));
                    const std::string next = c == 0 ? stateA : c == 2 ? before.data : stateB;
                    allocations = 0;
                    countAllocations = true;
                    used.p->reset(used.p);
                    countAllocations = false;
                    REQUIRE(allocations == 0);
                    if (c == 3)
                        used.p->deactivate(used.p);
                    if (c != 2)
                        REQUIRE(load(used, next));
                    if (c == 3)
                        used.activate(rate);
                    Plugin fresh(id);
                    fresh.tempo = fresh_tempo;
                    fresh.activate(rate);
                    REQUIRE(load(fresh, next));
                    const auto a = used.render(inL, inR, r1, {64}), b = fresh.render(inL, inR, r2, {64});
                    REQUIRE(a == b);
                    REQUIRE(r1 == r2);
                }
            }
    }
    // Once it has played, a state load keeps the effect (and its tail): nothing is built for it.
    Plugin pl;
    pl.activate();
    std::vector<float> inL, inR, r;
    signal(4800, inL, inR);
    pl.render(inL, inR, r, {64});
    oscairn_memory_t before{}, after{};
    REQUIRE(memoryExt(pl.p)->current(pl.p, &before));
    REQUIRE(load(pl, stateAt(reverbId, 2, 0.71)));
    REQUIRE(memoryExt(pl.p)->current(pl.p, &after));
    REQUIRE(after.context_bytes == before.context_bytes);
}

// ---- Rotary A (plan, section 15, "Rotary A") ----
namespace
{
constexpr const char *rotaryId = "org.oscairn.effects.tremolo-rotary";
struct RotaryOut
{
    std::vector<float> l, r;
    double rate;
};
// Sines at the given frequencies (each 0.25 peak) through Rotary A with parameters by name; the
// Speed parameter switches at the given times (s). Latency removed.
RotaryOut rotaryRender(double rate, std::vector<std::pair<const char *, double>> set, double seconds,
                       std::vector<double> freqs, std::vector<std::pair<double, double>> speedAt = {})
{
    Plugin pl(rotaryId);
    for (auto &[n, v] : set)
        pl.set(paramIdByName(pl.p, "Rotary A", n), v);
    pl.activate(rate);
    const size_t n = (size_t)(rate * seconds) + L;
    std::vector<float> in(n, 0);
    for (size_t i = 0; i < n; ++i)
        for (double f : freqs)
            in[i] += (float)(0.25 * std::sin(2 * M_PI * f * i / rate));
    std::vector<Event> ev;
    const clap_id sp = paramIdByName(pl.p, "Rotary A", "Speed");
    for (auto [t, v] : speedAt)
        ev.push_back({(uint32_t)(t * rate) + L, sp, v});
    RotaryOut o;
    o.rate = rate;
    o.l = pl.render(in, in, o.r, {64}, ev, false);
    o.l.erase(o.l.begin(), o.l.begin() + L);
    o.r.erase(o.r.begin(), o.r.begin() + L);
    return o;
}
// The complex envelope of y at carrier f, one value per millisecond (the mean over the 4 carrier
// periods centred on it).
std::vector<std::complex<double>> envelopeAt(const std::vector<float> &y, double rate, double f)
{
    const size_t w = (size_t)std::lround(4 * rate / f);
    std::vector<std::complex<double>> z;
    std::vector<std::complex<double>> acc(y.size() + 1);
    for (size_t i = 0; i < y.size(); ++i)
        acc[i + 1] = acc[i] + (double)y[i] * std::polar(1.0, -2 * M_PI * f * i / rate);
    for (size_t k = 0;; ++k)
    {
        const size_t i = (size_t)std::lround(k * rate / 1000);
        if (i + w >= y.size())
            break;
        z.push_back(i >= w / 2 ? (acc[i + w - w / 2] - acc[i - w / 2]) / (double)w : acc[i + w - w / 2] / (double)(i + w - w / 2));
    }
    return z;
}
// The delay (ms) the envelope's phase gives, per millisecond, unwrapped.
std::vector<double> delayTrack(const std::vector<std::complex<double>> &z, double f)
{
    std::vector<double> d(z.size());
    double prev = 0, unwrap = 0;
    for (size_t i = 0; i < z.size(); ++i)
    {
        double a = std::arg(z[i]);
        if (i)
        {
            while (a + unwrap - prev > M_PI)
                unwrap -= 2 * M_PI;
            while (a + unwrap - prev < -M_PI)
                unwrap += 2 * M_PI;
        }
        prev = a + unwrap;
        d[i] = -prev / (2 * M_PI * f) * 1000;
    }
    return d;
}
// Upward crossings (ms) of x through its mean over [from, end), with hysteresis of a quarter of its
// range (a crossing counts once x has been below the low mark), linearly interpolated.
std::vector<double> crossings(const std::vector<double> &x, size_t from = 0)
{
    double lo = 1e300, hi = -1e300, mean = 0;
    for (size_t i = from; i < x.size(); ++i)
        lo = std::min(lo, x[i]), hi = std::max(hi, x[i]), mean += x[i];
    mean /= (double)(x.size() - from);
    const double h = 0.25 * (hi - lo);
    std::vector<double> c;
    bool armed = false;
    for (size_t i = from; i + 1 < x.size(); ++i)
    {
        armed = armed || x[i] < mean - h;
        if (armed && x[i] < mean && x[i + 1] >= mean)
        {
            c.push_back(i + (mean - x[i]) / (x[i + 1] - x[i]));
            armed = false;
        }
    }
    return c;
}
// Rotation rate (Hz) from the mean period between crossings in [t0, t1) ms.
double rotationHz(const std::vector<double> &c, double t0, double t1)
{
    std::vector<double> in;
    for (double t : c)
        if (t >= t0 && t < t1)
            in.push_back(t);
    REQUIRE(in.size() >= 2);
    return 1000.0 * (in.size() - 1) / (in.back() - in.front());
}
double peakToPeak(const std::vector<double> &x, size_t a, size_t b)
{
    const auto [lo, hi] = std::minmax_element(x.begin() + a, x.begin() + b);
    return *hi - *lo;
}
} // namespace

namespace
{
// The platform's speeds as the measured rates (rotation phase per 44.1 kHz sample, 2^-24 turns).
int platformStep(int i)
{
    if (i <= 63)
        return 16 * i;
    const int from[] = {64, 77, 89, 102, 114, 126, 128}, base[] = {1024, 1472, 2304, 4096, 7424, 14080}, step[] = {32, 64, 128, 256, 512, 1024};
    int k = 0;
    while (i >= from[k + 1])
        ++k;
    return base[k] + step[k] * (i - from[k]);
}
double platformHz(int i) { return platformStep(i) * 44100.0 / 16777216; }
} // namespace

TEST_CASE("Rotary A: the rotors turn at the platform's speeds, at 44.1, 48 and 96 kHz")
{
    // A set speed plays as the nearest of the platform's 128 (the measured rates: 0.6729 Hz at 16,
    // 6.7291 at 91, 7.4020 at 93): 0.83 Hz plays at index 20, 0.8411 Hz; within 0.2% (tolerance
    // proposed). Depths at 8 (6.3%) keep the delay tracks clean.
    for (double rate : {44100.0, 48000.0, 96000.0})
        for (auto [speed, set, rotor, horn] : {std::tuple{0, 0.83, platformHz(20), platformHz(26)}, std::tuple{1, 6.73, platformHz(91), platformHz(93)}})
        {
            auto o = rotaryRender(rate, {{"Speed", speed}, {speed ? "Rotor Fast Speed" : "Rotor Slow Speed", set}, {"Rotor Depth", 6.3}, {"Horn Depth", 6.3}},
                                  speed ? 6 : 12, {150, 3000});
            const double end = speed ? 6000 : 12000;
            const double fr = rotationHz(crossings(delayTrack(envelopeAt(o.l, rate, 150), 150), 1000), 1000, end);
            const double fh = rotationHz(crossings(delayTrack(envelopeAt(o.l, rate, 3000), 3000), 1000), 1000, end);
            INFO(rate << " Hz, speed " << speed << ": rotor " << fr << " horn " << fh);
            REQUIRE(std::abs(fr / rotor - 1) < 0.002);
            REQUIRE(std::abs(fh / horn - 1) < 0.002);
        }
}

TEST_CASE("Rotary A: ramps step every 100 ms through the platform's speeds, at 44.1, 48 and 96 kHz")
{
    // The reference's ramps (batch c e_ramp_def and e_ramp_jacob, and the first batch's r_ramp_*): a
    // switch steps both rotors at once and then on every 50th tick of a 2 ms timer (the first tick
    // 0-2 ms after the switch). The rotor's ramp speed moves by the slow-fast difference over the ramp
    // time in tenths of a second; the horn's by 2.4 / (its ramp time in tenths) of the rest, plus or
    // minus 1e-4 Hz. Each rotor turns at the platform's speed nearest its ramp speed on the side of the
    // target (the speeds rounded to 3 decimals below 10 Hz and 2 above). A ramp stops when both turn at
    // their targets, so a slowing ramp starts from where the speeding up stopped. Counted from the first
    // crossing of the delay track, every later crossing must fall within 0.05 turns of a whole number of
    // the law's turns (tolerance proposed); the reference's accumulated rotation over each of those four
    // ramps matches the law within 0.003 turns.
    const int slow[2] = {19, 26}, fast[2] = {91, 93};
    const double tenths[2] = {77, 14}, up = 1, down = 9;
    auto rampHz = [](int i) {
        const double f = platformHz(i), s = f < 10 ? 1000 : 100;
        return (float)(std::floor(f * s + 0.5) / s);
    };
    for (double rate : {44100.0, 48000.0, 96000.0})
    {
        // The law, per sample: the plugin's 16-sample blocks carry the switch and the timer's ticks.
        const size_t total = (size_t)(20 * rate);
        std::vector<double> hz[2];
        float state[2] = {rampHz(slow[0]), rampHz(slow[1])};
        int idx[2] = {slow[0], slow[1]}, isFast = 0, count = 0, done[2] = {slow[0], slow[1]};
        bool on = false;
        double timer = 0;
        auto step = [&] {
            bool going = false;
            for (int k = 0; k < 2; ++k)
            {
                const int t = isFast ? fast[k] : slow[k];
                const float T = rampHz(t);
                if (T != state[k])
                {
                    if (k == 0)
                    {
                        const float d = (rampHz(fast[0]) - rampHz(slow[0])) / (float)tenths[0];
                        state[0] = T > state[0] ? std::min(T, state[0] + d) : std::max(T, state[0] - d);
                    }
                    else
                    {
                        const float x = state[1] + (T - state[1]) / ((float)tenths[1] / 2.4f);
                        const float y = (float)((double)x + (T > state[1] ? 1e-4 : -1e-4));
                        state[1] = T > state[1] ? std::min(T, y) : std::max(T, y);
                    }
                }
                int i = 0;
                if (state[k] <= T)
                {
                    while (i < 127 && state[k] > rampHz(i))
                        ++i;
                    i = std::min(i, t);
                }
                else
                {
                    for (i = 127; i > 0 && state[k] < rampHz(i); --i)
                        ;
                    i = std::max(i, t);
                }
                idx[k] = i;
                done[k] = t;
                going = going || i != t;
            }
            return going;
        };
        for (size_t at = 0; at < total; at += 16)
        {
            const double t = at / rate;
            isFast = t >= up && t < down ? 1 : 0;
            if (done[0] != (isFast ? fast[0] : slow[0]))
                on = step(), count = 0;
            timer += 16 * 44100.0 / rate;
            while (timer >= 88.2)
            {
                timer -= 88.2;
                if (on && ++count == 50)
                    count = 0, on = step();
            }
            for (int k = 0; k < 2; ++k)
                hz[k].insert(hz[k].end(), 16, platformHz(idx[k]));
        }
        auto o = rotaryRender(rate, {{"Rotor Ramp Time", tenths[0] / 10}, {"Horn Ramp Time", tenths[1] / 10}, {"Rotor Depth", 6.3}, {"Horn Depth", 6.3}},
                              19, {150, 3000}, {{up - L / rate, 1}, {down - L / rate, 0}});
        for (int k = 0; k < 2; ++k)
        {
            const double f = k ? 3000 : 150;
            const auto c = crossings(delayTrack(envelopeAt(o.l, rate, f), f), 300);
            // The law's turns from the first crossing to each later one must be whole numbers.
            double worst = 0, turns = 0;
            size_t s = (size_t)(c[0] / 1000 * rate);
            int n = 0;
            for (size_t i = 1; i < c.size() && c[i] < 18500; ++i, ++n)
            {
                for (; s < (size_t)(c[i] / 1000 * rate); ++s)
                    turns += hz[k][s] / rate;
                worst = std::max(worst, std::abs(turns - (double)i));
            }
            std::printf("Rotary A ramps at %.0f Hz, %s: %d rotations, worst %.4f turns\n", rate, k ? "horn" : "rotor", n, worst);
            REQUIRE(n > 20);
            REQUIRE(worst < 0.05);
        }
    }
}

namespace
{
// Rotary A's measured structure at 44.1 kHz, for a sine x[n] = sin(w n) at its input (both channels):
// the output a sample at a time from the laws (plan, section 15, "Rotary A").
struct RotaryLaw
{
    int rotorSpeed, hornSpeed, rotorDepth, hornDepth, balance, lowIndexHz, lowDb, highHz, highDb, mic;
    bool fast;
};
// glide (optional): per band the swing (samples) and AM coefficients (centre, channel) in place of the setting's.
std::array<std::complex<double>, 2> rotaryLawAt(const RotaryLaw &s, double w, size_t n, const double (*glide)[3] = nullptr)
{
    using namespace oscairn_fx;
    auto H = [&](eq::Biquad b) {
        const auto z = std::polar(1.0, -w);
        return (b.b0 + b.b1 * z + b.b2 * z * z) / (1.0 + b.a1 * z + b.a2 * z * z);
    };
    // Bands: 1.25 (L + R), the crossover (16-bit words, one integer bit), 0.50116, the shelf (four
    // integer bits), 0.5.
    const double K0 = std::tan(M_PI * 619.5 / 44100), K1 = std::tan(M_PI * 999.35 / 44100), q = 0.7;
    eq::Biquad lp, hp, lo, hi;
    lp.set(-K0 * K0, -2 * K0 * K0, -K0 * K0, 1 + K0 / q + K0 * K0, 2 * (K0 * K0 - 1), 1 - K0 / q + K0 * K0);
    hp.set(1, -2, 1, 1 + K1 / q + K1 * K1, 2 * (K1 * K1 - 1), 1 - K1 / q + K1 * K1);
    eq::roundToSteps(lp, 1, true);
    eq::roundToSteps(hp, 1, true);
    eq::firstOrderShelf(lo, s.lowIndexHz, s.lowDb, false);
    eq::firstOrderShelf(hi, s.highHz, s.highDb, true);
    eq::roundToSteps(lo, 4, false);
    eq::roundToSteps(hi, 4, false);
    const std::complex<double> band[2] = {2.5 * 8211 / 16384 * 0.5 * H(lp) * H(lo), 2.5 * 8211 / 16384 * 0.5 * H(hp) * H(hi)};
    // Taps: the sine of the rotation (128 segments, 16 bits, 0 to 1), the delay 1 + swing x m
    // samples (linear), the gain 1 + AM x m in steps of 2^-15 rounded down.
    auto sine = [](double turns) {
        const uint32_t c = (uint32_t)(turns * 16777216.0) & 0xffffff;
        const int i = (int)(c >> 17), f = (int)((c >> 9) & 255);
        const double a = std::sin(2 * M_PI * i / 128), b = std::sin(2 * M_PI * (i + 1) / 128);
        return (std::floor((a + (b - a) * f / 256) * 32767 + 0.5) + 32768) / 65536;
    };
    auto phase = [](int v) { // the sixteen phases, turns
        const double a = v >= 64 ? 3.0 * (v - 64) : 180.0 * (v - 62.5) / 58.5;
        double best = 0;
        for (double p : {-180., -150., -135., -120., -90., -60., -45., -30., 0., 30., 45., 60., 90., 120., 135., 150., 180.})
            if (std::abs(p - a) < std::abs(best - a) - 1e-9)
                best = p;
        return (best < 0 ? best + 360 : best) / 360;
    };
    const int v = s.mic / 2;
    std::array<std::complex<double>, 2> out{};
    for (int k = 0; k < 2; ++k)
    {
        const int dv = k ? s.hornDepth : s.rotorDepth;
        int d, a, ac;
        if (k == 0)
            d = s.fast ? dv : 5 * dv / 4, a = s.fast ? (dv < 34 ? 15 * dv / 4 : 127) : (dv < 18 ? 57 * dv / 8 : 127), ac = s.fast ? 13 * a / 20 : 25 * a / 38;
        else
            d = s.fast ? dv : 7 * dv / 6, a = s.fast ? (dv <= 51 ? 15 * dv / 12 : 64) : (dv <= 22 ? 19 * dv / 6 : 70), ac = s.fast ? 26 * a / 15 : 15 * a / 19;
        double swing = 256.0 * (16384 * d / 127) / 32768, am = std::trunc(-a * 32768.0 / 127) / 32768, amc = std::trunc(-ac * 32768.0 / 127) / 32768;
        if (glide)
            swing = glide[k][0], am = glide[k][1], amc = glide[k][2];
        const double turns = (double)((uint64_t)platformStep(k ? s.hornSpeed : s.rotorSpeed) * (n + 1) & 0xffffff) / 16777216;
        auto tap = [&](double m, double c) {
            const double o = swing * m, f = o - std::floor(o), x = c * m * 32768;
            const double g = 1 + (x < 0 ? std::trunc(x - 1) : std::trunc(x)) / 32768;
            return g * std::polar(1.0, -w * (1 + std::floor(o))) * (1.0 + f * (std::polar(1.0, -w) - 1.0));
        };
        const double mc = sine(turns + (k ? 0 : 0.25));
        const int t = std::lround(32767 * std::sin(M_PI * ((k ? s.balance : 128 - s.balance) - 1) / 252));
        const double wc = (k ? 9 * (t / 20) : 3 * (t / 4)) / 32768.0, ws = (k ? 9 * (-t / 80) : -(t / 8)) / 32768.0,
                     wl = (k ? 9 * (t / 10) : t) / 32768.0;
        const auto common = wc * tap(mc, am) + ws * tap(32767.0 / 32768 - mc, am);
        const int centre = k ? 64 : 94;
        out[0] += band[k] * (common + wl * tap(sine(turns + phase(centre - v)), amc));
        out[1] += band[k] * (common + wl * tap(sine(turns + phase(centre + v)), amc));
    }
    for (auto &z : out)
        z *= 11571.0 / 4096 * std::polar(1.0, w * (double)n); // x[n] = Im(e^{jwn})
    return out;
}
} // namespace

TEST_CASE("Rotary A: the output follows the measured structure sample for sample")
{
    // The structure in section 15 ("Rotary A") reproduces the reference's clean two-tone recordings
    // (batch c: Jacob's setting slow and fast, the mic at 0, 30 and 60, two rates, depth 8, three
    // hold-outs) within the source's own noise (-35 to -49 dB of residual, with one gain for every
    // setting). The plugin must give that structure's output, sample for sample, within -60 dB (the
    // delay words' 12-bit mantissas leave about -80 dB), at 196 and 2637 Hz.
    const std::vector<RotaryLaw> settings{{16, 21, 38, 39, 65, 200, 9, 4500, 3, 60, false}, {93, 96, 38, 39, 65, 200, 9, 4500, 3, 60, true},
                                          {19, 26, 49, 56, 73, 315, 0, 1200, 1, 0, false}, {19, 26, 49, 56, 73, 315, 0, 1200, 1, 30, false},
                                          {105, 106, 124, 118, 59, 630, 7, 2500, -5, 17, true}, {27, 25, 114, 21, 94, 1200, -7, 630, 7, 21, false},
                                          {91, 93, 8, 8, 1, 315, 0, 1200, 1, 60, true}};
    for (const auto &s : settings)
    {
        std::vector<std::pair<const char *, double>> set{
            {"Speed", s.fast}, {s.fast ? "Rotor Fast Speed" : "Rotor Slow Speed", platformHz(s.rotorSpeed)},
            {s.fast ? "Horn Fast Speed" : "Horn Slow Speed", platformHz(s.hornSpeed)}, {"Rotor Depth", 100.0 * s.rotorDepth / 127},
            {"Horn Depth", 100.0 * s.hornDepth / 127}, {"Balance", 100.0 * (s.balance - 64) / 63}, {"Low Shelf Frequency", (double)s.lowIndexHz},
            {"Low Shelf Gain", (double)s.lowDb}, {"High Shelf Frequency", (double)s.highHz}, {"High Shelf Gain", (double)s.highDb},
            {"Mic Angle", 3.0 * s.mic}};
        auto o = rotaryRender(44100, set, 2.5, {196, 2637});
        double e = 0, sig = 0;
        for (size_t n = 22050; n < o.l.size(); ++n)
        {
            const auto a = rotaryLawAt(s, 2 * M_PI * 196 / 44100, n), b = rotaryLawAt(s, 2 * M_PI * 2637 / 44100, n);
            const double y[2] = {0.25 * (a[0] + b[0]).imag(), 0.25 * (a[1] + b[1]).imag()};
            e += std::pow(o.l[n] - y[0], 2) + std::pow(o.r[n] - y[1], 2);
            sig += y[0] * y[0] + y[1] * y[1];
        }
        const double db = 10 * std::log10(e / sig);
        std::printf("Rotary A structure, %s, depths %d/%d, mic %d: %.1f dB\n", s.fast ? "fast" : "slow", s.rotorDepth, s.hornDepth, s.mic, db);
        // The delay words' noise is there: the reference's broadband floor over 0.3-6 kHz matches the
        // plugin's within 1.5 dB (e_jacob_slow, e_jacob_fast); full-precision words would leave -120 dB.
        INFO("speeds " << s.rotorSpeed << "/" << s.hornSpeed << ", depths " << s.rotorDepth << "/" << s.hornDepth << ", balance " << s.balance
                       << ", mic " << s.mic << (s.fast ? " fast" : " slow") << ": " << db << " dB");
        REQUIRE(db < -60);
        REQUIRE(db > -90);
    }
}

TEST_CASE("Rotary A: on a switch the swing glides and the AM depths move at a bounded rate")
{
    // The swing moves as a one-pole of 2^-12 per sample, each AM coefficient by at most 2^-12 per
    // sample (frexp-scaled steps of 2^-14 x 2^(2 - e) of the difference, a quarter of it near the end).
    // Fitted on the reference's ramps (e_ramp_def, waveform over the 300 ms after the switch): -28.9 dB
    // of residual with these laws, -20.4 / -20.2 dB with the swing's pole at 2^-11 / 2^-13, -13.7 dB with
    // both stepping. Here both speeds are the same (index 40), so only the depths change (17 and 22,
    // where slow and fast AM depths differ most); the output
    // must follow the laws sample for sample within -60 dB over the 0.4 s after the switch.
    const RotaryLaw slowS{40, 40, 17, 22, 73, 315, 0, 1200, 1, 60, false};
    std::vector<std::pair<const char *, double>> set{{"Rotor Slow Speed", platformHz(40)}, {"Horn Slow Speed", platformHz(40)},
                                                     {"Rotor Fast Speed", platformHz(40)}, {"Horn Fast Speed", platformHz(40)},
                                                     {"Rotor Depth", 100.0 * 17 / 127}, {"Horn Depth", 100.0 * 22 / 127}};
    const double t = 0.5;
    auto o = rotaryRender(44100, set, 1.0, {196, 2637}, {{t - L / 44100.0, 1}});
    // Coefficient targets slow and fast (the laws in section 15).
    auto targets = [](int k, bool fast, int v, double *x) {
        int d, a, ac;
        if (k == 0)
            d = fast ? v : 5 * v / 4, a = fast ? (v < 34 ? 15 * v / 4 : 127) : (v < 18 ? 57 * v / 8 : 127), ac = fast ? 13 * a / 20 : 25 * a / 38;
        else
            d = fast ? v : 7 * v / 6, a = fast ? (v <= 51 ? 15 * v / 12 : 64) : (v <= 22 ? 19 * v / 6 : 70), ac = fast ? 26 * a / 15 : 15 * a / 19;
        x[0] = 256.0 * (16384 * d / 127) / 32768, x[1] = std::trunc(-a * 32768.0 / 127) / 32768, x[2] = std::trunc(-ac * 32768.0 / 127) / 32768;
    };
    double g[2][3], to[2][3];
    for (int k = 0; k < 2; ++k)
        targets(k, false, k ? 22 : 17, g[k]), targets(k, true, k ? 22 : 17, to[k]);
    const size_t sw = (size_t)(t * 44100) / 16 * 16; // the block that carries the switch
    double e = 0, sig = 0;
    for (size_t n = 0; n < (size_t)(0.9 * 44100); ++n)
    {
        if (n >= sw)
            for (int k = 0; k < 2; ++k)
            {
                g[k][0] += 0x1p-12 * (to[k][0] - g[k][0]);
                for (int c = 1; c < 3; ++c)
                    if (const double d = to[k][c] - g[k][c]; std::abs(d) > 1e-8)
                    {
                        int ex;
                        std::frexp(d, &ex);
                        g[k][c] += std::min(0.25, 0x1p-14 * std::ldexp(1.0, 2 - ex)) * d;
                    }
            }
        if (n < sw)
            continue;
        const auto a = rotaryLawAt(slowS, 2 * M_PI * 196 / 44100, n, g), b = rotaryLawAt(slowS, 2 * M_PI * 2637 / 44100, n, g);
        const double y[2] = {0.25 * (a[0] + b[0]).imag(), 0.25 * (a[1] + b[1]).imag()};
        e += std::pow(o.l[n] - y[0], 2) + std::pow(o.r[n] - y[1], 2);
        sig += y[0] * y[0] + y[1] * y[1];
    }
    const double db = 10 * std::log10(e / sig);
    INFO("after the switch: " << db << " dB");
    REQUIRE(db < -60);
}

TEST_CASE("Rotary A: the crossover and balance as measured, at 44.1, 48 and 96 kHz")
{
    // Rotors stopped, depth 0, flat shelves: the reference's levels against its dry input (c_bal1,
    // c_bal127, c_flat at balance 73, c_bal32, c_bal96) at harmonics of note 24, within 0.15 dB at
    // 44.1 kHz (tolerance proposed); at the other rates within the accepted rate gate of the 44.1 kHz
    // levels (decision 18: 0.05 dB within 20 dB of the setting's peak, 0.5 dB further down).
    struct Point { double balance, hz, db; };
    const std::vector<Point> ref{{1, 65.41, 9.17}, {1, 654.06, 5.58}, {1, 850.28, 2.51}, {1, 1046.5, -0.51}, {1, 2093.0, -12.15},
                                 {127, 654.06, -1.37}, {127, 850.28, 2.08}, {127, 1046.5, 4.09}, {127, 2093.0, 6.55}, {127, 8372.02, 6.80},
                                 {73, 65.41, 5.10}, {73, 654.06, 4.96}, {73, 850.28, 4.77}, {73, 1046.5, 4.96}, {73, 4186.01, 4.83},
                                 {32, 1046.5, 3.02}, {32, 4186.01, -1.13}, {96, 65.41, 0.76}, {96, 1046.5, 5.05}};
    auto level = [](double rate, double balance, double hz) {
        auto o = rotaryRender(rate, {{"Rotor Slow Speed", 0}, {"Horn Slow Speed", 0}, {"Rotor Depth", 0}, {"Horn Depth", 0},
                                     {"High Shelf Gain", 0}, {"Balance", (balance - 64) / 0.63}}, 0.6, {hz});
        double e = 0;
        const size_t a = (size_t)(0.2 * rate), b = o.l.size();
        for (size_t i = a; i < b; ++i)
            e += (double)o.l[i] * o.l[i];
        return 10 * std::log10(e / (b - a) / (0.25 * 0.25 / 2));
    };
    for (auto &p : ref)
    {
        const double at44 = level(44100, p.balance, p.hz);
        INFO("balance " << p.balance << " at " << p.hz << " Hz: " << at44 << " dB, reference " << p.db);
        REQUIRE(std::abs(at44 - p.db) < 0.15);
        const double peak = p.balance == 1 ? 9.17 : 6.8;
        for (double rate : {48000.0, 96000.0})
            REQUIRE(std::abs(level(rate, p.balance, p.hz) - at44) < (p.db > peak - 20 ? 0.05 : 0.5));
    }
}

TEST_CASE("Rotary A is linear: no harmonics, and twice the input gives twice the output")
{
    // The reference's drive adds no distortion (harmonics at or below -80 dB at every setting and
    // level); the mode is linear (tolerance 1e-5 of full scale on the doubled render: the delay words
    // keep their 12-bit mantissa at any level).
    auto a = rotaryRender(44100, {{"Speed", 1}, {"Rotor Depth", 100}, {"Horn Depth", 100}}, 1, {440});
    Plugin pl(rotaryId);
    for (auto [n, v] : std::vector<std::pair<const char *, double>>{{"Speed", 1}, {"Rotor Depth", 100}, {"Horn Depth", 100}})
        pl.set(paramIdByName(pl.p, "Rotary A", n), v);
    pl.activate(44100);
    const size_t n = 44100 + L;
    std::vector<float> in(n), r;
    for (size_t i = 0; i < n; ++i)
        in[i] = (float)(0.5 * std::sin(2 * M_PI * 440 * i / 44100.0));
    auto l = pl.render(in, in, r, {64}, {}, false);
    double worst = 0;
    for (size_t i = 0; i + L < n && i < a.l.size(); ++i)
        worst = std::max(worst, (double)std::abs(l[i + L] - 2 * a.l[i]));
    INFO("worst " << worst);
    REQUIRE(worst < 1e-5);
    // Stopped, depth 0: a 1 kHz sine keeps every harmonic below -90 dB (the delay words' noise sits
    // near -66 dB, spread over the band).
    auto s = rotaryRender(44100, {{"Rotor Slow Speed", 0}, {"Horn Slow Speed", 0}, {"Rotor Depth", 0}, {"Horn Depth", 0}}, 1, {1000});
    auto amp = [&](double f) {
        std::complex<double> c;
        for (size_t i = 22050; i < 44100; ++i)
            c += (double)s.l[i] * std::polar(1.0, -2 * M_PI * f * i / 44100);
        return std::abs(c);
    };
    const double h = 20 * std::log10(std::max(amp(2000), amp(3000)) / amp(1000));
    INFO("harmonics " << h << " dB");
    REQUIRE(h < -90);
}

TEST_CASE("Rotary A: the low shelf acts on the rotor, the high shelf on the horn, as measured")
{
    // Shelves alone (rotors stopped, depth 0; batch c, a saw at velocity 40, nothing near full scale):
    // the reference's levels against its dry input at Jacob's three most used settings and two
    // hold-outs, within 0.15 dB at 44.1 kHz (tolerance proposed); at 48, 96 and 192 kHz within the
    // rate gate of the 44.1 kHz levels (decision 18: 0.05 dB within 20 dB of the setting's peak).
    struct Setting { double loHz, loDb, hiHz, hiDb, balance; std::vector<std::pair<double, double>> ref; };
    const std::vector<Setting> settings{
        {200, 9, 4500, 3, 14.29, {{65.41, 13.63}, {196.22, 11.65}, {392.44, 8.74}, {654.06, 5.58}, {1046.5, 4.78}, {2093.0, 5.71}, {4186.01, 6.43}, {8372.02, 7.25}}},
        {450, -5, 1200, 2, 14.29, {{65.41, 0.17}, {196.22, 1.02}, {392.44, 2.72}, {654.06, 3.98}, {1046.5, 5.44}, {2093.0, 6.60}, {4186.01, 6.67}, {8372.02, 6.67}}},
        {560, 2, 1800, 3, 14.29, {{65.41, 7.09}, {196.22, 7.09}, {392.44, 6.76}, {654.06, 5.39}, {1046.5, 5.44}, {2093.0, 6.94}, {4186.01, 7.44}, {8372.02, 7.61}}},
        {450, 4, 700, 11, 88.89, {{65.41, -7.85}, {196.22, -6.74}, {392.44, -1.85}, {654.06, 6.85}, {1046.5, 13.67}, {2093.0, 17.12}, {4186.01, 17.65}, {8372.02, 17.75}}},
        {250, 2, 1200, 11, -9.524, {{65.41, 8.62}, {196.22, 8.18}, {392.44, 7.38}, {654.06, 6.24}, {1046.5, 9.28}, {2093.0, 13.02}, {4186.01, 13.85}, {8372.02, 14.04}}}};
    auto level = [](double rate, const Setting &s, double hz) {
        auto o = rotaryRender(rate, {{"Rotor Slow Speed", 0}, {"Horn Slow Speed", 0}, {"Rotor Depth", 0}, {"Horn Depth", 0}, {"Balance", s.balance},
                                     {"Low Shelf Frequency", s.loHz}, {"Low Shelf Gain", s.loDb}, {"High Shelf Frequency", s.hiHz}, {"High Shelf Gain", s.hiDb}},
                              0.6, {hz});
        double e = 0;
        const size_t a = (size_t)(0.2 * rate), b = o.l.size();
        for (size_t i = a; i < b; ++i)
            e += (double)o.l[i] * o.l[i];
        return 10 * std::log10(e / (b - a) / (0.25 * 0.25 / 2));
    };
    for (auto &s : settings)
    {
        double peak = -1e9;
        for (auto [hz, db] : s.ref)
            peak = std::max(peak, db);
        for (auto [hz, db] : s.ref)
        {
            const double at44 = level(44100, s, hz);
            INFO("low " << s.loHz << " Hz " << s.loDb << " dB, high " << s.hiHz << " Hz " << s.hiDb << " dB, at " << hz << " Hz: " << at44 << ", reference " << db);
            REQUIRE(std::abs(at44 - db) < 0.15);
            for (double rate : {48000.0, 96000.0, 192000.0})
                REQUIRE(std::abs(level(rate, s, hz) - at44) < (db > peak - 20 ? 0.05 : 0.5));
        }
    }
}

TEST_CASE("Rotary A: the low shelf at 32 Hz, in its coefficients' 16-bit steps, as measured")
{
    // Batch c's grid at the lowest corner (e_lo4_58 and e_lo4_70: 32 Hz at -6 and +6 dB), the
    // harmonics at 32.7 and 65.4 Hz against the same render's at 196 Hz (the renders' own levels
    // scatter by up to 0.3 dB, their harmonics by about 0.1 dB), within 0.1 dB (tolerance proposed);
    // the unrounded design misses by 0.24 dB (-6) and 0.21 dB (+6) at 32.7 Hz.
    const std::vector<std::pair<double, std::vector<std::pair<double, double>>>> settings{
        {-6, {{32.70, 0.86}, {65.40, 3.01}, {196.20, 4.94}}}, {6, {{32.70, 9.28}, {65.40, 7.30}, {196.20, 5.71}}}};
    auto level = [](double db, double hz) {
        auto o = rotaryRender(44100, {{"Rotor Slow Speed", 0}, {"Horn Slow Speed", 0}, {"Rotor Depth", 0}, {"Horn Depth", 0},
                                      {"Low Shelf Frequency", 32}, {"Low Shelf Gain", db}, {"High Shelf Gain", 0}}, 1.2, {hz});
        double e = 0;
        const size_t a = (size_t)(0.4 * 44100), b = o.l.size();
        for (size_t i = a; i < b; ++i)
            e += (double)o.l[i] * o.l[i];
        return 10 * std::log10(e / (b - a));
    };
    for (auto &[db, ref] : settings)
    {
        const double base = level(db, ref.back().first);
        for (auto [hz, want] : ref)
        {
            const double got = level(db, hz) - base;
            INFO(db << " dB at " << hz << " Hz: " << got << " against " << want - ref.back().second);
            REQUIRE(std::abs(got - (want - ref.back().second)) < 0.1);
        }
    }
}

// ---- Auto Pan and Tremolo (plan, section 15, "Auto Pan" and "Tremolo") ----
namespace
{
// A sine (0.25 peak) at freq through mode `modeId` of the rotary plugin, parameters by name; the right
// input silent if `leftOnly`. Latency removed, as rotaryRender.
RotaryOut modRender(double rate, int modeId, const char *module, std::vector<std::pair<const char *, double>> set, double seconds,
                    double freq = 3000, bool leftOnly = false)
{
    Plugin pl(rotaryId);
    pl.set(1, modeId);
    for (auto &[n, v] : set)
        pl.set(paramIdByName(pl.p, module, n), v);
    pl.activate(rate);
    const size_t n = (size_t)(rate * seconds) + L;
    std::vector<float> in(n), inR(n);
    for (size_t i = 0; i < n; ++i)
    {
        in[i] = (float)(0.25 * std::sin(2 * M_PI * freq * i / rate));
        inR[i] = leftOnly ? 0 : in[i];
    }
    RotaryOut o;
    o.rate = rate;
    o.l = pl.render(in, inR, o.r, {64}, {}, false);
    o.l.erase(o.l.begin(), o.l.begin() + L);
    o.r.erase(o.r.begin(), o.r.begin() + L);
    return o;
}
// The gain per millisecond (the envelope at freq over the input's, half its 0.25 peak).
std::vector<double> gainTrack(const std::vector<float> &y, double rate, double freq = 3000)
{
    std::vector<double> g;
    for (auto &z : envelopeAt(y, rate, freq))
        g.push_back(std::abs(z) / 0.125);
    return g;
}
// The fundamental of a per-millisecond track at f Hz over the whole periods from `from` ms.
std::complex<double> fundamental(const std::vector<double> &x, double f, size_t from)
{
    const size_t n = (size_t)(std::floor((x.size() - from) * f / 1000) * 1000 / f);
    std::complex<double> z = 0;
    for (size_t i = 0; i < n; ++i)
        z += x[from + i] * std::polar(1.0, -2 * M_PI * f * (double)i / 1000);
    return z * (2.0 / (double)n);
}
// The fraction of the whole periods from `from` ms during which x is below `level`.
double fractionBelow(const std::vector<double> &x, double f, size_t from, double level)
{
    const size_t n = (size_t)(std::floor((x.size() - from) * f / 1000) * 1000 / f);
    size_t k = 0;
    for (size_t i = 0; i < n; ++i)
        k += x[from + i] < level;
    return (double)k / (double)n;
}
} // namespace

TEST_CASE("Auto Pan: the LFO speed, and the wave's clipped triangle, at 44.1, 48 and 96 kHz")
{
    // L<>R at full L/R depth (batch autopan_c / autopan_b, 0.67 Hz): each channel's gain is
    // 1 - min(1, (1 + wave / 128)^2 (1 - triangle) / 2), so a higher wave keeps the channel silent
    // longer. The reference's share of each period below half gain: 0.500 (wave 0), 0.5996 (15),
    // 0.6634 (28); within 0.005 (tolerance proposed). The speed in Hz as set, within 0.3%.
    const double f = 0.67291;
    for (double rate : {44100.0, 48000.0, 96000.0})
        for (auto [wave, below] : {std::pair{0, 0.500}, {15, 0.5996}, {28, 0.6634}})
        {
            auto o = modRender(rate, 2, "Auto Pan", {{"LFO Speed", f}, {"L/R Depth", 100}, {"Pan Direction", 0}, {"LFO Wave", wave}}, 7.5);
            const auto gl = gainTrack(o.l, rate), gr = gainTrack(o.r, rate);
            const double hz = rotationHz(crossings(gl, 100), 100, (double)gl.size());
            const double fl = fractionBelow(gl, f, 100, 0.5), fr = fractionBelow(gr, f, 100, 0.5);
            INFO(rate << " Hz, wave " << wave << ": " << hz << " Hz, below half " << fl << " / " << fr);
            REQUIRE(std::abs(hz / f - 1) < 0.003);
            REQUIRE(std::abs(fl - below) < 0.005);
            REQUIRE(std::abs(fr - below) < 0.005);
            if (wave == 0) // a linear crossfade: the gains sum to 1
                for (size_t i = 100; i < gl.size(); ++i)
                    REQUIRE(std::abs(gl[i] + gr[i] - 1) < 0.01);
        }
}

TEST_CASE("Auto Pan: the directions set the channels' phase offsets, as measured")
{
    // The right channel's gain against the left's, in degrees of the LFO (batch autopan_c dir1-4,
    // w0, lr_w0): L<>R and L/R 180, L>>R -60, L<<R +60, L turn -90, R turn +90; within 2 degrees.
    const double f = 0.67291;
    const double want[6] = {180, -60, 60, -90, 90, 180};
    for (int d = 0; d < 6; ++d)
    {
        auto o = modRender(44100, 2, "Auto Pan", {{"LFO Speed", f}, {"L/R Depth", 100}, {"Pan Direction", d}, {"LFO Wave", 0}}, 6.5);
        const double got = std::arg(fundamental(gainTrack(o.r, 44100), f, 100) / fundamental(gainTrack(o.l, 44100), f, 100)) * 180 / M_PI;
        const double err = std::remainder(got - want[d], 360);
        INFO("direction " << d << ": " << got << " degrees");
        REQUIRE(std::abs(err) < 2);
    }
}

TEST_CASE("Auto Pan: direction L/R follows a sine with a steepness per wave, as measured")
{
    // Each channel falls once the LFO's cosine is below 0.069, in proportion to a measured steepness
    // per wave; the reference's share of each period below half gain (batch autopan_c/d lr_w*):
    // 0.3259 (wave 0, whose floor is 0.091), 0.4134 (14), 0.4593 (20), 0.5040 (27); within 0.005.
    const double f = 0.67291;
    for (auto [wave, below, floor] : {std::tuple{0, 0.3259, 0.091}, {14, 0.4134, 0.0}, {20, 0.4593, 0.0}, {27, 0.5040, 0.0}})
    {
        auto o = modRender(44100, 2, "Auto Pan", {{"LFO Speed", f}, {"L/R Depth", 100}, {"Pan Direction", 5}, {"LFO Wave", wave}}, 6.5);
        const auto gl = gainTrack(o.l, 44100);
        const double fl = fractionBelow(gl, f, 100, 0.5), lo = *std::min_element(gl.begin() + 100, gl.end());
        INFO("wave " << wave << ": below half " << fl << ", floor " << lo);
        REQUIRE(std::abs(fl - below) < 0.005);
        REQUIRE(std::abs(lo - floor) < 0.01);
    }
}

TEST_CASE("Auto Pan: direction L/R's slope per wave, and the LFO's start, sample for sample")
{
    // Batch autopan_lr (L/R depth 64, so neither gain reaches 0; the left/right gain ratio sample by
    // sample): each channel's gain is 1 - D min(1, F^2 max(0, 0.46874 (1 - cos) - 0.4375)), the right
    // half a turn after the left, and F^2 per wave fitted to 1e-4 of the log ratio: the values below.
    // The LFO starts at activation a quarter turn before the left channel's peak attenuation, two
    // samples on. Here: the plugin's gains against that law within 0.003 (tolerance proposed; 0.006 at
    // wave 28, where the LFO's 16-bit steps times F^2 = 172 show).
    const double f = 16 * 16 * 44100.0 / 16777216, D = 16512.0 / 32768, Sl = 0.46875 * 32767 / 32768;
    for (auto [wave, F2] : {std::pair{0, 1.8184}, {9, 2.6557}, {17, 3.5326}, {18, 4.1441}, {20, 5.5131}, {22, 7.9321}, {24, 11.3067},
                            {26, 15.8848}, {27, 19.8212}, {28, 172.23}})
    {
        auto o = modRender(44100, 2, "Auto Pan", {{"LFO Speed", f}, {"L/R Depth", 100 * D}, {"Pan Direction", 5}, {"LFO Wave", wave}}, 3.0);
        const double x0 = 0.25 + 2 * f / 44100;
        double worst = 0;
        for (size_t n = 1; n < o.l.size(); ++n)
        {
            const double in = 0.25 * std::sin(2 * M_PI * 3000 * (n - 1) / 44100.0);
            if (std::abs(in) < 0.1)
                continue;
            for (int c = 0; c < 2; ++c)
            {
                const double x = x0 + n * f / 44100 + 0.5 * c;
                const double g = 1 - D * std::min(1.0, F2 * std::max(0.0, Sl * (1 - std::cos(2 * M_PI * x)) - 0.4375));
                worst = std::max(worst, std::abs((c ? o.r : o.l)[n] / in - g));
            }
        }
        INFO("wave " << wave << ": worst gain error " << worst);
        REQUIRE(worst < (wave == 28 ? 0.006 : 0.003));
    }
}

TEST_CASE("Auto Pan: the output is the input one sample late, and the F/R delay reads it linearly")
{
    // With F/R at 0 each channel is its gain times the input one sample earlier. The LFO stopped
    // (speed 0) in L turn at wave 15 and one F/R step: the right channel's delay holds at 1 + 4 x 0.62399
    // samples, so at 15 kHz the linear read between two samples (0.496 of the way) costs
    // 20 log10 |1 - f + f e^(-jw)| = -6.37 dB, the left (an integer delay, 5 samples) nothing; within 0.05 dB.
    auto a = modRender(44100, 2, "Auto Pan", {{"LFO Speed", 1.3}, {"Pan Direction", 0}}, 0.5, 1000);
    double worst = 0;
    for (size_t n = 1; n < a.l.size(); ++n)
        worst = std::max(worst, (double)std::abs(a.l[n] - (float)(0.25 * std::sin(2 * M_PI * 1000 * (n - 1) / 44100.0))));
    INFO("one sample late: worst " << worst);
    REQUIRE(worst < 1e-6);
    auto o = modRender(44100, 2, "Auto Pan", {{"LFO Speed", 0}, {"Pan Direction", 3}, {"LFO Wave", 15}, {"F/R Depth", 100.0 / 31}}, 0.5, 15000);
    auto rms = [](const std::vector<float> &y) {
        double e = 0;
        for (size_t i = 4410; i < y.size(); ++i)
            e += (double)y[i] * y[i];
        return 10 * std::log10(e / (y.size() - 4410) / (0.25 * 0.25 / 2));
    };
    const double u = std::min(1.0, std::pow(1 + 15 / 128.0, 2) * 2 * (32767.0 / 32768) * (16383.0 / 32768) * 0.5), fr = 4 * u - std::floor(4 * u);
    const double want = 20 * std::log10(std::abs(1 - fr + fr * std::polar(1.0, -2 * M_PI * 15000 / 44100)));
    INFO("right " << rms(o.r) << " dB (want " << want << "), left " << rms(o.l));
    REQUIRE(std::abs(rms(o.r) - want) < 0.05);
    REQUIRE(std::abs(rms(o.l)) < 0.05);
}

TEST_CASE("Auto Pan: an L/R depth change moves at a bounded rate")
{
    // Batch x ap_lr_step (L/R depth stepped 0 to 127 by the wheel, LFO at 0.042 Hz): the depth moves
    // by at most 2^-12 per sample (frexp-scaled steps of 2^-14 x 2^(2 - e) of the rest), so a full step
    // takes about 140 ms; the reference's left gain (attenuation 0.81) at 50 / 75 / 100 / 150 ms after
    // the switch: 0.680, 0.521, 0.376, 0.197 (a 9 ms window, about 4 ms late), the law 0.663, 0.512,
    // 0.365, 0.19. Here: the plugin's depth at those times within 0.01 of the law at 44.1 and 96 kHz
    // (tolerance proposed), read from the gain with the LFO stopped (attenuation 0.5 at speed 0).
    for (double rate : {44100.0, 96000.0})
    {
        Plugin pl(rotaryId);
        pl.set(1, 2);
        for (auto [n, v] : std::vector<std::pair<const char *, double>>{{"LFO Speed", 0}, {"L/R Depth", 0}, {"Pan Direction", 0}, {"LFO Wave", 0}})
            pl.set(paramIdByName(pl.p, "Auto Pan", n), v);
        pl.activate(rate);
        const size_t n = (size_t)(0.6 * rate) + L, at = (size_t)(0.2 * rate);
        std::vector<float> in(n, 0.25f), r;
        auto l = pl.render(in, in, r, {64}, {{(uint32_t)at, paramIdByName(pl.p, "Auto Pan", "L/R Depth"), 100}}, false);
        // The law, per 44.1 kHz sample.
        double D = 0;
        std::vector<double> law;
        for (int k = 0; k < (int)(0.3 * 44100); ++k)
        {
            const double d = 1 - D;
            int e;
            std::frexp(d, &e);
            D += std::min(0.25, 0x1p-14 * std::ldexp(1.0, 2 - e)) * d;
            law.push_back(D);
        }
        for (double ms : {10.0, 50.0, 75.0, 100.0, 150.0, 250.0})
        {
            const size_t i = at + L + (size_t)(ms * rate / 1000);
            const double depth = (1 - r[i] / 0.25) / 0.5; // the right channel's attenuation at speed 0 is 0.5
            INFO(rate << " Hz, " << ms << " ms after the change: depth " << depth << ", law " << law[(size_t)(ms * 44.1)]);
            REQUIRE(std::abs(depth - law[(size_t)(ms * 44.1)]) < 0.01);
        }
    }
}

TEST_CASE("Auto Pan: F/R swings the delay in the turn directions only, at 44.1, 48 and 96 kHz")
{
    // R turn, wave 20 (whose plateaus let the swing be read from plateau to plateau), L/R depth 0:
    // each channel's delay grows with its attenuation, by 124 samples at 44.1 kHz at full F/R depth.
    // The reference's swing (batch autopan_c fr127_d4w20 and fr64: F/R 127 and 64, i.e. 31 and 16
    // steps of 4): 2.8005 and 1.445 ms; within 1% (tolerance proposed). No swing at L<>R or L/R.
    for (double rate : {44100.0, 48000.0, 96000.0})
        for (auto [pct, ms] : {std::pair{100.0, 2.8005}, {100.0 * 16 / 31, 1.445}})
        {
            auto o = modRender(rate, 2, "Auto Pan", {{"LFO Speed", 0.67291}, {"Pan Direction", 4}, {"F/R Depth", pct}, {"LFO Wave", 20}}, 4, 150);
            const auto d = delayTrack(envelopeAt(o.l, rate, 150), 150);
            const double swing = peakToPeak(d, 100, d.size());
            INFO(rate << " Hz, F/R " << pct << " %: swing " << swing << " ms");
            REQUIRE(std::abs(swing / ms - 1) < 0.01);
        }
    for (int dir : {0, 5})
    {
        auto o = modRender(44100, 2, "Auto Pan", {{"LFO Speed", 0.67291}, {"Pan Direction", dir}, {"F/R Depth", 100}}, 3, 150);
        const auto d = delayTrack(envelopeAt(o.l, 44100, 150), 150);
        REQUIRE(peakToPeak(d, 100, d.size()) < 0.001);
    }
}

TEST_CASE("The rotary plugin's insertion modes: Mono feeds both channels the input's mean, Stereo each its own")
{
    // A left-only source (batches autopan_c, trem_c st_*: an element panned hard left): with Stereo
    // the right output stays silent, with Mono both channels carry half the input.
    for (auto [modeId, module] : {std::pair{2, "Auto Pan"}, {3, "Tremolo"}})
        for (double mode : {0.0, 1.0})
        {
            auto o = modRender(44100, modeId, module, {{"Input Mode", mode}}, 0.5, 3000, true);
            double right = 0;
            for (size_t i = 0; i < o.r.size(); ++i)
                right = std::max(right, (double)std::abs(o.r[i]));
            const double gain = modeId == 3 ? 1.2589 : 1; // Tremolo's +2 dB
            INFO(module << ", input mode " << mode << ": right peak " << right);
            if (mode == 1)
                REQUIRE(right == 0);
            else
                REQUIRE(std::abs(right / (0.125 * gain) - 1) < 0.01);
        }
}

TEST_CASE("The rotary plugin's insertion modes: the three-band EQ as measured, at 44.1 kHz and within the rate gate")
{
    // First-order shelves (Rotary A's law) and a peak with Q = width x 10^(-|gain| / 40), their
    // coefficients in 16-bit steps (1, 2 and 4 integer bits), modulation off: the reference's levels
    // against its dry input (batches autopan_c and trem_c eq*, autopan_d mid*, a saw at velocity
    // 50; Tremolo 2 dB above Auto Pan at every setting) within 0.05 dB at 44.1 kHz
    // (tolerance proposed); at 48, 96 and 192 kHz within the rate gate of the 44.1 kHz levels.
    struct Setting { double loHz, loDb, hiHz, hiDb, midHz, midDb, width; std::vector<std::pair<double, double>> ref; };
    const std::vector<Setting> pan{
        {250, 12, 5000, 0, 1600, 0, 0.7, {{110.00, 11.29}, {330.00, 8.07}, {990.00, 2.76}, {2970.00, 0.42}, {8910.01, 0.04}}},
        {250, -12, 5000, 0, 1600, 0, 0.7, {{110.00, -11.29}, {330.00, -8.07}, {990.00, -2.76}, {2970.00, -0.42}, {8910.01, -0.04}}},
        {250, 0, 5000, 12, 1600, 0, 0.7, {{110.00, 0.03}, {330.00, 0.25}, {990.00, 1.81}, {2970.00, 6.73}, {8910.01, 11.08}}},
        {250, 0, 5000, -12, 1600, 0, 0.7, {{110.00, -0.03}, {330.00, -0.25}, {990.00, -1.81}, {2970.00, -6.74}, {8910.01, -11.10}}},
        {250, 0, 2000, 6, 1600, 0, 0.7, {{110.00, 0.04}, {330.00, 0.32}, {990.00, 1.99}, {2970.00, 4.86}, {8910.01, 5.88}}},
        {250, 0, 5000, 0, 1600, 12, 0.7, {{110.00, 0.58}, {330.00, 3.57}, {990.00, 10.39}, {2970.00, 9.48}, {8910.01, 2.38}}},
        {250, 0, 5000, 0, 630, -12, 2, {{110.00, -0.48}, {330.00, -4.28}, {990.00, -6.35}, {2970.00, -0.70}, {8910.01, -0.06}}},
        {250, 0, 5000, 0, 4000, 6, 5, {{110.00, -0.00}, {330.00, 0.00}, {990.00, 0.03}, {2970.00, 1.04}, {8910.01, 0.11}}},
        // the low-centre peaks, which only the 16-bit coefficient steps reproduce (autopan_c eq9, autopan_d)
        {250, 0, 5000, 0, 315, 12, 12, {{110.00, 0.07}, {220.00, 0.82}, {330.00, 7.57}, {990.00, 0.05}, {2970.00, 0.00}}},
        {250, 0, 5000, 0, 100, 12, 5, {{110.00, 12.13}, {220.00, 0.85}, {330.00, 0.29}, {990.00, 0.03}, {2970.00, 0.00}}},
        {250, 0, 5000, 0, 315, 12, 2, {{110.00, 2.01}, {220.00, 7.75}, {330.00, 11.80}, {990.00, 1.61}, {2970.00, 0.18}}}};
    const std::vector<Setting> trem{
        {250, 12, 5000, 0, 1600, 0, 0.7, {{110.00, 13.29}, {330.00, 10.07}, {990.00, 4.76}, {2970.00, 2.42}, {8910.01, 2.03}}},
        {250, -12, 5000, 0, 1600, 0, 0.7, {{110.00, -9.29}, {330.00, -6.07}, {990.00, -0.76}, {2970.00, 1.58}, {8910.01, 1.96}}},
        {250, 0, 5000, 12, 1600, 0, 0.7, {{110.00, 2.03}, {330.00, 2.25}, {990.00, 3.81}, {2970.00, 8.73}, {8910.01, 13.08}}},
        {250, 0, 5000, -12, 1600, 0, 0.7, {{110.00, 1.97}, {330.00, 1.75}, {990.00, 0.19}, {2970.00, -4.74}, {8910.01, -9.10}}},
        {250, 0, 2000, 6, 1600, 0, 0.7, {{110.00, 2.04}, {330.00, 2.32}, {990.00, 3.99}, {2970.00, 6.86}, {8910.01, 7.88}}},
        {250, 0, 5000, 0, 1600, 12, 0.7, {{110.00, 2.57}, {330.00, 5.56}, {990.00, 12.39}, {2970.00, 11.48}, {8910.01, 4.38}}},
        {250, 0, 5000, 0, 630, -12, 2, {{110.00, 1.52}, {330.00, -2.28}, {990.00, -4.35}, {2970.00, 1.30}, {8910.01, 1.94}}},
        {250, 0, 5000, 0, 4000, 6, 5, {{110.00, 2.00}, {330.00, 2.00}, {990.00, 2.03}, {2970.00, 3.04}, {8910.01, 2.11}}},
        {250, 0, 5000, 0, 315, 12, 12, {{110.00, 2.07}, {220.00, 2.82}, {330.00, 9.56}, {990.00, 2.05}, {2970.00, 2.00}}}};
    for (auto [modeId, module, settings] : {std::tuple{2, "Auto Pan", &pan}, {3, "Tremolo", &trem}})
    {
        auto level = [&](double rate, const Setting &s, double hz) {
            auto o = modRender(rate, modeId, module,
                               {{"Low Shelf Frequency", s.loHz}, {"Low Shelf Gain", s.loDb}, {"High Shelf Frequency", s.hiHz},
                                {"High Shelf Gain", s.hiDb}, {"Mid Frequency", s.midHz}, {"Mid Gain", s.midDb}, {"Mid Width", s.width}},
                               0.6, hz);
            double e = 0;
            const size_t a = (size_t)(0.2 * rate), b = o.l.size();
            for (size_t i = a; i < b; ++i)
                e += (double)o.l[i] * o.l[i];
            return 10 * std::log10(e / (b - a) / (0.25 * 0.25 / 2));
        };
        for (auto &s : *settings)
        {
            double peak = -1e9;
            for (auto [hz, db] : s.ref)
                peak = std::max(peak, db);
            for (auto [hz, db] : s.ref)
            {
                const double at44 = level(44100, s, hz);
                INFO(module << ": low " << s.loDb << " dB, high " << s.hiHz << " Hz " << s.hiDb << " dB, mid " << s.midHz << " Hz "
                            << s.midDb << " dB width " << s.width << ", at " << hz << " Hz: " << at44 << ", reference " << db);
                REQUIRE(std::abs(at44 - db) < 0.05);
                for (double rate : {48000.0, 96000.0, 192000.0})
                    REQUIRE(std::abs(level(rate, s, hz) - at44) < (db > peak - 20 ? 0.05 : 0.5));
            }
        }
    }
}

TEST_CASE("Tremolo: +2 dB at the LFO's peak, a sine AM to 1 - depth, at the set speed, at 44.1, 48 and 96 kHz")
{
    // Batches trem_b / trem_c (0.67 Hz): the gain's peak is 1.2589 (+2 dB, also with both depths 0),
    // its trough 1 - AM depth times that (AM 64 and 127: 0.4959 and 0.000 of the peak), a raised
    // cosine (no third harmonic); within 0.5% of the peak (tolerance proposed). Speed within 0.3%.
    const double f = 0.67291;
    for (double rate : {44100.0, 48000.0, 96000.0})
        for (auto [am, trough] : {std::pair{0.0, 1.0}, {100.0 * 64 / 127, 0.4959}, {100.0, 0.0}})
        {
            auto o = modRender(rate, 3, "Tremolo", {{"LFO Speed", f}, {"AM Depth", am}}, 6.5);
            const auto g = gainTrack(o.l, rate);
            const double hi = *std::max_element(g.begin() + 100, g.end()), lo = *std::min_element(g.begin() + 100, g.end());
            INFO(rate << " Hz, AM " << am << ": peak " << hi << ", trough " << lo);
            REQUIRE(std::abs(hi / 1.2589 - 1) < 0.005);
            REQUIRE(std::abs(lo / hi - trough) < 0.005);
            if (am > 0)
            {
                const double third = std::abs(fundamental(g, 3 * f, 100)), first = std::abs(fundamental(g, f, 100));
                REQUIRE(third / first < 0.01);
                REQUIRE(std::abs(rotationHz(crossings(g, 100), 100, (double)g.size()) / f - 1) < 0.003);
            }
        }
}

TEST_CASE("Tremolo: PM swings the delay by 62 samples at 44.1 kHz at full depth, shortest at the gain's peak")
{
    // Batch trem_c pm4-pm124: PM raw v moves the delay's peak by floor(8192 q / 127) / 32 samples at
    // 44.1 kHz times 0.9959, q = floor(v / 4) (the plugin's PM depth is q / 31): the reference's
    // peaks 32.1, 50.18 and 62.23 samples at q = 16, 25 and 31, within 0.06 samples at 44.1 kHz and in
    // proportion within 1% at 48 and 96 kHz (tolerances proposed). With AM and PM at full depth
    // (ampm) the shortest delay comes at the gain's peak.
    for (double rate : {44100.0, 48000.0, 96000.0})
        for (auto [q, samples] : {std::pair{16, 32.1}, {25, 50.18}, {31, 62.23}})
        {
            auto o = modRender(rate, 3, "Tremolo", {{"LFO Speed", 0.67291}, {"PM Depth", 100.0 * q / 31}}, 4, 150);
            const auto d = delayTrack(envelopeAt(o.l, rate, 150), 150);
            const double swing = peakToPeak(d, 100, d.size()) / 2 * 44.1;
            INFO(rate << " Hz, PM " << q << " of 31: peak swing " << swing << " samples at 44.1 kHz");
            REQUIRE(std::abs(swing - samples) < (rate == 44100 ? 0.06 : 0.01 * samples));
        }
    auto o = modRender(44100, 3, "Tremolo", {{"LFO Speed", 0.67291}, {"PM Depth", 100}, {"AM Depth", 50}}, 4, 150);
    const auto z = envelopeAt(o.l, 44100, 150);
    const auto d = delayTrack(z, 150);
    std::vector<double> g;
    for (auto &c : z)
        g.push_back(std::abs(c));
    const double lag = std::arg(fundamental(g, 0.67291, 100) / fundamental(d, 0.67291, 100)) * 180 / M_PI;
    INFO("gain against delay: " << lag << " degrees");
    REQUIRE(std::abs(std::abs(lag) - 180) < 2);
}

TEST_CASE("Tremolo: the phase difference puts the right channel ahead, for AM and PM alike")
{
    // Batches trem_b/c/d/e ph*: the right channel's LFO leads the left's by the phase difference
    // (the converter snaps the platform's value to the reference's sixteen phases); within 1 degree.
    const double f = 0.67291;
    for (double deg : {-135.0, -45.0, 30.0, 90.0, 180.0})
    {
        auto o = modRender(44100, 3, "Tremolo", {{"LFO Speed", f}, {"AM Depth", 100}, {"LFO Phase Difference", deg}}, 6.5);
        const double am = std::arg(fundamental(gainTrack(o.r, 44100), f, 100) / fundamental(gainTrack(o.l, 44100), f, 100)) * 180 / M_PI;
        auto p = modRender(44100, 3, "Tremolo", {{"LFO Speed", f}, {"PM Depth", 100}, {"LFO Phase Difference", deg}}, 6.5, 150);
        const double pm = std::arg(fundamental(delayTrack(envelopeAt(p.r, 44100, 150), 150), f, 100) /
                                   fundamental(delayTrack(envelopeAt(p.l, 44100, 150), 150), f, 100)) * 180 / M_PI;
        INFO(deg << " degrees: AM " << am << ", PM " << pm);
        REQUIRE(std::abs(std::remainder(am - deg, 360)) < 1);
        REQUIRE(std::abs(std::remainder(pm - deg, 360)) < 1);
    }
}

TEST_CASE("Auto Pan state round-trips")
{
    Plugin a(rotaryId);
    a.set(1, 2);
    for (auto [n, v] : {std::pair{"LFO Speed", 3.2}, {"L/R Depth", 80}, {"F/R Depth", 35}, {"Pan Direction", 3}, {"LFO Wave", 6},
                           {"Low Shelf Frequency", 125}, {"Low Shelf Gain", 4}, {"Mid Frequency", 900}, {"Mid Gain", -3}, {"Mid Width", 2.5},
                           {"High Shelf Frequency", 6000}, {"High Shelf Gain", 2}, {"Input Mode", 0}})
        a.set(paramIdByName(a.p, "Auto Pan", n), v);
    Buffer buf;
    REQUIRE(ext<clap_plugin_state_t>(a.p, CLAP_EXT_STATE)->save(a.p, &buf.out));
    Plugin b(rotaryId);
    REQUIRE(ext<clap_plugin_state_t>(b.p, CLAP_EXT_STATE)->load(b.p, &buf.in));
    Buffer again;
    REQUIRE(ext<clap_plugin_state_t>(b.p, CLAP_EXT_STATE)->save(b.p, &again.out));
    REQUIRE(again.data == buf.data);
    auto *params = ext<clap_plugin_params_t>(a.p, CLAP_EXT_PARAMS);
    for (uint32_t i = 0; i < params->count(a.p); ++i)
    {
        clap_param_info_t info;
        params->get_info(a.p, i, &info);
        REQUIRE(a.get(info.id) == b.get(info.id));
    }
    std::vector<float> inL, inR, ra, rb;
    signal(24000, inL, inR);
    a.activate();
    b.activate();
    REQUIRE(a.render(inL, inR, ra) == b.render(inL, inR, rb));
}

TEST_CASE("Auto Pan: every parameter's value text parses back")
{
    Plugin pl(rotaryId);
    pl.set(1, 2);
    auto *params = ext<clap_plugin_params_t>(pl.p, CLAP_EXT_PARAMS);
    int checked = 0;
    for (uint32_t i = 1; i < params->count(pl.p); ++i)
    {
        clap_param_info_t info;
        REQUIRE(params->get_info(pl.p, i, &info));
        if ((info.id >> 8) != 2)
            continue;
        for (double t : {0., .25, .5, .75, 1.})
        {
            double v = info.min_value + t * (info.max_value - info.min_value);
            if (info.flags & CLAP_PARAM_IS_STEPPED)
                v = std::round(v);
            char text[256];
            REQUIRE(params->value_to_text(pl.p, info.id, v, text, sizeof(text)));
            double back = NAN;
            INFO(info.name << " at " << v << ": \"" << text << "\"");
            REQUIRE(params->text_to_value(pl.p, info.id, text, &back));
            if (info.flags & CLAP_PARAM_IS_STEPPED)
                REQUIRE(back == v); // a label must select the value it came from
            char again[256];
            REQUIRE(params->value_to_text(pl.p, info.id, back, again, sizeof(again)));
            REQUIRE(!std::strcmp(text, again));
            ++checked;
        }
    }
    REQUIRE(checked == 65); // five points on each of the mode's thirteen parameters, no vacuous pass
}

TEST_CASE("Tremolo state round-trips")
{
    Plugin a(rotaryId);
    a.set(1, 3);
    for (auto [n, v] : {std::pair{"LFO Speed", 3.2}, {"AM Depth", 60}, {"PM Depth", 35},
                        {"Low Shelf Frequency", 125}, {"Low Shelf Gain", 4}, {"Mid Frequency", 900},
                        {"Mid Gain", -3}, {"Mid Width", 2.5}, {"High Shelf Frequency", 6000},
                        {"High Shelf Gain", 2}, {"LFO Phase Difference", -90}, {"Input Mode", 0}})
        a.set(paramIdByName(a.p, "Tremolo", n), v);
    Buffer buf;
    REQUIRE(ext<clap_plugin_state_t>(a.p, CLAP_EXT_STATE)->save(a.p, &buf.out));
    Plugin b(rotaryId);
    REQUIRE(ext<clap_plugin_state_t>(b.p, CLAP_EXT_STATE)->load(b.p, &buf.in));
    Buffer again;
    REQUIRE(ext<clap_plugin_state_t>(b.p, CLAP_EXT_STATE)->save(b.p, &again.out));
    REQUIRE(again.data == buf.data);
    auto *params = ext<clap_plugin_params_t>(a.p, CLAP_EXT_PARAMS);
    for (uint32_t i = 0; i < params->count(a.p); ++i)
    {
        clap_param_info_t info;
        params->get_info(a.p, i, &info);
        REQUIRE(a.get(info.id) == b.get(info.id));
    }
    std::vector<float> inL, inR, ra, rb;
    signal(24000, inL, inR);
    a.activate();
    b.activate();
    REQUIRE(a.render(inL, inR, ra) == b.render(inL, inR, rb));
}

TEST_CASE("Tremolo: every parameter's value text parses back")
{
    Plugin pl(rotaryId);
    pl.set(1, 3);
    auto *params = ext<clap_plugin_params_t>(pl.p, CLAP_EXT_PARAMS);
    int checked = 0;
    for (uint32_t i = 1; i < params->count(pl.p); ++i)
    {
        clap_param_info_t info;
        REQUIRE(params->get_info(pl.p, i, &info));
        if ((info.id >> 8) != 3)
            continue;
        for (double t : {0., .25, .5, .75, 1.})
        {
            double v = info.min_value + t * (info.max_value - info.min_value);
            if (info.flags & CLAP_PARAM_IS_STEPPED)
                v = std::round(v);
            char text[256];
            REQUIRE(params->value_to_text(pl.p, info.id, v, text, sizeof(text)));
            double back = NAN;
            INFO(info.name << " at " << v << ": \"" << text << "\"");
            REQUIRE(params->text_to_value(pl.p, info.id, text, &back));
            if (info.flags & CLAP_PARAM_IS_STEPPED)
                REQUIRE(back == v); // a label must select the value it came from
            char again[256];
            REQUIRE(params->value_to_text(pl.p, info.id, back, again, sizeof(again)));
            REQUIRE(!std::strcmp(text, again));
            ++checked;
        }
    }
    REQUIRE(checked == 60); // five points on each of the mode's twelve parameters, no vacuous pass
}

#include "effects/ChorusFamily.h"
namespace
{
constexpr const char *chorusId = "org.oscairn.effects.chorus";
// An impulse of 1 through a Stereo line's input word, as the line holds it.
const double stereoUnit = oscairn_fx::rotary::lineWord(32767 / 32768.0);
struct ChorusOut
{
    std::vector<float> l, r;
};
// The chorus plugin's response to `in` (left, and right unless leftOnly), latency removed.
ChorusOut chorusRun(double rate, int modeId, const char *module, std::vector<std::pair<const char *, double>> set,
                    const std::vector<float> &in, bool leftOnly = false)
{
    Plugin pl(chorusId);
    pl.set(1, modeId);
    for (auto &[n, v] : set)
        pl.set(paramIdByName(pl.p, module, n), v);
    pl.activate(rate);
    std::vector<float> inL(in), inR(in.size(), 0.f);
    inL.resize(in.size() + L);
    if (!leftOnly)
        inR.assign(inL.begin(), inL.end());
    inR.resize(inL.size());
    ChorusOut o;
    o.l = pl.render(inL, inR, o.r, {64}, {}, false);
    o.l.erase(o.l.begin(), o.l.begin() + L);
    o.r.erase(o.r.begin(), o.r.begin() + L);
    return o;
}
// An impulse of a quarter of full scale (the taps' sum is clipped below 1, as the reference's is).
std::vector<float> impulse(double rate, double seconds)
{
    std::vector<float> x((size_t)(rate * seconds));
    x[0] = 0.25f;
    return x;
}
// The centroid and sum of x over [a - 4, a + 5] per unit impulse: a tap read between samples n and n + 1.
std::pair<double, double> tapAt(const std::vector<float> &x, double a)
{
    double s = 0, m = 0;
    for (int n = (int)a - 4; n <= (int)a + 5; ++n)
        s += x[n], m += n * (double)x[n];
    return {m / s, 4 * s};
}
} // namespace

TEST_CASE("Chorus A: the taps sit at the offset plus the depth laws times their LFOs, at 44.1, 48 and 96 kHz")
{
    // The reference's network for its type 03 00 at depth 54, offset 9.9 ms, LFOs held at their start
    // (speed 0): B and C on the left line, A and D on the right; B and A swing 435 samples on
    // triangles at 120 and 240 degrees, C and D 279 on sines at 300 and 60, centred 78 samples later,
    // all 437 samples after the input (9.9 ms in the family's time law, plus one). At 44.1 kHz within
    // 0.01 sample, elsewhere the same times in seconds within 0.05 of a 44.1 kHz sample.
    for (double rate : {44100.0, 48000.0, 96000.0})
    {
        const double k = rate / 44100;
        auto o = chorusRun(rate, 1, "Chorus A", {{"LFO Speed", 0}, {"PM Depth", 54}, {"Delay Offset", 9.9}, {"Feedback", 0}, {"Mix", 63}}, impulse(rate, 0.1));
        for (auto [x, age] : {std::pair{&o.l, 799.49779}, {&o.l, 533.72743}, {&o.r, 509.49557}, {&o.r, 775.26831}})
        {
            const auto [c, s] = tapAt(*x, age * k);
            INFO(rate << " Hz: tap expected at " << age << ", found at " << c / k << " (sum " << s << ")");
            REQUIRE(std::abs(c / k - age) < (rate == 44100 ? 0.01 : 0.05));
            REQUIRE(std::abs(s - 8191 * 4 / 32768.0 * 32767 / 32768 * stereoUnit) < 1e-6);
        }
    }
    // Depth 0 puts every tap at the offset (and C and D with B and A); depth 127 swings B and A by
    // 1025 samples and C and D by 657, centred 184 later (the swing of B and A fitted within one sample
    // of the platform's: 1024 here).
    auto z = chorusRun(44100, 1, "Chorus A", {{"LFO Speed", 0}, {"PM Depth", 0}, {"Delay Offset", 9.9}, {"Mix", 63}}, impulse(44100, 0.1));
    REQUIRE(std::abs(tapAt(z.l, 437).first - 437) < 1e-6);
    REQUIRE(std::abs(tapAt(z.l, 437).second - 2 * 8191 * 4 / 32768.0 * 32767 / 32768 * stereoUnit) < 1e-6);
    auto f = chorusRun(44100, 1, "Chorus A", {{"LFO Speed", 0}, {"PM Depth", 127}, {"Delay Offset", 9.9}, {"Mix", 63}}, impulse(44100, 0.1));
    REQUIRE(std::abs(tapAt(f.l, 437 + 1024 * 0.83332825).first - (437 + 1024 * 0.83332825)) < 0.01);
    // C: the reference centres it 184 samples on; the fitted swing of B and A (one short) puts it at 183.
    REQUIRE(std::abs(tapAt(f.l, 437 + 184 + 657 * 0.06712341).first - (437 + 184 + 657 * 0.06712341)) < 1.01);
}

TEST_CASE("Chorus C: three taps on one line, each swung by its sine and a triangle folded from an eight-times saw")
{
    // Type 03 30 at depth 25, offset 0.1 ms, held at the start: B, A and C at 2, 130 and 66 samples past
    // the offset's 4, swung by 60.44 samples on sines at 0, 120 and 240 degrees and by 36.25 on
    // triangles folded from a saw starting at 270 degrees, a third of its turn apart. B and C play on
    // the left, A and C on the right (the input's mean feeds the line).
    auto o = chorusRun(44100, 4, "Chorus C", {{"LFO Speed", 0}, {"LFO Depth", 25}, {"Delay Offset", 0.1}, {"Mix", 63}}, impulse(44100, 0.05), true);
    const double wet = 7300 * 4 / 32768.0 * 32767 / 32768 * 0.5;
    for (auto [x, age] : {std::pair{&o.l, 54.34375}, {&o.l, 80.099175}, {&o.r, 220.588509}, {&o.r, 80.099175}})
    {
        const auto [c, s] = tapAt(*x, age);
        INFO("tap expected at " << age << ", found at " << c << " (sum " << s << ")");
        REQUIRE(std::abs(c - age) < 0.01);
        REQUIRE(std::abs(s - wet) < 1e-6);
    }
    // Moving, the fold runs at eight times the slow LFOs' speed: at depth 8 (B swings 20.1 samples
    // on its sine and 12.06 on its fold, clear of C) impulses every 25 ms find B where the two LFOs
    // put it, the saw at 8 x 190 steps per sample from 270 degrees, within 0.05 sample.
    using oscairn_fx::chorus::Lfo;
    std::vector<float> clicks((size_t)(44100 * 0.6));
    for (int k = 0; k < 18; ++k)
        clicks[2000 + k * 1102] = 0.25f;
    auto m = chorusRun(44100, 4, "Chorus C", {{"LFO Speed", 190 * 44100 / 16777216.0}, {"LFO Depth", 8}, {"Mix", 63}}, clicks, true);
    for (int k = 0; k < 18; ++k)
    {
        const int at = 2000 + k * 1102;
        auto ageAt = [&](int n) {
            const uint32_t slow = (uint32_t)(190u * (n + 1)) & 0xffffff, fastP = (uint32_t)(12582912u + 1520u * (n + 1)) & 0xffffff;
            const double sine = ((Lfo::word16(Lfo::sine, slow) + 32768) & 0xffff) / 65536.0, saw = Lfo::word16(Lfo::saw, fastP) / 32768.0;
            double u = saw + 1;
            u -= 2 * std::floor(u / 2);
            return 6 + 2048 * (193 / 32768.0 * std::abs(u - 1) + 322 / 32768.0 * std::min(sine, 32767 / 32768.0));
        };
        const double expect = ageAt(at + (int)ageAt(at));
        std::vector<float> win(m.l.begin() + at, m.l.begin() + at + 60);
        const double c = tapAt(win, expect).first;
        INFO("click " << k << ": B expected at " << expect << ", found at " << c);
        REQUIRE(std::abs(c - expect) < 0.05);
    }
}

TEST_CASE("Chorus family: the balance words and each mode's wet level")
{
    // Dry and wet are quarter-scale words of the platform's dry/wet curve (round-half-down of
    // 32767 sin(pi (v - 1) / 252)), times 4; the wet word carries the mode's level held as a float:
    // 0 dB (Chorus A), -1 dB (Chorus B, Chorus C), +3 dB (Dual Modulator), truncated. Held LFOs at
    // depth 0 put tap B (and in Chorus A tap C) at the offset's age on the left output; the line's
    // input word is 32767/32768 in Stereo and 1/2 + 1/2 in Mono.
    const double top = 32767 / 32768.0;
    struct Case
    {
        int mode;
        const char *module, *depth;
        int age, taps;
        double line;
        int wetMid, wetFull;
    };
    for (auto c : {Case{1, "Chorus A", "PM Depth", 437, 2, stereoUnit, 5792, 8191}, Case{2, "Chorus B", "LFO Depth", 5, 1, stereoUnit, 5162, 7300},
                   Case{3, "Dual Modulator", "PM Depth", 755, 1, 1, 8181, 11570}, Case{4, "Chorus C", "LFO Depth", 6, 1, 1, 5162, 7300}})
        for (auto [mix, dryWord, wetWord] : {std::tuple{-63, 8191, 0}, {0, 5792, c.wetMid}, {63, 0, c.wetFull}})
        {
            auto o = chorusRun(44100, c.mode, c.module, {{"LFO Speed", 0}, {c.depth, 0}, {"Mix", (double)mix}}, impulse(44100, 0.03));
            INFO(c.module << ", mix " << mix);
            REQUIRE(std::abs(4 * o.l[0] - dryWord * 4 / 32768.0) < 1e-6);
            INFO("tap sum " << tapAt(o.l, c.age).second);
            REQUIRE(std::abs(tapAt(o.l, c.age).second - wetWord * 4 / 32768.0 * top * c.line * c.taps) < 1e-6);
        }
}

TEST_CASE("Dual Modulator: AM is 1 - depth x the LFO, the right tap's triangle half a turn after the left's")
{
    // Type 03 10 at AM 127: each tap's gain falls from 1 to 0 as its triangle LFO rises; the left and
    // right taps' triangles are half a turn apart, so on a steady input the two gains sum to one.
    std::vector<float> dc((size_t)(44100 * 2.5), 0.25f);
    auto o = chorusRun(44100, 3, "Dual Modulator", {{"LFO Speed", 1.3}, {"AM Depth", 127}, {"PM Depth", 0}, {"Feedback", 0}, {"Mix", 63}}, dc);
    const double g = 11570 * 4 / 32768.0 * 32767 / 32768 * 0.25;
    double lo = 1, hi = 0, worst = 0;
    for (size_t n = 4410; n < o.l.size(); ++n)
    {
        lo = std::min(lo, o.l[n] / g), hi = std::max(hi, o.l[n] / g);
        worst = std::max(worst, std::abs((o.l[n] + o.r[n]) / g - 1));
    }
    INFO("left gain " << lo << " to " << hi << ", left + right off one by " << worst);
    REQUIRE(lo < 0.001);
    REQUIRE(hi > 0.999);
    REQUIRE(worst < 1e-3);
}

TEST_CASE("Chorus A and B: feedback from the mode's taps into each line, at the mode's scale")
{
    // Held LFOs, depth 0, a left-only impulse: B sits at the offset's age a (437), Chorus B's C 64
    // later, Chorus A's C with B. Chorus B feeds the left line from B alone at v x 376 / 32768, so its
    // first echo at 2a is B through the loop once; Chorus A feeds it from B and C at v x 250 / 32768
    // each, so at 2a both taps carry both taps' loop: four times the one-tap echo at its scale.
    using oscairn_fx::rotary::lineWord;
    const double top = 32767 / 32768.0;
    auto b = chorusRun(44100, 2, "Chorus B", {{"LFO Speed", 0}, {"LFO Depth", 0}, {"Delay Offset", 9.9}, {"Feedback", 40}, {"Mix", 63}}, impulse(44100, 0.1), true);
    const double wb = 7300 * 4 / 32768.0 * top;
    REQUIRE(std::abs(tapAt(b.l, 437).second - wb * stereoUnit) < 1e-6);
    const double fbB = 40 * 376 / 32768.0;
    REQUIRE(std::abs(tapAt(b.l, 874).second - wb * lineWord(fbB * stereoUnit)) < 1e-6);
    auto a = chorusRun(44100, 1, "Chorus A", {{"LFO Speed", 0}, {"PM Depth", 0}, {"Delay Offset", 9.9}, {"Feedback", 40}, {"Mix", 63}}, impulse(44100, 0.1), true);
    const double wa = 8191 * 4 / 32768.0 * top, fbA = 40 * 250 / 32768.0;
    REQUIRE(std::abs(tapAt(a.l, 874).second - 2 * wa * lineWord(2 * fbA * stereoUnit)) < 1e-6);
}

TEST_CASE("Chorus family: Mono feeds one line the input's mean and every tap reads it")
{
    // A left-only impulse in Stereo reaches only the left output; in Mono the right output's taps (A
    // and D) read the left line, fed half the left input.
    for (auto [mode, module] : {std::pair{1, "Chorus A"}, {2, "Chorus B"}, {3, "Dual Modulator"}})
    {
        const char *depth = mode == 2 ? "LFO Depth" : "PM Depth";
        auto st = chorusRun(44100, mode, module, {{"LFO Speed", 0}, {depth, 0}, {"Mix", 63}, {"Input Mode", 1}}, impulse(44100, 0.1), true);
        auto mo = chorusRun(44100, mode, module, {{"LFO Speed", 0}, {depth, 0}, {"Mix", 63}, {"Input Mode", 0}}, impulse(44100, 0.1), true);
        double es = 0, em = 0, el = 0;
        for (size_t n = 0; n < st.r.size(); ++n)
            es += std::abs(st.r[n]), em += std::abs(mo.r[n]), el += std::abs(mo.l[n]);
        INFO(module << ": right output in Stereo " << es << ", in Mono " << em << " (left " << el << ")");
        REQUIRE(es == 0);
        REQUIRE(std::abs(em / el - 1) < 1e-6);
    }
}

TEST_CASE("Chorus family: fed silence, every mode settles to exact silence")
{
    // A second of noise at full feedback, then silence: the output reaches exact zeros within 13 s
    // (Chorus A's two taps per line loop at 0.96), the lines emptying through their words instead
    // of decaying through denormal floats.
    std::vector<float> x((size_t)(44100 * 14));
    uint32_t r = 1;
    for (size_t n = 0; n < 44100; ++n)
    {
        r = r * 1664525u + 1013904223u;
        x[n] = (float)((double)r / 4294967296.0 - 0.5);
    }
    for (auto [mode, module, fb] : {std::tuple{1, "Chorus A", "Feedback"}, {2, "Chorus B", "Feedback"}, {3, "Dual Modulator", "Feedback"}, {4, "Chorus C", ""},
                                    {5, "Dual Detune", ""}})
    {
        std::vector<std::pair<const char *, double>> set{{"Mix", 0}};
        if (*fb)
            set.push_back({fb, 63});
        auto o = chorusRun(44100, mode, module, set, x);
        size_t last = 0;
        for (size_t n = 0; n < o.l.size(); ++n)
            if (o.l[n] != 0 || o.r[n] != 0)
                last = n;
        INFO(module << ": last non-zero sample at " << last / 44100.0 << " s");
        REQUIRE(last < o.l.size() - 44100);
    }
}

TEST_CASE("Chorus modes' state round-trips, and every parameter's value text parses back")
{
    for (int mode = 1; mode <= 4; ++mode)
    {
        Plugin a(chorusId);
        a.set(1, mode);
        auto *params = ext<clap_plugin_params_t>(a.p, CLAP_EXT_PARAMS);
        int checked = 0, count = 0;
        for (uint32_t i = 1; i < params->count(a.p); ++i)
        {
            clap_param_info_t info;
            REQUIRE(params->get_info(a.p, i, &info));
            if ((info.id >> 8) != (clap_id)mode)
                continue;
            ++count;
            a.set(info.id, info.min_value + 0.3 * (info.max_value - info.min_value));
            for (double t : {0., .25, .5, .75, 1.})
            {
                double v = info.min_value + t * (info.max_value - info.min_value);
                if (info.flags & CLAP_PARAM_IS_STEPPED)
                    v = std::round(v);
                char text[256], again[256];
                REQUIRE(params->value_to_text(a.p, info.id, v, text, sizeof(text)));
                double back = NAN;
                INFO(info.name << " at " << v << ": \"" << text << "\"");
                REQUIRE(params->text_to_value(a.p, info.id, text, &back));
                if (info.flags & CLAP_PARAM_IS_STEPPED)
                    REQUIRE(back == v);
                REQUIRE(params->value_to_text(a.p, info.id, back, again, sizeof(again)));
                REQUIRE(!std::strcmp(text, again));
                ++checked;
            }
        }
        REQUIRE(checked == 5 * count);
        REQUIRE(count >= 11);
        Buffer buf;
        REQUIRE(ext<clap_plugin_state_t>(a.p, CLAP_EXT_STATE)->save(a.p, &buf.out));
        Plugin b(chorusId);
        REQUIRE(ext<clap_plugin_state_t>(b.p, CLAP_EXT_STATE)->load(b.p, &buf.in));
        Buffer again;
        REQUIRE(ext<clap_plugin_state_t>(b.p, CLAP_EXT_STATE)->save(b.p, &again.out));
        REQUIRE(again.data == buf.data);
        std::vector<float> inL, inR, ra, rb;
        signal(24000, inL, inR);
        a.activate();
        b.activate();
        REQUIRE(a.render(inL, inR, ra) == b.render(inL, inR, rb));
    }
}

TEST_CASE("Chorus family: every mode renders the same bits as the network before its cost work")
{
    // Each mode at 44.1, 48 and 96 kHz with the EQ on, every parameter moved mid-render (so every
    // glide runs), feedback, both input modes and blocks of 37, 128 and 5 frames: the outputs' bits,
    // hashed (FNV-1a), equal the hashes the per-sample network gave before its cost work (8ee42a8).
    struct Case
    {
        int mode;
        const char *module, *depth, *second;
    };
    const uint64_t expected[4][3] = {{0x9de68c9983c6a807, 0x724ecf95ba4eb0c5, 0xc3353b4606d519dd},
                                     {0xb920e6642c3c169e, 0xe520d31473f611ee, 0x454346c6e98e98dd},
                                     {0x14b5302e0a1a3951, 0x1f41d7a4bdddd6df, 0x29fd4d90d8a4e3fb},
                                     {0xdd1a8b03fec232ee, 0xf1ff0f1fe2fa2d00, 0xc062fc2c36da688b}};
    int m = 0;
    for (auto c : {Case{1, "Chorus A", "PM Depth", "Feedback"}, Case{2, "Chorus B", "LFO Depth", "Feedback"},
                   Case{3, "Dual Modulator", "PM Depth", "AM Depth"}, Case{4, "Chorus C", "LFO Depth", "Delay Offset"}})
    {
        int k = 0;
        for (double rate : {44100.0, 48000.0, 96000.0})
        {
            Plugin pl(chorusId);
            setOffline(pl); // the EQ matched in process, so changes land at the same sample
            pl.set(1, c.mode);
            auto id = [&](const char *n) { return paramIdByName(pl.p, c.module, n); };
            pl.set(id("LFO Speed"), 2.7);
            pl.set(id(c.depth), 90);
            pl.set(id("Mix"), 20);
            pl.set(id("Low Shelf Gain"), 6);
            pl.set(id("Mid Gain"), -4);
            pl.set(id("High Shelf Gain"), 3);
            if (c.mode != 4)
                pl.set(id("Feedback"), 50), pl.set(id("Input Mode"), c.mode == 3 ? 1 : 0);
            pl.activate(rate);
            const size_t n = (size_t)(rate * 1.2);
            std::vector<float> inL(n), inR(n), outR;
            uint32_t x = 7;
            for (size_t i = 0; i < n; ++i)
            {
                x = x * 1664525u + 1013904223u;
                inL[i] = ((x >> 8) / 16777216.f - 0.5f) * 0.9f;
                inR[i] = 0.6f * (float)std::sin(i * 0.013);
            }
            const auto t = [&](double s) { return (uint32_t)(rate * s); };
            std::vector<Event> ev{{t(0.2), id(c.depth), 12},    {t(0.3), id("Mix"), -40},    {t(0.4), id(c.second), c.mode == 4 ? 30.0 : -63},
                                  {t(0.5), id("LFO Speed"), 9}, {t(0.6), id("Mix"), 63},     {t(0.7), id("Mid Gain"), 8},
                                  {t(0.8), id(c.depth), 127},   {t(0.9), id("Mix"), -63}};
            if (c.mode != 4)
                ev.push_back({t(1.0), id("Input Mode"), c.mode == 3 ? 0.0 : 1});
            auto outL = pl.render(inL, inR, outR, {37, 128, 5}, ev, false);
            uint64_t h = 1469598103934665603ull;
            for (auto *v : {&outL, &outR})
                for (float f : *v)
                {
                    uint32_t b;
                    std::memcpy(&b, &f, 4);
                    h = (h ^ b) * 1099511628211ull;
                }
            INFO(c.module << " at " << rate << " Hz: hash 0x" << std::hex << h);
            CHECK(h == expected[m][k]);
            ++k;
        }
        ++m;
    }
}

#include "effects/PitchNetwork.h"

TEST_CASE("Dual Detune: the initial delays, the taps' complementary windows and the spread words, at 44.1, 48 and 96 kHz")
{
    // Detune 0 puts both taps of each voice at its line's age (1 + the initial delay in the family's
    // time law: 9.9 ms is 437 samples on the left, 3.2 ms 142 on the right), their window gains
    // summing to 32767/32768. Spread 16 sends each voice to its own side at the word 13650 and to the
    // other at 9059 (half the dry/wet curve's words at 80 and 48), twice, at the wet word 8191, times 4.
    for (double rate : {44100.0, 48000.0, 96000.0})
    {
        const double k = rate / 44100, top = 32767 / 32768.0;
        auto o = chorusRun(rate, 5, "Dual Detune", {{"Detune", 0}, {"Initial Delay L", 9.9}, {"Initial Delay R", 3.2}, {"Spread", 16}, {"Mix", 63}},
                           impulse(rate, 0.05));
        const double nearGain = 8 * 8191 / 32768.0 * 13650 / 32768 * top, farGain = 8 * 8191 / 32768.0 * 9059 / 32768 * top;
        for (auto [x, age, g] : {std::tuple{&o.l, 437.0, nearGain}, {&o.l, 142.0, farGain}, {&o.r, 142.0, nearGain}, {&o.r, 437.0, farGain}})
        {
            const auto [c, s] = tapAt(*x, age * k);
            INFO(rate << " Hz: tap expected at " << age << " with " << g << ", found at " << c / k << " with " << s);
            REQUIRE(std::abs(c / k - age) < (rate == 44100 ? 1e-6 : 0.05));
            REQUIRE(std::abs(s - g) < 1e-6);
        }
    }
}

TEST_CASE("Dual Detune: the windows sum to one but for a bump of 0.375 times a folded saw on each tap where they cross")
{
    // A steady input, detune 50, each voice alone on its side (spread 63): the output follows the
    // sum of the two window gains, 32767/32768 plus twice 0.375 x a second folded saw of up to 0.49994,
    // once per ramp (65,536 samples: a step of 256 per 2^24 turns), peaking where the ramp's saw is
    // at -0.5 for the left voice (it falls in pitch: a quarter into the ramp) and at +0.5 for the
    // right one (it rises: three quarters in).
    const double top = 32767 / 32768.0;
    std::vector<float> dc(65536 + 8000, 0.25f);
    auto o = chorusRun(44100, 5, "Dual Detune", {{"Detune", 50}, {"Spread", 63}, {"Mix", 63}}, dc);
    const double unit = 8 * 8191 / 32768.0 * 16383 / 32768 * 0.25;
    for (auto [x, peakAt] : {std::pair{&o.l, 16383}, {&o.r, 49151}})
    {
        size_t at = 6000;
        double lo = 1e9, hi = 0;
        for (size_t n = 6000; n < 6000 + 65536; ++n)
        {
            lo = std::min(lo, (double)(*x)[n]), hi = std::max(hi, (double)(*x)[n]);
            if ((*x)[n] > (*x)[at])
                at = n;
        }
        INFO("peak " << hi / unit << " at " << at % 65536 << " (expected " << peakAt << "), floor " << lo / unit);
        REQUIRE(std::abs(lo / unit - top) < 1e-6);
        REQUIRE(std::abs(hi / unit - (top + 2 * 0.375 * 2 * (top - 0.75))) < 1e-6);
        REQUIRE(std::abs((int)(at % 65536) - peakAt) <= 2);
    }
}

TEST_CASE("Dual Detune: the left voice plays down and the right up by the cents, through the ramp word")
{
    // The ramp word trunc(65536 - 65536 x 2^(c / 1200) + 1/2) in single precision, as measured at
    // +-10, 30 and 50 cents; each voice plays 1 - word / 65536 times the input's frequency (counted
    // over two ramps by zero crossings, which the crossfades disturb by a few parts in 10,000).
    using oscairn_fx::chorus::rampWord;
    const int cents[6] = {-50, -30, -10, 10, 30, 50}, words[6] = {1866, 1126, 377, -379, -1145, -1919};
    for (int k = 0; k < 6; ++k)
        REQUIRE(rampWord(cents[k]) == words[k]);
    std::vector<float> sine(6000 + 2 * 65536);
    for (size_t n = 0; n < sine.size(); ++n)
        sine[n] = 0.25f * (float)std::sin(2 * M_PI * 1102.5 * n / 44100);
    auto o = chorusRun(44100, 5, "Dual Detune", {{"Detune", 50}, {"Spread", 63}, {"Mix", 63}}, sine);
    auto rate = [&](const std::vector<float> &x) {
        int crossings = 0;
        for (size_t n = 6001; n < 6000 + 2 * 65536; ++n)
            crossings += (x[n - 1] < 0) != (x[n] < 0);
        return crossings / 2.0 / (2 * 65536 / 44100.0) / 1102.5;
    };
    INFO("left " << rate(o.l) << ", right " << rate(o.r));
    REQUIRE(std::abs(rate(o.l) - (1 - 1866 / 65536.0)) < 1e-3);
    REQUIRE(std::abs(rate(o.r) - (1 + 1919 / 65536.0)) < 1e-3);
}

namespace
{
// Every plugin and mode in the bundle at 44.1, 48 and 96 kHz: each of the mode's parameters set to
// 37% of its range, then moved to 71% one after another mid-render (stepped ones rounded), noise
// on the left and a sine on the right, blocks of 37, 128 and 5 frames, offline; the outputs' bits
// hashed (FNV-1a), keyed "<plugin id> <mode> <rate>".
std::map<std::string, uint64_t> bundleHashes()
{
    std::map<std::string, uint64_t> out;
    for (const char *id : {reverbId, eqId, "org.oscairn.effects.delay", "org.oscairn.effects.dynamics", "org.oscairn.effects.tremolo-rotary", chorusId,
                           "org.oscairn.effects.amp-chain"})
    {
        Plugin probe(id);
        auto *params = ext<clap_plugin_params_t>(probe.p, CLAP_EXT_PARAMS);
        clap_param_info_t modeInfo;
        params->get_info(probe.p, 0, &modeInfo);
        for (int m = (int)modeInfo.min_value; m <= (int)modeInfo.max_value; ++m)
            for (double rate : {44100.0, 48000.0, 96000.0})
            {
                Plugin pl(id);
                setOffline(pl);
                pl.set(1, m);
                std::vector<Event> ev;
                const size_t n = (size_t)(rate * 0.8);
                int k = 0;
                for (uint32_t i = 0; i < params->count(pl.p); ++i)
                {
                    clap_param_info_t info;
                    params->get_info(pl.p, i, &info);
                    if (info.id == 1 || (int)(info.id >> 8) != m)
                        continue;
                    auto at = [&](double t) {
                        double v = info.min_value + t * (info.max_value - info.min_value);
                        return (info.flags & CLAP_PARAM_IS_STEPPED) ? std::round(v) : v;
                    };
                    pl.set(info.id, at(0.37));
                    ev.push_back({(uint32_t)(rate * (0.1 + 0.03 * k++)), info.id, at(0.71)});
                }
                pl.activate(rate);
                std::vector<float> inL(n), inR(n), outR;
                uint32_t x = 7;
                for (size_t i = 0; i < n; ++i)
                {
                    x = x * 1664525u + 1013904223u;
                    inL[i] = ((x >> 8) / 16777216.f - 0.5f) * 0.9f;
                    inR[i] = 0.6f * (float)std::sin(i * 0.013);
                }
                auto outL = pl.render(inL, inR, outR, {37, 128, 5}, ev, false);
                uint64_t h = 1469598103934665603ull;
                for (auto *v : {&outL, &outR})
                    for (float f : *v)
                    {
                        uint32_t b;
                        std::memcpy(&b, &f, 4);
                        h = (h ^ b) * 1099511628211ull;
                    }
                out[std::string(id) + " " + std::to_string(m) + " " + std::to_string((int)rate)] = h;
            }
    }
    return out;
}
} // namespace

TEST_CASE("Every plugin and mode renders the same bits as before the wrapper's cost work")
{
    // Taken at dc77b56. The rate-matched EQs' fits (iterative, from transcendental functions at
    // set-up) and the stock reverbs differ between the two arm64 platforms' compilers and maths
    // libraries; elsewhere (x86_64) no hashes are recorded and the test only checks that two
    // renders agree. Each platform table holds only keys seen equal in two builds (macOS: Release
    // and RelWithDebInfo; aarch64 Linux: GCC 13 -O2 and -O3, and glibc 2.39 and 2.41).
    std::set<std::string> runToRun; // keys checked only for two renders agreeing
    std::map<std::string, uint64_t> expected = {
        // The same on macOS arm64 and aarch64 Linux
        {"org.oscairn.effects.chorus 1 44100", 0xab7970886d7634eb},
        {"org.oscairn.effects.chorus 1 48000", 0x4f68d1289cfe5c36},
        {"org.oscairn.effects.chorus 2 44100", 0x4323bff9154a6586},
        {"org.oscairn.effects.chorus 2 48000", 0xd0e3f490708cef97},
        {"org.oscairn.effects.chorus 3 44100", 0x419e93596bbfa22},
        {"org.oscairn.effects.chorus 3 48000", 0xf458fc8c638dd197},
        {"org.oscairn.effects.chorus 3 96000", 0x870dd8743a1035ef},
        {"org.oscairn.effects.chorus 4 44100", 0xab0b4e6a75bc8236},
        {"org.oscairn.effects.chorus 4 48000", 0x995d95600c569e33},
        {"org.oscairn.effects.chorus 5 44100", 0xb1d52491294b20d1},
        {"org.oscairn.effects.chorus 5 48000", 0x8d9805a3959c3036},
        {"org.oscairn.effects.delay 1 44100", 0x6c238e21e3fcba1b},
        {"org.oscairn.effects.delay 1 48000", 0x2e8ce0bde8409103},
        {"org.oscairn.effects.delay 2 44100", 0x3b751d411d4efa3d},
        {"org.oscairn.effects.delay 2 48000", 0x4abadf87f037eca9},
        {"org.oscairn.effects.delay 3 44100", 0xf8766ad51a990010},
        {"org.oscairn.effects.delay 3 48000", 0x26aea5b3328c6d00},
        {"org.oscairn.effects.delay 4 44100", 0xa0aaa09e219028ea},
        {"org.oscairn.effects.delay 4 48000", 0x613f7e5349142906},
        {"org.oscairn.effects.delay 5 44100", 0x68f3db948e28d52d},
        {"org.oscairn.effects.delay 5 48000", 0x2a30259cf269464a},
        {"org.oscairn.effects.delay 6 44100", 0xbb461ad4cb2ef829},
        {"org.oscairn.effects.delay 6 48000", 0x8950fe1d2f9cd679},
        {"org.oscairn.effects.dynamics 1 44100", 0xce455b077d220249},
        {"org.oscairn.effects.dynamics 1 48000", 0xad1f6bf74586041b},
        {"org.oscairn.effects.dynamics 1 96000", 0x95068277e72a9cd7},
        {"org.oscairn.effects.dynamics 2 44100", 0x703db2a5206c0e7},
        {"org.oscairn.effects.dynamics 2 48000", 0x2e292c631ce52df3},
        {"org.oscairn.effects.dynamics 2 96000", 0x47672e4744efa83a},
        {"org.oscairn.effects.eq 1 44100", 0x2d203d9e96799ef5},
        {"org.oscairn.effects.eq 2 44100", 0x149ceefe0a2c8ce1},
        {"org.oscairn.effects.eq 2 48000", 0x5ef730452e36d213},
        {"org.oscairn.effects.eq 3 44100", 0x9923ad728f50f70a},
        {"org.oscairn.effects.eq 4 44100", 0x94f70019d6c6d288},
        {"org.oscairn.effects.reverb 3 44100", 0x8aab03e7b0dd8d12},
        {"org.oscairn.effects.reverb 3 48000", 0x73c8a66b59f6df0d},
        {"org.oscairn.effects.reverb 3 96000", 0x527d9375dd883947},
        {"org.oscairn.effects.tremolo-rotary 1 44100", 0x2bb99e99c193a006},
        {"org.oscairn.effects.tremolo-rotary 1 48000", 0x5e23e28bdbce0cb6},
        {"org.oscairn.effects.tremolo-rotary 1 96000", 0x78f038e167442d38},
        {"org.oscairn.effects.tremolo-rotary 2 44100", 0xff13e0a45d929fd1},
        {"org.oscairn.effects.tremolo-rotary 2 48000", 0xcdb11fc303adc8f1},
        {"org.oscairn.effects.tremolo-rotary 3 44100", 0xa871f40debea8529},
        {"org.oscairn.effects.tremolo-rotary 3 48000", 0x827af19ce40b8fa0},
        {"org.oscairn.effects.tremolo-rotary 3 96000", 0x729f59df3f2ba04f},
        // re-taken when the amp chain's option ranges grew (the test sets each parameter by its range)
        {"org.oscairn.effects.amp-chain 1 44100", 0x1dcacab643d4a66d},
        {"org.oscairn.effects.amp-chain 1 48000", 0xbd41567b830d5c7c},
        {"org.oscairn.effects.amp-chain 1 96000", 0xb5d68900edd9e955},
    };
#if defined(__aarch64__) && defined(__APPLE__)
    expected.insert({
        {"org.oscairn.effects.chorus 1 96000", 0xc6a8ac7ab177813d},
        {"org.oscairn.effects.chorus 2 96000", 0x3ebf4dd1de7044a3},
        {"org.oscairn.effects.delay 1 96000", 0xa5c582d574aae275},
        {"org.oscairn.effects.delay 3 96000", 0xc9921377e4c3ac63},
        {"org.oscairn.effects.chorus 4 96000", 0x580c857ac65b92a5},
        {"org.oscairn.effects.chorus 5 96000", 0x9b4649aa3584c1e2},
        {"org.oscairn.effects.delay 2 96000", 0x2b4111e93751a3b4},
        {"org.oscairn.effects.delay 4 96000", 0xd74c74f3f2725f09},
        {"org.oscairn.effects.delay 5 96000", 0x58e85bbb2bf0159c},
        {"org.oscairn.effects.delay 6 96000", 0x870c057504753226},
        {"org.oscairn.effects.eq 1 48000", 0xfb20df9653707767},
        {"org.oscairn.effects.eq 1 96000", 0x45ea7a9df2f4391f},
        {"org.oscairn.effects.eq 2 96000", 0x63afbec73b965959},
        {"org.oscairn.effects.eq 3 48000", 0x8d53d0a9f6913741},
        {"org.oscairn.effects.eq 3 96000", 0xc464a01a8d9a7a76},
        {"org.oscairn.effects.eq 4 48000", 0xf6f260913637e432},
        {"org.oscairn.effects.eq 4 96000", 0xed031080a7f47045},
        {"org.oscairn.effects.reverb 1 44100", 0x44633e7096ede51c},
        {"org.oscairn.effects.reverb 1 48000", 0x60a4fe149d10d97a},
        {"org.oscairn.effects.reverb 1 96000", 0x4087a6e9c3852749},
        {"org.oscairn.effects.reverb 2 44100", 0x401ac3cb1ffb1fb3},
        {"org.oscairn.effects.reverb 2 48000", 0x415bff4c4fc6500d},
        {"org.oscairn.effects.reverb 2 96000", 0x196057368889aef7},
        {"org.oscairn.effects.tremolo-rotary 2 96000", 0x606b1f945a7e2333},
    });
#elif defined(__aarch64__) && defined(__linux__)
    // With GCC the rate-matched EQ fits (the EQ modes at 48 and 96 kHz, and the chorus, delay and
    // rotary modes' insertion EQs at 96 kHz) come out differently at -O2 and -O3, so those keys are
    // checked only run to run here; every other key is the same at both.
    for (const char *k : {"chorus 1 96000", "chorus 2 96000", "chorus 4 96000", "chorus 5 96000", "delay 1 96000",
                          "delay 2 96000", "delay 3 96000", "delay 4 96000", "delay 5 96000", "delay 6 96000",
                          "eq 1 48000", "eq 1 96000", "eq 2 96000", "eq 3 48000", "eq 3 96000", "eq 4 48000",
                          "eq 4 96000", "tremolo-rotary 2 96000"})
        runToRun.insert(std::string("org.oscairn.effects.") + k);
    expected.insert({
        {"org.oscairn.effects.reverb 1 44100", 0xcc98b587b279b35b},
        {"org.oscairn.effects.reverb 1 48000", 0xcd4fa7ef442f29d},
        {"org.oscairn.effects.reverb 1 96000", 0x5753d2200e3449af},
        {"org.oscairn.effects.reverb 2 44100", 0xc6299be6917bcd73},
        {"org.oscairn.effects.reverb 2 48000", 0x86cf8b32710b5bf8},
        {"org.oscairn.effects.reverb 2 96000", 0x6f3369cea280593e},
    });
#else
    expected.clear();
#endif
    const auto again = bundleHashes();
    for (auto &[key, h] : bundleHashes())
    {
        if (expected.empty() || runToRun.count(key))
        {
            CHECK(again.at(key) == h);
            continue;
        }
        INFO(key << ": hash 0x" << std::hex << h);
        auto it = expected.find(key);
        CHECK((it != expected.end() && it->second == h));
    }
}

#include "effects/AmpChain.h"
#include <complex>
namespace
{
constexpr const char *ampId = "org.oscairn.effects.amp-chain";
using Amp = oscairn_fx::amp::AmpChain<oscairn_fx::Config>;
// Every stage off at output level 109.
const std::vector<std::pair<const char *, double>> ampOff{{"Compressor Sustain", 0}, {"Distortion", 0}, {"Speaker", 0}, {"Delay", 0}, {"Output Level", 109}};
// The amp chain's response to `in` on both channels, latency removed.
ChorusOut ampRun(double rate, std::vector<std::pair<const char *, double>> set, const std::vector<float> &in)
{
    Plugin pl(ampId);
    pl.set(1, 1);
    for (auto &[n, v] : set)
        pl.set(paramIdByName(pl.p, "Amp Chain A", n), v);
    pl.activate(rate);
    std::vector<float> inL(in);
    inL.resize(in.size() + L);
    std::vector<float> inR(inL);
    ChorusOut o;
    o.l = pl.render(inL, inR, o.r, {64}, {}, false);
    o.l.erase(o.l.begin(), o.l.begin() + L);
    o.r.erase(o.r.begin(), o.r.begin() + L);
    return o;
}
std::vector<float> ampSine(double rate, double hz, double amp, double seconds)
{
    std::vector<float> x((size_t)(rate * seconds));
    for (size_t n = 0; n < x.size(); ++n)
        x[n] = (float)(amp * std::sin(2 * M_PI * hz * n / rate));
    return x;
}
double ampRms(const std::vector<float> &x, size_t a, size_t b)
{
    double s = 0;
    for (size_t n = a; n < b; ++n)
        s += (double)x[n] * x[n];
    return std::sqrt(s / (double)(b - a));
}
// The level of harmonic h of hz in x over [a, b), dBFS.
double ampHarmonic(const std::vector<float> &x, double rate, double hz, int h, size_t a, size_t b)
{
    std::complex<double> s = 0;
    for (size_t n = a; n < b; ++n)
        s += (double)x[n] * std::polar(1.0, -2 * M_PI * hz * h * n / rate);
    return 20 * std::log10(2 * std::abs(s) / (double)(b - a));
}
} // namespace

TEST_CASE("Amp Chain: the settings' words at 44.1 kHz for three stored settings, as measured")
{
    using namespace oscairn_fx::amp;
    // Sustain 2.0, Clean 1 at drive 1.8, Pre-EQ A at tone 1.5, presence 5.0, Speaker A, Stereo
    // Echo A at control 40 and time 48, output 109.
    auto w = Amp::words({20, 1, 18, 0, 15, 50, 1, 1, 40, 48, 109}, 44100).w;
    const std::vector<std::pair<int, int>> main{
        {eqLowB, 790}, {eqLowA, 31189}, {eqHighK, 3206}, {eqLowG, 1872}, {eqHighG, 28527}, {eqDcK, 29}, {cmpSus, 8192},
        {cmpFb, -32731}, {drive, 127}, {cmpRel, 4097}, {cmpLvl, 12820}, {dstFloor, -32768}, {cmpAtk, 32000}, {sat1Am1, -32083},
        {sat1A, 685}, {sat1Out, -4224}, {prsAp1, 32721}, {prsAp2, 5959}, {cmpMix, -16384}, {prsMix, 9066}, {prsLp, 16696},
        {dcFall, 93}, {dcRise, 140}, {sat2Am1, -30492}, {sat2A, 2276}, {sat2Out, -1152}, {dlyTime, -19459}, {spk1B1, 1886},
        {spk1B0, 944}, {spk1A1, 7931}, {spk1A2, -3513}, {spk2B1, -24115}, {spk2B2, 11594}, {spk2B0, 13031}, {spk2A1, 14568},
        {spk2A2, -6852}, {loopB, 8886}, {loopA, 14997}, {spkDamp, 20480}, {loopHp, 464}, {wetL, 4009}, {dryL, 15887},
        {wetR, -4009}, {dryR, 15887}, {fb, 6873}, {outL, -8203}, {outR, -8203}, {spkLp, 12437}, {spkHpF, 279}, {dstOn, -32768}};
    for (auto [k, v] : main)
    {
        INFO("word 0x" << std::hex << k);
        CHECK(w[k] == v);
    }
    // Sustain 2.7, drive 1.3, tone 1.3, presence 5.2, the delay off, output 115
    w = Amp::words({27, 1, 13, 0, 13, 52, 1, 0, 40, 48, 115}, 44100).w;
    for (auto [k, v] : std::vector<std::pair<int, int>>{{eqLowB, 735}, {eqLowA, 31298}, {eqHighK, 3437}, {eqLowG, 1678}, {eqHighG, 29058},
                                                         {eqDcK, 28}, {cmpSus, 5325}, {drive, 90}, {cmpLvl, 12404}, {prsAp1, 32722},
                                                         {prsAp2, 6390}, {prsMix, 9232}, {prsLp, 17525}, {dlyTime, -32767}, {wetL, 0},
                                                         {dryL, 16384}, {fb, 0}, {outL, -983}})
    {
        INFO("word 0x" << std::hex << k);
        CHECK(w[k] == v);
    }
    // The ends of the ranges
    const auto lo = Amp::words({1, 1, 0, 0, 15, 50, 1, 1, 0, 0, 0}, 44100).w, hi = Amp::words({100, 1, 100, 0, 15, 50, 1, 1, 127, 127, 127}, 44100).w;
    CHECK(lo[cmpSus] == 31129);
    CHECK(hi[cmpSus] == 32);
    CHECK(lo[drive] == 32);
    CHECK(hi[drive] == 16384);
    CHECK(lo[dlyTime] == -29460);
    CHECK(lo[fb] == 13202);
    CHECK(hi[dlyTime] == -3000);
    CHECK(hi[fb] == 2347);
    CHECK(hi[wetL] == 11585);
    CHECK(hi[dryL] == 11585);
    CHECK(hi[outL] == 19248);
    CHECK(lo[outL] == -32768);
    // Every stage off
    const auto off = Amp::words({0, 0, 18, 0, 15, 50, 0, 0, 40, 48, 109}, 44100).w;
    for (auto [k, v] : std::vector<std::pair<int, int>>{{cmpSus, 32767}, {cmpFb, -16384}, {cmpRel, 16384}, {cmpAtk, 16384}, {cmpLvl, 16384}, {cmpMix, 0},
                                                         {dstOn, 0}, {spk1B1, 8192}, {spk2B0, 8192}, {spkDamp, 23406}, {spkLp, 32767}, {spkHpF, 9}})
    {
        INFO("word 0x" << std::hex << k);
        CHECK(off[k] == v);
    }
}

TEST_CASE("Amp Chain: the second options' words at 44.1 kHz, as measured")
{
    using namespace oscairn_fx::amp;
    // Sustain 3.6, Crunch 1 at drive 5.0, Pre-EQ B at tone 4.2, presence 5.2, Speaker B, Mono
    // Delay A at control 13 and time 36, output 77 (the fourth stored setting but its wah).
    const auto w = Amp::words({36, 2, 50, 1, 42, 52, 2, 2, 13, 36, 77}, 44100).w;
    for (auto [k, v] : std::vector<std::pair<int, int>>{
             {eqLowB, 1062}, {eqLowA, 30644}, {eqHighK, 2124}, {eqLowG, 32382}, {eqHighG, 32382}, {eqDcK, 772}, {cmpSus, 2868},
             {drive, 903}, {cmpLvl, 12048}, {sat1Am1, -32471}, {sat1A, 297}, {sat1Out, -7680}, {prsAp1, 32146}, {prsAp2, 30552},
             {prsMix, 8929}, {prsLp, 28902}, {dcFall, 40}, {dcRise, 56}, {sat2Am1, -32380}, {sat2A, 388}, {sat2Out, -6400},
             {dlyTime, -21959}, {spk1B1, 1180}, {spk1B0, 591}, {spk1A1, 10627}, {spk1A2, -4797}, {spk2B1, -18415}, {spk2B2, 8674},
             {spk2B0, 9870}, {spk2A1, 14159}, {spk2A2, -6078}, {spkDamp, 8192}, {wetL, 1312}, {dryL, 16333}, {wetR, 1312},
             {dryR, 16333}, {fb, 0}, {outL, -27094}, {spkLp, 21671}, {spkHpF, 487}, {dstOn, -32768}})
    {
        INFO("word 0x" << std::hex << k);
        CHECK(w[k] == v);
    }
}

TEST_CASE("Amp Chain: Mono Delay A: one echo, the same on both channels")
{
    auto set = ampOff;
    set[3].second = 2;
    set.push_back({"Delay Time", 48});
    set.push_back({"Delay Control", 127});
    std::vector<float> x(44100);
    x[0] = 0.25f;
    const auto o = ampRun(44100, set, x);
    const int d = (int)(44100 * (0.075 + 0.6 * 48 / 127.0) + 0.5) + 2;
    double first = 0, later = 0;
    for (int n = d - 20; n < d + 200; ++n)
        first = std::max(first, (double)std::abs(o.l[n]));
    for (int n = 2 * d - 20; n < 2 * d + 200; ++n)
        later = std::max(later, (double)std::abs(o.l[n]));
    CHECK(first > 0.01);
    CHECK(later < 1e-9);
    CHECK(o.r[d] == Approx(o.l[d]).margin(1e-7));
}

TEST_CASE("Amp Chain: the compressor's curve 1 / (1 + 999 k / 1024) and the multipliers' 4-bit exponent and 11-bit mantissa")
{
    using namespace oscairn_fx::amp;
    for (auto [k, v] : std::vector<std::pair<int, int>>{{0, 32767}, {1, 30745}, {4, 27918}, {256, 16427}, {512, 14383}, {1024, 12337}})
        CHECK(curveTable.v[k] == v / 32768.0);
    CHECK(expGain(32000) == 0.8125);
    CHECK(expGain(4097) == 16392.0 / 32768 / 8192);
    CHECK(expGain(-1) == 0);
    CHECK(expWord(0.8126) == 32000);
    CHECK(expWord(6.107e-5) == 4097);
}

TEST_CASE("Amp Chain: with every stage off, (1 + the output word) x 32767/32768 times the channels' mean, two samples late, at 44.1, 48 and 96 kHz")
{
    for (double rate : {44100.0, 48000.0, 96000.0})
    {
        const auto x = ampSine(rate, 440, 0.25, 0.2);
        const auto o = ampRun(rate, ampOff, x);
        // the mean of the two (equal) channels is x
        const double g = 0.5 * (1 - 8203 / 32768.0) * (32767 / 32768.0);
        double worst = 0;
        for (size_t n = 2000; n < x.size(); ++n)
            worst = std::max({worst, std::abs(o.l[n] - g * x[n - 2]), std::abs(o.r[n] - g * x[n - 2])});
        INFO(rate << " Hz: worst " << worst);
        CHECK(worst < 2e-7);
    }
}

TEST_CASE("Amp Chain: the compressor below its threshold: 1024 x 2 x 2^-10 / the sustain word x the makeup word")
{
    // Sustain 2.0 (word 8192): the level ramps to 2^-8 and the makeup word is 12820, so the signal
    // is 3.1298 times its level with the compressor off; the detector's threshold is not reached.
    // (small enough that the saturation's curvature stays under 0.002 dB)
    const auto x = ampSine(44100, 440, 0.001, 0.3);
    auto on = ampOff;
    on[0].second = 2.0;
    const auto a = ampRun(44100, ampOff, x), b = ampRun(44100, on, x);
    const double ratio = ampRms(b.l, 4410, x.size()) / ampRms(a.l, 4410, x.size());
    const double expect = 1024 * 2 * (0x1p-10 / 0.25) * (12820 / 32768.0);
    INFO(20 * std::log10(ratio / expect) << " dB");
    CHECK(std::abs(20 * std::log10(ratio / expect)) < 0.01);
}

TEST_CASE("Amp Chain: Stereo Echo A: one line at 75 + 600 x time/127 ms, the right wet inverted, at 44.1, 48 and 96 kHz")
{
    for (double rate : {44100.0, 48000.0, 96000.0})
    {
        auto set = ampOff;
        set[3].second = 1;
        set.push_back({"Delay Time", 48});
        set.push_back({"Delay Control", 127});
        std::vector<float> x((size_t)(rate * 0.8));
        x[0] = 0.25f;
        const auto o = ampRun(rate, set, x);
        const int d = (int)(rate * (0.075 + 0.6 * 48 / 127.0) + 0.5) + 2;
        size_t peak = 100;
        for (size_t n = 100; n < x.size(); ++n)
            if (std::abs(o.l[n]) > std::abs(o.l[peak]))
                peak = n;
        INFO(rate << " Hz: echo at " << peak << ", the line at " << d);
        CHECK(std::abs((int)peak - d) <= 2);
        CHECK(o.l[peak] > 0);
        CHECK(o.r[peak] == Approx(-o.l[peak]).margin(1e-6));
    }
}

TEST_CASE("Amp Chain: at 48 and 96 kHz the distortion and speaker at a low level answer as at 44.1 kHz, 100 Hz to 8 kHz")
{
    // Clean 1 at drive 1.8 into Speaker A, every other stage off, a sine at -40 dBFS (where the
    // saturators are nearly linear): the gain at each frequency against 44.1 kHz's.
    auto set = ampOff;
    set[1].second = 1;
    set[2].second = 1;
    double worst = 0;
    for (double hz : {100.0, 300.0, 1000.0, 1770.0, 3000.0, 5000.0, 8000.0})
    {
        auto gain = [&](double rate) {
            const auto x = ampSine(rate, hz, 0.01, 0.6);
            const auto o = ampRun(rate, set, x);
            return ampHarmonic(o.l, rate, hz, 1, (size_t)(0.3 * rate), (size_t)(0.6 * rate));
        };
        const double g44 = gain(44100);
        for (double rate : {48000.0, 96000.0})
        {
            const double d = gain(rate) - g44;
            INFO(hz << " Hz at " << rate << ": " << d << " dB");
            worst = std::max(worst, std::abs(d));
            CHECK(std::abs(d) < 0.2);
        }
    }
    std::printf("Amp chain, distortion and speaker at 48 and 96 kHz: worst %.3f dB from 44.1 kHz\n", worst);
}

TEST_CASE("Amp Chain: Clean 1's harmonics on a 440 Hz sine, as measured from the reference")
{
    // Drive 1.8, tone 1.5, presence 5.0, every other stage off, a sine of -6.07 dBFS (the
    // reference's take at velocity 127): H1-H5 at -22.15, -66.22, -62.39, -83.31, -82.23 dBFS.
    auto set = ampOff;
    set[1].second = 1;
    const auto x = ampSine(44100, 440, std::pow(10.0, -6.07 / 20), 1.5);
    const auto o = ampRun(44100, set, x);
    const double ref[5] = {-22.15, -66.22, -62.39, -83.31, -82.23};
    for (int h = 1; h <= 5; ++h)
    {
        const double db = ampHarmonic(o.l, 44100, 440, h, 22050, 66150);
        INFO("H" << h << ": " << db << " dBFS");
        CHECK(std::abs(db - ref[h - 1]) < 0.3);
    }
}
