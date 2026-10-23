#include <audioclient.h>
#include <avrt.h>
#include <endpointvolume.h>
#include <propkeydef.h>
#include <propsys.h>
#include <functiondiscoverykeys_devpkey.h>
#include <mmreg.h>
#include <mmdeviceapi.h>
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

// Capture side.
constexpr REFERENCE_TIME kCaptureBufferDuration = 2000000; // 200 ms in 100 ns units.
constexpr DWORD kCaptureRetryMs = 500;

// Render side. Each output keeps "ring buffer + device buffer" at a common target latency,
// so every speaker plays a captured sample the same time after it was captured.
constexpr REFERENCE_TIME kRenderBufferDuration = 300000; // 30 ms device buffer.
constexpr double kDefaultLatencyMs = 100.0;
constexpr double kMinLatencyMs = 40.0;
constexpr double kMaxLatencyMs = 1000.0;
constexpr double kMaxDelayOffsetMs = 3000.0;
constexpr double kRingSeconds = 5.0;
constexpr size_t kStagingFrames = 64;

// Drift controller. The measured latency is smoothed, then a PI controller nudges the
// playback rate by at most kMaxCorrection (1000 ppm, inaudible) to hold the target.
constexpr double kFilterSeconds = 1.0;
constexpr double kControlKp = 0.1;     // 10 ms error -> 1000 ppm.
constexpr double kControlKi = 0.0025;  // Learns the steady clock offset; critically damped with Kp.
constexpr double kMaxCorrection = 0.001;
constexpr double kSnapThresholdMs = 25.0; // Bigger errors are fixed at once instead of slowly.
constexpr double kSettleSeconds = 1.5;
constexpr double kIdleSeconds = 0.25;
constexpr double kFadeSeconds = 0.005;
constexpr DWORD kReopenDelayMs = 1000;
constexpr DWORD kStallTimeoutMs = 2000;

std::atomic<bool> g_stop_requested = false;
std::atomic<float> g_master_gain = 1.0f;
std::atomic<double> g_latency_ms = kDefaultLatencyMs;
std::atomic<bool> g_sync_enabled = true;
std::atomic<bool> g_calibration_active = false; // Outputs play calibration sweeps at fixed level.
std::atomic<int> g_calibration_request = 0;     // 1 = calibrate, 2 = mic check only.
std::wstring g_source_id;    // Written once before output threads start.
std::wstring g_mic_selector; // Written once at startup; empty picks the default microphone.

BOOL WINAPI console_handler(DWORD event) {
    if (event == CTRL_C_EVENT || event == CTRL_BREAK_EVENT || event == CTRL_CLOSE_EVENT) {
        g_stop_requested = true;
        return TRUE;
    }
    return FALSE;
}

std::mutex g_log_mutex;

std::string to_utf8(const std::wstring& text) {
    if (text.empty()) {
        return {};
    }
    int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(std::max(0, size)), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size, nullptr, nullptr);
    return out;
}

// One line per call, UTF-8, flushed: the GUI reads this through a pipe, which is otherwise
// block-buffered, and device names can contain non-ASCII characters.
void log_utf8(const std::string& utf8) {
    std::lock_guard<std::mutex> lock(g_log_mutex);
    std::fwrite(utf8.data(), 1, utf8.size(), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

void log_line(const std::wstring& text) {
    log_utf8(to_utf8(text));
}

double qpc_seconds() {
    static const double frequency = [] {
        LARGE_INTEGER value;
        QueryPerformanceFrequency(&value);
        return static_cast<double>(value.QuadPart);
    }();
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    return static_cast<double>(counter.QuadPart) / frequency;
}

std::wstring hr_text(HRESULT hr) {
    std::wostringstream stream;
    stream << L"0x" << std::hex << std::setw(8) << std::setfill(L'0') << static_cast<unsigned long>(hr);
    return stream.str();
}

void sleep_unless_stopped(DWORD milliseconds) {
    for (DWORD waited = 0; waited < milliseconds && !g_stop_requested; waited += 50) {
        Sleep(50);
    }
}

template <typename T>
class ComPtr {
public:
    ComPtr() = default;
    ~ComPtr() { reset(); }
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;
    ComPtr(ComPtr&& other) noexcept : ptr_(other.ptr_) {
        other.ptr_ = nullptr;
    }
    ComPtr& operator=(ComPtr&& other) noexcept {
        if (this != &other) {
            reset();
            ptr_ = other.ptr_;
            other.ptr_ = nullptr;
        }
        return *this;
    }

    T* get() const { return ptr_; }
    T** put() {
        reset();
        return &ptr_;
    }
    T* operator->() const { return ptr_; }
    explicit operator bool() const { return ptr_ != nullptr; }

    void reset() {
        if (ptr_) {
            ptr_->Release();
            ptr_ = nullptr;
        }
    }

private:
    T* ptr_ = nullptr;
};

struct CoTaskMemDeleter {
    void operator()(void* value) const {
        if (value) {
            CoTaskMemFree(value);
        }
    }
};

using WaveFormatPtr = std::unique_ptr<WAVEFORMATEX, CoTaskMemDeleter>;

std::wstring to_lower(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t ch) {
        return static_cast<wchar_t>(std::towlower(ch));
    });
    return value;
}

bool contains_case_insensitive(const std::wstring& haystack, const std::wstring& needle) {
    return to_lower(haystack).find(to_lower(needle)) != std::wstring::npos;
}

bool equals_case_insensitive(const std::wstring& left, const std::wstring& right) {
    return to_lower(left) == to_lower(right);
}

std::wstring endpoint_name(IMMDevice* device) {
    ComPtr<IPropertyStore> store;
    if (FAILED(device->OpenPropertyStore(STGM_READ, store.put()))) {
        return L"<unknown>";
    }

    PROPVARIANT value;
    PropVariantInit(&value);
    std::wstring name = L"<unknown>";
    if (SUCCEEDED(store->GetValue(PKEY_Device_FriendlyName, &value)) && value.vt == VT_LPWSTR) {
        name = value.pwszVal;
    }
    PropVariantClear(&value);
    return name;
}

std::wstring endpoint_id(IMMDevice* device) {
    LPWSTR id = nullptr;
    std::wstring result;
    if (SUCCEEDED(device->GetId(&id)) && id) {
        result = id;
        CoTaskMemFree(id);
    }
    return result;
}

struct Endpoint {
    std::wstring id;
    std::wstring name;
    ComPtr<IMMDevice> device;
};

class MmcssScope {
public:
    explicit MmcssScope(const wchar_t* task_name = L"Audio") {
        DWORD task_index = 0;
        handle_ = AvSetMmThreadCharacteristicsW(task_name, &task_index);
        if (handle_) {
            AvSetMmThreadPriority(handle_, AVRT_PRIORITY_HIGH);
        }
    }

    ~MmcssScope() {
        if (handle_) {
            AvRevertMmThreadCharacteristics(handle_);
        }
    }

    bool enabled() const { return handle_ != nullptr; }

private:
    HANDLE handle_ = nullptr;
};

bool is_float_format(const WAVEFORMATEX* format) {
    if (!format) {
        return false;
    }
    if (format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
        return true;
    }
    if (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        const auto* extensible = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(format);
        return extensible->SubFormat.Data1 == WAVE_FORMAT_IEEE_FLOAT;
    }
    return false;
}

bool is_pcm_format(const WAVEFORMATEX* format) {
    if (!format) {
        return false;
    }
    if (format->wFormatTag == WAVE_FORMAT_PCM) {
        return true;
    }
    if (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        const auto* extensible = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(format);
        return extensible->SubFormat.Data1 == WAVE_FORMAT_PCM;
    }
    return false;
}

WaveFormatPtr make_float_format(DWORD sample_rate, WORD channels) {
    auto* format = static_cast<WAVEFORMATEX*>(CoTaskMemAlloc(sizeof(WAVEFORMATEXTENSIBLE)));
    if (!format) {
        throw std::bad_alloc();
    }
    ZeroMemory(format, sizeof(WAVEFORMATEXTENSIBLE));
    auto* extensible = reinterpret_cast<WAVEFORMATEXTENSIBLE*>(format);
    extensible->Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    extensible->Format.nChannels = channels;
    extensible->Format.nSamplesPerSec = sample_rate;
    extensible->Format.wBitsPerSample = 32;
    extensible->Format.nBlockAlign = extensible->Format.nChannels * extensible->Format.wBitsPerSample / 8;
    extensible->Format.nAvgBytesPerSec = extensible->Format.nSamplesPerSec * extensible->Format.nBlockAlign;
    extensible->Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    extensible->Samples.wValidBitsPerSample = 32;
    extensible->dwChannelMask = channels == 1 ? SPEAKER_FRONT_CENTER : (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT);
    extensible->SubFormat.Data1 = WAVE_FORMAT_IEEE_FLOAT;
    extensible->SubFormat.Data2 = 0x0000;
    extensible->SubFormat.Data3 = 0x0010;
    extensible->SubFormat.Data4[0] = 0x80;
    extensible->SubFormat.Data4[1] = 0x00;
    extensible->SubFormat.Data4[2] = 0x00;
    extensible->SubFormat.Data4[3] = 0xaa;
    extensible->SubFormat.Data4[4] = 0x00;
    extensible->SubFormat.Data4[5] = 0x38;
    extensible->SubFormat.Data4[6] = 0x9b;
    extensible->SubFormat.Data4[7] = 0x71;
    return WaveFormatPtr(format);
}

WaveFormatPtr make_float_stereo_format(DWORD sample_rate = 48000) {
    return make_float_format(sample_rate, 2);
}

std::wstring format_summary(const WAVEFORMATEX* format) {
    if (!format) {
        return L"<null>";
    }
    return std::to_wstring(format->nSamplesPerSec) + L" Hz, " +
        std::to_wstring(format->nChannels) + L" ch, " +
        std::to_wstring(format->wBitsPerSample) + L" bit";
}

std::vector<Endpoint> enumerate_endpoints(EDataFlow flow) {
    ComPtr<IMMDeviceEnumerator> enumerator;
    HRESULT hr = CoCreateInstance(
        __uuidof(MMDeviceEnumerator),
        nullptr,
        CLSCTX_ALL,
        __uuidof(IMMDeviceEnumerator),
        reinterpret_cast<void**>(enumerator.put())
    );
    if (FAILED(hr)) {
        throw std::runtime_error("Could not create MMDeviceEnumerator.");
    }

    ComPtr<IMMDeviceCollection> collection;
    hr = enumerator->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, collection.put());
    if (FAILED(hr)) {
        throw std::runtime_error("Could not enumerate audio endpoints.");
    }

    UINT count = 0;
    collection->GetCount(&count);
    std::vector<Endpoint> endpoints;
    endpoints.reserve(count);
    for (UINT i = 0; i < count; ++i) {
        Endpoint endpoint;
        if (FAILED(collection->Item(i, endpoint.device.put()))) {
            continue;
        }
        endpoint.id = endpoint_id(endpoint.device.get());
        endpoint.name = endpoint_name(endpoint.device.get());
        endpoints.push_back(std::move(endpoint));
    }
    return endpoints;
}

// An exact id or name wins over a substring match, so a short selector cannot grab the
// wrong endpoint when a more specific one exists.
Endpoint* find_endpoint(std::vector<Endpoint>& endpoints, const std::wstring& selector) {
    for (auto& endpoint : endpoints) {
        if (equals_case_insensitive(endpoint.id, selector) || equals_case_insensitive(endpoint.name, selector)) {
            return &endpoint;
        }
    }
    for (auto& endpoint : endpoints) {
        if (contains_case_insensitive(endpoint.name, selector) || contains_case_insensitive(endpoint.id, selector)) {
            return &endpoint;
        }
    }
    return nullptr;
}

enum class ChannelMode {
    Stereo,
    Left,
    Right,
};

ChannelMode parse_channel_mode(const std::wstring& value) {
    auto lower = to_lower(value);
    if (lower == L"left") {
        return ChannelMode::Left;
    }
    if (lower == L"right") {
        return ChannelMode::Right;
    }
    return ChannelMode::Stereo;
}

std::wstring channel_mode_name(ChannelMode mode) {
    switch (mode) {
    case ChannelMode::Left:
        return L"Left";
    case ChannelMode::Right:
        return L"Right";
    default:
        return L"Stereo";
    }
}

void print_usage() {
    log_line(
        L"maf_native_engine --list\n"
        L"maf_native_engine --source <name-or-id> --out <name-or-id>[|stereo|left|right[|gain[|delay_ms]]] ...\n"
        L"                  [--master <gain>] [--latency-ms <ms>] [--no-sync] [--exit-on-stdin-eof] [--duration <seconds>]\n\n"
        L"Example:\n"
        L"  maf_native_engine --source \"MAF Virtual Audio Output\" --out \"Motion Boom|left|1.0|0\" --out \"X600|right|1.0|35\"\n\n"
        L"                  [--mic <name-or-id>] [--calibrate]\n\n"
        L"stdin commands: master <g> | gain <i> <g> | mode <i> <stereo|left|right> | delay <i> <ms> |\n"
        L"                latency <ms> | sync <0|1> | resync | calibrate | miccheck | quit"
    );
}

struct OutputSpec {
    std::wstring selector;
    ChannelMode channel_mode = ChannelMode::Stereo;
    float gain = 1.0f;
    double delay_ms = 0.0;
};

std::vector<std::wstring> split(const std::wstring& value, wchar_t separator) {
    std::vector<std::wstring> parts;
    size_t start = 0;
    while (true) {
        size_t end = value.find(separator, start);
        parts.push_back(value.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start));
        if (end == std::wstring::npos) {
            break;
        }
        start = end + 1;
    }
    return parts;
}

// New form: "selector|mode|gain|delay_ms". The old "selector:mode:gain" form is still accepted
// when there is no '|', but names containing ':' only work with the new form.
OutputSpec parse_output_spec(const std::wstring& spec) {
    OutputSpec output;
    auto parts = split(spec, spec.find(L'|') != std::wstring::npos ? L'|' : L':');
    output.selector = parts[0];
    if (parts.size() > 1) {
        output.channel_mode = parse_channel_mode(parts[1]);
    }
    try {
        if (parts.size() > 2) {
            output.gain = std::max(0.0f, std::stof(parts[2]));
        }
        if (parts.size() > 3) {
            output.delay_ms = std::stod(parts[3]);
        }
    } catch (...) {
    }
    return output;
}

struct Args {
    bool list = false;
    bool selftest = false;
    bool exit_on_stdin_eof = false;
    bool sync_enabled = true;
    int duration_seconds = 0;
    float master_gain = 1.0f;
    double latency_ms = kDefaultLatencyMs;
    std::wstring source_selector = L"MAF Virtual Audio Output";
    std::wstring mic_selector;
    bool calibrate = false; // Run one calibration a few seconds after start (testing).
    std::vector<OutputSpec> outputs;
};

Args parse_args(int argc, wchar_t** argv) {
    Args args;
    for (int i = 1; i < argc; ++i) {
        std::wstring arg = argv[i];
        if (arg == L"--help" || arg == L"-h") {
            print_usage();
            std::exit(0);
        }
        if (arg == L"--list") {
            args.list = true;
            continue;
        }
        if (arg == L"--selftest") {
            args.selftest = true;
            continue;
        }
        if (arg == L"--exit-on-stdin-eof") {
            args.exit_on_stdin_eof = true;
            continue;
        }
        if (arg == L"--no-sync") {
            args.sync_enabled = false;
            continue;
        }
        if (arg == L"--source" && i + 1 < argc) {
            args.source_selector = argv[++i];
            continue;
        }
        if (arg == L"--out" && i + 1 < argc) {
            args.outputs.push_back(parse_output_spec(argv[++i]));
            continue;
        }
        if (arg == L"--duration" && i + 1 < argc) {
            args.duration_seconds = std::max(0, std::stoi(argv[++i]));
            continue;
        }
        if (arg == L"--master" && i + 1 < argc) {
            args.master_gain = std::max(0.0f, std::stof(argv[++i]));
            continue;
        }
        if (arg == L"--mic" && i + 1 < argc) {
            args.mic_selector = argv[++i];
            continue;
        }
        if (arg == L"--calibrate") {
            args.calibrate = true;
            continue;
        }
        if (arg == L"--latency-ms" && i + 1 < argc) {
            args.latency_ms = std::clamp(std::stod(argv[++i]), kMinLatencyMs, kMaxLatencyMs);
            continue;
        }
        throw std::runtime_error("Unknown or incomplete argument.");
    }
    return args;
}

// Single producer (capture thread), single consumer (one output's render thread).
class FrameRing {
public:
    explicit FrameRing(size_t capacity_frames)
        : data_(capacity_frames * 2, 0.0f), capacity_(capacity_frames) {}

    // A null source writes silence. Frames that do not fit are dropped; returns frames written.
    size_t write(const float* frames, size_t count) {
        const uint64_t write_pos = write_pos_.load(std::memory_order_relaxed);
        const uint64_t read_pos = read_pos_.load(std::memory_order_acquire);
        const size_t free_frames = capacity_ - static_cast<size_t>(write_pos - read_pos);
        const size_t n = std::min(count, free_frames);
        for (size_t i = 0; i < n; ++i) {
            const size_t slot = static_cast<size_t>((write_pos + i) % capacity_) * 2;
            data_[slot] = frames ? frames[i * 2] : 0.0f;
            data_[slot + 1] = frames ? frames[i * 2 + 1] : 0.0f;
        }
        write_pos_.store(write_pos + n, std::memory_order_release);
        return n;
    }

    size_t read(float* destination, size_t count) {
        const uint64_t read_pos = read_pos_.load(std::memory_order_relaxed);
        const uint64_t write_pos = write_pos_.load(std::memory_order_acquire);
        const size_t n = std::min(count, static_cast<size_t>(write_pos - read_pos));
        for (size_t i = 0; i < n; ++i) {
            const size_t slot = static_cast<size_t>((read_pos + i) % capacity_) * 2;
            destination[i * 2] = data_[slot];
            destination[i * 2 + 1] = data_[slot + 1];
        }
        read_pos_.store(read_pos + n, std::memory_order_release);
        return n;
    }

    size_t discard(size_t count) {
        const uint64_t read_pos = read_pos_.load(std::memory_order_relaxed);
        const uint64_t write_pos = write_pos_.load(std::memory_order_acquire);
        const size_t n = std::min(count, static_cast<size_t>(write_pos - read_pos));
        read_pos_.store(read_pos + n, std::memory_order_release);
        return n;
    }

    size_t size() const {
        return static_cast<size_t>(write_pos_.load(std::memory_order_acquire) - read_pos_.load(std::memory_order_acquire));
    }

private:
    std::vector<float> data_;
    size_t capacity_;
    std::atomic<uint64_t> write_pos_ = 0;
    std::atomic<uint64_t> read_pos_ = 0;
};

float catmull_rom(float x0, float x1, float x2, float x3, float mu) {
    const float c1 = 0.5f * (x2 - x0);
    const float c2 = x0 - (2.5f * x1) + (2.0f * x2) - (0.5f * x3);
    const float c3 = (0.5f * (x3 - x0)) + (1.5f * (x1 - x2));
    return ((((c3 * mu) + c2) * mu + c1) * mu) + x1;
}

enum class OutputState {
    Opening,
    Waiting,
    Idle,
    Settling,
    Locked,
    Free,
    Stopped,
};

const wchar_t* output_state_name(OutputState state) {
    switch (state) {
    case OutputState::Opening:
        return L"opening";
    case OutputState::Waiting:
        return L"waiting";
    case OutputState::Idle:
        return L"idle";
    case OutputState::Settling:
        return L"settling";
    case OutputState::Locked:
        return L"locked";
    case OutputState::Free:
        return L"free";
    default:
        return L"stopped";
    }
}

// One render endpoint with its own thread. The capture thread pushes source frames into the
// ring; the render thread feeds the device on the device's clock and trims the playback rate
// so that ring + device buffer stays at the shared target latency plus this output's offset.
class Output {
public:
    Output(size_t index, OutputSpec spec, UINT32 source_rate)
        : index_(index),
          spec_(std::move(spec)),
          source_rate_(source_rate),
          ring_(static_cast<size_t>(source_rate * kRingSeconds)),
          staging_(kStagingFrames * 2, 0.0f) {
        gain_.store(std::max(0.0f, spec_.gain));
        channel_mode_.store(spec_.channel_mode);
        raw_delay_ms_.store(spec_.delay_ms);
    }

    void start() { thread_ = std::thread(&Output::thread_main, this); }

    void join() {
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    void push(const float* frames, UINT32 count) {
        const size_t written = ring_.write(frames, count);
        const auto state = state_.load();
        if (written < count && state != OutputState::Waiting && state != OutputState::Opening) {
            overflow_frames_ += count - written;
        }
    }

    void set_gain(float gain) { gain_.store(std::max(0.0f, gain)); }
    void set_channel_mode(ChannelMode mode) { channel_mode_.store(mode); }
    void set_raw_delay_ms(double delay_ms) { raw_delay_ms_.store(delay_ms); }
    double raw_delay_ms() const { return raw_delay_ms_.load(); }
    void set_delay_offset_ms(double offset_ms) { offset_ms_.store(std::clamp(offset_ms, 0.0, kMaxDelayOffsetMs)); }
    void request_resync() { resync_requested_.store(true); }

    // Smoothed time from pushing a frame into the ring until it leaves the device buffer.
    double pc_latency_seconds() const { return latency_ms_.load() / 1000.0; }

    bool is_playing() const {
        const auto state = state_.load();
        return state == OutputState::Settling || state == OutputState::Locked || state == OutputState::Free;
    }

    std::wstring status_line() const {
        const auto state = state_.load();
        std::wostringstream stream;
        stream << std::fixed << std::setprecision(1);
        stream << L"dev " << index_ << L" state=" << output_state_name(state);
        if (state == OutputState::Settling || state == OutputState::Locked || state == OutputState::Free) {
            const double error = error_ms_.load();
            stream << L" lat=" << latency_ms_.load() << L" target=" << target_ms_.load()
                   << L" err=" << (error >= 0.0 ? L"+" : L"") << error
                   << L" ppm=" << std::llround(correction_ppm_.load());
        }
        stream << L" under=" << underruns_.load() << L" snaps=" << snaps_.load()
               << L" over=" << overflow_frames_.load() << L" mode=" << (rate_adjust_.load() ? L"rateadjust" : L"resample");
        return stream.str();
    }

private:
    std::wstring prefix() const {
        return L"out " + std::to_wstring(index_) + L" ";
    }

    void thread_main() {
        HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        MmcssScope mmcss;
        std::wstring last_error;
        while (!g_stop_requested) {
            state_ = OutputState::Opening;
            std::wstring error;
            if (open(error)) {
                last_error.clear();
                std::wstring reason = run_stream();
                close();
                if (g_stop_requested) {
                    break;
                }
                log_line(prefix() + L"stream ended (" + reason + L"), reopening");
            } else {
                state_ = OutputState::Waiting;
                if (error != last_error) {
                    log_line(prefix() + L"waiting: " + error);
                    last_error = error;
                }
            }
            sleep_unless_stopped(kReopenDelayMs);
        }
        close();
        state_ = OutputState::Stopped;
        if (SUCCEEDED(co)) {
            CoUninitialize();
        }
    }

    bool resolve_device(ComPtr<IMMDevice>& device, std::wstring& error) {
        ComPtr<IMMDeviceEnumerator> enumerator;
        HRESULT hr = CoCreateInstance(
            __uuidof(MMDeviceEnumerator),
            nullptr,
            CLSCTX_ALL,
            __uuidof(IMMDeviceEnumerator),
            reinterpret_cast<void**>(enumerator.put())
        );
        if (FAILED(hr)) {
            error = L"could not create device enumerator " + hr_text(hr);
            return false;
        }

        auto try_id = [&](const std::wstring& id) {
            ComPtr<IMMDevice> candidate;
            if (id.empty() || FAILED(enumerator->GetDevice(id.c_str(), candidate.put()))) {
                return false;
            }
            DWORD state = 0;
            if (FAILED(candidate->GetState(&state)) || state != DEVICE_STATE_ACTIVE) {
                return false;
            }
            device = std::move(candidate);
            return true;
        };

        if (!try_id(device_id_)) {
            if (try_id(spec_.selector)) {
                device_id_ = spec_.selector;
            } else {
                std::vector<Endpoint> endpoints;
                try {
                    endpoints = enumerate_endpoints(eRender);
                } catch (const std::exception&) {
                    error = L"could not enumerate devices";
                    return false;
                }
                Endpoint* match = find_endpoint(endpoints, spec_.selector);
                if (!match) {
                    error = L"device not connected: " + spec_.selector;
                    return false;
                }
                device_id_ = match->id;
                device = std::move(match->device);
            }
        }
        if (equals_case_insensitive(device_id_, g_source_id)) {
            error = L"refusing to render back into the source endpoint";
            device.reset();
            return false;
        }
        return true;
    }

    // Preferred: the Windows audio engine converts formats and applies our rate trim
    // (AUDCLNT_STREAMFLAGS_RATEADJUST). Fallback: the device's own mix format with our
    // stateful cubic resampler.
    bool initialize_client(IMMDevice* device, bool rate_adjust, std::wstring& error) {
        audio_client_.reset();
        clock_adjust_.reset();
        HRESULT hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(audio_client_.put()));
        if (FAILED(hr)) {
            error = L"activate failed " + hr_text(hr);
            return false;
        }

        WaveFormatPtr format;
        DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
        if (rate_adjust) {
            format = make_float_stereo_format(source_rate_);
            flags |= AUDCLNT_STREAMFLAGS_RATEADJUST | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
        } else {
            WAVEFORMATEX* raw_format = nullptr;
            hr = audio_client_->GetMixFormat(&raw_format);
            if (FAILED(hr)) {
                error = L"mix format failed " + hr_text(hr);
                return false;
            }
            format = WaveFormatPtr(raw_format);
            const bool is_float = is_float_format(format.get());
            const bool is_pcm = is_pcm_format(format.get());
            const WORD bits = format->wBitsPerSample;
            if (format->nChannels < 1 || format->nChannels > 8 ||
                (!is_float && !is_pcm) ||
                (is_float && bits != 32) ||
                (is_pcm && bits != 16 && bits != 24 && bits != 32)) {
                error = L"unsupported mix format " + format_summary(format.get());
                return false;
            }
        }

        hr = audio_client_->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, kRenderBufferDuration, 0, format.get(), nullptr);
        if (FAILED(hr)) {
            error = L"initialize failed " + hr_text(hr);
            return false;
        }
        if (rate_adjust) {
            hr = audio_client_->GetService(__uuidof(IAudioClockAdjustment), reinterpret_cast<void**>(clock_adjust_.put()));
            if (SUCCEEDED(hr)) {
                hr = clock_adjust_->SetSampleRate(static_cast<float>(source_rate_));
            }
            if (FAILED(hr)) {
                error = L"rate adjust unavailable " + hr_text(hr);
                return false;
            }
        }

        stream_format_ = std::move(format);
        rate_adjust_ = rate_adjust;
        stream_rate_ = stream_format_->nSamplesPerSec;
        stream_channels_ = stream_format_->nChannels;
        stream_bits_ = stream_format_->wBitsPerSample;
        stream_is_float_ = is_float_format(stream_format_.get());
        return true;
    }

    bool open(std::wstring& error) {
        ComPtr<IMMDevice> device;
        if (!resolve_device(device, error)) {
            return false;
        }
        name_ = endpoint_name(device.get());

        std::wstring rate_adjust_error;
        if (!initialize_client(device.get(), true, rate_adjust_error) &&
            !initialize_client(device.get(), false, error)) {
            error = name_ + L": " + rate_adjust_error + L"; fallback " + error;
            close();
            return false;
        }

        event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        HRESULT hr = event_ ? audio_client_->SetEventHandle(event_) : E_FAIL;
        if (SUCCEEDED(hr)) {
            hr = audio_client_->GetBufferSize(&buffer_frames_);
        }
        if (SUCCEEDED(hr)) {
            hr = audio_client_->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<void**>(render_client_.put()));
        }
        if (FAILED(hr)) {
            error = name_ + L": render setup failed " + hr_text(hr);
            close();
            return false;
        }

        // Fresh stream: drop whatever piled up while the device was away and realign on the next audio.
        ring_.discard(ring_.size());
        pending_silence_ = 0;
        staged_count_ = 0;
        staged_pos_ = 0;
        starved_frames_ = 0;
        idle_ = true;
        fade_on_audio_ = true;
        std::fill(&history_[0][0], &history_[0][0] + 8, 0.0f);
        mu_ = 0.0;
        step_ = static_cast<double>(source_rate_) / static_cast<double>(stream_rate_);
        correction_ = 0.0;
        applied_rate_ = static_cast<float>(source_rate_);
        fade_total_ = std::max<UINT32>(1, static_cast<UINT32>(stream_rate_ * kFadeSeconds));
        fade_remaining_ = 0;
        idle_threshold_frames_ = static_cast<UINT64>(source_rate_ * kIdleSeconds);
        applied_target_s_ = target_seconds();
        warned_target_too_low_ = false;

        std::wostringstream message;
        message << prefix() << L"opened \"" << name_ << L"\" channel=" << channel_mode_name(channel_mode_.load())
                << L" stream=(" << format_summary(stream_format_.get()) << L") buffer="
                << (buffer_frames_ * 1000 / std::max<UINT32>(1, stream_rate_)) << L"ms mode="
                << (rate_adjust_.load() ? L"rateadjust" : L"resample");
        log_line(message.str());
        return true;
    }

    void close() {
        render_client_.reset();
        clock_adjust_.reset();
        audio_client_.reset();
        if (event_) {
            CloseHandle(event_);
            event_ = nullptr;
        }
    }

    std::wstring run_stream() {
        UINT32 padding = 0;
        BYTE* buffer = nullptr;
        HRESULT hr = audio_client_->GetCurrentPadding(&padding);
        if (SUCCEEDED(hr) && buffer_frames_ > padding) {
            hr = render_client_->GetBuffer(buffer_frames_ - padding, &buffer);
            if (SUCCEEDED(hr)) {
                hr = render_client_->ReleaseBuffer(buffer_frames_ - padding, AUDCLNT_BUFFERFLAGS_SILENT);
            }
        }
        if (SUCCEEDED(hr)) {
            hr = audio_client_->Start();
        }
        if (FAILED(hr)) {
            return L"start failed " + hr_text(hr);
        }
        state_ = OutputState::Idle;

        DWORD stalled_ms = 0;
        while (!g_stop_requested) {
            DWORD wait = WaitForSingleObject(event_, 100);
            if (wait == WAIT_TIMEOUT) {
                stalled_ms += 100;
                if (stalled_ms >= kStallTimeoutMs) {
                    audio_client_->Stop();
                    return L"device stopped requesting audio";
                }
                continue;
            }
            if (wait != WAIT_OBJECT_0) {
                audio_client_->Stop();
                return L"event wait failed";
            }
            stalled_ms = 0;

            hr = audio_client_->GetCurrentPadding(&padding);
            if (FAILED(hr)) {
                audio_client_->Stop();
                return L"device lost " + hr_text(hr);
            }
            const UINT32 frames = buffer_frames_ > padding ? buffer_frames_ - padding : 0;
            if (frames == 0) {
                continue;
            }

            before_fill(padding);
            hr = render_client_->GetBuffer(frames, &buffer);
            if (FAILED(hr)) {
                audio_client_->Stop();
                return L"get buffer failed " + hr_text(hr);
            }
            fill(buffer, frames);
            hr = render_client_->ReleaseBuffer(frames, 0);
            if (FAILED(hr)) {
                audio_client_->Stop();
                return L"release buffer failed " + hr_text(hr);
            }
            after_fill(padding + frames, frames);
        }
        audio_client_->Stop();
        return L"stopped";
    }

    double target_seconds() const {
        return (g_latency_ms.load() + offset_ms_.load()) / 1000.0;
    }

    size_t queued_source_frames() const {
        return ring_.size() + (staged_count_ - staged_pos_) + static_cast<size_t>(pending_silence_);
    }

    double measured_latency_seconds(UINT32 device_padding) const {
        return (static_cast<double>(queued_source_frames()) / source_rate_) +
            (static_cast<double>(device_padding) / stream_rate_);
    }

    void discard_source(UINT64 frames) {
        const UINT64 from_silence = std::min(frames, pending_silence_);
        pending_silence_ -= from_silence;
        frames -= from_silence;
        const UINT64 from_staging = std::min<UINT64>(frames, staged_count_ - staged_pos_);
        staged_pos_ += static_cast<size_t>(from_staging);
        frames -= from_staging;
        ring_.discard(static_cast<size_t>(frames));
    }

    // Changes this output's latency at once: positive inserts silence, negative skips audio.
    void shift_latency(double delta_seconds) {
        const long long frames = std::llround(delta_seconds * source_rate_);
        if (frames > 0) {
            pending_silence_ += static_cast<UINT64>(frames);
        } else if (frames < 0) {
            discard_source(static_cast<UINT64>(-frames));
        }
        filtered_s_ += delta_seconds;
        settle_until_ = std::chrono::steady_clock::now() + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<double>(kSettleSeconds));
        fade_on_audio_ = true;
    }

    void before_fill(UINT32 device_padding) {
        if (resync_requested_.exchange(false)) {
            discard_source(queued_source_frames());
            idle_ = true;
            log_line(prefix() + L"resync");
        }

        const double target = target_seconds();
        if (std::abs(target - applied_target_s_) > 0.0001) {
            if (!idle_) {
                shift_latency(target - applied_target_s_);
            }
            applied_target_s_ = target;
        }

        // Audio (re)started after silence, a reopen or a resync: place it exactly on the target.
        if (idle_ && ring_.size() > 0) {
            idle_ = false;
            starved_frames_ = 0;
            shift_latency(target - measured_latency_seconds(device_padding));
            filtered_s_ = target;
        }
    }

    void note_starved() {
        if (!idle_) {
            if (starved_frames_ == 0) {
                ++underruns_;
                fade_on_audio_ = true;
            }
            if (starved_frames_ + 1 >= idle_threshold_frames_) {
                idle_ = true;
            }
        }
        ++starved_frames_;
    }

    void next_source_frame(float& left, float& right) {
        left = 0.0f;
        right = 0.0f;
        if (pending_silence_ > 0) {
            --pending_silence_;
            return;
        }
        if (idle_) {
            // Wait for before_fill() to realign instead of starting at a random latency.
            note_starved();
            return;
        }
        if (staged_pos_ >= staged_count_) {
            staged_count_ = ring_.read(staging_.data(), kStagingFrames);
            staged_pos_ = 0;
            if (staged_count_ == 0) {
                note_starved();
                return;
            }
        }
        starved_frames_ = 0;
        if (fade_on_audio_) {
            fade_on_audio_ = false;
            fade_remaining_ = fade_total_;
        }
        left = staging_[staged_pos_ * 2];
        right = staging_[staged_pos_ * 2 + 1];
        ++staged_pos_;
    }

    void next_output_frame(float& left, float& right) {
        if (rate_adjust_.load(std::memory_order_relaxed)) {
            next_source_frame(left, right);
            return;
        }
        // Stateful 4-point interpolation: the fractional position carries across buffers,
        // so no frames are lost or invented at buffer boundaries.
        const float mu = static_cast<float>(mu_);
        left = catmull_rom(history_[0][0], history_[1][0], history_[2][0], history_[3][0], mu);
        right = catmull_rom(history_[0][1], history_[1][1], history_[2][1], history_[3][1], mu);
        mu_ += step_;
        while (mu_ >= 1.0) {
            mu_ -= 1.0;
            for (int i = 0; i < 3; ++i) {
                history_[i][0] = history_[i + 1][0];
                history_[i][1] = history_[i + 1][1];
            }
            next_source_frame(history_[3][0], history_[3][1]);
        }
    }

    void write_frame(BYTE* buffer, UINT32 frame, float left, float right) const {
        if (stream_is_float_) {
            float* out = reinterpret_cast<float*>(buffer) + (static_cast<size_t>(frame) * stream_channels_);
            out[0] = left;
            if (stream_channels_ > 1) {
                out[1] = right;
            }
            for (WORD channel = 2; channel < stream_channels_; ++channel) {
                out[channel] = 0.0f;
            }
            return;
        }
        if (stream_bits_ == 16) {
            int16_t* out = reinterpret_cast<int16_t*>(buffer) + (static_cast<size_t>(frame) * stream_channels_);
            out[0] = static_cast<int16_t>(left * 32767.0f);
            if (stream_channels_ > 1) {
                out[1] = static_cast<int16_t>(right * 32767.0f);
            }
            for (WORD channel = 2; channel < stream_channels_; ++channel) {
                out[channel] = 0;
            }
            return;
        }
        if (stream_bits_ == 24) {
            BYTE* out = buffer + (static_cast<size_t>(frame) * stream_channels_ * 3);
            auto write_one = [](BYTE* target, float value) {
                int32_t scaled = static_cast<int32_t>(value * 8388607.0f);
                target[0] = static_cast<BYTE>(scaled & 0xff);
                target[1] = static_cast<BYTE>((scaled >> 8) & 0xff);
                target[2] = static_cast<BYTE>((scaled >> 16) & 0xff);
            };
            write_one(out, left);
            if (stream_channels_ > 1) {
                write_one(out + 3, right);
            }
            for (WORD channel = 2; channel < stream_channels_; ++channel) {
                write_one(out + (channel * 3), 0.0f);
            }
            return;
        }
        int32_t* out = reinterpret_cast<int32_t*>(buffer) + (static_cast<size_t>(frame) * stream_channels_);
        out[0] = static_cast<int32_t>(static_cast<double>(left) * 2147483647.0);
        if (stream_channels_ > 1) {
            out[1] = static_cast<int32_t>(static_cast<double>(right) * 2147483647.0);
        }
        for (WORD channel = 2; channel < stream_channels_; ++channel) {
            out[channel] = 0;
        }
    }

    void fill(BYTE* buffer, UINT32 frames) {
        const ChannelMode mode = channel_mode_.load();
        // Calibration sweeps play at their own fixed level, independent of the volume sliders.
        const float gain = g_calibration_active.load() ? 1.0f : gain_.load() * g_master_gain.load();
        for (UINT32 i = 0; i < frames; ++i) {
            float left = 0.0f;
            float right = 0.0f;
            next_output_frame(left, right);
            if (mode == ChannelMode::Left) {
                right = left;
            } else if (mode == ChannelMode::Right) {
                left = right;
            }
            float frame_gain = gain;
            if (fade_remaining_ > 0) {
                frame_gain *= 1.0f - (static_cast<float>(fade_remaining_) / static_cast<float>(fade_total_));
                --fade_remaining_;
            }
            write_frame(buffer, i, std::clamp(left * frame_gain, -1.0f, 1.0f), std::clamp(right * frame_gain, -1.0f, 1.0f));
        }
    }

    void after_fill(UINT32 device_padding, UINT32 frames_written) {
        const auto now = std::chrono::steady_clock::now();
        target_ms_.store(applied_target_s_ * 1000.0);
        if (idle_) {
            state_ = OutputState::Idle;
            return;
        }

        const double dt = static_cast<double>(frames_written) / stream_rate_;
        const double latency = measured_latency_seconds(device_padding);
        filtered_s_ += (latency - filtered_s_) * (dt / (kFilterSeconds + dt));
        double error = filtered_s_ - applied_target_s_;
        const bool settling = now < settle_until_;

        if (g_sync_enabled.load()) {
            if (!settling) {
                // The device buffer alone can exceed a low target; skipping audio cannot fix that.
                const bool nothing_to_skip = error > 0.0 && queued_source_frames() < source_rate_ / 200;
                if (std::abs(error) * 1000.0 > kSnapThresholdMs && !nothing_to_skip) {
                    std::wostringstream message;
                    message << std::fixed << std::setprecision(1) << prefix() << L"snap " << (error * 1000.0) << L"ms";
                    log_line(message.str());
                    shift_latency(-error);
                    ++snaps_;
                    error = 0.0;
                } else {
                    if (nothing_to_skip && error * 1000.0 > kSnapThresholdMs && !warned_target_too_low_) {
                        warned_target_too_low_ = true;
                        log_line(prefix() + L"target latency is below this device's buffer; raise the latency setting");
                    }
                    const double limit = kMaxCorrection / kControlKi;
                    integral_ = std::clamp(integral_ + (error * dt), -limit, limit);
                }
            }
            correction_ = std::clamp((kControlKp * error) + (kControlKi * integral_), -kMaxCorrection, kMaxCorrection);
        } else {
            correction_ = 0.0;
            integral_ = 0.0;
        }
        apply_correction(now);

        latency_ms_.store(filtered_s_ * 1000.0);
        error_ms_.store(error * 1000.0);
        correction_ppm_.store(correction_ * 1e6);
        if (!g_sync_enabled.load()) {
            state_ = OutputState::Free;
        } else {
            state_ = now < settle_until_ ? OutputState::Settling : OutputState::Locked;
        }
    }

    // Positive correction consumes source audio faster, which lowers this output's latency.
    void apply_correction(std::chrono::steady_clock::time_point now) {
        if (!rate_adjust_.load(std::memory_order_relaxed)) {
            step_ = (static_cast<double>(source_rate_) / stream_rate_) * (1.0 + correction_);
            return;
        }
        const float rate = static_cast<float>(source_rate_ * (1.0 + correction_));
        if (std::abs(rate - applied_rate_) < 0.01f || now - last_rate_update_ < std::chrono::milliseconds(50)) {
            return;
        }
        last_rate_update_ = now;
        if (SUCCEEDED(clock_adjust_->SetSampleRate(rate))) {
            applied_rate_ = rate;
        }
    }

    size_t index_;
    OutputSpec spec_;
    UINT32 source_rate_;
    FrameRing ring_;
    std::thread thread_;

    // Shared with the capture and control threads.
    std::atomic<float> gain_ = 1.0f;
    std::atomic<ChannelMode> channel_mode_ = ChannelMode::Stereo;
    std::atomic<double> raw_delay_ms_ = 0.0;
    std::atomic<double> offset_ms_ = 0.0;
    std::atomic<bool> resync_requested_ = false;
    std::atomic<OutputState> state_ = OutputState::Opening;
    std::atomic<double> latency_ms_ = 0.0;
    std::atomic<double> target_ms_ = 0.0;
    std::atomic<double> error_ms_ = 0.0;
    std::atomic<double> correction_ppm_ = 0.0;
    std::atomic<UINT64> underruns_ = 0;
    std::atomic<UINT64> snaps_ = 0;
    std::atomic<UINT64> overflow_frames_ = 0;
    std::atomic<bool> rate_adjust_ = false;

    // Render thread only.
    std::wstring device_id_;
    std::wstring name_;
    ComPtr<IAudioClient> audio_client_;
    ComPtr<IAudioRenderClient> render_client_;
    ComPtr<IAudioClockAdjustment> clock_adjust_;
    WaveFormatPtr stream_format_;
    HANDLE event_ = nullptr;
    UINT32 buffer_frames_ = 0;
    UINT32 stream_rate_ = 48000;
    WORD stream_channels_ = 2;
    WORD stream_bits_ = 32;
    bool stream_is_float_ = true;

    std::vector<float> staging_;
    size_t staged_count_ = 0;
    size_t staged_pos_ = 0;
    UINT64 pending_silence_ = 0;
    UINT64 starved_frames_ = 0;
    UINT64 idle_threshold_frames_ = 12000;
    bool idle_ = true;
    bool fade_on_audio_ = true;
    float history_[4][2] = {};
    double mu_ = 0.0;
    double step_ = 1.0;
    UINT32 fade_total_ = 240;
    UINT32 fade_remaining_ = 0;

    double applied_target_s_ = 0.0;
    double filtered_s_ = 0.0;
    double integral_ = 0.0;
    double correction_ = 0.0;
    float applied_rate_ = 48000.0f;
    bool warned_target_too_low_ = false;
    std::chrono::steady_clock::time_point settle_until_{};
    std::chrono::steady_clock::time_point last_rate_update_{};
};

using OutputList = std::vector<std::unique_ptr<Output>>;

// Delays are relative: the output with the smallest delay gets offset 0, the others wait the difference.
void apply_delay_offsets(OutputList& outputs) {
    if (outputs.empty()) {
        return;
    }
    double floor = outputs.front()->raw_delay_ms();
    for (const auto& output : outputs) {
        floor = std::min(floor, output->raw_delay_ms());
    }
    for (auto& output : outputs) {
        output->set_delay_offset_ms(output->raw_delay_ms() - floor);
    }
}

// ---- Microphone calibration -----------------------------------------------------------------
//
// The PC can only keep its own side in sync. What happens after audio leaves Windows (Bluetooth
// stack, codec, the speaker's own buffer, distance to the listener) differs per speaker and can
// only be measured acoustically: play a short sweep on one speaker at a time, record it with a
// microphone and compare arrival times. Only differences between speakers matter, so the mic's
// own latency cancels out.
//
// A broken mic must never be mistaken for a silent speaker, so:
// 1. The mic is checked (device state, privacy, mute, driver start, data rate, signal) before any
//    sweep plays, and again afterwards. Any failure ends calibration with a mic error.
// 2. A speaker is only reported as not heard when the same run heard another speaker. When no
//    speaker is heard at all the result is "cannot tell", never "speaker not playing".

constexpr double kPi = 3.14159265358979323846;
constexpr double kCalPreSilenceSeconds = 1.0; // Music muted; lets the rings drain.
constexpr double kCalBaselineSeconds = 1.0;   // Room noise with every speaker silent.
constexpr double kCalLeadSeconds = 0.3;
constexpr double kCalChirpSeconds = 0.4;
constexpr double kCalSlotSeconds = 1.4;       // Sweep + speaker latency + room echo.
constexpr double kCalSearchSeconds = 0.9;     // Largest speaker latency searched for.
constexpr double kCalChirpStartHz = 300.0;
constexpr double kCalChirpEndHz = 6000.0;
constexpr float kCalChirpLevel = 0.25f;
constexpr int kCalRounds = 3;
constexpr double kCalMinSnr = 8.0;
constexpr double kCalMaxSpreadMs = 5.0;
constexpr double kCalTimeoutSeconds = 40.0;
constexpr double kMicCheckSeconds = 1.5;
constexpr UINT32 kMicRate = 48000;

std::wstring format_number(double value, int precision) {
    std::wostringstream stream;
    stream << std::fixed << std::setprecision(precision) << value;
    return stream.str();
}

std::string json_escape(const std::string& text) {
    std::string out;
    out.reserve(text.size() + 8);
    for (unsigned char ch : text) {
        if (ch == '"') {
            out += "\\\"";
        } else if (ch == '\\') {
            out += "\\\\";
        } else if (ch < 0x20) {
            char buffer[8];
            std::snprintf(buffer, sizeof(buffer), "\\u%04x", ch);
            out += buffer;
        } else {
            out += static_cast<char>(ch);
        }
    }
    return out;
}

// One "cal {json}" line for the GUI.
class CalReport {
public:
    explicit CalReport(const char* event) { stream_ << "{\"event\":\"" << event << '"'; }

    CalReport& text(const char* key, const std::wstring& value) {
        stream_ << ",\"" << key << "\":\"" << json_escape(to_utf8(value)) << '"';
        return *this;
    }

    CalReport& number(const char* key, double value) {
        stream_ << ",\"" << key << "\":" << std::fixed << std::setprecision(2) << (std::isfinite(value) ? value : 0.0);
        return *this;
    }

    CalReport& integer(const char* key, long long value) {
        stream_ << ",\"" << key << "\":" << value;
        return *this;
    }

    CalReport& flag(const char* key, bool value) {
        stream_ << ",\"" << key << "\":" << (value ? "true" : "false");
        return *this;
    }

    void emit() { log_utf8("cal " + stream_.str() + "}"); }

private:
    std::ostringstream stream_;
};

// Exponential sweep with short fades; its autocorrelation is a single sharp peak.
std::vector<float> make_chirp(UINT32 rate) {
    const size_t frames = static_cast<size_t>(kCalChirpSeconds * rate);
    const double k = std::log(kCalChirpEndHz / kCalChirpStartHz);
    const double fade = 0.01 * rate;
    std::vector<float> chirp(frames);
    for (size_t i = 0; i < frames; ++i) {
        const double t = static_cast<double>(i) / rate;
        const double phase = 2.0 * kPi * kCalChirpStartHz * kCalChirpSeconds / k * (std::exp(t * k / kCalChirpSeconds) - 1.0);
        const double edge = static_cast<double>(std::min(i, frames - 1 - i));
        const double envelope = edge < fade ? 0.5 - (0.5 * std::cos(kPi * edge / fade)) : 1.0;
        chirp[i] = static_cast<float>(std::sin(phase) * envelope);
    }
    return chirp;
}

void fft(std::vector<std::complex<double>>& data, bool inverse) {
    const size_t n = data.size();
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) {
            j ^= bit;
        }
        j ^= bit;
        if (i < j) {
            std::swap(data[i], data[j]);
        }
    }
    for (size_t length = 2; length <= n; length <<= 1) {
        const double angle = 2.0 * kPi / static_cast<double>(length) * (inverse ? 1.0 : -1.0);
        const std::complex<double> step(std::cos(angle), std::sin(angle));
        for (size_t start = 0; start < n; start += length) {
            std::complex<double> w(1.0, 0.0);
            for (size_t k = 0; k < length / 2; ++k) {
                const auto even = data[start + k];
                const auto odd = data[start + k + (length / 2)] * w;
                data[start + k] = even + odd;
                data[start + k + (length / 2)] = even - odd;
                w *= step;
            }
        }
    }
    if (inverse) {
        for (auto& value : data) {
            value /= static_cast<double>(n);
        }
    }
}

// Cross-correlation for lags 0 .. signal_size - templ.size().
std::vector<double> correlate(const float* signal, size_t signal_size, const std::vector<float>& templ) {
    if (signal_size < templ.size()) {
        return {};
    }
    size_t n = 1;
    while (n < signal_size + templ.size()) {
        n <<= 1;
    }
    std::vector<std::complex<double>> a(n);
    std::vector<std::complex<double>> b(n);
    for (size_t i = 0; i < signal_size; ++i) {
        a[i] = signal[i];
    }
    for (size_t i = 0; i < templ.size(); ++i) {
        b[i] = templ[i];
    }
    fft(a, false);
    fft(b, false);
    for (size_t i = 0; i < n; ++i) {
        a[i] *= std::conj(b[i]);
    }
    fft(a, true);
    std::vector<double> out(signal_size - templ.size() + 1);
    for (size_t lag = 0; lag < out.size(); ++lag) {
        out[lag] = a[lag].real();
    }
    return out;
}

// PKEY_AudioEndpoint_FormFactor; mmdeviceapi.h only declares it in this include order.
const PROPERTYKEY kFormFactorKey = { { 0x1da5d803, 0xd492, 0x4edd, { 0x8c, 0x23, 0xe0, 0xc0, 0xff, 0xee, 0x7f, 0x0e } }, 0 };

std::wstring endpoint_property_text(IMMDevice* device, const PROPERTYKEY& key) {
    ComPtr<IPropertyStore> store;
    if (FAILED(device->OpenPropertyStore(STGM_READ, store.put()))) {
        return {};
    }
    PROPVARIANT value;
    PropVariantInit(&value);
    std::wstring text;
    if (SUCCEEDED(store->GetValue(key, &value))) {
        if (value.vt == VT_LPWSTR && value.pwszVal) {
            text = value.pwszVal;
        } else if (value.vt == VT_UI4) {
            text = std::to_wstring(value.ulVal);
        } else if (value.vt == VT_CLSID && value.puuid) {
            wchar_t buffer[64] = {};
            StringFromGUID2(*value.puuid, buffer, 64);
            text = buffer;
        }
    }
    PropVariantClear(&value);
    return text;
}

const wchar_t* const kLocalMachineContainer = L"{00000000-0000-0000-FFFF-FFFFFFFFFFFF}";

bool is_bluetooth_bus(IMMDevice* device) {
    return to_lower(endpoint_property_text(device, PKEY_Device_EnumeratorName)).rfind(L"bth", 0) == 0;
}

// Containers (physical devices) that own any Bluetooth endpoint. A Bluetooth speaker's call
// microphone can sit on another bus (e.g. ROOT) but shares the speaker's container.
std::vector<std::wstring> bluetooth_containers() {
    std::vector<std::wstring> containers;
    ComPtr<IMMDeviceEnumerator> enumerator;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                reinterpret_cast<void**>(enumerator.put())))) {
        return containers;
    }
    ComPtr<IMMDeviceCollection> collection;
    if (FAILED(enumerator->EnumAudioEndpoints(eAll, DEVICE_STATEMASK_ALL, collection.put()))) {
        return containers;
    }
    UINT count = 0;
    collection->GetCount(&count);
    for (UINT i = 0; i < count; ++i) {
        ComPtr<IMMDevice> device;
        if (SUCCEEDED(collection->Item(i, device.put())) && is_bluetooth_bus(device.get())) {
            const std::wstring container = endpoint_property_text(device.get(), PKEY_Device_ContainerId);
            if (!container.empty() && !equals_case_insensitive(container, kLocalMachineContainer)) {
                containers.push_back(container);
            }
        }
    }
    return containers;
}

// A Bluetooth headset mic would switch that speaker from music (A2DP) to call quality (HFP).
bool is_bluetooth_endpoint(IMMDevice* device, const std::wstring& name, const std::vector<std::wstring>& bt_containers) {
    if (contains_case_insensitive(name, L"hands-free") || is_bluetooth_bus(device)) {
        return true;
    }
    const std::wstring container = endpoint_property_text(device, PKEY_Device_ContainerId);
    for (const auto& bt_container : bt_containers) {
        if (equals_case_insensitive(container, bt_container)) {
            return true;
        }
    }
    return false;
}

// Inputs that carry the PC's own output digitally instead of sound from the room.
bool is_loopback_input(const std::wstring& name) {
    for (const wchar_t* marker : { L"stereo mix", L"what u hear", L"wave out", L"loopback", L"cable output",
                                   L"voicemeeter", L"virtual" }) {
        if (contains_case_insensitive(name, marker)) {
            return true;
        }
    }
    return false;
}

// Windows hands desktop apps silent audio when microphone access is off.
bool microphone_privacy_denied(std::wstring& reason) {
    struct ConsentKey {
        HKEY root;
        const wchar_t* path;
        const wchar_t* meaning;
    };
    const ConsentKey keys[] = {
        { HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows\\CurrentVersion\\CapabilityAccessManager\\ConsentStore\\microphone",
          L"Microphone access is turned off for this device" },
        { HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\CapabilityAccessManager\\ConsentStore\\microphone",
          L"Microphone access is turned off" },
        { HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\CapabilityAccessManager\\ConsentStore\\microphone\\NonPackaged",
          L"\"Let desktop apps access your microphone\" is turned off" },
    };
    for (const auto& key : keys) {
        wchar_t value[32] = {};
        DWORD size = sizeof(value);
        if (RegGetValueW(key.root, key.path, L"Value", RRF_RT_REG_SZ, nullptr, value, &size) == ERROR_SUCCESS &&
            _wcsicmp(value, L"Deny") == 0) {
            reason = key.meaning;
            return true;
        }
    }
    return false;
}

std::wstring device_state_text(DWORD state) {
    switch (state) {
    case DEVICE_STATE_DISABLED:
        return L"disabled";
    case DEVICE_STATE_UNPLUGGED:
        return L"unplugged";
    case DEVICE_STATE_NOTPRESENT:
        return L"not present (driver not loaded)";
    default:
        return L"active";
    }
}

struct MicVerdict {
    bool ok = true;
    std::wstring code;
    std::wstring detail;
    double level_dbfs = -200.0;
    std::vector<std::pair<std::wstring, std::wstring>> warnings;
};

const wchar_t* const kDriverHint = L" The audio driver is probably stuck: restart the \"Windows Audio\" service or reboot.";

class MicRecorder {
public:
    struct Packet {
        size_t first_sample = 0;
        size_t frames = 0;
        double time = 0.0; // QPC seconds of the first frame.
        bool device_time = false;
        bool silent = false;
    };

    ~MicRecorder() { stop(); }

    bool open(const std::wstring& selector, std::wstring& code, std::wstring& detail) {
        ComPtr<IMMDeviceEnumerator> enumerator;
        HRESULT hr = CoCreateInstance(
            __uuidof(MMDeviceEnumerator),
            nullptr,
            CLSCTX_ALL,
            __uuidof(IMMDeviceEnumerator),
            reinterpret_cast<void**>(enumerator.put())
        );
        ComPtr<IMMDeviceCollection> collection;
        if (SUCCEEDED(hr)) {
            hr = enumerator->EnumAudioEndpoints(eCapture, DEVICE_STATEMASK_ALL, collection.put());
        }
        if (FAILED(hr)) {
            code = L"MIC_DRIVER_ERROR";
            detail = L"Windows could not list the microphones (" + hr_text(hr) + L")." + kDriverHint;
            return false;
        }

        struct Candidate {
            ComPtr<IMMDevice> device;
            std::wstring id;
            std::wstring name;
            DWORD state = 0;
            bool bluetooth = false;
            bool loopback = false;
            bool built_in = false; // Lives in the computer itself (not a separate device).
            bool microphone = false;
            bool virtual_device = false;
        };
        const auto bt_containers = bluetooth_containers();
        std::vector<Candidate> candidates;
        UINT count = 0;
        collection->GetCount(&count);
        for (UINT i = 0; i < count; ++i) {
            Candidate candidate;
            if (FAILED(collection->Item(i, candidate.device.put()))) {
                continue;
            }
            IMMDevice* item = candidate.device.get();
            item->GetState(&candidate.state);
            candidate.id = endpoint_id(item);
            candidate.name = endpoint_name(item);
            candidate.bluetooth = is_bluetooth_endpoint(item, candidate.name, bt_containers);
            candidate.loopback = is_loopback_input(candidate.name) || contains_case_insensitive(candidate.name, L"MAF");
            candidate.built_in = equals_case_insensitive(endpoint_property_text(item, PKEY_Device_ContainerId), kLocalMachineContainer);
            candidate.microphone = endpoint_property_text(item, kFormFactorKey) == L"4";
            const std::wstring bus = endpoint_property_text(item, PKEY_Device_EnumeratorName);
            candidate.virtual_device = candidate.built_in && equals_case_insensitive(bus, L"ROOT");
            candidates.push_back(std::move(candidate));
        }
        auto usable = [](const Candidate& candidate) {
            return candidate.state == DEVICE_STATE_ACTIVE && !candidate.bluetooth && !candidate.loopback && !candidate.virtual_device;
        };

        Candidate* chosen = nullptr;
        if (!selector.empty()) {
            for (auto& candidate : candidates) {
                if (equals_case_insensitive(candidate.id, selector) || equals_case_insensitive(candidate.name, selector)) {
                    chosen = &candidate;
                    break;
                }
            }
            for (auto& candidate : candidates) {
                if (!chosen && contains_case_insensitive(candidate.name, selector)) {
                    chosen = &candidate;
                }
            }
            if (!chosen) {
                code = L"MIC_NOT_FOUND";
                detail = L"No microphone matches \"" + selector + L"\".";
                return false;
            }
            if (chosen->bluetooth) {
                code = L"MIC_BLUETOOTH";
                detail = L"\"" + chosen->name + L"\" belongs to a Bluetooth device; using it would switch that speaker to call quality.";
                return false;
            }
            if (chosen->loopback) {
                code = L"MIC_NOT_A_MICROPHONE";
                detail = L"\"" + chosen->name + L"\" records the PC's own output, not sound from the room.";
                return false;
            }
            if (chosen->state != DEVICE_STATE_ACTIVE) {
                code = L"MIC_UNAVAILABLE";
                detail = L"\"" + chosen->name + L"\" is " + device_state_text(chosen->state) + L".";
                return false;
            }
        } else {
            // Windows' default mic first, then the computer's built-in mic, then any usable one.
            std::wstring default_id;
            ComPtr<IMMDevice> default_device;
            if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eCapture, eConsole, default_device.put()))) {
                default_id = endpoint_id(default_device.get());
            }
            for (auto& candidate : candidates) {
                if (!chosen && usable(candidate) && candidate.id == default_id) {
                    chosen = &candidate;
                }
            }
            for (auto& candidate : candidates) {
                if (!chosen && usable(candidate) && candidate.built_in && candidate.microphone) {
                    chosen = &candidate;
                }
            }
            for (auto& candidate : candidates) {
                if (!chosen && usable(candidate)) {
                    chosen = &candidate;
                }
            }
            if (!chosen) {
                code = L"MIC_MISSING";
                detail = L"Windows reports no working microphone. The audio driver may not be loaded.";
                const Candidate* broken = nullptr;
                for (const auto& candidate : candidates) {
                    if (!candidate.bluetooth && !candidate.loopback && !candidate.virtual_device &&
                        (!broken || (candidate.built_in && candidate.microphone))) {
                        broken = &candidate;
                    }
                }
                if (broken) {
                    code = L"MIC_UNAVAILABLE";
                    detail = L"The microphone \"" + broken->name + L"\" is " + device_state_text(broken->state) +
                        L". Enable it in Settings > System > Sound > Input, or restart / reinstall the audio driver.";
                } else {
                    for (const auto& candidate : candidates) {
                        if (candidate.state == DEVICE_STATE_ACTIVE && candidate.bluetooth) {
                            code = L"MIC_ONLY_BLUETOOTH";
                            detail = L"The only microphone is \"" + candidate.name +
                                L"\", which belongs to a Bluetooth device; using it would switch that speaker to call quality.";
                            break;
                        }
                    }
                }
                return false;
            }
        }
        name_ = chosen->name;
        IMMDevice* device = chosen->device.get();

        std::wstring privacy;
        if (microphone_privacy_denied(privacy)) {
            code = L"MIC_PRIVACY_BLOCKED";
            detail = privacy + L". Turn it on in Settings > Privacy & security > Microphone.";
            return false;
        }

        if (SUCCEEDED(device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(volume_.put())))) {
            BOOL muted = FALSE;
            if (SUCCEEDED(volume_->GetMute(&muted)) && muted) {
                code = L"MIC_MUTED";
                detail = L"\"" + name_ + L"\" is muted in Windows. Unmute it in Settings > System > Sound > Input.";
                return false;
            }
            volume_->GetMasterVolumeLevelScalar(&volume_level_);
        }

        // Raw mode skips Windows microphone effects (noise suppression etc.) where the driver allows it.
        auto initialize = [&](bool raw, WORD channels) {
            client_.reset();
            raw_ = false;
            HRESULT result = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(client_.put()));
            if (FAILED(result)) {
                return result;
            }
            if (raw) {
                ComPtr<IAudioClient2> client2;
                if (SUCCEEDED(client_->QueryInterface(__uuidof(IAudioClient2), reinterpret_cast<void**>(client2.put())))) {
                    AudioClientProperties properties = {};
                    properties.cbSize = sizeof(properties);
                    properties.bIsOffload = FALSE;
                    properties.eCategory = AudioCategory_Other;
                    properties.Options = AUDCLNT_STREAMOPTIONS_RAW;
                    raw_ = SUCCEEDED(client2->SetClientProperties(&properties));
                }
            }
            auto format = make_float_format(kMicRate, channels);
            result = client_->Initialize(
                AUDCLNT_SHAREMODE_SHARED,
                AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                kCaptureBufferDuration,
                0,
                format.get(),
                nullptr
            );
            if (SUCCEEDED(result)) {
                channels_ = channels;
            }
            return result;
        };
        hr = initialize(true, 1);
        if (FAILED(hr)) {
            hr = initialize(true, 2);
        }
        if (FAILED(hr)) {
            hr = initialize(false, 1);
        }
        if (FAILED(hr)) {
            hr = initialize(false, 2);
        }
        if (SUCCEEDED(hr)) {
            hr = client_->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void**>(capture_.put()));
        }
        if (SUCCEEDED(hr)) {
            hr = client_->Start();
        }
        if (FAILED(hr)) {
            if (hr == AUDCLNT_E_DEVICE_IN_USE) {
                code = L"MIC_IN_USE";
                detail = L"Another app is using \"" + name_ + L"\" exclusively. Close it and try again.";
            } else {
                code = L"MIC_DRIVER_ERROR";
                detail = L"Windows could not start \"" + name_ + L"\" (" + hr_text(hr) + L")." + kDriverHint;
            }
            stop();
            return false;
        }
        samples_.reserve(static_cast<size_t>(kMicRate * 30));
        return true;
    }

    void poll() {
        if (!capture_ || FAILED(error_)) {
            return;
        }
        UINT32 packet_frames = 0;
        HRESULT hr = capture_->GetNextPacketSize(&packet_frames);
        while (SUCCEEDED(hr) && packet_frames > 0) {
            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            UINT64 device_position = 0;
            UINT64 qpc_position = 0;
            hr = capture_->GetBuffer(&data, &frames, &flags, &device_position, &qpc_position);
            if (FAILED(hr) || hr == AUDCLNT_S_BUFFER_EMPTY) {
                break;
            }
            Packet packet;
            packet.first_sample = samples_.size();
            packet.frames = frames;
            packet.device_time = qpc_position != 0 && !(flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR);
            packet.time = packet.device_time
                ? static_cast<double>(qpc_position) / 1e7
                : qpc_seconds() - (static_cast<double>(frames) / kMicRate);
            packet.silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
            const float* input = reinterpret_cast<const float*>(data);
            for (UINT32 i = 0; i < frames; ++i) {
                float value = 0.0f;
                if (!packet.silent) {
                    for (WORD channel = 0; channel < channels_; ++channel) {
                        value += input[(static_cast<size_t>(i) * channels_) + channel];
                    }
                    value /= channels_;
                }
                samples_.push_back(value);
            }
            hr = capture_->ReleaseBuffer(frames);
            packets_.push_back(packet);
            if (SUCCEEDED(hr)) {
                hr = capture_->GetNextPacketSize(&packet_frames);
            }
        }
        if (FAILED(hr)) {
            error_ = hr;
        }
    }

    void stop() {
        if (client_) {
            client_->Stop();
        }
        capture_.reset();
        client_.reset();
        volume_.reset();
    }

    // Maps sample index to QPC seconds: time = offset + slope * index (least squares over packets).
    bool time_line(size_t from_sample, double& offset, double& slope, bool& device_times) const {
        size_t device_count = 0;
        for (const auto& packet : packets_) {
            if (packet.first_sample >= from_sample && packet.device_time) {
                ++device_count;
            }
        }
        device_times = device_count >= 10;
        double n = 0.0, sx = 0.0, sy = 0.0, sxx = 0.0, sxy = 0.0;
        double x0 = 0.0, y0 = 0.0;
        bool have_origin = false;
        for (const auto& packet : packets_) {
            if (packet.first_sample < from_sample || (device_times && !packet.device_time)) {
                continue;
            }
            if (!have_origin) {
                x0 = static_cast<double>(packet.first_sample);
                y0 = packet.time;
                have_origin = true;
            }
            const double x = static_cast<double>(packet.first_sample) - x0;
            const double y = packet.time - y0;
            n += 1.0;
            sx += x;
            sy += y;
            sxx += x * x;
            sxy += x * y;
        }
        const double denominator = (n * sxx) - (sx * sx);
        if (n < 2.0 || denominator <= 0.0) {
            return false;
        }
        slope = ((n * sxy) - (sx * sy)) / denominator;
        offset = y0 + ((sy - (slope * sx)) / n) - (slope * x0);
        return slope > 0.0;
    }

    double rms(size_t from, size_t to) const {
        to = std::min(to, samples_.size());
        if (to <= from) {
            return 0.0;
        }
        double sum = 0.0;
        double sum_squares = 0.0;
        for (size_t i = from; i < to; ++i) {
            sum += samples_[i];
            sum_squares += static_cast<double>(samples_[i]) * samples_[i];
        }
        const double n = static_cast<double>(to - from);
        const double mean = sum / n;
        return std::sqrt(std::max(0.0, (sum_squares / n) - (mean * mean)));
    }

    // Is the mic delivering a real, live signal? speakers_silent: the room should be quiet now.
    MicVerdict check(size_t from_sample, double elapsed_seconds, bool speakers_silent) const {
        MicVerdict verdict;
        auto fail = [&](const wchar_t* code, const std::wstring& detail) {
            verdict.ok = false;
            verdict.code = code;
            verdict.detail = detail;
            return verdict;
        };
        if (FAILED(error_)) {
            return fail(L"MIC_DRIVER_ERROR", L"The microphone stopped working while recording (" + hr_text(error_) + L")." + kDriverHint);
        }
        const size_t received = samples_.size() > from_sample ? samples_.size() - from_sample : 0;
        if (received == 0) {
            return fail(L"MIC_NO_DATA", L"Windows opened the microphone but its driver delivers no audio." + std::wstring(kDriverHint));
        }
        const double ratio = static_cast<double>(received) / std::max(0.001, elapsed_seconds * kMicRate);
        if (ratio < 0.8 || ratio > 1.25) {
            return fail(L"MIC_CLOCK_BAD", L"The microphone delivers " + format_number(ratio * 100.0, 0) +
                L"% of the audio it should." + kDriverHint);
        }
        double offset = 0.0;
        double slope = 0.0;
        bool device_times = false;
        if (time_line(from_sample, offset, slope, device_times) && device_times && std::abs((slope * kMicRate) - 1.0) > 0.02) {
            return fail(L"MIC_CLOCK_BAD", L"The microphone's clock runs " + format_number(slope * kMicRate * 100.0, 1) +
                L"% of normal speed." + kDriverHint);
        }

        size_t silent = 0;
        for (const auto& packet : packets_) {
            if (packet.first_sample >= from_sample && packet.silent) {
                silent += packet.frames;
            }
        }
        size_t zeros = 0;
        size_t clipped = 0;
        for (size_t i = from_sample; i < samples_.size(); ++i) {
            zeros += samples_[i] == 0.0f ? 1 : 0;
            clipped += std::abs(samples_[i]) >= 0.99f ? 1 : 0;
        }
        if (silent * 100 >= received * 95) {
            return fail(L"MIC_SILENT", L"Windows delivers only silence from \"" + name_ +
                L"\". Check a mute key or switch on the laptop and the microphone privacy setting; otherwise the driver is broken.");
        }
        if (zeros * 100 >= received * 95) {
            return fail(L"MIC_SILENT", L"\"" + name_ + L"\" delivers only digital zeros. Check a mute key or switch on the laptop "
                L"and the microphone privacy setting; otherwise the driver is broken.");
        }
        const double level = rms(from_sample, samples_.size());
        if (level < 1e-7) {
            return fail(L"MIC_STUCK", L"The microphone signal is frozen at a constant value." + std::wstring(kDriverHint));
        }
        if (clipped * 100 >= received) {
            return fail(L"MIC_CLIPPING", L"The microphone signal is mostly at full scale (garbage or extreme noise)." +
                std::wstring(kDriverHint));
        }
        verdict.level_dbfs = 20.0 * std::log10(level);
        if (speakers_silent && verdict.level_dbfs > -25.0) {
            verdict.warnings.emplace_back(L"MIC_NOISY", L"The room or the microphone is very noisy (" +
                format_number(verdict.level_dbfs, 0) + L" dBFS with the speakers silent); results may be unreliable.");
        }
        if (verdict.level_dbfs < -100.0) {
            verdict.warnings.emplace_back(L"MIC_VERY_QUIET", L"The microphone signal is extremely quiet (" +
                format_number(verdict.level_dbfs, 0) + L" dBFS).");
        }
        if (volume_level_ < 0.1f) {
            verdict.warnings.emplace_back(L"MIC_LEVEL_LOW", L"The microphone level in Windows is " +
                format_number(volume_level_ * 100.0, 0) + L"%; raise it in Settings > System > Sound > Input.");
        }
        return verdict;
    }

    // Self-test only: pretend these samples were recorded from start_time on.
    void load_for_test(std::vector<float> samples, double start_time, bool silent_flag) {
        samples_ = std::move(samples);
        packets_.clear();
        for (size_t first = 0; first < samples_.size(); first += 480) {
            Packet packet;
            packet.first_sample = first;
            packet.frames = std::min<size_t>(480, samples_.size() - first);
            packet.time = start_time + (static_cast<double>(first) / kMicRate);
            packet.device_time = true;
            packet.silent = silent_flag;
            packets_.push_back(packet);
        }
        name_ = L"test mic";
    }

    const std::wstring& name() const { return name_; }
    bool raw() const { return raw_; }
    float volume() const { return volume_level_; }
    const std::vector<float>& samples() const { return samples_; }

private:
    ComPtr<IAudioClient> client_;
    ComPtr<IAudioCaptureClient> capture_;
    ComPtr<IAudioEndpointVolume> volume_;
    std::wstring name_;
    bool raw_ = false;
    float volume_level_ = 1.0f;
    WORD channels_ = 1;
    std::vector<float> samples_;
    std::vector<Packet> packets_;
    HRESULT error_ = S_OK;
};

struct CalEvent {
    size_t output = 0;
    UINT64 offset_frames = 0;
    std::atomic<bool> pushed = false;
    double play_time = 0.0; // QPC seconds when the sweep start leaves Windows' buffer for this speaker.
    bool output_playing = false;
};

// While active, the capture thread feeds the outputs from here instead of the MAF source:
// silence, then one sweep per speaker per round, paced by the PC clock.
class CalibrationSignal {
public:
    void prepare(size_t output_count, UINT32 rate) {
        std::lock_guard<std::mutex> lock(mutex_);
        chirp_ = make_chirp(rate);
        events_ = std::vector<CalEvent>(output_count * kCalRounds);
        for (size_t i = 0; i < events_.size(); ++i) {
            events_[i].output = i % output_count;
            events_[i].offset_frames = static_cast<UINT64>(static_cast<double>(i) * kCalSlotSeconds * rate);
        }
        chirp_base_.store(-1);
        started_ = false;
        generated_ = 0;
        frames_generated_.store(0);
    }

    void activate() {
        active_.store(true, std::memory_order_release);
        g_calibration_active.store(true);
    }

    void deactivate() {
        active_.store(false, std::memory_order_release);
        g_calibration_active.store(false);
    }

    bool active() const { return active_.load(std::memory_order_acquire); }

    void enable_chirps(UINT32 rate) {
        chirp_base_.store(static_cast<long long>(frames_generated_.load()) + static_cast<long long>(kCalLeadSeconds * rate));
    }

    bool all_pushed() const {
        for (const auto& event : events_) {
            if (!event.pushed.load(std::memory_order_acquire)) {
                return false;
            }
        }
        return true;
    }

    double last_play_time() const {
        double latest = 0.0;
        for (const auto& event : events_) {
            latest = std::max(latest, event.play_time);
        }
        return latest;
    }

    const std::vector<CalEvent>& events() const { return events_; }

    // Capture thread.
    void generate(OutputList& outputs, UINT32 rate) {
        std::lock_guard<std::mutex> lock(mutex_);
        const double now = qpc_seconds();
        if (!started_) {
            started_ = true;
            start_time_ = now;
        }
        const UINT64 due = static_cast<UINT64>((now - start_time_) * rate);
        const long long base = chirp_base_.load();
        while (generated_ < due) {
            const UINT64 frames = std::min<UINT64>(due - generated_, 480);
            for (size_t o = 0; o < outputs.size(); ++o) {
                scratch_.assign(static_cast<size_t>(frames) * 2, 0.0f);
                for (auto& event : events_) {
                    if (base < 0 || event.output != o) {
                        continue;
                    }
                    const UINT64 start = static_cast<UINT64>(base) + event.offset_frames;
                    const UINT64 end = start + chirp_.size();
                    const UINT64 from = std::max(start, generated_);
                    const UINT64 to = std::min(end, generated_ + frames);
                    for (UINT64 f = from; f < to; ++f) {
                        const float value = chirp_[static_cast<size_t>(f - start)] * kCalChirpLevel;
                        scratch_[static_cast<size_t>(f - generated_) * 2] = value;
                        scratch_[(static_cast<size_t>(f - generated_) * 2) + 1] = value;
                    }
                    if (start >= generated_ && start < generated_ + frames) {
                        // Everything up to `due` is in the ring now; the sweep start is (due - start) frames
                        // before the newest frame, which leaves the device buffer pc_latency from now.
                        event.play_time = now + outputs[o]->pc_latency_seconds() - (static_cast<double>(due - start) / rate);
                        event.output_playing = outputs[o]->is_playing();
                        event.pushed.store(true, std::memory_order_release);
                    }
                }
                outputs[o]->push(scratch_.data(), static_cast<UINT32>(frames));
            }
            generated_ += frames;
        }
        frames_generated_.store(generated_);
    }

private:
    std::mutex mutex_;
    std::atomic<bool> active_ = false;
    std::atomic<long long> chirp_base_ = -1;
    std::atomic<UINT64> frames_generated_ = 0;
    std::vector<float> chirp_;
    std::vector<CalEvent> events_;
    std::vector<float> scratch_;
    bool started_ = false;
    double start_time_ = 0.0;
    UINT64 generated_ = 0;
};

CalibrationSignal g_cal_signal;

struct SweepResult {
    bool in_range = false;
    bool heard = false;
    double latency = 0.0;
    double snr = 0.0;
    double rise_db = -200.0;
};

SweepResult analyze_sweep(const MicRecorder& mic, double offset, double slope, const std::vector<float>& templ,
                          double play_time, double baseline_rms) {
    SweepResult result;
    const auto& samples = mic.samples();
    const long long first = std::llround((play_time - 0.05 - offset) / slope);
    const long long last = std::llround((play_time + kCalSearchSeconds + kCalChirpSeconds - offset) / slope);
    if (first < 0 || last > static_cast<long long>(samples.size()) || last - first <= static_cast<long long>(templ.size())) {
        return result;
    }
    result.in_range = true;
    const size_t size = static_cast<size_t>(last - first);
    const double level = mic.rms(static_cast<size_t>(first), static_cast<size_t>(last));
    result.rise_db = 20.0 * std::log10(std::max(level, 1e-12) / std::max(baseline_rms, 1e-12));

    const auto correlation = correlate(samples.data() + first, size, templ);
    size_t peak = 0;
    for (size_t lag = 1; lag < correlation.size(); ++lag) {
        if (std::abs(correlation[lag]) > std::abs(correlation[peak])) {
            peak = lag;
        }
    }
    const size_t guard = static_cast<size_t>(0.01 * kMicRate);
    double noise_squares = 0.0;
    size_t noise_count = 0;
    for (size_t lag = 0; lag < correlation.size(); ++lag) {
        if (lag + guard < peak || lag > peak + guard) {
            noise_squares += correlation[lag] * correlation[lag];
            ++noise_count;
        }
    }
    const double noise = std::sqrt(noise_squares / std::max<size_t>(1, noise_count));
    result.snr = noise > 0.0 ? std::abs(correlation[peak]) / noise : 0.0;
    result.latency = offset + (slope * static_cast<double>(first + static_cast<long long>(peak))) - play_time;
    result.heard = result.snr >= kCalMinSnr && result.latency > -0.02;
    return result;
}

void calibration_failed(const std::wstring& code, const std::wstring& detail) {
    CalReport("fail").text("code", code).text("detail", detail).emit();
}

void run_calibration_steps(OutputList& outputs, UINT32 source_rate, bool full) {
    CalReport("start").text("mode", full ? L"calibrate" : L"miccheck").emit();
    MicRecorder mic;
    std::wstring code;
    std::wstring detail;
    if (!mic.open(g_mic_selector, code, detail)) {
        calibration_failed(code, detail);
        return;
    }
    CalReport("mic").text("name", mic.name()).flag("raw", mic.raw()).number("volume", mic.volume()).emit();

    auto record_for = [&](double seconds) {
        const double until = qpc_seconds() + seconds;
        while (qpc_seconds() < until) {
            if (g_stop_requested) {
                return false;
            }
            mic.poll();
            Sleep(5);
        }
        mic.poll();
        return true;
    };

    if (full) {
        g_cal_signal.prepare(outputs.size(), source_rate);
        g_cal_signal.activate();
    }
    if (!record_for(full ? kCalPreSilenceSeconds : 0.3)) {
        calibration_failed(L"STOPPED", L"The engine stopped.");
        return;
    }
    const size_t baseline_from = mic.samples().size();
    const double baseline_start = qpc_seconds();
    if (!record_for(full ? kCalBaselineSeconds : kMicCheckSeconds)) {
        calibration_failed(L"STOPPED", L"The engine stopped.");
        return;
    }
    const MicVerdict verdict = mic.check(baseline_from, qpc_seconds() - baseline_start, full);
    for (const auto& [warning_code, warning_detail] : verdict.warnings) {
        CalReport("warning").text("code", warning_code).text("detail", warning_detail).emit();
    }
    if (!verdict.ok) {
        calibration_failed(verdict.code, verdict.detail);
        return;
    }
    CalReport("mic_ok").number("level_dbfs", verdict.level_dbfs).emit();
    if (!full) {
        CalReport("done").flag("applied", false).text("code", L"MIC_OK")
            .text("detail", L"The microphone \"" + mic.name() + L"\" works.").emit();
        return;
    }
    const double baseline_rms = mic.rms(baseline_from, mic.samples().size());

    g_cal_signal.enable_chirps(source_rate);
    CalReport("playing").integer("sweeps", static_cast<long long>(g_cal_signal.events().size())).emit();
    const double deadline = qpc_seconds() + kCalTimeoutSeconds;
    while (true) {
        if (g_stop_requested) {
            calibration_failed(L"STOPPED", L"The engine stopped.");
            return;
        }
        mic.poll();
        const double now = qpc_seconds();
        if (g_cal_signal.all_pushed() && now > g_cal_signal.last_play_time() + kCalSearchSeconds + kCalChirpSeconds + 0.3) {
            break;
        }
        if (now > deadline) {
            calibration_failed(L"TIMEOUT", L"The calibration sweeps did not finish playing.");
            return;
        }
        Sleep(5);
    }
    g_cal_signal.deactivate();
    mic.poll();
    const double recorded_until = qpc_seconds();
    mic.stop();

    // The mic must still have been delivering audio while the sweeps played.
    const MicVerdict after = mic.check(baseline_from, recorded_until - baseline_start, false);
    if (!after.ok && after.code != L"MIC_CLIPPING") {
        calibration_failed(after.code, L"During the sweeps: " + after.detail);
        return;
    }
    double offset = 0.0;
    double slope = 0.0;
    bool device_times = false;
    if (!mic.time_line(0, offset, slope, device_times)) {
        calibration_failed(L"MIC_CLOCK_BAD", L"The microphone's timing information is unusable." + std::wstring(kDriverHint));
        return;
    }

    struct OutputResult {
        std::vector<double> latencies;
        double best_snr = 0.0;
        int sweeps = 0;
        bool playing = false;
    };
    const auto templ = make_chirp(kMicRate);
    std::vector<OutputResult> results(outputs.size());
    bool any_heard = false;
    double max_rise = -200.0;
    for (const auto& event : g_cal_signal.events()) {
        auto& result = results[event.output];
        ++result.sweeps;
        result.playing = result.playing || event.output_playing;
        const SweepResult sweep = analyze_sweep(mic, offset, slope, templ, event.play_time, baseline_rms);
        if (!sweep.in_range) {
            continue;
        }
        max_rise = std::max(max_rise, sweep.rise_db);
        result.best_snr = std::max(result.best_snr, sweep.snr);
        if (sweep.heard) {
            result.latencies.push_back(sweep.latency);
            any_heard = true;
        }
    }

    if (!any_heard) {
        // Without a single positive result we cannot tell a deaf mic from silent speakers.
        for (size_t i = 0; i < outputs.size(); ++i) {
            CalReport("dev").integer("index", static_cast<long long>(i)).text("status", L"unknown")
                .number("snr", results[i].best_snr).emit();
        }
        if (max_rise >= 6.0) {
            calibration_failed(L"MIC_SOUND_UNRECOGNISED", L"The microphone level rose by " + format_number(max_rise, 0) +
                L" dB while the sweeps played, but they could not be recognised. Windows microphone effects (noise suppression, "
                L"voice focus, Studio Effects) are probably filtering them: turn them off for this microphone and try again.");
        } else {
            calibration_failed(L"MIC_HEARD_NOTHING", L"The microphone works but heard none of the speakers, so it cannot tell "
                L"whether the microphone or the speakers are at fault. Check that the speakers are on and loud enough, and "
                L"that the microphone is not covered.");
        }
        return;
    }

    // At least one speaker was heard, so the mic is proven to work in this run.
    bool all_ok = true;
    std::vector<double> inherent(outputs.size(), 0.0);
    for (size_t i = 0; i < outputs.size(); ++i) {
        auto& result = results[i];
        CalReport report("dev");
        report.integer("index", static_cast<long long>(i))
            .integer("heard", static_cast<long long>(result.latencies.size()))
            .integer("sweeps", result.sweeps)
            .number("snr", result.best_snr);
        if (result.latencies.empty()) {
            all_ok = false;
            if (result.playing) {
                report.text("status", L"not_heard").text("detail", L"Windows is sending audio to this speaker and the "
                    L"microphone heard the others, but not this one: check its volume, that it is on, and its distance.");
            } else {
                report.text("status", L"not_receiving").text("detail", L"This speaker was not receiving audio from the engine "
                    L"(disconnected or still opening).");
            }
        } else {
            std::sort(result.latencies.begin(), result.latencies.end());
            const double median = result.latencies[result.latencies.size() / 2];
            const double spread_ms = (result.latencies.back() - result.latencies.front()) * 1000.0;
            report.number("latency_ms", median * 1000.0).number("spread_ms", spread_ms);
            if (result.latencies.size() >= 2 && spread_ms <= kCalMaxSpreadMs) {
                report.text("status", L"ok");
                inherent[i] = median;
            } else {
                all_ok = false;
                report.text("status", L"unstable").text("detail", result.latencies.size() < 2
                    ? L"Heard only once; too few measurements."
                    : L"The measurements disagree; echoes or noise may be confusing the microphone.");
            }
        }
        report.emit();
    }
    if (!all_ok) {
        CalReport("done").flag("applied", false).text("code", L"INCOMPLETE")
            .text("detail", L"Not every speaker was measured reliably, so the delays were left unchanged.").emit();
        return;
    }
    if (outputs.size() < 2) {
        CalReport("done").flag("applied", false).text("code", L"SINGLE_OUTPUT")
            .text("detail", L"Only one speaker is selected; there is nothing to align.").emit();
        return;
    }
    const double slowest = *std::max_element(inherent.begin(), inherent.end());
    for (size_t i = 0; i < outputs.size(); ++i) {
        const double delay_ms = std::round((slowest - inherent[i]) * 10000.0) / 10.0;
        outputs[i]->set_raw_delay_ms(delay_ms);
        CalReport("delay").integer("index", static_cast<long long>(i)).number("ms", delay_ms).emit();
    }
    apply_delay_offsets(outputs);
    CalReport("done").flag("applied", true).text("code", L"OK").text("detail", L"Speakers aligned.").emit();
}

// Offline check of the mic verdicts and sweep detection with synthetic recordings.
int run_selftest() {
    int failures = 0;
    auto expect = [&](bool condition, const std::wstring& what) {
        log_line((condition ? L"PASS " : L"FAIL ") + what);
        failures += condition ? 0 : 1;
    };
    uint32_t seed = 12345;
    auto noise = [&seed](double level) {
        seed = (seed * 1664525u) + 1013904223u;
        return static_cast<float>(((static_cast<double>(seed) / 4294967295.0) - 0.5) * 2.0 * level);
    };

    const double start = 1000.0;
    const double seconds = 12.0;
    const size_t total = static_cast<size_t>(seconds * kMicRate);
    const auto chirp = make_chirp(kMicRate);
    const double latencies[2] = { 0.180, 0.2153 };
    std::vector<double> play_times;
    std::vector<float> room(total);
    for (auto& sample : room) {
        sample = noise(0.003); // About -55 dBFS room noise.
    }
    for (int round = 0; round < kCalRounds; ++round) {
        for (int speaker = 0; speaker < 2; ++speaker) {
            const double play = start + 2.3 + ((round * 2 + speaker) * kCalSlotSeconds);
            play_times.push_back(play);
            const double arrival = play + latencies[speaker];
            const size_t at = static_cast<size_t>((arrival - start) * kMicRate);
            for (size_t i = 0; i < chirp.size() && at + i < total; ++i) {
                room[at + i] += 0.02f * chirp[i];
                // A wall reflection 7 ms later at half strength.
                const size_t echo = at + i + static_cast<size_t>(0.007 * kMicRate);
                if (echo < total) {
                    room[echo] += 0.01f * chirp[i];
                }
            }
        }
    }

    MicRecorder mic;
    mic.load_for_test(room, start, false);
    const size_t baseline_to = static_cast<size_t>(2.0 * kMicRate);
    const MicVerdict quiet = mic.check(static_cast<size_t>(1.0 * kMicRate), seconds - 1.0, true);
    expect(quiet.ok, L"healthy mic passes the check (" + format_number(quiet.level_dbfs, 1) + L" dBFS)");
    double offset = 0.0;
    double slope = 0.0;
    bool device_times = false;
    expect(mic.time_line(0, offset, slope, device_times) && std::abs(slope * kMicRate - 1.0) < 1e-6, L"time line fits");
    const double baseline = mic.rms(static_cast<size_t>(1.0 * kMicRate), baseline_to);
    for (size_t e = 0; e < play_times.size(); ++e) {
        const SweepResult sweep = analyze_sweep(mic, offset, slope, chirp, play_times[e], baseline);
        const double error_ms = (sweep.latency - latencies[e % 2]) * 1000.0;
        expect(sweep.heard && std::abs(error_ms) < 0.1, L"sweep " + std::to_wstring(e) + L" heard, error " +
            format_number(error_ms, 3) + L" ms, snr " + format_number(sweep.snr, 1));
    }

    std::vector<float> deaf(total);
    for (auto& sample : deaf) {
        sample = noise(0.003);
    }
    mic.load_for_test(deaf, start, false);
    bool any_heard = false;
    double max_rise = -200.0;
    for (const double play : play_times) {
        const SweepResult sweep = analyze_sweep(mic, offset, slope, chirp, play, mic.rms(0, baseline_to));
        any_heard = any_heard || sweep.heard;
        max_rise = std::max(max_rise, sweep.rise_db);
    }
    expect(!any_heard && max_rise < 6.0, L"noise-only recording: nothing heard, level rise " + format_number(max_rise, 1) + L" dB");

    mic.load_for_test(std::vector<float>(total, 0.0f), start, false);
    const MicVerdict zeros = mic.check(0, seconds, true);
    expect(!zeros.ok && zeros.code == L"MIC_SILENT", L"all-zero mic reported as " + zeros.code);
    mic.load_for_test(std::vector<float>(total, 0.0f), start, true);
    const MicVerdict flagged = mic.check(0, seconds, true);
    expect(!flagged.ok && flagged.code == L"MIC_SILENT", L"silent-flagged mic reported as " + flagged.code);
    mic.load_for_test(std::vector<float>(total / 2, 0.01f), start, false);
    const MicVerdict slow = mic.check(0, seconds, true);
    expect(!slow.ok && slow.code == L"MIC_CLOCK_BAD", L"half-rate mic reported as " + slow.code);
    mic.load_for_test(std::vector<float>(total, 0.25f), start, false);
    const MicVerdict frozen = mic.check(0, seconds, true);
    expect(!frozen.ok && frozen.code == L"MIC_STUCK", L"frozen mic reported as " + frozen.code);

    log_line(failures == 0 ? L"selftest passed" : L"selftest FAILED: " + std::to_wstring(failures));
    return failures == 0 ? 0 : 5;
}

void run_calibration(OutputList* outputs, UINT32 source_rate, bool full) {
    HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    run_calibration_steps(*outputs, source_rate, full);
    g_cal_signal.deactivate();
    if (SUCCEEDED(co)) {
        CoUninitialize();
    }
}

void run_control_stdin(OutputList* outputs, bool exit_on_eof) {
    std::string line;
    while (!g_stop_requested && std::getline(std::cin, line)) {
        std::istringstream stream(line);
        std::string command;
        stream >> command;
        size_t index = 0;
        if (command == "master") {
            float value = 1.0f;
            if (stream >> value) {
                g_master_gain.store(std::max(0.0f, value));
            }
        } else if (command == "gain") {
            float value = 1.0f;
            if ((stream >> index >> value) && index < outputs->size()) {
                (*outputs)[index]->set_gain(value);
            }
        } else if (command == "mode") {
            std::string value;
            if ((stream >> index >> value) && index < outputs->size()) {
                (*outputs)[index]->set_channel_mode(parse_channel_mode(std::wstring(value.begin(), value.end())));
            }
        } else if (command == "delay") {
            double value = 0.0;
            if ((stream >> index >> value) && index < outputs->size()) {
                (*outputs)[index]->set_raw_delay_ms(value);
                apply_delay_offsets(*outputs);
            }
        } else if (command == "latency") {
            double value = kDefaultLatencyMs;
            if (stream >> value) {
                g_latency_ms.store(std::clamp(value, kMinLatencyMs, kMaxLatencyMs));
            }
        } else if (command == "sync") {
            int value = 1;
            if (stream >> value) {
                g_sync_enabled.store(value != 0);
            }
        } else if (command == "resync") {
            for (auto& output : *outputs) {
                output->request_resync();
            }
        } else if (command == "calibrate") {
            g_calibration_request.store(1);
        } else if (command == "miccheck") {
            g_calibration_request.store(2);
        } else if (command == "quit" || command == "stop") {
            g_stop_requested = true;
            break;
        }
    }
    if (exit_on_eof) {
        // The GUI went away (or asked to quit); do not linger as an orphan holding the speakers.
        g_stop_requested = true;
    }
}

class CaptureSource {
public:
    ~CaptureSource() { close(); }

    bool open(const std::wstring& selector, std::wstring& error) {
        close();
        std::vector<Endpoint> endpoints;
        try {
            endpoints = enumerate_endpoints(eRender);
        } catch (const std::exception&) {
            error = L"could not enumerate devices";
            return false;
        }
        Endpoint* source = find_endpoint(endpoints, selector);
        if (!source) {
            error = L"source endpoint not found: " + selector;
            return false;
        }

        HRESULT hr = source->device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(client_.put()));
        if (FAILED(hr)) {
            error = L"could not activate source " + hr_text(hr);
            return false;
        }
        WAVEFORMATEX* raw_format = nullptr;
        hr = client_->GetMixFormat(&raw_format);
        if (FAILED(hr)) {
            error = L"could not read source mix format " + hr_text(hr);
            close();
            return false;
        }
        WaveFormatPtr mix_format(raw_format);
        WaveFormatPtr engine_format = make_float_stereo_format(mix_format->nSamplesPerSec);
        hr = client_->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, kCaptureBufferDuration, 0, engine_format.get(), nullptr);
        if (FAILED(hr)) {
            // Source mix format is not float stereo: let Windows convert it.
            hr = source->device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(client_.put()));
            if (SUCCEEDED(hr)) {
                hr = client_->Initialize(
                    AUDCLNT_SHAREMODE_SHARED,
                    AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                    kCaptureBufferDuration,
                    0,
                    engine_format.get(),
                    nullptr
                );
            }
        }
        if (SUCCEEDED(hr)) {
            hr = client_->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void**>(capture_.put()));
        }
        if (SUCCEEDED(hr)) {
            hr = client_->Start();
        }
        if (FAILED(hr)) {
            error = L"could not start loopback capture " + hr_text(hr);
            close();
            return false;
        }

        id_ = source->id;
        name_ = source->name;
        mix_summary_ = format_summary(mix_format.get());
        rate_ = engine_format->nSamplesPerSec;
        return true;
    }

    void close() {
        if (client_) {
            client_->Stop();
        }
        capture_.reset();
        client_.reset();
    }

    bool is_open() const { return static_cast<bool>(capture_); }

    // forward=false drains the source without playing it (used while calibrating).
    HRESULT pump(OutputList& outputs, bool& got_data, bool forward) {
        UINT32 packet_frames = 0;
        HRESULT hr = capture_->GetNextPacketSize(&packet_frames);
        while (SUCCEEDED(hr) && packet_frames > 0) {
            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            hr = capture_->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
            if (FAILED(hr) || hr == AUDCLNT_S_BUFFER_EMPTY) {
                break;
            }
            const float* frame_data = (flags & AUDCLNT_BUFFERFLAGS_SILENT) ? nullptr : reinterpret_cast<const float*>(data);
            if (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) {
                ++glitches_;
            }
            for (auto& output : outputs) {
                if (forward) {
                    output->push(frame_data, frames);
                }
            }
            hr = capture_->ReleaseBuffer(frames);
            ++packets_;
            got_data = true;
            if (SUCCEEDED(hr)) {
                hr = capture_->GetNextPacketSize(&packet_frames);
            }
        }
        return FAILED(hr) ? hr : S_OK;
    }

    const std::wstring& id() const { return id_; }
    const std::wstring& name() const { return name_; }
    const std::wstring& mix_summary() const { return mix_summary_; }
    UINT32 rate() const { return rate_; }
    UINT64 packets() const { return packets_; }
    UINT64 glitches() const { return glitches_; }

private:
    ComPtr<IAudioClient> client_;
    ComPtr<IAudioCaptureClient> capture_;
    std::wstring id_;
    std::wstring name_;
    std::wstring mix_summary_;
    UINT32 rate_ = 48000;
    UINT64 packets_ = 0;
    UINT64 glitches_ = 0;
};

void list_devices() {
    auto renders = enumerate_endpoints(eRender);
    log_line(L"Render endpoints:");
    for (const auto& endpoint : renders) {
        log_line(L"  " + endpoint.name + L"  [" + endpoint.id + L"]");
        log_line(L"      bus=" + endpoint_property_text(endpoint.device.get(), PKEY_Device_EnumeratorName) +
            L" container=" + endpoint_property_text(endpoint.device.get(), PKEY_Device_ContainerId));
    }

    log_line(L"Microphones:");
    const auto bt_containers = bluetooth_containers();
    ComPtr<IMMDeviceEnumerator> enumerator;
    ComPtr<IMMDeviceCollection> collection;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                reinterpret_cast<void**>(enumerator.put()))) ||
        FAILED(enumerator->EnumAudioEndpoints(eCapture, DEVICE_STATEMASK_ALL, collection.put()))) {
        log_line(L"  <could not list microphones>");
        return;
    }
    UINT count = 0;
    collection->GetCount(&count);
    for (UINT i = 0; i < count; ++i) {
        ComPtr<IMMDevice> device;
        if (FAILED(collection->Item(i, device.put()))) {
            continue;
        }
        DWORD state = 0;
        device->GetState(&state);
        const std::wstring name = endpoint_name(device.get());
        log_line(L"  " + name + L"  [" + device_state_text(state) +
            (is_bluetooth_endpoint(device.get(), name, bt_containers) ? L", bluetooth" : L"") + L"]  [" + endpoint_id(device.get()) + L"]");
        if (state == DEVICE_STATE_ACTIVE) {
            log_line(L"      bus=" + endpoint_property_text(device.get(), PKEY_Device_EnumeratorName) +
                L" form=" + endpoint_property_text(device.get(), kFormFactorKey) +
                L" container=" + endpoint_property_text(device.get(), PKEY_Device_ContainerId) +
                L" desc=" + endpoint_property_text(device.get(), PKEY_Device_DeviceDesc));
        }
    }
}

int run_engine(const Args& args) {
    g_master_gain.store(std::max(0.0f, args.master_gain));
    g_latency_ms.store(args.latency_ms);
    g_sync_enabled.store(args.sync_enabled);
    g_mic_selector = args.mic_selector;
    if (args.outputs.empty()) {
        log_line(L"No outputs selected. Pass one or more --out arguments.");
        return 2;
    }

    CaptureSource capture;
    std::wstring error;
    std::wstring last_error;
    while (!g_stop_requested && !capture.open(args.source_selector, error)) {
        if (error != last_error) {
            log_line(L"src waiting: " + error);
            last_error = error;
        }
        sleep_unless_stopped(kCaptureRetryMs);
    }
    if (g_stop_requested) {
        return 0;
    }
    const UINT32 source_rate = capture.rate();
    g_source_id = capture.id();
    log_line(L"Source: " + capture.name() + L" (" + capture.mix_summary() + L")");
    log_line(L"Engine: " + std::to_wstring(source_rate) + L" Hz float stereo, target latency " +
        std::to_wstring(std::lround(args.latency_ms)) + L" ms, sync " + (args.sync_enabled ? L"on" : L"off"));

    // Intentionally never freed: the stdin thread may still be blocked in getline when we return.
    auto* outputs = new OutputList();
    for (size_t i = 0; i < args.outputs.size(); ++i) {
        outputs->push_back(std::make_unique<Output>(i, args.outputs[i], source_rate));
    }
    apply_delay_offsets(*outputs);
    for (auto& output : *outputs) {
        output->start();
    }
    std::thread(run_control_stdin, outputs, args.exit_on_stdin_eof).detach();

    MmcssScope mmcss;
    log_line(mmcss.enabled() ? L"MMCSS Audio scheduling enabled." : L"MMCSS unavailable; running normal priority.");
    log_line(L"Running.");

    int exit_code = 0;
    const auto started_at = std::chrono::steady_clock::now();
    auto last_status = started_at;
    auto last_reopen_attempt = started_at;
    bool calibrate_at_start = args.calibrate;
    std::thread calibration_thread;
    std::atomic<bool> calibration_running = false;
    while (!g_stop_requested) {
        const auto now = std::chrono::steady_clock::now();
        if (args.duration_seconds > 0 && now - started_at >= std::chrono::seconds(args.duration_seconds)) {
            break;
        }

        if (calibrate_at_start && now - started_at >= std::chrono::seconds(3)) {
            calibrate_at_start = false;
            g_calibration_request.store(1);
        }
        const int request = g_calibration_request.exchange(0);
        if (request != 0) {
            if (calibration_running.load()) {
                calibration_failed(L"BUSY", L"A calibration or mic check is already running.");
            } else {
                if (calibration_thread.joinable()) {
                    calibration_thread.join();
                }
                calibration_running = true;
                calibration_thread = std::thread([outputs, source_rate, request, &calibration_running] {
                    run_calibration(outputs, source_rate, request == 1);
                    calibration_running = false;
                });
            }
        }

        const bool calibrating = g_cal_signal.active();
        bool got_data = false;
        if (capture.is_open()) {
            HRESULT hr = capture.pump(*outputs, got_data, !calibrating);
            if (FAILED(hr)) {
                log_line(L"src lost " + hr_text(hr) + L", reopening");
                capture.close();
                last_reopen_attempt = now;
            }
        } else if (now - last_reopen_attempt >= std::chrono::milliseconds(kCaptureRetryMs)) {
            last_reopen_attempt = now;
            if (capture.open(args.source_selector, error)) {
                if (capture.rate() != source_rate) {
                    log_line(L"Error: source sample rate changed to " + std::to_wstring(capture.rate()) + L" Hz; restart required.");
                    exit_code = 4;
                    break;
                }
                log_line(L"src reopened");
                last_error.clear();
            } else if (error != last_error) {
                log_line(L"src waiting: " + error);
                last_error = error;
            }
        }
        if (calibrating) {
            g_cal_signal.generate(*outputs, source_rate);
        }
        if (!got_data || calibrating) {
            Sleep(2);
        }

        if (now - last_status >= std::chrono::seconds(1)) {
            last_status = now;
            log_line(L"src state=" + std::wstring(capture.is_open() ? L"running" : L"waiting") +
                L" packets=" + std::to_wstring(capture.packets()) + L" glitches=" + std::to_wstring(capture.glitches()));
            for (const auto& output : *outputs) {
                log_line(output->status_line());
            }
        }
    }

    g_stop_requested = true;
    if (calibration_thread.joinable()) {
        calibration_thread.join();
    }
    for (auto& output : *outputs) {
        output->join();
    }
    capture.close();
    log_line(L"Stopped.");
    return exit_code;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    SetConsoleCtrlHandler(console_handler, TRUE);
    SetConsoleOutputCP(CP_UTF8);
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr)) {
        log_line(L"COM initialization failed.");
        return 1;
    }

    int result = 0;
    try {
        Args args = parse_args(argc, argv);
        if (args.list) {
            list_devices();
        } else if (args.selftest) {
            result = run_selftest();
        } else {
            result = run_engine(args);
        }
    } catch (const std::exception& exc) {
        log_line(L"Error: " + std::wstring(exc.what(), exc.what() + std::strlen(exc.what())));
        print_usage();
        result = 1;
    }

    CoUninitialize();
    return result;
}
