/*
 * Renderer stubs for the BBAP-only AirPlay receiver sidecar.
 *
 * The sidecar forwards compressed video/audio to Sender and never renders via
 * GStreamer. Keeping these symbols as no-ops lets the upstream protocol code
 * remain intact without adding GStreamer DLLs or plugins to the distribution.
 */

#include <glib.h>

#include "renderers/audio_renderer.h"
#include "renderers/mux_renderer.h"
#include "renderers/video_renderer.h"

extern "C" {

bool gstreamer_init() { return true; }

void audio_renderer_init(logger_t *, const char *, const bool *, const bool *, const char *) {}
void audio_renderer_start(unsigned char *) {}
void audio_renderer_stop() {}
void audio_renderer_render_buffer(unsigned char *, int *, unsigned short *, uint64_t *) {}
void audio_renderer_set_volume(double) {}
void audio_renderer_flush() {}
void audio_renderer_destroy() {}
unsigned int audio_renderer_listen(void *, int) { return 0; }

void video_renderer_init(logger_t *, const char *, videoflip_t[2], const char *, const char *,
                         const char *, const char *, const char *, const char *, bool, bool,
                         bool, bool, guint, const char *) {}
void video_renderer_start() {}
void video_renderer_stop() {}
void video_renderer_set_device_model(const char *, const char *) {}
void video_renderer_set_track_metadata(const char *, const char *, const char *) {}
void video_renderer_pause() {}
void video_renderer_hls_ready() {}
void video_renderer_seek(float) {}
void video_renderer_set_start(float) {}
void video_renderer_resume() {}
int video_renderer_cycle() { return 0; }
bool video_renderer_is_paused() { return false; }
bool video_renderer_eos_watch() { return false; }
uint64_t video_renderer_render_buffer(unsigned char *, int *, int *, uint64_t *) { return 0; }
void video_renderer_display_jpeg(const void *, int *) {}
void video_renderer_flush() {}
unsigned int video_renderer_listen(void *, int) { return 0; }
void video_renderer_destroy() {}
void video_renderer_size(float *width_source, float *height_source, float *width, float *height) {
    if (width_source) *width_source = 0;
    if (height_source) *height_source = 0;
    if (width) *width = 0;
    if (height) *height = 0;
}
bool waiting_for_x11_window() { return false; }
bool video_get_playback_info(double *, double *, double *, double *, float *, bool *, bool *) {
    return false;
}
int video_renderer_choose_codec(bool, bool) { return 0; }

void mux_renderer_init(logger_t *, const char *, bool, bool) {}
void mux_renderer_choose_audio_codec(unsigned char) {}
void mux_renderer_choose_video_codec(bool) {}
void mux_renderer_push_video(unsigned char *, int, uint64_t) {}
void mux_renderer_push_audio(unsigned char *, int, uint64_t) {}
void mux_renderer_stop() {}
void mux_renderer_destroy() {}

}  // extern "C"
