#ifndef BBT_BBAP_WRITER_H
#define BBT_BBAP_WRITER_H

#include <cstddef>
#include <cstdint>
#include <string>

namespace bbap {

struct Configure {
    std::string receiver_name;
    std::string pin_mode;
    std::string interface_name;
    bool allow_hevc;
    bool audio_enabled;
    unsigned int width;
    unsigned int height;
    unsigned int refresh_rate;
    unsigned int max_fps;

    Configure();
};

typedef void (*ControlCallback)(const char *type);

bool stdio_initialize(unsigned int requested_major, std::string *error);
bool send_hello();
bool read_configure(Configure *configure, std::string *error);
void start_control_reader(ControlCallback callback);

bool send_state(const std::string &state, const std::string &pin = std::string(),
                const std::string &reason = std::string());
bool send_heartbeat();
bool send_error(const std::string &code, const std::string &message);
bool send_stopped();

void configure_video(unsigned int requested_fps);
void configure_audio(bool enabled);
void set_video_codec(bool is_h265);
void set_video_size(unsigned int width, unsigned int height);
void mark_video_discontinuity();
void reset_video();
bool push_video(const unsigned char *data, std::size_t size, std::uint64_t remote_ntp_ns);

void set_audio_format(unsigned char compression_type, unsigned short samples_per_frame);
void reset_audio();
void reset_media();
bool push_audio(const unsigned char *data, std::size_t size, unsigned char compression_type,
                unsigned short sequence, std::uint64_t remote_ntp_ns);

}  // namespace bbap

#endif
