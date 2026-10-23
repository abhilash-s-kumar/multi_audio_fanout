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
#include <cstring>
#include <cwctype>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

constexpr REFERENCE_TIME kBufferDuration = 2000000; // 200 ms in 100 ns units.
constexpr UINT32 kRenderSafetyFrames = 256;
std::atomic<bool> g_stop_requested = false;
std::atomic<float> g_master_gain = 1.0f;

BOOL WINAPI console_handler(DWORD event) {
    if (event == CTRL_C_EVENT || event == CTRL_BREAK_EVENT || event == CTRL_CLOSE_EVENT) {
        g_stop_requested = true;
        return TRUE;
    }
    return FALSE;
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

bool is_float_stereo(const WAVEFORMATEX* format) {
    if (!format || format->nChannels != 2 || format->wBitsPerSample != 32) {
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

WaveFormatPtr clone_wave_format(const WAVEFORMATEX* input) {
    size_t size = sizeof(WAVEFORMATEX) + input->cbSize;
    auto* format = static_cast<WAVEFORMATEX*>(CoTaskMemAlloc(size));
    if (!format) {
        throw std::bad_alloc();
    }
    std::memcpy(format, input, size);
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
        LPWSTR id = nullptr;
        if (SUCCEEDED(endpoint.device->GetId(&id))) {
            endpoint.id = id;
            CoTaskMemFree(id);
        }
        endpoint.name = endpoint_name(endpoint.device.get());
        endpoints.push_back(std::move(endpoint));
    }
    return endpoints;
}

Endpoint* find_endpoint(std::vector<Endpoint>& endpoints, const std::wstring& selector) {
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

void apply_channel_mode(float* frames, UINT32 frame_count, ChannelMode mode) {
    if (mode == ChannelMode::Stereo) {
        return;
    }
    for (UINT32 i = 0; i < frame_count; ++i) {
        float* frame = frames + (i * 2);
        const float value = mode == ChannelMode::Left ? frame[0] : frame[1];
        frame[0] = value;
        frame[1] = value;
    }
}

void apply_gain(float* frames, UINT32 sample_count, float gain) {
    if (std::abs(gain - 1.0f) < 0.0001f) {
        return;
    }
    for (UINT32 i = 0; i < sample_count; ++i) {
        frames[i] *= gain;
    }
}

void print_usage() {
    std::wcout
        << L"maf_native_engine --list\n"
        << L"maf_native_engine --source <name-substring> --out <name-substring>[:stereo|left|right][:gain] ... [--master <gain>] [--duration <seconds>]\n\n"
        << L"Example:\n"
        << L"  maf_native_engine --source \"MAF Virtual Audio Output\" --master 0.8 --out \"Motion Boom:left:1.0\" --out \"X600:right:1.0\" --duration 20\n";
}

struct OutputSpec {
    std::wstring selector;
    ChannelMode channel_mode = ChannelMode::Stereo;
    float gain = 1.0f;
};

OutputSpec parse_output_spec(const std::wstring& spec) {
    OutputSpec output;
    size_t first = spec.find(L':');
    if (first == std::wstring::npos) {
        output.selector = spec;
        return output;
    }
    output.selector = spec.substr(0, first);
    size_t second = spec.find(L':', first + 1);
    auto mode = spec.substr(first + 1, second == std::wstring::npos ? std::wstring::npos : second - first - 1);
    output.channel_mode = parse_channel_mode(mode);
    if (second != std::wstring::npos) {
        try {
            output.gain = std::stof(spec.substr(second + 1));
        } catch (...) {
            output.gain = 1.0f;
        }
    }
    return output;
}

struct Args {
    bool list = false;
    int duration_seconds = 0;
    float master_gain = 1.0f;
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
        throw std::runtime_error("Unknown or incomplete argument.");
    }
    return args;
}

class RenderClient {
public:
    RenderClient(Endpoint* endpoint, UINT32 source_sample_rate, ChannelMode channel_mode, float gain)
        : endpoint_(endpoint), channel_mode_(channel_mode), gain_(gain) {
        HRESULT hr = endpoint_->device->Activate(
            __uuidof(IAudioClient),
            CLSCTX_ALL,
            nullptr,
            reinterpret_cast<void**>(audio_client_.put())
        );
        if (FAILED(hr)) {
            throw std::runtime_error("Could not activate render audio client.");
        }

        WAVEFORMATEX* raw_format = nullptr;
        hr = audio_client_->GetMixFormat(&raw_format);
        if (FAILED(hr)) {
            throw std::runtime_error("Could not read render mix format.");
        }
        mix_format_ = WaveFormatPtr(raw_format);
        source_sample_rate_ = source_sample_rate;
        output_sample_rate_ = mix_format_->nSamplesPerSec;
        output_channels_ = mix_format_->nChannels;
        output_bits_per_sample_ = mix_format_->wBitsPerSample;
        output_is_float_ = is_float_format(mix_format_.get());
        output_is_pcm_ = is_pcm_format(mix_format_.get());

        if (output_channels_ < 1 || output_channels_ > 8) {
            throw std::runtime_error("Unsupported render channel count.");
        }
        if (!output_is_float_ && !output_is_pcm_) {
            throw std::runtime_error("Unsupported render sample format.");
        }
        if (output_is_float_ && output_bits_per_sample_ != 32) {
            throw std::runtime_error("Unsupported render float width.");
        }
        if (output_is_pcm_ && output_bits_per_sample_ != 16 && output_bits_per_sample_ != 24 && output_bits_per_sample_ != 32) {
            throw std::runtime_error("Unsupported render PCM width.");
        }

        hr = audio_client_->Initialize(
            AUDCLNT_SHAREMODE_SHARED,
            0,
            kBufferDuration,
            0,
            mix_format_.get(),
            nullptr
        );
        if (FAILED(hr)) {
            throw std::runtime_error("Could not initialize render client with its mix format.");
        }

        audio_client_->GetBufferSize(&buffer_frames_);
        hr = audio_client_->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<void**>(render_client_.put()));
        if (FAILED(hr)) {
            throw std::runtime_error("Could not get IAudioRenderClient.");
        }
    }

    void start() {
        audio_client_->Start();
    }

    void stop() {
        audio_client_->Stop();
    }

    void render(const float* frames, UINT32 frame_count) {
        UINT32 padding = 0;
        if (FAILED(audio_client_->GetCurrentPadding(&padding))) {
            return;
        }
        UINT32 available = buffer_frames_ > padding ? buffer_frames_ - padding : 0;
        if (available <= kRenderSafetyFrames) {
            dropped_packets_++;
            return;
        }
        UINT32 converted_frames = converted_frame_count(frame_count);
        UINT32 writable = std::min(converted_frames, available - kRenderSafetyFrames);
        BYTE* buffer = nullptr;
        if (FAILED(render_client_->GetBuffer(writable, &buffer))) {
            return;
        }
        write_converted(buffer, writable, frames, frame_count);
        render_client_->ReleaseBuffer(writable, 0);
    }

    const std::wstring& name() const { return endpoint_->name; }
    const WAVEFORMATEX* mix_format() const { return mix_format_.get(); }
    UINT64 dropped_packets() const { return dropped_packets_; }
    ChannelMode channel_mode() const { return channel_mode_; }
    void set_gain(float gain) { gain_.store(std::max(0.0f, gain)); }

private:
    UINT32 converted_frame_count(UINT32 input_frames) const {
        const double ratio = static_cast<double>(output_sample_rate_) / static_cast<double>(source_sample_rate_);
        return std::max<UINT32>(1, static_cast<UINT32>(std::llround(input_frames * ratio)));
    }

    float sample_channel(const float* frames, UINT32 input_frames, double position, int channel) const {
        if (input_frames == 0) {
            return 0.0f;
        }
        if (position <= 0.0) {
            return frames[channel];
        }
        double max_pos = static_cast<double>(input_frames - 1);
        if (position >= max_pos) {
            return frames[(input_frames - 1) * 2 + channel];
        }
        UINT32 left_index = static_cast<UINT32>(position);
        UINT32 right_index = left_index + 1;
        float fraction = static_cast<float>(position - left_index);
        float left = frames[left_index * 2 + channel];
        float right = frames[right_index * 2 + channel];
        return left + ((right - left) * fraction);
    }

    void write_converted(BYTE* buffer, UINT32 output_frames, const float* input, UINT32 input_frames) const {
        const double source_step = static_cast<double>(source_sample_rate_) / static_cast<double>(output_sample_rate_);
        if (output_is_float_) {
            auto* out = reinterpret_cast<float*>(buffer);
            for (UINT32 frame = 0; frame < output_frames; ++frame) {
                write_float_frame(out + (frame * output_channels_), sample_frame(input, input_frames, frame * source_step));
            }
            return;
        }

        if (output_bits_per_sample_ == 16) {
            auto* out = reinterpret_cast<int16_t*>(buffer);
            for (UINT32 frame = 0; frame < output_frames; ++frame) {
                write_int16_frame(out + (frame * output_channels_), sample_frame(input, input_frames, frame * source_step));
            }
            return;
        }

        if (output_bits_per_sample_ == 24) {
            for (UINT32 frame = 0; frame < output_frames; ++frame) {
                write_int24_frame(buffer + (frame * output_channels_ * 3), sample_frame(input, input_frames, frame * source_step));
            }
            return;
        }

        auto* out = reinterpret_cast<int32_t*>(buffer);
        for (UINT32 frame = 0; frame < output_frames; ++frame) {
            write_int32_frame(out + (frame * output_channels_), sample_frame(input, input_frames, frame * source_step));
        }
    }

    std::pair<float, float> sample_frame(const float* input, UINT32 input_frames, double position) const {
        float left = sample_channel(input, input_frames, position, 0);
        float right = sample_channel(input, input_frames, position, 1);
        if (channel_mode_ == ChannelMode::Left) {
            right = left;
        } else if (channel_mode_ == ChannelMode::Right) {
            left = right;
        }
        const float gain = gain_.load() * g_master_gain.load();
        left = std::clamp(left * gain, -1.0f, 1.0f);
        right = std::clamp(right * gain, -1.0f, 1.0f);
        return { left, right };
    }

    void write_float_frame(float* out, std::pair<float, float> frame) const {
        out[0] = frame.first;
        if (output_channels_ > 1) {
            out[1] = frame.second;
        }
        for (WORD channel = 2; channel < output_channels_; ++channel) {
            out[channel] = 0.0f;
        }
    }

    void write_int16_frame(int16_t* out, std::pair<float, float> frame) const {
        out[0] = static_cast<int16_t>(std::clamp(frame.first, -1.0f, 1.0f) * 32767.0f);
        if (output_channels_ > 1) {
            out[1] = static_cast<int16_t>(std::clamp(frame.second, -1.0f, 1.0f) * 32767.0f);
        }
        for (WORD channel = 2; channel < output_channels_; ++channel) {
            out[channel] = 0;
        }
    }

    void write_int24_frame(BYTE* out, std::pair<float, float> frame) const {
        auto write_one = [](BYTE* target, float value) {
            int32_t scaled = static_cast<int32_t>(std::clamp(value, -1.0f, 1.0f) * 8388607.0f);
            target[0] = static_cast<BYTE>(scaled & 0xff);
            target[1] = static_cast<BYTE>((scaled >> 8) & 0xff);
            target[2] = static_cast<BYTE>((scaled >> 16) & 0xff);
        };
        write_one(out, frame.first);
        if (output_channels_ > 1) {
            write_one(out + 3, frame.second);
        }
        for (WORD channel = 2; channel < output_channels_; ++channel) {
            write_one(out + (channel * 3), 0.0f);
        }
    }

    void write_int32_frame(int32_t* out, std::pair<float, float> frame) const {
        out[0] = static_cast<int32_t>(std::clamp(frame.first, -1.0f, 1.0f) * 2147483647.0f);
        if (output_channels_ > 1) {
            out[1] = static_cast<int32_t>(std::clamp(frame.second, -1.0f, 1.0f) * 2147483647.0f);
        }
        for (WORD channel = 2; channel < output_channels_; ++channel) {
            out[channel] = 0;
        }
    }

    Endpoint* endpoint_;
    ChannelMode channel_mode_;
    std::atomic<float> gain_;
    ComPtr<IAudioClient> audio_client_;
    ComPtr<IAudioRenderClient> render_client_;
    WaveFormatPtr mix_format_;
    UINT32 source_sample_rate_ = 48000;
    UINT32 output_sample_rate_ = 48000;
    WORD output_channels_ = 2;
    WORD output_bits_per_sample_ = 32;
    bool output_is_float_ = true;
    bool output_is_pcm_ = false;
    UINT32 buffer_frames_ = 0;
    UINT64 dropped_packets_ = 0;
};

void run_control_stdin(std::vector<std::unique_ptr<RenderClient>>* outputs) {
    std::string line;
    while (!g_stop_requested && std::getline(std::cin, line)) {
        std::istringstream stream(line);
        std::string command;
        stream >> command;
        if (command == "master") {
            float value = 1.0f;
            if (stream >> value) {
                g_master_gain.store(std::max(0.0f, value));
            }
        } else if (command == "gain") {
            size_t index = 0;
            float value = 1.0f;
            if ((stream >> index >> value) && index < outputs->size()) {
                (*outputs)[index]->set_gain(value);
            }
        } else if (command == "quit" || command == "stop") {
            g_stop_requested = true;
            break;
        }
    }
}

void list_devices() {
    auto renders = enumerate_endpoints(eRender);
    std::wcout << L"Render endpoints:\n";
    for (const auto& endpoint : renders) {
        std::wcout << L"  " << endpoint.name << L"\n";
    }
}

int run_engine(const Args& args) {
    g_master_gain.store(std::max(0.0f, args.master_gain));
    auto renders = enumerate_endpoints(eRender);
    Endpoint* source = find_endpoint(renders, args.source_selector);
    if (!source) {
        std::wcerr << L"Source render endpoint not found: " << args.source_selector << L"\n";
        return 2;
    }
    if (args.outputs.empty()) {
        std::wcerr << L"No outputs selected. Pass one or more --out arguments.\n";
        return 2;
    }

    ComPtr<IAudioClient> capture_client;
    HRESULT hr = source->device->Activate(
        __uuidof(IAudioClient),
        CLSCTX_ALL,
        nullptr,
        reinterpret_cast<void**>(capture_client.put())
    );
    if (FAILED(hr)) {
        std::wcerr << L"Could not activate source endpoint.\n";
        return 3;
    }

    WAVEFORMATEX* raw_source_format = nullptr;
    hr = capture_client->GetMixFormat(&raw_source_format);
    if (FAILED(hr)) {
        std::wcerr << L"Could not read source mix format.\n";
        return 3;
    }
    WaveFormatPtr source_mix_format(raw_source_format);
    WaveFormatPtr engine_format = make_float_stereo_format(source_mix_format->nSamplesPerSec);
    std::wcout << L"Source: " << source->name << L" (" << format_summary(source_mix_format.get()) << L")\n";
    std::wcout << L"Engine format: " << format_summary(engine_format.get()) << L"\n";

    std::vector<std::unique_ptr<RenderClient>> outputs;
    for (const auto& spec : args.outputs) {
        Endpoint* endpoint = find_endpoint(renders, spec.selector);
        if (!endpoint) {
            std::wcerr << L"Output endpoint not found: " << spec.selector << L"\n";
            return 2;
        }
        if (endpoint == source) {
            std::wcerr << L"Refusing to render back into the source endpoint: " << endpoint->name << L"\n";
            return 2;
        }
        outputs.push_back(std::make_unique<RenderClient>(endpoint, engine_format->nSamplesPerSec, spec.channel_mode, spec.gain));
        std::wcout << L"Output: " << endpoint->name << L" channel=" << channel_mode_name(spec.channel_mode)
                   << L" gain=" << spec.gain << L" mix=(" << format_summary(outputs.back()->mix_format()) << L")\n";
    }

    std::thread control_thread(run_control_stdin, &outputs);
    hr = capture_client->Initialize(
        AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_LOOPBACK,
        kBufferDuration,
        0,
        engine_format.get(),
        nullptr
    );
    if (FAILED(hr)) {
        std::wcerr << L"Could not initialize loopback capture.\n";
        return 3;
    }

    ComPtr<IAudioCaptureClient> capture;
    hr = capture_client->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void**>(capture.put()));
    if (FAILED(hr)) {
        std::wcerr << L"Could not get IAudioCaptureClient.\n";
        return 3;
    }

    MmcssScope mmcss;
    std::wcout << (mmcss.enabled() ? L"MMCSS Audio scheduling enabled.\n" : L"MMCSS unavailable; running normal priority.\n");

    for (auto& output : outputs) {
        output->start();
    }
    capture_client->Start();
    std::wcout << L"Running. Press Ctrl+C to stop.\n";

    auto started_at = std::chrono::steady_clock::now();
    auto last_status = std::chrono::steady_clock::now();
    UINT64 captured_packets = 0;
    std::vector<float> silent_frames;
    while (!g_stop_requested) {
        if (args.duration_seconds > 0 &&
            std::chrono::steady_clock::now() - started_at >= std::chrono::seconds(args.duration_seconds)) {
            break;
        }
        UINT32 packet_frames = 0;
        capture->GetNextPacketSize(&packet_frames);
        if (packet_frames == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        while (packet_frames > 0) {
            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            if (FAILED(capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr))) {
                break;
            }
            const float* frame_data = reinterpret_cast<const float*>(data);
            if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                silent_frames.assign(frames * 2, 0.0f);
                frame_data = silent_frames.data();
            }
            for (auto& output : outputs) {
                output->render(frame_data, frames);
            }
            capture->ReleaseBuffer(frames);
            captured_packets++;
            capture->GetNextPacketSize(&packet_frames);
        }

        auto now = std::chrono::steady_clock::now();
        if (now - last_status > std::chrono::seconds(5)) {
            std::wcout << L"status packets=" << captured_packets;
            for (const auto& output : outputs) {
                std::wcout << L" | " << output->name() << L" dropped=" << output->dropped_packets();
            }
            std::wcout << L"\n";
            last_status = now;
        }
    }

    capture_client->Stop();
    for (auto& output : outputs) {
        output->stop();
    }
    if (control_thread.joinable()) {
        control_thread.detach();
    }
    std::wcout << L"Stopped.\n";
    return 0;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    SetConsoleCtrlHandler(console_handler, TRUE);
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr)) {
        std::wcerr << L"COM initialization failed.\n";
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
        std::cerr << "Error: " << exc.what() << "\n";
        print_usage();
        result = 1;
    }

    CoUninitialize();
    return result;
}
