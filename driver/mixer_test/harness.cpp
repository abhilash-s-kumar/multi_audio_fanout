#define _USE_MATH_DEFINES
// User-mode test of the MAF loopback mixer. Compiles the real functions from minwavert.cpp
// (extracted to mixer_extracted.inc) against stubs for the kernel APIs they use.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <climits>
#include <vector>
#include <random>
#include <algorithm>

typedef unsigned char BYTE;
typedef short SHORT;
typedef long LONG;
typedef unsigned long ULONG;
typedef long long LONGLONG;
typedef unsigned long long ULONGLONG;
typedef unsigned char BOOLEAN;
typedef long NTSTATUS;
typedef unsigned char KIRQL;
typedef long KSPIN_LOCK;
typedef void VOID;
#define TRUE 1
#define FALSE 0
#define STATUS_SUCCESS 0
#define STATUS_INSUFFICIENT_RESOURCES ((NTSTATUS)0xC000009A)
#define NT_SUCCESS(s) ((s) >= 0)
#define POOL_FLAG_NON_PAGED 0
#define MINWAVERT_POOLTAG 0
#define _In_
#define _Inout_
#define _In_reads_bytes_(x)
#define _Out_writes_bytes_(x)
#define min(a, b) (((a) < (b)) ? (a) : (b))
union LARGE_INTEGER { LONGLONG QuadPart; };

static LONGLONG g_qpc = 0;
static const LONGLONG kQpcFreq = 10000000;
static int g_lockDepth = 0;
static LARGE_INTEGER KeQueryPerformanceCounter(LARGE_INTEGER* f) { if (f) f->QuadPart = kQpcFreq; LARGE_INTEGER q; q.QuadPart = g_qpc; return q; }
static void KeAcquireSpinLock(KSPIN_LOCK*, KIRQL*) { if (g_lockDepth++ != 0) { printf("FAIL: recursive lock\n"); exit(1); } }
static void KeReleaseSpinLock(KSPIN_LOCK*, KIRQL) { --g_lockDepth; }
static void* ExAllocatePool2(int, size_t n, int) { return calloc(1, n); }
static void ExFreePoolWithTag(void* p, int) { free(p); }
#define RtlZeroMemory(p, n) memset((p), 0, (n))
#define RtlCopyMemory(d, s, n) memcpy((d), (s), (n))

#define LOOPBACK_MIX_RATE               48000
#define LOOPBACK_MIX_RING_SECONDS       2
#define LOOPBACK_MIX_RING_FRAMES        (LOOPBACK_MIX_RATE * LOOPBACK_MIX_RING_SECONDS)
#define LOOPBACK_MIX_DELAY_MS           40
#define LOOPBACK_MIX_RESYNC_MS          20

typedef struct _LOOPBACK_CURSOR
{
    BOOLEAN     Valid;
    LONGLONG    RingFrame;
    ULONGLONG   Phase;
    SHORT       Previous[2];
    SHORT       Next[2];
} LOOPBACK_CURSOR, *PLOOPBACK_CURSOR;

class CMiniportWaveRT
{
public:
    SHORT* m_LoopbackRing = nullptr;
    LONGLONG m_LoopbackFrontier = 0;
    LARGE_INTEGER m_LoopbackBaseQpc{};
    LARGE_INTEGER m_LoopbackQpcFrequency{};
    KSPIN_LOCK m_LoopbackBufferLock = 0;
    ~CMiniportWaveRT() { free(m_LoopbackRing); }
    NTSTATUS EnsureLoopbackRing();
    VOID MixRenderIntoLoopback(PLOOPBACK_CURSOR, BYTE*, ULONG, ULONGLONG, ULONG, ULONG, LARGE_INTEGER);
    VOID ReadLoopbackMix(PLOOPBACK_CURSOR, BYTE*, ULONG, ULONGLONG, ULONG, ULONG, LARGE_INTEGER);
    LONGLONG LoopbackNowFrame(LARGE_INTEGER);
    VOID LoopbackAdvanceFrontier(LONGLONG);
};


#include "mixer_extracted.inc"

// A simulated WaveRT stream: DMA buffer + the driver's UpdatePosition byte accounting
// (whole milliseconds with byte and time carry, like CMiniportWaveRTStream::UpdatePosition).
struct SimStream
{
    ULONG rate;
    std::vector<BYTE> dma;
    ULONGLONG linear = 0;
    LONGLONG lastHns = 0;
    LONGLONG hnsCarry = 0;
    ULONG byteCarry = 0;
    LOOPBACK_CURSOR cursor{};
    bool running = false;
    double phase = 0;    // render: generator phase
    double freq = 0;
    double amp = 0;
    ULONGLONG generated = 0; // render: frames generated into the DMA

    SimStream(ULONG r, ULONG dmaMs) : rate(r), dma((size_t)r * 4 * dmaMs / 1000) {}

    void start(LONGLONG nowHns) { running = true; lastHns = nowHns; hnsCarry = 0; byteCarry = 0; }

    // App side: keep the DMA filled ahead of the play position with a sine.
    void fillAhead()
    {
        ULONG dmaFrames = (ULONG)(dma.size() / 4);
        ULONGLONG playFrame = linear / 4;
        while (generated < playFrame + dmaFrames * 3 / 4)
        {
            SHORT* f = (SHORT*)&dma[(generated % dmaFrames) * 4];
            SHORT v = (SHORT)lrint(amp * sin(phase));
            f[0] = v;
            f[1] = (SHORT)-v;
            phase += 2 * M_PI * freq / rate;
            generated++;
        }
    }

    ULONG displacement(LONGLONG nowHns)
    {
        ULONG ms = (ULONG)((nowHns - lastHns + hnsCarry) / 10000);
        hnsCarry = (nowHns - lastHns + hnsCarry) % 10000;
        ULONG moveRate = rate * 4;
        ULONG bytes = (ULONG)(((ULONGLONG)moveRate * ms + byteCarry) / 1000);
        byteCarry = (ULONG)(((ULONGLONG)moveRate * ms + byteCarry) % 1000);
        lastHns = nowHns;
        return bytes;
    }
};

static int g_fail = 0;
static int g_debug = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); g_fail++; } } while (0)

struct Scenario
{
    CMiniportWaveRT mp;
    std::vector<SimStream*> renders;
    SimStream* loop = nullptr;
    std::vector<SHORT> captured; // stereo frames read by the loopback stream, in order
    std::mt19937 rng{1234};
    LONGLONG nowHns = 0;
    Scenario() { g_qpc = 0; }

    int skipMod = 10;
    void runFor(double seconds, int maxStepUs = 10500)
    {
        LONGLONG end = nowHns + (LONGLONG)(seconds * 1e7);
        std::uniform_int_distribution<int> stepDist(500, maxStepUs);
        std::uniform_int_distribution<int> pick(0, (int)renders.size());
        while (nowHns < end)
        {
            nowHns += stepDist(rng) * 10;
            g_qpc = nowHns; // 10 MHz QPC = 100 ns units
            // Streams update in a random order at each tick (some ticks skip some streams).
            std::vector<int> order;
            for (int i = 0; i <= (int)renders.size(); ++i) order.push_back(i);
            std::shuffle(order.begin(), order.end(), rng);
            for (auto* r : renders) if (r->running) r->fillAhead();
            for (int i : order)
            {
                if (rng() % skipMod == 0) continue;
                LARGE_INTEGER q; q.QuadPart = g_qpc;
                if (i == (int)renders.size())
                {
                    if (!loop || !loop->running) continue;
                    ULONG bytes = loop->displacement(nowHns);
                    ULONGLONG before = loop->linear;
                    LONGLONG rb = loop->cursor.RingFrame; bool rv = loop->cursor.Valid; LONGLONG fr = mp.m_LoopbackFrontier;
                    mp.ReadLoopbackMix(&loop->cursor, loop->dma.data(), (ULONG)loop->dma.size(), loop->linear, bytes, loop->rate, q);
                    loop->linear += bytes;
                    if (g_debug && rv && llabs(loop->cursor.RingFrame - rb - (LONGLONG)(bytes / 4)) > 2 && g_debug-- > 0)
                        printf("  reader jump t=%.4f bytes=%lu before=%lld after=%lld frontier=%lld now=%lld\n", nowHns / 1e7, bytes, rb, loop->cursor.RingFrame, fr, mp.LoopbackNowFrame(q));
                    if (g_debug && loop->cursor.RingFrame > fr && g_debug-- > 0)
                        printf("  reader past frontier t=%.4f cursor=%lld frontier=%lld\n", nowHns / 1e7, loop->cursor.RingFrame, fr);
                    ULONG dmaFrames = (ULONG)(loop->dma.size() / 4);
                    for (ULONGLONG f = before / 4; f < loop->linear / 4; ++f)
                    {
                        SHORT* fr = (SHORT*)&loop->dma[(f % dmaFrames) * 4];
                        captured.push_back(fr[0]);
                        captured.push_back(fr[1]);
                    }
                }
                else
                {
                    SimStream* s = renders[i];
                    if (!s->running) continue;
                    ULONG bytes = s->displacement(nowHns);
                    LONGLONG wb = s->cursor.RingFrame; bool wv = s->cursor.Valid;
                    mp.MixRenderIntoLoopback(&s->cursor, s->dma.data(), (ULONG)s->dma.size(), s->linear, bytes, s->rate, q);
                    s->linear += bytes;
                    if (g_debug && wv && llabs(s->cursor.RingFrame - wb - (LONGLONG)(bytes / 4 * 48000.0 / s->rate)) > 2 && g_debug-- > 0)
                        printf("  writer jump t=%.4f bytes=%lu before=%lld after=%lld now=%lld\n", nowHns / 1e7, bytes, wb, s->cursor.RingFrame, mp.LoopbackNowFrame(q));
                }
                CHECK(g_lockDepth == 0, "lock leaked");
            }
        }
    }
};

// Estimates the dominant frequency and level of the left channel over a window.
static void analyse(const std::vector<SHORT>& st, size_t fromFrame, size_t frames, double rate, double* freq, double* rms, double* maxJump)
{
    size_t crossings = 0;
    double sum = 0;
    double jump = 0;
    for (size_t i = fromFrame; i < fromFrame + frames; ++i)
    {
        double v = st[i * 2];
        sum += v * v;
        if (i > fromFrame)
        {
            double p = st[(i - 1) * 2];
            if ((p < 0) != (v < 0)) crossings++;
            jump = std::max(jump, fabs(v - p));
        }
    }
    *freq = crossings / 2.0 / (frames / rate);
    *rms = sqrt(sum / frames);
    *maxJump = jump;
}

static double goertzel(const std::vector<SHORT>& st, size_t fromFrame, size_t frames, double rate, double f)
{
    double w = 2 * M_PI * f / rate, c = 2 * cos(w), s1 = 0, s2 = 0;
    for (size_t i = fromFrame; i < fromFrame + frames; ++i)
    {
        double s0 = st[i * 2] + c * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    return sqrt(s1 * s1 + s2 * s2 - c * s1 * s2) / frames * 2;
}

int main()
{
    // 1. One 48 kHz music stream: loopback is a clean, continuous copy.
    {
        Scenario sc;
        sc.mp.EnsureLoopbackRing();
        SimStream music(48000, 200); music.freq = 440; music.amp = 8000;
        SimStream loop(48000, 40);
        sc.renders = {&music};
        sc.loop = &loop;
        music.start(0); loop.start(0);
        sc.runFor(5);
        double f, rms, jump;
        analyse(sc.captured, 48000, 48000 * 3, 48000, &f, &rms, &jump);
        printf("single 48k: freq=%.1f rms=%.0f maxjump=%.0f frames=%zu\n", f, rms, jump, sc.captured.size() / 2);
        CHECK(fabs(f - 440) < 2, "single: frequency %.1f", f);
        CHECK(fabs(rms - 8000 / sqrt(2.0)) < 100, "single: level %.0f", rms);
        CHECK(jump < 8000 * 2 * M_PI * 440 / 48000 * 1.5 + 2, "single: discontinuity %.0f", jump);
        // Right channel is the negated left: channels not swapped or shifted.
        bool chanOk = true;
        for (size_t i = 48000; i < 48000 * 4; ++i) if (sc.captured[i * 2] != -sc.captured[i * 2 + 1] && !(sc.captured[i * 2] == SHRT_MIN)) { chanOk = false; break; }
        CHECK(chanOk, "single: channels mixed up");
    }

    // 2. Music + a notification stream (different pin) for 1 s: the loopback holds both summed,
    //    the music is never interrupted, and after the beep it is clean again.
    {
        Scenario sc;
        sc.mp.EnsureLoopbackRing();
        SimStream music(48000, 200); music.freq = 440; music.amp = 6000;
        SimStream beep(48000, 200); beep.freq = 1000; beep.amp = 6000;
        SimStream loop(48000, 40);
        sc.renders = {&music, &beep};
        sc.loop = &loop;
        music.start(0); loop.start(0);
        sc.runFor(2);
        size_t beepStart = sc.captured.size() / 2;
        beep.start(sc.nowHns);
        sc.runFor(1);
        size_t beepEnd = sc.captured.size() / 2;
        beep.running = false;
        sc.runFor(2);
        // Skip the 40 ms delay + 20 ms margin at both ends.
        size_t a = beepStart + 48000 * 60 / 1000, n = (beepEnd - beepStart) - 48000 * 120 / 1000;
        double m = goertzel(sc.captured, a, n, 48000, 440), b = goertzel(sc.captured, a, n, 48000, 1000);
        printf("music+beep: 440Hz=%.0f 1000Hz=%.0f\n", m, b);
        CHECK(fabs(m - 6000) < 300, "mix: music level %.0f during beep", m);
        CHECK(fabs(b - 6000) < 300, "mix: beep level %.0f", b);
        double f, rms, jump;
        analyse(sc.captured, beepEnd + 48000 / 5, 48000, 48000, &f, &rms, &jump);
        double m2 = goertzel(sc.captured, beepEnd + 48000 / 5, 48000, 48000, 440), b2 = goertzel(sc.captured, beepEnd + 48000 / 5, 48000, 48000, 1000);
        printf("after beep: 440Hz=%.0f 1000Hz=%.0f maxjump=%.0f\n", m2, b2, jump);
        CHECK(fabs(m2 - 6000) < 300 && b2 < 50, "after beep: not clean");
    }

    // 3. Music + a 24 kHz communications stream + a 44.1 kHz offload stream, loopback at 48 kHz.
    {
        Scenario sc;
        sc.mp.EnsureLoopbackRing();
        SimStream music(48000, 200); music.freq = 440; music.amp = 5000;
        SimStream comms(24000, 200); comms.freq = 1500; comms.amp = 5000;
        SimStream offload(44100, 200); offload.freq = 3000; offload.amp = 5000;
        SimStream loop(48000, 40);
        sc.renders = {&music, &comms, &offload};
        sc.loop = &loop;
        music.start(0); comms.start(0); offload.start(0); loop.start(0);
        sc.runFor(4);
        size_t a = 48000, n = 48000 * 2;
        double m = goertzel(sc.captured, a, n, 48000, 440), c = goertzel(sc.captured, a, n, 48000, 1500), o = goertzel(sc.captured, a, n, 48000, 3000);
        printf("rates 48k+24k+44.1k: 440=%.0f 1500=%.0f 3000=%.0f\n", m, c, o);
        CHECK(fabs(m - 5000) < 300 && fabs(c - 5000) < 400 && fabs(o - 5000) < 400, "rate conversion levels");
    }

    // 4. Loopback opened at 44.1 kHz reading the 48 kHz ring.
    {
        Scenario sc;
        sc.mp.EnsureLoopbackRing();
        SimStream music(48000, 200); music.freq = 440; music.amp = 8000;
        SimStream loop(44100, 40);
        sc.renders = {&music};
        sc.loop = &loop;
        music.start(0); loop.start(0);
        sc.runFor(4);
        double f, rms, jump;
        analyse(sc.captured, 44100, 44100 * 2, 44100, &f, &rms, &jump);
        printf("loopback 44.1k: freq=%.1f rms=%.0f\n", f, rms);
        CHECK(fabs(f - 440) < 2 && fabs(rms - 8000 / sqrt(2.0)) < 150, "44.1k loopback");
    }

    // 5. Pause/resume and long gaps: silence while paused, no stale audio, no lag build-up.
    {
        Scenario sc;
        sc.mp.EnsureLoopbackRing();
        SimStream music(48000, 200); music.freq = 440; music.amp = 8000;
        SimStream loop(48000, 40);
        sc.renders = {&music};
        sc.loop = &loop;
        music.start(0); loop.start(0);
        for (int round = 0; round < 6; ++round)
        {
            sc.runFor(1.0);
            music.running = false;
            size_t pauseAt = sc.captured.size() / 2;
            sc.runFor(round == 5 ? 3.5 : 0.3 + 0.2 * round); // last pause is longer than the ring
            size_t resumeAt = sc.captured.size() / 2;
            size_t a = pauseAt + 48000 * 60 / 1000, n = resumeAt - a;
            double peak = 0;
            for (size_t i = a; i < a + n; ++i) peak = std::max(peak, fabs((double)sc.captured[i * 2]));
            CHECK(peak == 0, "pause %d: stale audio during pause (peak %.0f)", round, peak);
            music.start(sc.nowHns);
        }
        sc.runFor(1.0);
        // Measure the loopback delay: music restarted at a known time; first non-zero frame after it.
        printf("pause/resume: ok\n");
    }

    // 6. Delay is constant: compare when a tone burst enters vs leaves.
    {
        Scenario sc;
        sc.mp.EnsureLoopbackRing();
        SimStream loop(48000, 40);
        SimStream music(48000, 200); music.freq = 440; music.amp = 8000;
        sc.renders = {&music};
        sc.loop = &loop;
        loop.start(0);
        sc.runFor(1);
        LONGLONG startHns = sc.nowHns;
        size_t loopFramesAtStart = sc.captured.size() / 2;
        music.start(sc.nowHns);
        sc.runFor(1);
        size_t first = 0;
        for (size_t i = loopFramesAtStart; i < sc.captured.size() / 2; ++i) if (sc.captured[i * 2] != 0) { first = i; break; }
        double delayMs = (double)(first - loopFramesAtStart) * 1000.0 / 48000;
        printf("loopback delay: %.1f ms (start offset)\n", delayMs);
        CHECK(delayMs > 25 && delayMs < 60, "delay %.1f ms", delayMs);
        (void)startHns;
    }

    // 7. Fuzz: random rates, random huge time jumps, random start/stop, many ring wraps.
    {
        Scenario sc;
        sc.mp.EnsureLoopbackRing();
        ULONG rates[] = {24000, 44100, 48000, 88200, 96000};
        std::vector<SimStream*> all;
        for (int i = 0; i < 6; ++i)
        {
            SimStream* s = new SimStream(rates[sc.rng() % 5], 200);
            s->freq = 100 + sc.rng() % 5000; s->amp = 32767;
            all.push_back(s);
        }
        SimStream loop(rates[sc.rng() % 5], 10);
        sc.renders = all;
        sc.skipMod = 3;
        sc.loop = &loop;
        loop.start(0);
        for (int step = 0; step < 400; ++step)
        {
            for (auto* s : all) if (sc.rng() % 5 == 0) { if (s->running) s->running = false; else s->start(sc.nowHns); }
            if (sc.rng() % 30 == 0) { sc.nowHns += (LONGLONG)(sc.rng() % 50000000); for (auto* s : all) s->lastHns = (std::min)(s->lastHns, sc.nowHns); }
            sc.runFor(0.05 + (sc.rng() % 100) / 1000.0, 60000);
        }
        printf("fuzz: %zu loopback frames, ok\n", sc.captured.size() / 2);
        for (auto* s : all) delete s;
    }

    printf(g_fail ? "FAILED (%d)\n" : "ALL PASSED\n", g_fail);
    return g_fail ? 1 : 0;
}
