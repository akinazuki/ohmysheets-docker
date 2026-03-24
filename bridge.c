/*
 * C bridge for cgo: wraps sms_* calls with Go callback wrappers.
 * All functions here are extern (non-static) so cgo can resolve them.
 */
#include "sms_lib.h"

/* //export-ed from main.go */
extern void goLogCallback(char *msg, void *userdata);
extern void goProgressCallback(int page, int total_pages, int stage, char *name, void *userdata);

static void log_bridge(const char *msg, void *userdata) {
    goLogCallback((char *)msg, userdata);
}

static void progress_bridge(int page, int total_pages, int stage, const char *name, void *userdata) {
    goProgressCallback(page, total_pages, stage, (char *)name, userdata);
}

int bridge_init(void) {
    return sms_init(log_bridge, 0);
}

SmsResult bridge_analyze(void **pix_pages, int n) {
    return sms_analyze(pix_pages, n, log_bridge, progress_bridge, 0);
}

void *bridge_pix_read(const char *path) {
    return sms_pix_read(path);
}

void *bridge_pix_read_mem(const unsigned char *data, int len) {
    return sms_pix_read_mem(data, len);
}

int bridge_pix_width(void *p) {
    return sms_pix_width(p);
}

int bridge_pix_height(void *p) {
    return sms_pix_height(p);
}

void bridge_pix_free(void *p) {
    sms_pix_free(p);
}
