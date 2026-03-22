FROM arm64v8/debian:bookworm-slim

RUN apt-get update -qq && \
    apt-get install -qq -y gcc libc6-dev zlib1g-dev binutils python3 && \
    rm -rf /var/lib/apt/lists/*

WORKDIR /app

# Native libraries (original unpatched Android arm64 .so)
COPY lib/ /app/lib/

# ELF patch script
COPY patch_elf.py /app/

# Assets: NN models + templates
COPY assets/nnModels/ /app/assets/nnModels/
COPY assets/templates/ /app/assets/templates/

# Source code
COPY stubs.c sms.c /app/

# Patch ELF binaries and build
#   1. patch_elf.py: zero .gnu.version, null DT_VERNEED/DT_VERNEEDNUM
#   2. objcopy: remove .note.android.ident section
#   3. Symlink glibc system libs into the lib directory
#   4. Build Bionic compat stubs (libandroid_stubs.so)
#   5. Build the CLI binary
RUN set -e && \
    LIB=/app/lib && \
    python3 /app/patch_elf.py $LIB/*.so && \
    for f in $LIB/lib*.so; do objcopy --remove-section=.note.android.ident "$f" 2>/dev/null || true; done && \
    ln -sf /lib/aarch64-linux-gnu/libc.so.6 $LIB/libc.so && \
    ln -sf /lib/aarch64-linux-gnu/libm.so.6 $LIB/libm.so && \
    ln -sf /lib/aarch64-linux-gnu/libdl.so.2 $LIB/libdl.so && \
    ln -sf /lib/aarch64-linux-gnu/libz.so.1 $LIB/libz.so && \
    gcc -shared -fPIC -o $LIB/libandroid_stubs.so /app/stubs.c -ldl && \
    ln -sf libandroid_stubs.so $LIB/liblog.so && \
    ln -sf libandroid_stubs.so $LIB/libjnigraphics.so && \
    gcc -o /app/sms /app/sms.c \
        -L$LIB -landroid_stubs -lsource-lib -llept -ljpgt -lpngt \
        -ldl -lm -Wl,-rpath,$LIB

RUN mkdir -p /app/assets /app/output

ENTRYPOINT ["sh", "-c", "LD_PRELOAD=/app/lib/libandroid_stubs.so LD_LIBRARY_PATH=/app/lib /app/sms \"$@\"", "--"]
CMD ["--help"]
