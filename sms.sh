#!/bin/bash
# Sheet Music Scanner CLI (Docker)
# Usage: ./sms.sh <image|directory> [output.mid]
#
# Converts sheet music image(s) to MIDI using the native Android OMR engine.
# Multi-page: pass a directory of page images, analyzed per-page with session
# linking (matching App behavior) for 99% accuracy.
# Requires Docker with arm64 support (OrbStack/Docker Desktop).

set -e

IMAGE_NAME="sms-scanner"

# Build image if needed
if ! docker image inspect "$IMAGE_NAME" >/dev/null 2>&1; then
    echo "Building Docker image..."
    SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
    docker build --platform linux/arm64/v8 -t "$IMAGE_NAME" "$SCRIPT_DIR"
fi

# --- Serve mode ---
if [ "$1" = "serve" ]; then
    PORT="${2:-8080}"
    CONTAINER_NAME="sms-server"
    docker rm -f "$CONTAINER_NAME" 2>/dev/null || true

    echo "Starting SMS server on http://localhost:${PORT}"
    echo "  POST /analyze  — multipart 'images' field"
    echo "  GET  /health"
    echo ""
    echo "  curl -X POST http://localhost:${PORT}/analyze -F 'images=@score.png' -o output.mid"
    echo ""

    exec docker run --rm --name "$CONTAINER_NAME" \
        --platform linux/arm64/v8 \
        -p "${PORT}:8080" \
        "$IMAGE_NAME" \
        serve :8080
fi

# --- CLI mode ---

INPUT="$1"
if [ -z "$INPUT" ]; then
    echo "Usage: $0 <image.png|jpg|heic|directory> [output.mid]"
    echo "       $0 serve [port]"
    echo ""
    echo "Single page:  $0 score.png"
    echo "Multi-page:   $0 /path/to/pages/   (per-page session linking)"
    echo "HTTP server:  $0 serve 8080"
    exit 1
fi

# Helper: convert one image to PNG (handles HEIC, EXIF rotation, iCCP strip)
prepare_image() {
    local SRC="$1"
    local DST="$2"
    local EXT="${SRC##*.}"
    local EXT_LOWER=$(echo "$EXT" | tr '[:upper:]' '[:lower:]')

    if [ "$EXT_LOWER" = "heic" ] || [ "$EXT_LOWER" = "heif" ]; then
        sips -s format png "$SRC" --out "$DST" >/dev/null 2>&1
    else
        cp "$SRC" "$DST"
    fi
    python3 -c "
from PIL import Image, ImageOps
img = ImageOps.exif_transpose(Image.open('$DST'))
img.save('$DST', icc_profile=None)
" 2>/dev/null || true
}

# Helper: upscale if too small
upscale_if_needed() {
    local IMG="$1"
    local IMGW
    IMGW=$(python3 -c "from PIL import Image; print(Image.open('$IMG').size[0])" 2>/dev/null)
    if [ -n "$IMGW" ] && [ "$IMGW" -lt 2000 ]; then
        local UPSCALER="$HOME/workspace/realcugan-ncnn-vulkan/realcugan-ncnn-vulkan"
        if [ -x "$UPSCALER" ]; then
            echo "  Upscaling ${IMGW}px with RealCUGAN..."
            "$UPSCALER" -i "$IMG" -o "$IMG" -s 4 2>/dev/null
        else
            echo "  Upscaling ${IMGW}px with Lanczos..."
            python3 -c "
from PIL import Image
img = Image.open('$IMG')
scale = 3000 / img.size[0]
img.resize((int(img.size[0]*scale), int(img.size[1]*scale)), Image.LANCZOS).save('$IMG', icc_profile=None)
print(f'  Upscaled: {int(img.size[0]*scale)}x{int(img.size[1]*scale)}')
" 2>/dev/null
        fi
    fi
}

TMPDIR=$(mktemp -d)
trap "rm -rf $TMPDIR" EXIT

# Collect page files
PAGE_FILES=()

if [ -d "$INPUT" ]; then
    # Directory: collect all image files sorted
    # echo "Multi-page mode: $INPUT"
    while IFS= read -r f; do
        PAGE_FILES+=("$f")
    done < <(find "$INPUT" -maxdepth 1 -type f \( -iname "*.png" -o -iname "*.jpg" -o -iname "*.jpeg" -o -iname "*.heic" -o -iname "*.heif" -o -iname "*.bmp" -o -iname "*.tiff" \) | sort)

    if [ ${#PAGE_FILES[@]} -eq 0 ]; then
        echo "Error: no image files found in $INPUT"
        exit 1
    fi
else
    PAGE_FILES=("$INPUT")
fi

# Prepare all pages: convert to PNG, upscale if needed
DOCKER_ARGS=()
MOUNT_ARGS=()
for i in "${!PAGE_FILES[@]}"; do
    DST="$TMPDIR/page_${i}.png"
    prepare_image "${PAGE_FILES[$i]}" "$DST"
    upscale_if_needed "$DST"
    # echo "  Page $((i+1))/${#PAGE_FILES[@]}: $(basename "${PAGE_FILES[$i]}")"
    DOCKER_ARGS+=("/app/pages/page_${i}.png")
done

# Output path & format detection
OUTPUT="$2"
if [ -z "$OUTPUT" ]; then
    if [ -d "$INPUT" ]; then
        OUTPUT="$(cd "$INPUT" && pwd)/output.mid"
    else
        OUTPUT="${INPUT%.*}.mid"
    fi
fi
# Detect format from output extension
case "${OUTPUT##*.}" in
    musicxml|xml) OUT_EXT="${OUTPUT##*.}" ;;
    *)            OUT_EXT="mid" ;;
esac

# Run in Docker: mount pages dir + output dir
docker run --rm --platform linux/arm64/v8 \
    -v "$TMPDIR:/app/pages:ro" \
    -v "$TMPDIR:/app/output" \
    "$IMAGE_NAME" \
    "${DOCKER_ARGS[@]}" "/app/output/result.${OUT_EXT}" 2>&1

# Copy output
RESULT="$TMPDIR/result.${OUT_EXT}"
if [ -f "$RESULT" ]; then
    cp "$RESULT" "$OUTPUT"
    echo ""
    echo "Output: $OUTPUT"

    # Convert to WAV if timidity available
    # if command -v timidity &>/dev/null; then
    #     WAV="${OUTPUT%.mid}.wav"
    #     timidity "$OUTPUT" -Ow -o "$WAV" 2>/dev/null
    #     echo "WAV:    $WAV"
    # fi
else
    echo "Error: no output generated"
    exit 1
fi
