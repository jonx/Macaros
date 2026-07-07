/* ffprobe_stubs.c -- satisfy the avfilter/avdevice symbols that ffmpeg's shared
 * fftools (cmdutils.c / opt_common.c) reference for the -filters / -sources /
 * -sinks CLI options. Our videox ffmpeg is built --disable-everything (no
 * avfilter, no avdevice), and basic media probing never touches these, so a
 * no-op stub lets FFProbe link without pulling in those libraries. Only the
 * "list the available filters/devices" options are affected; probing a file
 * (the whole point) works.
 *
 * C has no name mangling, so the exact prototypes do not matter for linking --
 * these just need to exist and never be reached on the probe path.
 */

/* avfilter */
const void *av_filter_iterate(void **opaque) { (void)opaque; return 0; }
const void *avfilter_get_by_name(const char *name) { (void)name; return 0; }
const char *avfilter_pad_get_name(const void *pads, int idx) { (void)pads; (void)idx; return 0; }
int avfilter_pad_get_type(const void *pads, int idx) { (void)pads; (void)idx; return 0; }
unsigned avfilter_filter_pad_count(const void *filter, int is_output) { (void)filter; (void)is_output; return 0; }
const char *avfilter_configuration(void) { return ""; }
unsigned avfilter_version(void) { return 0; }

/* avdevice */
const void *av_input_video_device_next(const void *d) { (void)d; return 0; }
const void *av_input_audio_device_next(const void *d) { (void)d; return 0; }
const void *av_output_video_device_next(const void *d) { (void)d; return 0; }
const void *av_output_audio_device_next(const void *d) { (void)d; return 0; }
const char *avdevice_configuration(void) { return ""; }
void avdevice_free_list_devices(void **d) { (void)d; }
int avdevice_list_input_sources(void *a, const char *b, void *c, void **d) { (void)a; (void)b; (void)c; (void)d; return -1; }
int avdevice_list_output_sinks(void *a, const char *b, void *c, void **d) { (void)a; (void)b; (void)c; (void)d; return -1; }
void avdevice_register_all(void) { }
unsigned avdevice_version(void) { return 0; }
