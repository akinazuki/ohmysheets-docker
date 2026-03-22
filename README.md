# sms

Sheet music → MIDI.

## Usage

```
./sms.sh score.png
./sms.sh /path/to/pages/
./sms.sh score.png output.mid
```

Requires Docker with arm64 support. First run builds the image automatically.

## What it does

Takes a photo or scan of sheet music, outputs a standard MIDI file. Handles multi-page scores, repeat signs, key signatures, and time signatures.

Supports PNG, JPG, HEIC. Low-resolution images are upscaled automatically.

## How it works

The analysis engine was not written from scratch. It was *recovered* from an existing commercial product through binary-level investigation, adapted to run in a standalone Linux environment. The internal data structures and calling conventions were determined empirically. A compatibility shim bridges the gap between the engine's expected runtime and standard Linux.

Neural network models for symbol classification ship as-is in their original format.

Use it at your own risk.

## Requirements

- Docker (OrbStack or Docker Desktop)
- Python 3 + Pillow (for image preprocessing)

## Build

```
docker build --platform linux/arm64/v8 -t sms-scanner .
```

## Notes

99% pitch accuracy on tested scores against the original product's output. BPM is fixed at 160.
