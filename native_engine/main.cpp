#include <audioclient.h>
#include <avrt.h>
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
std::wstring g_source_id; // Written once before output threads start.

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
void log_line(const std::wstring& text) {
    std::string utf8 = to_utf8(text);
    std::lock_guard<std::mutex> lock(g_log_mutex);
    std::fwrite(utf8.data(), 1, utf8.size(), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
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

WaveFormatPtr make_float_stereo_format(DWORD sample_rate = 48000) {
    auto* format = static_cast<WAVEFORMATEX*>(CoTaskMemAlloc(sizeof(WAVEFORMATEXTENSIBLE)));
    if (!format) {
        throw std::bad_alloc();
    }
    ZeroMemory(format, sizeof(WAVEFORMATEXTENSIBLE));
    auto* extensible = reinterpret_cast<WAVEFORMATEXTENSIBLE*>(format);
    extensible->Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    extensible->Format.nChannels = 2;
    extensible->Format.nSamplesPerSec = sample_rate;
    extensible->Format.wBitsPerSample = 32;
    extensible->Format.nBlockAlign = extensible->Format.nChannels * extensible->Format.wBitsPerSample / 8;
    extensible->Format.nAvgBytesPerSec = extensible->Format.nSamplesPerSec * extensible->Format.nBlockAlign;
    extensible->Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    extensible->Samples.wValidBitsPerSample = 32;
    extensible->dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
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
        L"stdin commands: master <g> | gain <i> <g> | mode <i> <stereo|left|right> | delay <i> <ms> |\n"
        L"                latency <ms> | sync <0|1> | resync | quit"
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
    bool exit_on_stdin_eof = false;
    bool sync_enabled = true;
    int duration_seconds = 0;
    float master_gain = 1.0f;
    double latency_ms = kDefaultLatencyMs;
    std::wstring source_selector = L"MAF Virtual Audio Output";
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
        const float gain = gain_.load() * g_master_gain.load();
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

    HRESULT pump(OutputList& outputs, bool& got_data) {
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
                output->push(frame_data, frames);
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
    }
}

int run_engine(const Args& args) {
    g_master_gain.store(std::max(0.0f, args.master_gain));
    g_latency_ms.store(args.latency_ms);
    g_sync_enabled.store(args.sync_enabled);
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
    while (!g_stop_requested) {
        const auto now = std::chrono::steady_clock::now();
        if (args.duration_seconds > 0 && now - started_at >= std::chrono::seconds(args.duration_seconds)) {
            break;
        }

        if (!capture.is_open()) {
            if (now - last_reopen_attempt >= std::chrono::milliseconds(kCaptureRetryMs)) {
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
            Sleep(10);
        } else {
            bool got_data = false;
            HRESULT hr = capture.pump(*outputs, got_data);
            if (FAILED(hr)) {
                log_line(L"src lost " + hr_text(hr) + L", reopening");
                capture.close();
                last_reopen_attempt = now;
            } else if (!got_data) {
                Sleep(2);
            }
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
