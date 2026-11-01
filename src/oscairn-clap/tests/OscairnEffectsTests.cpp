/*
 * Tests for the Oscairn effects plugins (doc/oscairn-plan.md, section 7).
 * GPL-3.0-or-later, see ../OscairnEffects.cpp.
 */
#define CATCH_CONFIG_MAIN
#include "catch2.hpp"

#include "OscairnConfig.h"
#include "OscairnEffects.h"
#include <oscairn-clap/effect-contexts.h>

#include "sst/basic-blocks/simd/setup.h"
#include "sst/effects/Reverb1.h"
#include "sst/effects/Reverb2.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <set>
#include <string>
#include <thread>
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
static void *allocateOrAbort(std::size_t n, std::size_t align = 0)
{
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
    Host()
    {
        host.clap_version = CLAP_VERSION;
        host.host_data = this;
        host.name = "test";
        host.vendor = "test";
        host.url = "";
        host.version = "0";
        host.get_extension = [](const clap_host_t *, const char *) -> const void * {
            return nullptr;
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
    std::vector<float> render(const std::vector<float> &inL, const std::vector<float> &inR,
                              std::vector<float> &outR, std::vector<uint32_t> blocks = {37},
                              std::vector<Event> events = {}, bool count = true)
    {
        std::vector<float> outL(inL.size());
        outR.assign(inL.size(), 0);
        std::vector<float> bl(4096), br(4096), ol(4096), orr(4096);
        size_t at = 0, b = 0;
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
            countAllocations = count;
            last = p->process(p, &pr);
            countAllocations = false;
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
    REQUIRE(res.context_bytes > sizeof(sst::effects::reverb2::Reverb2<Cfg>));

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
    ctx->resources(pl.p, &res);
    REQUIRE(res.context_bytes ==
            before.context_bytes - sizeof(sst::effects::reverb2::Reverb2<Cfg>) +
                sizeof(sst::effects::reverb1::Reverb1<Cfg>));
    pl.p->on_main_thread(pl.p);
    pl.render(inL, inR, r, {64});
    REQUIRE(pl.host.callbacks == 2); // nothing more to do
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
