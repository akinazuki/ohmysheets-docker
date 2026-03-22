/*
 * Comprehensive Android Bionic → glibc compatibility layer.
 *
 * Core problem: Android's __sF[3] array (FILE structs for stdin/stdout/stderr)
 * has a different layout than glibc's FILE. Any stdio function receiving a
 * FILE* that points into __sF will crash if passed to glibc's real implementation.
 *
 * Solution: Intercept ALL stdio functions. If the FILE* points into __sF,
 * redirect I/O to raw file descriptors 0/1/2. Otherwise, call the real
 * glibc function via dlsym(RTLD_NEXT).
 *
 * This file must be compiled as a shared library and linked BEFORE libc.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dlfcn.h>
#include <errno.h>

/* ========== __sF fake array ========== */

/*
 * Android Bionic ARM32 sizeof(FILE) = 84 bytes.
 * Code accesses stderr as &__sF[2], meaning address = __sF + 2*84 = __sF + 168.
 * We allocate enough space and provide detection logic.
 */
#define BIONIC_FILE_SIZE 84
char __sF[3 * BIONIC_FILE_SIZE] __attribute__((aligned(8)));

/* Check if a FILE* points into our __sF array */
static inline int is_sF(const void *fp) {
    return ((const char*)fp >= __sF &&
            (const char*)fp < __sF + 3 * BIONIC_FILE_SIZE);
}

/* Map __sF index to file descriptor */
static inline int sF_to_fd(const void *fp) {
    int idx = ((const char*)fp - __sF) / BIONIC_FILE_SIZE;
    return idx; /* 0=stdin, 1=stdout, 2=stderr */
}

/* ========== Real glibc function cache ========== */

#define LOAD_REAL(name) \
    static typeof(name) *real_##name = NULL; \
    if (!real_##name) real_##name = dlsym(RTLD_NEXT, #name); \
    if (!real_##name) real_##name = dlsym(RTLD_DEFAULT, "__real_" #name)

/* ========== fprintf / printf family ========== */

int fprintf(FILE *stream, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int ret;
    if (is_sF(stream)) {
        ret = vdprintf(sF_to_fd(stream), fmt, ap);
    } else {
        LOAD_REAL(vfprintf);
        ret = real_vfprintf(stream, fmt, ap);
    }
    va_end(ap);
    return ret;
}

int vfprintf(FILE *stream, const char *fmt, va_list ap) {
    if (is_sF(stream)) {
        return vdprintf(sF_to_fd(stream), fmt, ap);
    }
    LOAD_REAL(vfprintf);
    return real_vfprintf(stream, fmt, ap);
}

int printf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int ret = vdprintf(1, fmt, ap);
    va_end(ap);
    return ret;
}

int vprintf(const char *fmt, va_list ap) {
    return vdprintf(1, fmt, ap);
}

/* ========== fputs / fputc / putc / puts / putchar ========== */

int fputs(const char *s, FILE *stream) {
    if (is_sF(stream)) {
        size_t len = strlen(s);
        return write(sF_to_fd(stream), s, len) >= 0 ? 0 : EOF;
    }
    LOAD_REAL(fputs);
    return real_fputs(s, stream);
}

int fputc(int c, FILE *stream) {
    if (is_sF(stream)) {
        unsigned char ch = (unsigned char)c;
        return write(sF_to_fd(stream), &ch, 1) == 1 ? c : EOF;
    }
    LOAD_REAL(fputc);
    return real_fputc(c, stream);
}

int putc(int c, FILE *stream) {
    return fputc(c, stream);
}

int putchar(int c) {
    unsigned char ch = (unsigned char)c;
    return write(1, &ch, 1) == 1 ? c : EOF;
}

int puts(const char *s) {
    size_t len = strlen(s);
    write(1, s, len);
    write(1, "\n", 1);
    return 0;
}

/* ========== fwrite / fread ========== */

size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream) {
    if (is_sF(stream)) {
        ssize_t n = write(sF_to_fd(stream), ptr, size * nmemb);
        return n > 0 ? n / size : 0;
    }
    LOAD_REAL(fwrite);
    return real_fwrite(ptr, size, nmemb, stream);
}

size_t fread(void *ptr, size_t size, size_t nmemb, FILE *stream) {
    if (is_sF(stream)) {
        ssize_t n = read(sF_to_fd(stream), ptr, size * nmemb);
        return n > 0 ? n / size : 0;
    }
    LOAD_REAL(fread);
    return real_fread(ptr, size, nmemb, stream);
}

/* ========== fgets / fgetc / getc / ungetc ========== */

char *fgets(char *s, int size, FILE *stream) {
    if (is_sF(stream)) {
        /* Read one byte at a time from fd */
        int fd = sF_to_fd(stream);
        int i = 0;
        while (i < size - 1) {
            char c;
            ssize_t r = read(fd, &c, 1);
            if (r <= 0) break;
            s[i++] = c;
            if (c == '\n') break;
        }
        if (i == 0) return NULL;
        s[i] = '\0';
        return s;
    }
    LOAD_REAL(fgets);
    return real_fgets(s, size, stream);
}

int fgetc(FILE *stream) {
    if (is_sF(stream)) {
        unsigned char c;
        return read(sF_to_fd(stream), &c, 1) == 1 ? c : EOF;
    }
    LOAD_REAL(fgetc);
    return real_fgetc(stream);
}

int getc(FILE *stream) {
    return fgetc(stream);
}

/* ungetc on __sF is not practically needed; stub it */
int ungetc(int c, FILE *stream) {
    if (is_sF(stream)) return c; /* silently ignore */
    LOAD_REAL(ungetc);
    return real_ungetc(c, stream);
}

/* ========== fopen / fclose / freopen ========== */

/* fopen returns a real glibc FILE* - no interception needed,
   but we must provide it so our library resolves the symbol */
FILE *fopen(const char *path, const char *mode) {
    LOAD_REAL(fopen);
    return real_fopen(path, mode);
}

int fclose(FILE *stream) {
    if (is_sF(stream)) return 0; /* don't close stdin/stdout/stderr */
    LOAD_REAL(fclose);
    return real_fclose(stream);
}

/* ========== fseek / ftell / rewind / feof / ferror / clearerr ========== */

int fseek(FILE *stream, long offset, int whence) {
    if (is_sF(stream)) return -1;
    LOAD_REAL(fseek);
    return real_fseek(stream, offset, whence);
}

int fseeko(FILE *stream, off_t offset, int whence) {
    if (is_sF(stream)) return -1;
    LOAD_REAL(fseeko);
    return real_fseeko(stream, offset, whence);
}

long ftell(FILE *stream) {
    if (is_sF(stream)) return -1;
    LOAD_REAL(ftell);
    return real_ftell(stream);
}

off_t ftello(FILE *stream) {
    if (is_sF(stream)) return -1;
    LOAD_REAL(ftello);
    return real_ftello(stream);
}

void rewind(FILE *stream) {
    if (is_sF(stream)) return;
    LOAD_REAL(rewind);
    real_rewind(stream);
}

int feof(FILE *stream) {
    if (is_sF(stream)) return 0;
    LOAD_REAL(feof);
    return real_feof(stream);
}

int ferror(FILE *stream) {
    if (is_sF(stream)) return 0;
    LOAD_REAL(ferror);
    return real_ferror(stream);
}

void clearerr(FILE *stream) {
    if (is_sF(stream)) return;
    LOAD_REAL(clearerr);
    real_clearerr(stream);
}

/* ========== fflush / setvbuf / fileno ========== */

int fflush(FILE *stream) {
    if (!stream) {
        /* fflush(NULL) = flush all */
        fsync(1); fsync(2);
        LOAD_REAL(fflush);
        return real_fflush(NULL);
    }
    if (is_sF(stream)) {
        fsync(sF_to_fd(stream));
        return 0;
    }
    LOAD_REAL(fflush);
    return real_fflush(stream);
}

int setvbuf(FILE *stream, char *buf, int mode, size_t size) {
    if (is_sF(stream)) return 0;
    LOAD_REAL(setvbuf);
    return real_setvbuf(stream, buf, mode, size);
}

int fileno(FILE *stream) {
    if (is_sF(stream)) return sF_to_fd(stream);
    LOAD_REAL(fileno);
    return real_fileno(stream);
}

/* ========== fscanf ========== */

int fscanf(FILE *stream, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int ret;
    if (is_sF(stream)) {
        /* fscanf from stdin via fd 0 - rarely used, stub */
        ret = 0;
    } else {
        LOAD_REAL(vfscanf);
        ret = real_vfscanf(stream, fmt, ap);
    }
    va_end(ap);
    return ret;
}

int vfscanf(FILE *stream, const char *fmt, va_list ap) {
    if (is_sF(stream)) return 0;
    LOAD_REAL(vfscanf);
    return real_vfscanf(stream, fmt, ap);
}

/* ========== Android-specific Bionic stubs ========== */

/* Android logging */
int __android_log_print(int prio, const char *tag, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    dprintf(2, "[%s] ", tag ? tag : "?");
    vdprintf(2, fmt, args);
    write(2, "\n", 1);
    va_end(args);
    return 0;
}

/* Bionic fortified string functions */
size_t __strlen_chk(const char *s, size_t s_len) {
    return strlen(s);
}

int __vsnprintf_chk(char *buf, size_t maxlen, int flags, size_t slen,
                    const char *fmt, va_list ap) {
    return vsnprintf(buf, maxlen, fmt, ap);
}

int __vsprintf_chk(char *buf, int flags, size_t slen,
                   const char *fmt, va_list ap) {
    return vsprintf(buf, fmt, ap);
}

/* Bionic __errno */
int *__errno(void) {
    return &errno;
}

/* Bionic locale */
size_t __ctype_get_mb_cur_max(void) {
    return 4;
}

/* Bionic atfork */
int __register_atfork(void (*prepare)(void), void (*parent)(void),
                      void (*child)(void), void *dso) {
    return 0;
}

/* Android abort message */
void android_set_abort_message(const char *msg) {
    if (msg) dprintf(2, "abort: %s\n", msg);
}

/* Android bitmap stubs */
int AndroidBitmap_getInfo(void *env, void *bmp, void *info) { return -1; }
int AndroidBitmap_lockPixels(void *env, void *bmp, void **px) { return -1; }
int AndroidBitmap_unlockPixels(void *env, void *bmp) { return -1; }

/* Android system property stub */
int __system_property_get(const char *name, char *value) {
    if (value) value[0] = 0;
    return 0;
}

/* Safe atoi - loadTemplates calls atoi(NULL) on invalid filenames */
int atoi(const char *nptr) {
    if (!nptr) return 0;
    /* Call the real strtol */
    return (int)strtol(nptr, (char**)0, 10);
}
