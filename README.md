# Jpegli-gui

Win32 GUI wrapper for cjpegli — batch JPEG optimizer with metadata preservation,
dark theme, slider controls, and support for many input formats. Sort of rewrite of https://github.com/rudolphos/Jpegli-image-optimizer on C for windows.

<img width="638" height="642" alt="jpegli-gui" src="https://github.com/user-attachments/assets/21f0cd7e-4ffe-424f-81f7-c7c5e0fc5df6" />

## What is JPEGLI?

[JPEGLI](https://github.com/google/jpegli) is an advanced JPEG coding library
developed by Google. Compared to traditional JPEG encoders (libjpeg-turbo,
MozJPEG), it offers:

- **~35% better compression** at high quality settings — same visual quality,
  smaller file size
- **Adaptive quantization heuristics** from JPEG XL — spatially modulates the
  dead zone based on psychovisual modeling (Butteraugli)
- **Floating-point precision** throughout the entire encoding pipeline (colorspace
  conversion, chroma subsampling, DCT), converting to integer only at the final
  quantization stage
- **10+ bit internal precision** — reduces banding artifacts in smooth gradients
  while staying fully compatible with standard 8-bit JPEG viewers
- **API/ABI compatible** with libjpeg62 — existing software can drop it in
- **Encoding speed comparable** to libjpeg-turbo and MozJPEG

JPEGLI is the successor to Google's earlier [Guetzli](https://github.com/google/guetzli)
encoder. Guetzli produced excellent compression ratios but required **minutes per
megapixel**, making it impractical for batch processing. JPEGLI achieves similar
or better quality-density ratio while running **orders of magnitude faster**
(comparable to libjpeg-turbo), by incorporating adaptive quantization heuristics
from JPEG XL instead of Guetzli's expensive search-based approach.

`cjpegli` is the CLI encoder from the libjxl project that implements JPEGLI.
This GUI wraps it for convenient batch processing.

## Features

- Batch drag-and-drop processing
- Quality mode (0–100, editable edit or slider)
- Distance mode (slider 0.10–5.00 or type any value > 0)
- Auto-optimize — predicts optimal `--distance` via image analysis (Canny, Laplacian, noise)
- Resize to max width
- Threshold — skip if compression below N%
- Metadata preservation — EXIF, ICC, XMP, IPTC (extracted from JPEG/PNG/WebP, injected into output)
- Per-file JPEG parameter detection (subsampling + progressive/baseline from SOF marker)
- Auto-detects Windows dark theme
- Animated GIF detection with warning
- Clear queue button (appears when files are added; also clears the log)
- Cancel during processing (Process button turns into Cancel)
- Output to `processed/` folder in the first dropped input file's location
- Original files remain unchanged
- All output converted to `.jpg`
- stb_image decode for: PNG, WebP, BMP, TGA, PSD, HDR, PPM/PGM/PBM, GIF
- Single-threaded (files processed sequentially, UI stays responsive via PeekMessage loop)

## Auto-Optimize

Though author of original Python-based GUI says, that their auto-optimize method does it's work without perceptible visual quality loss, it's not completely true. The method is really saves details, but may harm noises or noisy-like details, which are, though, noticeable if you're like to zoom your photos. So, if you want to save almost all details, use 95% quality. If You don't care about details much, auto-optimize will do it's work fine for you (and files will be about 2 times smaller, than 95% quality). Here's showcase:

<img width="765" height="192" alt="jpegli compression methods showcase" src="https://github.com/user-attachments/assets/20a9015d-4e8b-4e45-b460-0bdfd912ba96" />

When the **Auto-optimize** checkbox is checked, the program analyzes the input
image before encoding and predicts an optimal `--distance` value, overriding
both Quality and Distance manual controls. This is a 1:1 C port of the
`jpegli_opt.py` pipeline from the companion script.

### Image Analysis

The input image is decoded via stb_image (RGB) and converted to grayscale using
OpenCV weights (`0.299*R + 0.587*G + 0.114*B`). Four statistics are computed:

| Stat | Method | Formula |
|---|---|---|
| `edge_density` | Canny 50/150 (Sobel 3×3, NMS 4-direction, hysteresis 8-conn) | `count(edge) / total_pixels` |
| `texture` | Laplacian 3×3 (`[0,1,0; 1,-4,1; 0,1,0]`) | `mean(abs(response))` |
| `variance` | Population variance | `sum((x - mean)²) / N` |
| `noise` | Center crop to 2048×2048 (only if both dims > 2048), GaussianBlur 3×3 (σ=0.8), absolute difference, median | `median(abs(src - blur))` via QuickSelect |

All four match the outputs of OpenCV functions used in `jpegli_opt.py`.

### Distance Prediction (`predict_safe_distance`)

```
if noise < 0.5:            return 0.55
distance = 0.65
if texture > 3.0:          distance += (texture - 3.0) * 0.04
if edge_density > 0.10:    distance -= (edge_density - 0.10) * 3.5
if noise > 5.0:            distance += 0.2
if variance < 600:         return 0.65
clamp:                     0.55 … 2.2
```

Intuition:
- **Noise < 0.5** → near-perfect source → lowest distance (0.55) for highest quality
- **Texture > 3.0** → detailed image → increase distance (saves bits where noise is masked)
- **Edge density > 10%** → many sharp edges → decrease distance (edges show compression
  artifacts easily, need more bits)
- **Noise > 5.0** → noisy image → increase distance (noise masks artifacts)
- **Variance < 600** → flat/low-contrast image → moderate distance (0.65), prevents
  over-allocating bits
- Final clamp at 0.55–2.2 prevents extreme values

The predicted distance is passed as `--distance=X.XX` to cjpegli. If image
analysis fails (corrupt/unreadable file), a fallback to the manual settings is
used and a warning is logged.

## Metadata Handling — EXIF / ICC / XMP / IPTC

All four metadata types are extracted from the source image before encoding and
injected into the output JPEG after encoding. The extraction is per-format, and
injection unifies everything into standard JPEG APP markers.

### Extraction

| Type | JPEG | PNG | WebP |
|---|---|---|---|
| **EXIF** | APP1 (`FF E1`, `Exif\0\0`) | `eXIf` chunk → TIFF wrapped as APP1 | `EXIF` RIFF chunk → same wrapping |
| **ICC** | APP2 segments (`FF E2`, `ICC_PROFILE\0`) — multi-seg reassembled into flat profile | `iCCP` chunk → decompressed via stb zlib (`stbi_zlib_decode_malloc`) | `ICCP` RIFF chunk |
| **XMP** | APP1 (`FF E1`, `http://ns.adobe.com/xap/1.0/\0`) | `iTXt` keyword `XML:com.adobe.xmp` → decompressed if compressed | `XMP ` RIFF chunk |
| **IPTC** | APP13 (`FF ED`, `Photoshop 3.0\0`) | not extracted (IPTC is virtually absent in PNG/WebP) | not extracted |

Reading is limited to the first 256 KB of each input file — sufficient for
all standard metadata sizes. Extended XMP (>64 KB) is skipped; ICC multi-segment
(up to 255 chunks of 64 KB) is fully reassembled.

### Injection

After `cjpegli` produces the output JPEG, `inject_metadata_blocks` rewrites
the file:

1. Read cjpegli output into memory
2. Scan past SOI, skip any existing APP markers that cjpegli may have written
3. Write **SOI** (`FF D8`)
4. Write **EXIF APP1** (`FF E1` + length + `Exif\0\0` + TIFF data)
5. Write **XMP APP1** (`FF E1` + length + namespace + XML body)
6. Write **IPTC APP13** (`FF ED` + length + `Photoshop 3.0\0` + IPTC data)
7. Write **ICC** as multi-segment **APP2** chunks:
   - Split flat profile into chunks of 65,519 bytes each
   - Each chunk: `FF E2` + length + `ICC_PROFILE\0` + seq_no + num_segments + data
   - Up to 255 chunks (handles profiles up to ~16 MB)
8. Append the rest of the original JPEG data (from where real scan data starts)
9. Atomic rename (`.meta_tmp` → original file)

If any individual metadata type was absent in the source, its corresponding
APP marker is simply skipped during injection.

### Atomicity

- Extraction reads the source file (unchanged).
- Injection writes to a temp file (`.meta_tmp`), then `MoveFileExA` replaces
  the cjpegli output atomically.
- On any failure (disk full, rename conflict) the temp file is cleaned up and
  the original cjpegli output is preserved (minus metadata).

## JPEG Parameter Detection

For each dropped JPEG file the program parses its SOF marker (`FF C0` for
baseline, `FF C2` for progressive) to read the original encoding parameters:

- **Subsampling** — extracted from the Y and Cb component sampling factors
  in the SOF component specification. The horizontal and vertical sampling
  ratios determine the mode:
  - `Y_h=2, Y_v=2, Cb_h=1, Cb_v=1` → `--chroma_subsampling=420` (`4:2:0`)
  - `Y_h=2, Y_v=2, Cb_h=1, Cb_v=2` → `--chroma_subsampling=440` (`4:4:0`)
  - `Y_h=2, Y_v=1` → `--chroma_subsampling=422` (`4:2:2`)
  - `Y_h=1, Y_v=1` → `--chroma_subsampling=444` (`4:4:4`)
- **Progressive / Baseline** — SOF0 means baseline, SOF2 means progressive.
  This translates to `--progressive_level=0` (sequential) or `--progressive_level=2` (progressive scan).

These detected parameters are passed to `cjpegli` for each file individually,
so the output preserves the same subsampling and progressive mode as the
source. This avoids unnecessary overhead when re-encoding JPEGs that were
originally 4:2:0 or baseline.

For non-JPEG inputs (PNG, WebP, BMP, etc.) no `--chroma_subsampling` or
`-p` flags are passed — cjpegli uses its own defaults (4:2:0 progressive).

## Files

### Required at runtime
- `jpegli-gui.exe` — compiled GUI (standalone, statically linked)
- `cjpegli.exe` — JPEGLI encoder binary from libjxl (not included in source)

### Source tree
- `src/main.c` — Win32 GUI implementation
- `src/README.md` — this file

## Build Requirements

- MinGW-w64 (tested with 16.1.0 (winlibs))
- stb headers (download links below)
- Libraries: comctl32, uxtheme, dwmapi, shell32, gdi32

## Build Instructions

1. Download stb headers into `src/`:
   - https://raw.githubusercontent.com/nothings/stb/master/stb_image.h
   - https://raw.githubusercontent.com/nothings/stb/master/stb_image_write.h
   - https://raw.githubusercontent.com/nothings/stb/master/stb_image_resize2.h

2. Compile:
   ```
   x86_64-w64-mingw32-gcc -static -O2 -s -mwindows \
      -o jpegli-gui.exe src/main.c \
      -lcomctl32 -luxtheme -ldwmapi -lshell32 -lgdi32
   ```

3. Download `cjpegli.exe` from the
   [libjxl releases page](https://github.com/libjxl/libjxl/releases)
   (`jxl-x64-windows-static.zip`). Extract `cjpegli.exe` and place it
   alongside the compiled `jpegli-gui.exe`. This binary is not distributed
   with the source — you must obtain it separately.

## Pitfalls

- **UTF-8 in source:** `SetWindowTextA` uses ANSI codepage, so non-ASCII
  characters (like →, ≥) produce garbled text. All UI labels are plain ASCII.
- **Sliders (trackbar)** require `ICC_BAR_CLASSES` in `InitCommonControlsEx`.
- **Static linking** (`-static`) avoids needing `libwinpthread-1.dll`.
- **stb headers** are large (~800 KB combined) but compiled into .exe, not
  distributed. Download them only for builds.
- **cjpegli.exe must be in the same folder as `jpegli-gui.exe`** — its path is
  resolved at startup via `GetModuleFileNameA`. If missing, a message box is
  shown with download instructions and the program exits.
- **File list is cleared after each batch** — dropped files are processed once
  and removed from the queue. Use the **Clear** button (appears when files are
  queued) to discard them manually. Clear also wipes the log.
- **Cancel replaces Process** — during a batch the Process button turns into
  Cancel. Clicking it stops after the current file finishes.
- **Clear button is hidden** until at least one file is dropped. It is also
  hidden during processing to prevent queue corruption.
- **Fixed window size** — 620x600, not resizable. Resizing would break the
  hard-coded control positions.
- **Quality and Distance edits are editable** — type any value directly. Quality
  is clamped to 0–100, Distance must be > 0. Distance values outside the
  slider range (0.10–5.00) are still sent to cjpegli as-is.
- **Multi-directory drag:** If dropped files come from different folders, a
  warning dialog explains that all output goes to `processed/` next to the
  first file in the queue.
- **Error on output folder creation:** If the `processed/` directory cannot be
  created, a message box is shown and the batch is aborted without clearing
  the file queue.
- **`_strlwr` is Microsoft-specific** — used for case-insensitive extension
  matching. The code compiles cleanly with MinGW-w64 but would need a
  portable replacement (e.g. `tolower` loop) for other toolchains.

