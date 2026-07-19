#include "bbap_writer.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <regex>
#include <thread>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#else
#include <unistd.h>
#endif

namespace bbap {
namespace {

const std::uint16_t kMajor = 1;
const std::uint16_t kMinor = 0;
const std::uint32_t kMaxControl = 64U << 10;

enum MessageType {
    kHello = 1,
    kState = 2,
    kVideoSession = 3,
    kVideoConfig = 4,
    kVideoFrame = 5,
    kAudioSession = 6,
    kAudioFrame = 7,
    kError = 9,
    kStopped = 10,
};

enum FrameFlags {
    kKeyframe = 1 << 0,
    kDiscontinuity = 1 << 1,
    kConfigAttached = 1 << 2,
};

int g_output_fd = -1;
std::mutex g_write_mutex;
std::uint64_t g_sequence = 0;

std::mutex g_state_mutex;
std::string g_last_state;
std::string g_last_pin;
std::string g_last_reason;

std::mutex g_video_mutex;
bool g_h265 = false;
bool g_have_codec = false;
unsigned int g_width = 0;
unsigned int g_height = 0;
unsigned int g_requested_fps = 60;
std::vector<unsigned char> g_config;
bool g_session_sent = false;
bool g_wait_keyframe = true;
bool g_pending_discontinuity = false;
bool g_streaming_sent = false;

std::mutex g_timeline_mutex;
bool g_have_pts_base = false;
std::uint64_t g_pts_base = 0;

std::mutex g_audio_mutex;
bool g_audio_enabled = false;
unsigned char g_audio_ct = 0;
unsigned short g_audio_spf = 0;
std::uint32_t g_audio_stream_id = 0;
bool g_audio_session_sent = false;
bool g_audio_have_seq = false;
unsigned short g_audio_last_seq = 0;
std::uint32_t g_audio_extended_seq = 0;
std::uint64_t g_audio_frames_sent = 0;

void put_u16(unsigned char *p, std::uint16_t value) {
    p[0] = static_cast<unsigned char>(value);
    p[1] = static_cast<unsigned char>(value >> 8);
}

void put_u32(unsigned char *p, std::uint32_t value) {
    for (int i = 0; i < 4; ++i) p[i] = static_cast<unsigned char>(value >> (8 * i));
}

void put_u64(unsigned char *p, std::uint64_t value) {
    for (int i = 0; i < 8; ++i) p[i] = static_cast<unsigned char>(value >> (8 * i));
}

std::uint32_t get_u32(const unsigned char *p) {
    return static_cast<std::uint32_t>(p[0]) |
           (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) |
           (static_cast<std::uint32_t>(p[3]) << 24);
}

std::uint64_t monotonic_ns() {
#ifdef _WIN32
    LARGE_INTEGER counter;
    LARGE_INTEGER frequency;
    if (QueryPerformanceCounter(&counter) && QueryPerformanceFrequency(&frequency) && frequency.QuadPart > 0) {
        const std::uint64_t whole = static_cast<std::uint64_t>(counter.QuadPart / frequency.QuadPart);
        const std::uint64_t remainder = static_cast<std::uint64_t>(counter.QuadPart % frequency.QuadPart);
        return whole * 1000000000ULL + remainder * 1000000000ULL /
               static_cast<std::uint64_t>(frequency.QuadPart);
    }
#endif
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

std::uint64_t normalize_media_pts(std::uint64_t remote_ntp_ns) {
    std::lock_guard<std::mutex> lock(g_timeline_mutex);
    if (!g_have_pts_base) {
        g_pts_base = remote_ntp_ns;
        g_have_pts_base = true;
    }
    return remote_ntp_ns >= g_pts_base ? remote_ntp_ns - g_pts_base : 0;
}

void reset_media_pts() {
    std::lock_guard<std::mutex> lock(g_timeline_mutex);
    g_have_pts_base = false;
    g_pts_base = 0;
}

bool write_all(const unsigned char *data, std::size_t size) {
    while (size != 0) {
#ifdef _WIN32
        const unsigned int chunk = static_cast<unsigned int>(std::min<std::size_t>(size, 1U << 30));
        const int written = _write(g_output_fd, data, chunk);
#else
        const ssize_t written = write(g_output_fd, data, size);
#endif
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) return false;
        data += written;
        size -= static_cast<std::size_t>(written);
    }
    return true;
}

bool read_all(unsigned char *data, std::size_t size) {
    while (size != 0) {
#ifdef _WIN32
        const unsigned int chunk = static_cast<unsigned int>(std::min<std::size_t>(size, 1U << 30));
        const int count = _read(_fileno(stdin), data, chunk);
#else
        const ssize_t count = read(STDIN_FILENO, data, size);
#endif
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return false;
        data += count;
        size -= static_cast<std::size_t>(count);
    }
    return true;
}

bool send_message(std::uint16_t type, std::uint16_t flags, std::uint64_t pts,
                  const unsigned char *payload, std::size_t payload_size) {
    if (g_output_fd < 0 || payload_size > (8U << 20)) return false;
    unsigned char header[40] = {};
    std::memcpy(header, "BBAP", 4);
    put_u16(header + 4, kMajor);
    put_u16(header + 6, kMinor);
    put_u16(header + 8, type);
    put_u16(header + 10, flags);
    put_u32(header + 12, static_cast<std::uint32_t>(payload_size));

    std::lock_guard<std::mutex> lock(g_write_mutex);
    put_u64(header + 16, ++g_sequence);
    put_u64(header + 24, pts);
    put_u64(header + 32, monotonic_ns());
    if (!write_all(header, sizeof(header))) return false;
    return payload_size == 0 || write_all(payload, payload_size);
}

bool send_json(std::uint16_t type, const std::string &json) {
    return send_message(type, 0, 0,
                        reinterpret_cast<const unsigned char *>(json.data()), json.size());
}

std::string json_escape(const std::string &value) {
    std::string out;
    out.reserve(value.size() + 8);
    for (std::string::const_iterator it = value.begin(); it != value.end(); ++it) {
        const unsigned char c = static_cast<unsigned char>(*it);
        switch (c) {
        case '\\': out += "\\\\"; break;
        case '"': out += "\\\""; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c >= 0x20) out.push_back(static_cast<char>(c));
            break;
        }
    }
    return out;
}

bool extract_string(const std::string &json, const char *key, std::string *value) {
    const std::regex re(std::string("\\\"") + key + "\\\"\\s*:\\s*\\\"((?:\\\\.|[^\\\"\\\\])*)\\\"");
    std::smatch match;
    if (!std::regex_search(json, match, re)) return false;
    std::string raw = match[1].str();
    value->clear();
    for (std::size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] != '\\' || i + 1 >= raw.size()) {
            value->push_back(raw[i]);
            continue;
        }
        const char escaped = raw[++i];
        switch (escaped) {
        case '"': case '\\': case '/': value->push_back(escaped); break;
        case 'b': value->push_back('\b'); break;
        case 'f': value->push_back('\f'); break;
        case 'n': value->push_back('\n'); break;
        case 'r': value->push_back('\r'); break;
        case 't': value->push_back('\t'); break;
        default: value->push_back('?'); break;
        }
    }
    return true;
}

bool extract_bool(const std::string &json, const char *key, bool *value) {
    const std::regex re(std::string("\\\"") + key + "\\\"\\s*:\\s*(true|false)");
    std::smatch match;
    if (!std::regex_search(json, match, re)) return false;
    *value = match[1].str() == "true";
    return true;
}

bool extract_uint(const std::string &json, const char *key, unsigned int *value) {
    const std::regex re(std::string("\\\"") + key + "\\\"\\s*:\\s*([0-9]+)");
    std::smatch match;
    if (!std::regex_search(json, match, re)) return false;
    try {
        *value = static_cast<unsigned int>(std::stoul(match[1].str()));
    } catch (...) {
        return false;
    }
    return true;
}

bool read_control_json(std::string *json) {
    unsigned char length_bytes[4];
    if (!read_all(length_bytes, sizeof(length_bytes))) return false;
    const std::uint32_t length = get_u32(length_bytes);
    if (length == 0 || length > kMaxControl) return false;
    json->assign(length, '\0');
    return read_all(reinterpret_cast<unsigned char *>(&(*json)[0]), length);
}

struct NalUnit {
    std::size_t payload;
    std::size_t end;
};

std::vector<NalUnit> split_annex_b(const unsigned char *data, std::size_t size) {
    std::vector<NalUnit> units;
    std::vector<std::size_t> starts;
    for (std::size_t i = 0; i + 3 < size;) {
        std::size_t prefix = 0;
        if (i + 4 <= size && data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 0 && data[i + 3] == 1) {
            prefix = 4;
        } else if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) {
            prefix = 3;
        }
        if (prefix != 0) {
            starts.push_back(i);
            i += prefix;
        } else {
            ++i;
        }
    }
    for (std::size_t i = 0; i < starts.size(); ++i) {
        const std::size_t start = starts[i];
        const std::size_t prefix = (start + 3 < size && data[start + 2] == 1) ? 3 : 4;
        const std::size_t end = i + 1 < starts.size() ? starts[i + 1] : size;
        if (start + prefix < end) units.push_back(NalUnit{start + prefix, end});
    }
    return units;
}

void append_config_nal(std::vector<unsigned char> *config, const unsigned char *data,
                       const NalUnit &unit) {
    static const unsigned char start_code[4] = {0, 0, 0, 1};
    config->insert(config->end(), start_code, start_code + 4);
    config->insert(config->end(), data + unit.payload, data + unit.end);
}

bool emit_video_session_locked() {
    if (!g_have_codec || g_width == 0 || g_height == 0) return false;
    unsigned char payload[24] = {};
    payload[0] = g_h265 ? 2 : 1;
    put_u32(payload + 4, g_width);
    put_u32(payload + 8, g_height);
    put_u32(payload + 12, g_requested_fps);
    put_u32(payload + 16, g_requested_fps ? 1 : 0);
    put_u32(payload + 20, 0);
    if (!send_message(kVideoSession, 0, 0, payload, sizeof(payload))) return false;
    g_session_sent = true;
    return true;
}

bool emit_audio_session_locked() {
    if (!g_audio_enabled || g_audio_ct != 8 || g_audio_spf == 0) return false;
    static const unsigned char aac_eld_config[4] = {0xf8, 0xe8, 0x50, 0x00};
    unsigned char payload[28] = {};
    payload[0] = 1;  // BBAP AudioCodecAACELD
    payload[1] = 2;
    payload[2] = 16;
    payload[3] = 0;
    put_u32(payload + 4, 44100);
    put_u32(payload + 8, g_audio_spf);
    put_u32(payload + 12, g_audio_stream_id);
    put_u32(payload + 16, sizeof(aac_eld_config));
    std::memcpy(payload + 24, aac_eld_config, sizeof(aac_eld_config));
    if (!send_message(kAudioSession, 0, 0, payload, sizeof(payload))) return false;
    g_audio_session_sent = true;
    return true;
}

}  // namespace

Configure::Configure()
    : receiver_name("BigBirdTouch"), pin_mode("off"), allow_hevc(false),
      audio_enabled(false), width(1280), height(720), refresh_rate(60), max_fps(60) {}

bool stdio_initialize(unsigned int requested_major, std::string *error) {
    if (requested_major != kMajor) {
        if (error) *error = "unsupported protocol major";
        return false;
    }
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
    g_output_fd = _dup(_fileno(stdout));
    if (g_output_fd < 0 || _setmode(g_output_fd, _O_BINARY) < 0 ||
        _dup2(_fileno(stderr), _fileno(stdout)) < 0) {
        if (error) *error = "failed to reserve binary stdout";
        return false;
    }
#else
    g_output_fd = dup(STDOUT_FILENO);
    if (g_output_fd < 0 || dup2(STDERR_FILENO, STDOUT_FILENO) < 0) {
        if (error) *error = "failed to reserve binary stdout";
        return false;
    }
#endif
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    return true;
}

bool send_hello() {
    return send_json(kHello,
        "{\"build\":\"bbt-airplay-receiver/0.1.0\","
        "\"upstream\":\"UxPlay/3ca7526387e894d6848b84c209de361c3bedd1ec\","
        "\"license\":\"GPL-3.0-or-later\",\"videoCodecs\":[\"h264\",\"hevc\"],"
        "\"audioCodecs\":[\"aac-eld\"],\"features\":[\"screen-mirroring\","
        "\"mirror-audio\",\"pin\"]}");
}

bool read_configure(Configure *configure, std::string *error) {
    if (!configure) return false;
    std::string json;
    if (!read_control_json(&json)) {
        if (error) *error = "failed to read configure control";
        return false;
    }
    std::string type;
    unsigned int version = 0;
    if (!extract_string(json, "type", &type) || type != "configure" ||
        !extract_uint(json, "version", &version) || version != 1) {
        if (error) *error = "first control is not configure v1";
        return false;
    }
    extract_string(json, "receiverName", &configure->receiver_name);
    extract_string(json, "pinMode", &configure->pin_mode);
    extract_string(json, "interface", &configure->interface_name);
    extract_bool(json, "allowHevc", &configure->allow_hevc);
    extract_bool(json, "audioEnabled", &configure->audio_enabled);
    extract_uint(json, "width", &configure->width);
    extract_uint(json, "height", &configure->height);
    extract_uint(json, "refreshRate", &configure->refresh_rate);
    extract_uint(json, "maxFps", &configure->max_fps);
    if (configure->receiver_name.empty()) configure->receiver_name = "BigBirdTouch";
    if (configure->width == 0 || configure->width > 8192 ||
        configure->height == 0 || configure->height > 8192 ||
        configure->refresh_rate == 0 || configure->refresh_rate > 240 ||
        configure->max_fps == 0 || configure->max_fps > 240) {
        if (error) *error = "invalid display request";
        return false;
    }
    return true;
}

void start_control_reader(ControlCallback callback) {
    std::thread([callback]() {
        std::string json;
        while (read_control_json(&json)) {
            std::string type;
            if (extract_string(json, "type", &type) && callback) callback(type.c_str());
        }
    }).detach();
}

bool send_state(const std::string &state, const std::string &pin, const std::string &reason) {
    {
        std::lock_guard<std::mutex> lock(g_state_mutex);
        g_last_state = state;
        g_last_pin = pin;
        g_last_reason = reason;
    }
    std::string json = "{\"state\":\"" + json_escape(state) + "\"";
    if (!pin.empty()) json += ",\"pin\":\"" + json_escape(pin) + "\"";
    if (!reason.empty()) json += ",\"reason\":\"" + json_escape(reason) + "\"";
    json += "}";
    return send_json(kState, json);
}

bool send_heartbeat() {
    std::string state, pin, reason;
    {
        std::lock_guard<std::mutex> lock(g_state_mutex);
        state = g_last_state;
        pin = g_last_pin;
        reason = g_last_reason;
    }
    return state.empty() || send_state(state, pin, reason);
}

bool send_error(const std::string &code, const std::string &message) {
    return send_json(kError, "{\"code\":\"" + json_escape(code) +
                     "\",\"message\":\"" + json_escape(message) + "\"}");
}

bool send_stopped() { return send_message(kStopped, 0, 0, NULL, 0); }

void configure_video(unsigned int requested_fps) {
    std::lock_guard<std::mutex> lock(g_video_mutex);
    g_requested_fps = requested_fps;
}

void configure_audio(bool enabled) {
    std::lock_guard<std::mutex> lock(g_audio_mutex);
    g_audio_enabled = enabled;
    if (!enabled) {
        g_audio_session_sent = false;
        g_audio_have_seq = false;
    }
}

void set_video_codec(bool is_h265) {
    std::lock_guard<std::mutex> lock(g_video_mutex);
    if (!g_have_codec || g_h265 != is_h265) {
        g_h265 = is_h265;
        g_have_codec = true;
        g_config.clear();
        g_session_sent = false;
        g_wait_keyframe = true;
        g_pending_discontinuity = false;
    }
}

void set_video_size(unsigned int width, unsigned int height) {
    std::lock_guard<std::mutex> lock(g_video_mutex);
    if (width != 0 && height != 0 && (g_width != width || g_height != height)) {
        g_width = width;
        g_height = height;
        g_session_sent = false;
        g_wait_keyframe = true;
        g_pending_discontinuity = false;
    }
}

void mark_video_discontinuity() {
    std::lock_guard<std::mutex> lock(g_video_mutex);
    /* Once an inter-frame AU is lost or fails decryption, every following P
     * frame may reference damaged state.  Drop until the next independently
     * decodable frame and tell Sender to rebuild its reference chain. */
    g_wait_keyframe = true;
    g_pending_discontinuity = true;
}

void reset_video() {
    std::lock_guard<std::mutex> lock(g_video_mutex);
    g_config.clear();
    g_session_sent = false;
    g_wait_keyframe = true;
    g_pending_discontinuity = false;
    g_streaming_sent = false;
}

bool push_video(const unsigned char *data, std::size_t size, std::uint64_t remote_ntp_ns) {
    if (!data || size == 0) return false;
    std::lock_guard<std::mutex> lock(g_video_mutex);
    const std::vector<NalUnit> units = split_annex_b(data, size);
    if (units.empty()) return false;

    bool keyframe = false;
    std::vector<unsigned char> found_config;
    for (std::vector<NalUnit>::const_iterator it = units.begin(); it != units.end(); ++it) {
        const unsigned char first = data[it->payload];
        const unsigned int type = g_h265 ? ((first >> 1) & 0x3f) : (first & 0x1f);
        const bool config_nal = g_h265 ? (type == 32 || type == 33 || type == 34)
                                       : (type == 7 || type == 8);
        if (config_nal) append_config_nal(&found_config, data, *it);
        if (g_h265 ? (type >= 16 && type <= 21) : (type == 5)) keyframe = true;
    }
    const bool config_attached = !found_config.empty();
    if (config_attached && found_config != g_config) {
        g_config.swap(found_config);
        g_session_sent = false;
        g_wait_keyframe = true;
        g_pending_discontinuity = false;
    }
    if (g_config.empty() || !g_have_codec || g_width == 0 || g_height == 0) return false;
    if (!g_session_sent) {
        if (!emit_video_session_locked()) return false;
        if (!send_message(kVideoConfig, 0, 0, g_config.data(), g_config.size())) return false;
    }
    if (g_wait_keyframe && !keyframe) return false;
    const std::uint64_t pts = normalize_media_pts(remote_ntp_ns);
    const unsigned char *frame_data = data;
    std::size_t frame_size = size;
    bool frame_has_config = config_attached;
    std::vector<unsigned char> recovered_frame;
    if (g_pending_discontinuity && keyframe && !frame_has_config) {
        recovered_frame.reserve(g_config.size() + size);
        recovered_frame.insert(recovered_frame.end(), g_config.begin(), g_config.end());
        recovered_frame.insert(recovered_frame.end(), data, data + size);
        frame_data = recovered_frame.data();
        frame_size = recovered_frame.size();
        frame_has_config = true;
    }
    std::uint16_t flags = keyframe ? kKeyframe : 0;
    if (frame_has_config) flags |= kConfigAttached;
    if (g_pending_discontinuity) flags |= kDiscontinuity;
    if (!send_message(kVideoFrame, flags, pts, frame_data, frame_size)) return false;
    if (keyframe) {
        g_wait_keyframe = false;
        g_pending_discontinuity = false;
    }
    if (!g_streaming_sent) {
        g_streaming_sent = true;
        send_state("streaming");
    }
    return true;
}

void set_audio_format(unsigned char compression_type, unsigned short samples_per_frame) {
    std::lock_guard<std::mutex> lock(g_audio_mutex);
    if (!g_audio_enabled) return;
    if (compression_type != 8) {
        g_audio_session_sent = false;
        send_error("unsupported_codec", "AirPlay audio codec is not AAC-ELD");
        return;
    }
    if (g_audio_ct != compression_type || g_audio_spf != samples_per_frame || !g_audio_session_sent) {
        g_audio_ct = compression_type;
        g_audio_spf = samples_per_frame ? samples_per_frame : 480;
        ++g_audio_stream_id;
        if (g_audio_stream_id == 0) ++g_audio_stream_id;
        g_audio_session_sent = false;
        g_audio_have_seq = false;
        g_audio_frames_sent = 0;
        emit_audio_session_locked();
        std::fprintf(stderr, "BBAP audio format ct=%u spf=%u stream=%u\n",
                     static_cast<unsigned int>(g_audio_ct),
                     static_cast<unsigned int>(g_audio_spf), g_audio_stream_id);
    }
}

void reset_audio() {
    std::lock_guard<std::mutex> lock(g_audio_mutex);
    g_audio_session_sent = false;
    g_audio_have_seq = false;
    g_audio_ct = 0;
    g_audio_spf = 0;
    g_audio_frames_sent = 0;
}

void reset_media() {
    reset_video();
    reset_audio();
    reset_media_pts();
}

bool push_audio(const unsigned char *data, std::size_t size, unsigned char compression_type,
                unsigned short sequence, std::uint64_t remote_ntp_ns) {
    if (!data || size == 0) return false;
    std::lock_guard<std::mutex> lock(g_audio_mutex);
    if (!g_audio_enabled || compression_type != 8 || g_audio_ct != 8) return false;
    /* The decrypted AAC-ELD access unit is opaque compressed data.  Its first
     * byte is content-dependent; filtering on a small byte allow-list drops
     * valid packets in long runs and eventually produces permanent silence. */
    if (!g_audio_session_sent && !emit_audio_session_locked()) return false;

    if (!g_audio_have_seq) {
        g_audio_extended_seq = sequence;
        g_audio_have_seq = true;
    } else {
        const unsigned short delta = static_cast<unsigned short>(sequence - g_audio_last_seq);
        if (delta == 0 || delta >= 0x8000) return false;
        g_audio_extended_seq += delta;
    }
    g_audio_last_seq = sequence;

    std::vector<unsigned char> payload(16 + size);
    put_u32(payload.data(), g_audio_stream_id);
    put_u32(payload.data() + 4, g_audio_extended_seq);
    put_u32(payload.data() + 8, g_audio_spf);
    put_u32(payload.data() + 12, static_cast<std::uint32_t>(size));
    std::memcpy(payload.data() + 16, data, size);
    const bool sent = send_message(kAudioFrame, 0, normalize_media_pts(remote_ntp_ns),
                                   payload.data(), payload.size());
    if (sent) {
        ++g_audio_frames_sent;
        if (g_audio_frames_sent == 1 || (g_audio_frames_sent % 256) == 0) {
            std::fprintf(stderr, "BBAP audio frames=%llu last_bytes=%u\n",
                         static_cast<unsigned long long>(g_audio_frames_sent),
                         static_cast<unsigned int>(size));
        }
    }
    return sent;
}

}  // namespace bbap
