FROM arm64v8/debian:bookworm-slim

RUN apt-get update -qq && \
    apt-get install -qq -y gcc libc6-dev zlib1g-dev binutils python3 ca-certificates curl && \
    rm -rf /var/lib/apt/lists/*

# Install Go 1.22 (needed for gin and modern Go features)
RUN curl -fsSL https://go.dev/dl/go1.22.10.linux-arm64.tar.gz | tar -C /usr/local -xz
ENV PATH="/usr/local/go/bin:${PATH}"

WORKDIR /app

# Native libraries (original unpatched Android arm64 .so)
COPY lib/ /app/lib/

# ELF patch script
COPY patch_elf.py /app/

# Assets: NN models + templates
COPY assets/nnModels/ /app/assets/nnModels/
COPY assets/templates/ /app/assets/templates/

# Source code
# stubs.c goes to a separate dir (compiled as .so, must NOT be in cgo's scope)
# sms.c is the legacy standalone CLI, kept for reference only
COPY stubs.c /app/compat/
COPY go.mod go.sum* /app/
RUN cd /app && go mod download 2>/dev/null || true
COPY sms_lib.h sms_lib.c bridge.c sms.go callback.go main.go /app/

# Patch ELF binaries and build
#   1. patch_elf.py: zero .gnu.version, null DT_VERNEED/DT_VERNEEDNUM
#   2. objcopy: remove .note.android.ident section
#   3. Symlink glibc system libs into the lib directory
#   4. Build Bionic compat stubs (libandroid_stubs.so)
#   5. Build the Go binary (cgo compiles sms_lib.c + bridge.c automatically)
RUN set -e && \
    LIB=/app/lib && \
    python3 /app/patch_elf.py $LIB/*.so && \
    for f in $LIB/lib*.so; do objcopy --remove-section=.note.android.ident "$f" 2>/dev/null || true; done && \
    ln -sf /lib/aarch64-linux-gnu/libc.so.6 $LIB/libc.so && \
    ln -sf /lib/aarch64-linux-gnu/libm.so.6 $LIB/libm.so && \
    ln -sf /lib/aarch64-linux-gnu/libdl.so.2 $LIB/libdl.so && \
    ln -sf /lib/aarch64-linux-gnu/libz.so.1 $LIB/libz.so && \
    gcc -shared -fPIC -o $LIB/libandroid_stubs.so /app/compat/stubs.c -ldl && \
    ln -sf libandroid_stubs.so $LIB/liblog.so && \
    ln -sf libandroid_stubs.so $LIB/libjnigraphics.so && \
    cd /app && go mod tidy && CGO_ENABLED=1 go build -o /app/sms .

RUN mkdir -p /app/assets /app/output

ENTRYPOINT ["sh", "-c", "LD_PRELOAD=/app/lib/libandroid_stubs.so LD_LIBRARY_PATH=/app/lib /app/sms \"$@\"", "--"]
CMD ["--help"]
