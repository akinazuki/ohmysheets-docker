#ifndef SMS_LIB_H
#define SMS_LIB_H

typedef void (*sms_log_fn)(const char *msg, void *userdata);
/* stage: 2-7 per page, page/total_pages: 0-based page index */
typedef void (*sms_progress_fn)(int page, int total_pages, int stage, const char *name, void *userdata);

typedef struct {
    int result_code;     /* 0 = success */
    int total_notes;
    int total_bars;
    int num_staves;
    unsigned char *midi_data; /* caller must free() */
    int midi_len;
    char error_msg[256];
} SmsResult;

/* Initialize the SMS engine (OCR models, template path, symbol resolution).
 * Must be called once before sms_analyze(). Returns 0 on success. */
int sms_init(sms_log_fn log_cb, void *userdata);

/* --- PIX helpers (thin wrappers around Leptonica) --- */

/* Read image from file path. Returns opaque PIX* or NULL on failure. */
void *sms_pix_read(const char *path);

/* Read image from memory buffer. Returns opaque PIX* or NULL. */
void *sms_pix_read_mem(const unsigned char *data, int len);

/* Get image dimensions. */
int sms_pix_width(void *pix);
int sms_pix_height(void *pix);

/* Free a PIX*. */
void sms_pix_free(void *pix);

/* Analyze pre-loaded PIX* pages and produce MIDI in memory.
 * pix_pages:  array of PIX* (32bpp or will be converted internally)
 * num_pages:  number of pages
 * Result contains midi_data/midi_len — caller must free(midi_data). */
SmsResult sms_analyze(void **pix_pages, int num_pages,
                      sms_log_fn log_cb, sms_progress_fn progress_cb,
                      void *userdata);

#endif
