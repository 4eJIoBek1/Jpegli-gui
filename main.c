#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <commctrl.h>
#include <uxtheme.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdarg.h>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#define STB_IMAGE_RESIZE2_IMPLEMENTATION
#include "stb_image_resize2.h"

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "uxtheme.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "shell32.lib")

#define IDC_AUTO_CHK         100
#define IDC_QUALITY_RADIO    101
#define IDC_DISTANCE_RADIO   102
#define IDC_QUALITY_EDIT     103
#define IDC_QUALITY_SLIDER   104
#define IDC_DISTANCE_EDIT    105
#define IDC_DISTANCE_SLIDER  106
#define IDC_RESIZE_CHK       107
#define IDC_RESIZE_EDIT      108
#define IDC_RESIZE_SPIN      109
#define IDC_THRESHOLD_CHK    110
#define IDC_THRESHOLD_EDIT   111
#define IDC_THRESHOLD_SPIN   112
#define IDC_DROP_STATIC      113
#define IDC_PROGRESS         114
#define IDC_LOG_EDIT         115
#define IDC_PROCESS_BTN      116
#define IDC_CLEAR_BTN        117
#define IDC_STATUS           118
#define IDC_COPY_FAILED_CHK  119

typedef struct { char *path; WCHAR *wpath; int is_anim_gif; int is_jpeg; int s_h, s_v; int prog; int cb_h, cb_v; } FileEntry;

static HINSTANCE g_hInst;
static HWND g_hWnd, g_hQualityEdit, g_hDistEdit, g_hResizeEdit, g_hThreshEdit;
static HWND g_hQualitySlider, g_hDistSlider;
static HWND g_hQualityRadio, g_hDistRadio, g_hResizeChk, g_hThreshChk;
static HWND g_hDropStatic, g_hProgress, g_hLog, g_hProcessBtn, g_hClearBtn, g_hStatus;
static int g_dark = 0;
static HBRUSH g_bgBrush, g_editBrush;
static HFONT g_font, g_fontMono;
static FileEntry *g_files = NULL;
static int g_fileCount = 0, g_fileCap = 0;
static int g_processing = 0, g_cancel = 0;
static int g_qualityVal = 95;
static float g_distVal = 1.00f;
static int g_resizeEnabled = 0, g_resizeWidth = 2000;
static int g_thresholdEnabled = 0, g_thresholdPct = 15;
static int g_useQuality = 1;
static int g_copyFailed = 1;
static HWND g_hCopyFailedChk;
static char g_selfDir[MAX_PATH], g_cjpegliPath[MAX_PATH];
static int g_updating = 0;

// --- Auto-optimize + metadata globals ---
static int g_autoOptimize = 0;
static HWND g_hAutoChk;

#define META_TYPE_XMP  1
#define META_TYPE_ICC  2
#define META_TYPE_IPTC 3

typedef struct { unsigned char *data; int len; int type; } MetadataBlock;

typedef struct {
    float edge_density;
    float texture;
    float variance;
    float noise;
} ImageStats;

#define MAX_META_BLOCKS 32
static int is_dark_mode(void) {
    HKEY hk; DWORD v = 1, sz = sizeof(v);
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", 0, KEY_READ, &hk) == ERROR_SUCCESS) {
        RegQueryValueExW(hk, L"AppsUseLightTheme", NULL, NULL, (LPBYTE)&v, &sz);
        RegCloseKey(hk);
    }
    return v == 0;
}

static void set_dark_title(HWND hwnd) {
    BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd, 20, &dark, sizeof(dark));
}

static void log_msg(const char *fmt, ...) {
    char buf[4096]; va_list ap;
    va_start(ap, fmt); vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
    int len = GetWindowTextLengthA(g_hLog);
    SendMessageA(g_hLog, EM_SETSEL, len, len);
    SendMessageA(g_hLog, EM_REPLACESEL, FALSE, (LPARAM)buf);
    SendMessageA(g_hLog, EM_REPLACESEL, FALSE, (LPARAM)"\r\n");
    UpdateWindow(g_hLog);
}

static void set_status(const char *fmt, ...) {
    char buf[256]; va_list ap;
    va_start(ap, fmt); vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
    SetWindowTextA(g_hStatus, buf);
}

static void set_progress(int pct) { SendMessage(g_hProgress, PBM_SETPOS, pct, 0); }

static void detect_jpeg_params(FileEntry *fe) {
    fe->is_jpeg = 0; fe->s_h = 0; fe->s_v = 0; fe->prog = -1; fe->cb_h = 0; fe->cb_v = 0;
    const char *ext = strrchr(fe->path, '.');
    if (!ext || (_stricmp(ext, ".jpg") != 0 && _stricmp(ext, ".jpeg") != 0)) return;
    FILE *f = fopen(fe->path, "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    unsigned char *b = malloc(sz < 262144 ? sz : 262144);
    if (!b) { fclose(f); return; }
    size_t read = fread(b, 1, sz < 262144 ? sz : 262144, f);
    fclose(f);
    if (read < 12) { free(b); return; }
    fe->is_jpeg = 1;
    long pos = 2;
    while (pos + 3 < (long)read) {
        if (b[pos] != 0xFF) break;
        int m = b[pos+1];
        if (m == 0x00 || (m >= 0xD0 && m <= 0xD7)) { pos += 2 - (m==0); continue; }
        if (m == 0xD9 || m == 0xDA) break;
        int sl = (b[pos+2]<<8) + b[pos+3];
        if (sl < 2 || pos + 2 + sl > (long)read) break;
        if (m == 0xC0 || m == 0xC2) {
            fe->prog = (m == 0xC2);
            if (sl >= 12) {
                int nc = b[pos+9];
                if (nc >= 1) {
                    int s = b[pos+11];
                    fe->s_h = s >> 4; fe->s_v = s & 0xF;
                }
                if (nc >= 2) {
                    int scb = b[pos+14];
                    fe->cb_h = scb >> 4; fe->cb_v = scb & 0xF;
                }
            }
            break;
        }
        pos += 2 + sl;
    }
    free(b);
}

static void add_file(const char *path) {
    if (g_fileCount >= g_fileCap) {
        g_fileCap = g_fileCap ? g_fileCap * 2 : 256;
        FileEntry *tmp = realloc(g_files, g_fileCap * sizeof(FileEntry));
        if (!tmp) return;
        g_files = tmp;
    }
    FileEntry *fe = &g_files[g_fileCount++];
    fe->path = _strdup(path); if (!fe->path) { g_fileCount--; return; }
    fe->wpath = NULL;
    fe->is_anim_gif = 0;
    detect_jpeg_params(fe);
    // Animated GIF detection
    const char *ext = strrchr(path, '.');
    if (ext && _stricmp(ext, ".gif") == 0) {
        FILE *f = fopen(path, "rb");
        if (f) {
            unsigned char hdr[6], buf[4096];
            if (fread(hdr, 1, 6, f) == 6 && memcmp(hdr, "GIF", 3) == 0) {
                int frames = 0;
                size_t n; while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
                    for (size_t i = 0; i < n; i++)
                        if (buf[i] == 0x2C) frames++;
                fe->is_anim_gif = (frames > 1);
            }
            fclose(f);
        }
    }
}

static void format_bytes(char *buf, size_t bufsz, unsigned long long bytes) {
    if (bytes < 1024) snprintf(buf, bufsz, "%llu B", bytes);
    else if (bytes < 1024*1024) snprintf(buf, bufsz, "%.1f KB", bytes / 1024.0);
    else snprintf(buf, bufsz, "%.1f MB", bytes / (1024.0 * 1024.0));
}

// --- EXIF extraction ---
typedef struct { unsigned char *data; int len; } ExifBlock;

static ExifBlock exif_extract_jpeg(const char *path) {
    ExifBlock eb = {NULL, 0}; FILE *f = fopen(path, "rb"); if (!f) return eb;
    fseek(f, 0, SEEK_END); long fsz = ftell(f); fseek(f, 0, SEEK_SET);
    unsigned char *buf = malloc(fsz); if (!buf) { fclose(f); return eb; }
    if (fread(buf, 1, fsz, f) != (size_t)fsz) { free(buf); fclose(f); return eb; }
    fclose(f);
    for (long i = 0; i < fsz - 1; i++) {
        if (buf[i] == 0xFF && buf[i+1] == 0xE1 && i + 3 < fsz) {
            int len = (buf[i+2] << 8) | buf[i+3];
            if (i + 2 + len <= fsz && len >= 8 &&
                memcmp(buf + i + 4, "Exif\0\0", 6) == 0) {
                eb.data = malloc(len);
                eb.len = len;
                memcpy(eb.data, buf + i + 2, len);
                break;
            }
        }
    }
    free(buf); return eb;
}

static ExifBlock exif_extract_png(const char *path) {
    ExifBlock eb = {NULL, 0}; FILE *f = fopen(path, "rb"); if (!f) return eb;
    unsigned char sig[8]; if (fread(sig, 1, 8, f) != 8) { fclose(f); return eb; }
    if (memcmp(sig, "\x89PNG\r\n\x1a\n", 8) != 0) { fclose(f); return eb; }
    unsigned char hdr[8];
    while (fread(hdr, 1, 8, f) == 8) {
        int len = (hdr[0]<<24)|(hdr[1]<<16)|(hdr[2]<<8)|hdr[3];
        if (memcmp(hdr+4, "eXIf", 4) == 0) {
            eb.data = malloc(len + 8);
            unsigned char *p = eb.data;
            p[0] = (len+6)>>8; p[1] = (len+6)&0xFF;
            memcpy(p+2, "Exif\0\0", 6);
            if (fread(p+8, 1, len, f) == (size_t)len) { eb.len = len + 8; break; }
            free(eb.data); eb.data = NULL; break;
        }
        if (len > 0) fseek(f, len + 4, SEEK_CUR);
        if (memcmp(hdr+4, "IEND", 4) == 0) break;
    }
    fclose(f); return eb;
}

static ExifBlock exif_extract_webp(const char *path) {
    ExifBlock eb = {NULL, 0}; FILE *f = fopen(path, "rb"); if (!f) return eb;
    unsigned char hdr[12];
    if (fread(hdr, 1, 12, f) != 12 || memcmp(hdr, "RIFF", 4) != 0 || memcmp(hdr+8, "WEBP", 4) != 0) { fclose(f); return eb; }
    while (fread(hdr, 1, 8, f) == 8) {
        int len = (hdr[4])|(hdr[5]<<8)|(hdr[6]<<16)|(hdr[7]<<24);
        if (memcmp(hdr, "EXIF", 4) == 0) {
            eb.data = malloc(len + 8);
            unsigned char *p = eb.data;
            p[0] = (len+6)>>8; p[1] = (len+6)&0xFF;
            memcpy(p+2, "Exif\0\0", 6);
            if (fread(p+8, 1, len, f) == (size_t)len) { eb.len = len + 8; break; }
            free(eb.data); eb.data = NULL; break;
        }
        if (len > 0) fseek(f, len + (len&1), SEEK_CUR);
    }
    fclose(f); return eb;
}

static ExifBlock extract_exif(const char *path) {
    const char *ext = strrchr(path, '.');
    if (!ext) return (ExifBlock){NULL, 0};
    if (_stricmp(ext, ".jpg") == 0 || _stricmp(ext, ".jpeg") == 0) return exif_extract_jpeg(path);
    if (_stricmp(ext, ".png") == 0) return exif_extract_png(path);
    if (_stricmp(ext, ".webp") == 0) return exif_extract_webp(path);
    return (ExifBlock){NULL, 0};
}


// --- cjpegli runner ---
static int run_cjpegli(const char *input, const char *output, int quality_mode, int qual_val, float dist_val, const FileEntry *fe) {
    char cmd[4096], qual_arg[64], extra[256];
    if (quality_mode) {
        if (qual_val < 0) qual_val = 0;
        if (qual_val > 100) qual_val = 100;
        snprintf(qual_arg, sizeof(qual_arg), "--quality=%d", qual_val);
    } else {
        if (dist_val < 0.01f) dist_val = 0.01f;
        snprintf(qual_arg, sizeof(qual_arg), "--distance=%.4f", dist_val);
    }
    extra[0] = 0;
    if (fe && fe->is_jpeg && fe->s_h > 0) {
        const char *ss = "";
        if (fe->s_h == 2 && fe->s_v == 2 && fe->cb_h == 1 && fe->cb_v == 1) ss = "420";
        else if (fe->s_h == 2 && fe->s_v == 2 && fe->cb_h == 1 && fe->cb_v == 2) ss = "440";
        else if (fe->s_h == 2 && fe->s_v == 1) ss = "422";
        else if (fe->s_h == 1 && fe->s_v == 1) ss = "444";
        if (ss[0]) snprintf(extra, sizeof(extra), "--chroma_subsampling=%s", ss);
    }
    char prog_arg[32] = "";
    if (fe && fe->prog >= 0) snprintf(prog_arg, sizeof(prog_arg), "--progressive_level=%d", fe->prog ? 2 : 0);
    snprintf(cmd, sizeof(cmd), "\"%s\" \"%s\" \"%s\" %s %s %s",
             g_cjpegliPath, input, output, qual_arg, extra, prog_arg);
    HANDLE hErrRd = NULL, hErrWr = NULL;
    SECURITY_ATTRIBUTES sa = {sizeof(sa), NULL, TRUE};
    BOOL havePipe = CreatePipe(&hErrRd, &hErrWr, &sa, 0);
    if (havePipe) SetHandleInformation(hErrRd, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si = {sizeof(si)}; PROCESS_INFORMATION pi;
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    if (havePipe) {
        si.dwFlags |= STARTF_USESTDHANDLES;
        si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
        si.hStdError = hErrWr;
    }
    if (!CreateProcessA(NULL, cmd, NULL, NULL, havePipe ? TRUE : FALSE, 0, NULL, NULL, &si, &pi)) {
        if (havePipe) { CloseHandle(hErrWr); CloseHandle(hErrRd); }
        return -1;
    }
    if (havePipe) CloseHandle(hErrWr);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD ec = 0; GetExitCodeProcess(pi.hProcess, &ec);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    if (havePipe && ec != 0) {
        char errbuf[4096] = {0}; DWORD read = 0;
        if (ReadFile(hErrRd, errbuf, sizeof(errbuf)-1, &read, NULL) && read > 0) {
            while (read > 0 && (errbuf[read-1] == '\n' || errbuf[read-1] == '\r')) read--;
            errbuf[read] = 0;
            log_msg("cjpegli error: %s", errbuf);
        }
    }
    if (havePipe) CloseHandle(hErrRd);
    return (int)ec;
}

static int is_supported_by_cjpegli(const char *ext) {
    return _stricmp(ext, ".jpg") == 0 || _stricmp(ext, ".jpeg") == 0 ||
           _stricmp(ext, ".png") == 0 || _stricmp(ext, ".gif") == 0 ||
           _stricmp(ext, ".ppm") == 0 || _stricmp(ext, ".pgm") == 0 ||
           _stricmp(ext, ".pbm") == 0 || _stricmp(ext, ".pnm") == 0 ||
           _stricmp(ext, ".pfm") == 0 || _stricmp(ext, ".pam") == 0;
}

static int write_ppm(const char *path, int w, int h, const unsigned char *rgba) {
    FILE *f = fopen(path, "wb"); if (!f) return 0;
    char hdr[128]; int hlen = snprintf(hdr, sizeof(hdr), "P6\n%d %d\n255\n", w, h);
    if (fwrite(hdr, 1, hlen, f) != (size_t)hlen) { fclose(f); return 0; }
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            fwrite(rgba + (y * w + x) * 4, 1, 3, f);
    if (ferror(f)) { fclose(f); return 0; }
    return fclose(f) == 0;
}

// ======================= IMAGE ANALYSIS (OpenCV-compatible) =======================

#define CANNY_LOW 50
#define CANNY_HIGH 150
#define PI_F 3.141592653589793f

// OpenCV Gaussian kernel for ksize=3, sigma=0.8 (cv::getGaussianKernel(3,0,CV_32F))
// 1D: [0.274068619061197f, 0.451862761877606f, 0.274068619061197f]
static void gaussian_blur_3x3(const float *src, float *dst, int w, int h) {
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            float sum = 0, norm = 0;
            for (int ky = -1; ky <= 1; ky++) {
                for (int kx = -1; kx <= 1; kx++) {
                    int sx = x + kx, sy = y + ky;
                    if (sx < 0) sx = 0; if (sx >= w) sx = w - 1;
                    if (sy < 0) sy = 0; if (sy >= h) sy = h - 1;
                    static const float kernel[9] = {
                        0.057125f, 0.124757f, 0.057125f,
                        0.124757f, 0.272466f, 0.124757f,
                        0.057125f, 0.124757f, 0.057125f
                    };
                    sum += src[sy * w + sx] * kernel[(ky + 1) * 3 + (kx + 1)];
                    if (sx == x + kx && sy == y + ky) norm += kernel[(ky + 1) * 3 + (kx + 1)];
                }
            }
            dst[y * w + x] = sum / norm;
        }
    }
}

static void sobel_3x3(const float *src, float *gx, float *gy, int w, int h) {
    for (int y = 1; y < h - 1; y++) {
        for (int x = 1; x < w - 1; x++) {
            int i = y * w + x;
            float tl = src[i - w - 1], tc = src[i - w], tr = src[i - w + 1];
            float ml = src[i - 1],               mr = src[i + 1];
            float bl = src[i + w - 1], bc = src[i + w], br = src[i + w + 1];
            gx[i] = (tr + 2 * mr + br) - (tl + 2 * ml + bl);
            gy[i] = (bl + 2 * bc + br) - (tl + 2 * tc + tr);
        }
    }
    // Zero out borders
    for (int x = 0; x < w; x++) { gx[x] = 0; gy[x] = 0; gx[(h-1)*w+x] = 0; gy[(h-1)*w+x] = 0; }
    for (int y = 0; y < h; y++) { gx[y*w] = 0; gy[y*w] = 0; gx[y*w+w-1] = 0; gy[y*w+w-1] = 0; }
}

static void canny_nms_and_thresh(const float *mag, const float *gx, const float *gy,
                                  unsigned char *edge, int w, int h) {
    // Step 1: NMS + double threshold
    for (int y = 1; y < h - 1; y++) {
        for (int x = 1; x < w - 1; x++) {
            int i = y * w + x;
            float m = mag[i];
            if (m < 1e-6f) { edge[i] = 0; continue; }

            float mx = gx[i], my = gy[i];
            float a = atan2f(my, mx);
            a = (a < 0) ? a + PI_F : a;

            int dir; // gradient direction
            if (a < 0.392699f || a >= 2.748893f) dir = 0;
            else if (a < 1.178097f) dir = 1;
            else if (a < 1.963495f) dir = 2;
            else dir = 3;

            float n1, n2;
            switch (dir) {
                case 0: n1 = mag[i - 1]; n2 = mag[i + 1]; break;        // E/W
                case 1: n1 = mag[i - w - 1]; n2 = mag[i + w + 1]; break; // NW/SE
                case 2: n1 = mag[i - w]; n2 = mag[i + w]; break;        // N/S
                default: n1 = mag[i - w + 1]; n2 = mag[i + w - 1]; break; // NE/SW
            }

            if (m >= n1 && m >= n2) {
                edge[i] = (m >= CANNY_HIGH) ? 2 : (m >= CANNY_LOW ? 1 : 0);
            } else {
                edge[i] = 0;
            }
        }
    }

    // Step 2: Hysteresis — follow weak edges from strong edges using 8-connectivity
    int *stack = malloc(w * h * sizeof(int));
    if (!stack) return;
    int sp = 0;
    for (int y = 1; y < h - 1; y++) {
        for (int x = 1; x < w - 1; x++) {
            int i = y * w + x;
            if (edge[i] == 2) stack[sp++] = i;
        }
    }

    static const int dx8[8] = { -1, 0, 1, -1, 1, -1, 0, 1 };
    static const int dy8[8] = { -1, -1, -1, 0, 0, 1, 1, 1 };

    while (sp > 0) {
        int i = stack[--sp];
        int x = i % w, y = i / w;
        for (int k = 0; k < 8; k++) {
            int nx = x + dx8[k], ny = y + dy8[k];
            if (nx > 0 && nx < w - 1 && ny > 0 && ny < h - 1) {
                int ni = ny * w + nx;
                if (edge[ni] == 1) {
                    edge[ni] = 2;
                    stack[sp++] = ni;
                }
            }
        }
    }
    free(stack);

    // Convert to binary: 2→255, everything else→0
    int n = w * h;
    for (int i = 0; i < n; i++)
        edge[i] = (edge[i] == 2) ? 255 : 0;
}

static float compute_texture(const float *src, int w, int h) {
    static const int lap[9] = { 0, 1, 0, 1, -4, 1, 0, 1, 0 };
    double sum = 0;
    int count = 0;
    for (int y = 1; y < h - 1; y++) {
        for (int x = 1; x < w - 1; x++) {
            double v = 0;
            for (int ky = -1; ky <= 1; ky++)
                for (int kx = -1; kx <= 1; kx++)
                    v += src[(y + ky) * w + (x + kx)] * lap[(ky + 1) * 3 + (kx + 1)];
            sum += fabs(v);
            count++;
        }
    }
    return (count > 0) ? (float)(sum / count) : 0;
}

static float compute_variance(const float *src, int n) {
    double mean = 0;
    for (int i = 0; i < n; i++) mean += src[i];
    mean /= n;
    double var = 0;
    for (int i = 0; i < n; i++) { double d = src[i] - mean; var += d * d; }
    return (float)(var / n);
}

// QuickSelect for median (O(n) average)
static float quick_select(float *arr, int n) {
    int low = 0, high = n - 1;
    int k = n / 2;
    while (low < high) {
        int i = low, j = high;
        float pivot = arr[(low + high) / 2];
        while (i <= j) {
            while (arr[i] < pivot) i++;
            while (arr[j] > pivot) j--;
            if (i <= j) {
                float t = arr[i]; arr[i] = arr[j]; arr[j] = t;
                i++; j--;
            }
        }
        if (k <= j) high = j;
        else if (k >= i) low = i;
        else break;
    }
    return arr[k];
}

static float compute_noise(const float *src, int w, int h) {
    // Center crop to 2048×2048 only if both dimensions exceed 2048 (matches Python)
    int crop_w = w, crop_h = h, ox = 0, oy = 0;
    if (w > 2048 && h > 2048) {
        crop_w = 2048; crop_h = 2048;
        ox = (w - 2048) / 2; oy = (h - 2048) / 2;
    }

    // Allocate blurred version
    float *blurred = malloc(crop_w * crop_h * sizeof(float));
    if (!blurred) return 0;

    // Apply Gaussian blur 3×3 on crop region
    static const float gk[9] = {
        0.057125f, 0.124757f, 0.057125f,
        0.124757f, 0.272466f, 0.124757f,
        0.057125f, 0.124757f, 0.057125f
    };
    for (int y = 0; y < crop_h; y++) {
        for (int x = 0; x < crop_w; x++) {
            int sy = oy + y, sx = ox + x;
            double sum = 0, norm = 0;
            for (int ky = -1; ky <= 1; ky++) {
                for (int kx = -1; kx <= 1; kx++) {
                    int px = sx + kx, py = sy + ky;
                    if (px < 0) px = 0; if (px >= w) px = w - 1;
                    if (py < 0) py = 0; if (py >= h) py = h - 1;
                    sum += src[py * w + px] * gk[(ky + 1) * 3 + (kx + 1)];
                    norm += gk[(ky + 1) * 3 + (kx + 1)];
                }
            }
            blurred[y * crop_w + x] = (float)(sum / norm);
        }
    }

    // Compute absolute differences
    int n = crop_w * crop_h;
    float *diff = malloc(n * sizeof(float));
    if (!diff) { free(blurred); return 0; }
    for (int i = 0; i < n; i++)
        diff[i] = fabsf(src[(oy + i / crop_w) * w + (ox + i % crop_w)] - blurred[i]);
    free(blurred);

    // Median = QuickSelect
    float noise = quick_select(diff, n);
    free(diff);
    return noise;
}

static float predict_safe_distance(const ImageStats *stats) {
    if (stats->noise < 0.5f) return 0.55f;
    float distance = 0.65f;
    if (stats->texture > 3.0f)
        distance += (stats->texture - 3.0f) * 0.04f;
    if (stats->edge_density > 0.10f)
        distance -= (stats->edge_density - 0.10f) * 3.5f;
    if (stats->noise > 5.0f)
        distance += 0.2f;
    if (stats->variance < 600.0f)
        return 0.65f;
    if (distance < 0.55f) return 0.55f;
    if (distance > 2.2f) return 2.2f;
    return distance;
}

// Main analysis entry point — matches Python analyze_image_fast()
static int analyze_image(const char *path, ImageStats *stats) {
    int w, h, ch;
    unsigned char *rgb = stbi_load(path, &w, &h, &ch, 3);
    if (!rgb) return -1;

    // OpenCV-style grayscale: 0.299*R + 0.587*G + 0.114*B
    float *gray = malloc(w * h * sizeof(float));
    if (!gray) { stbi_image_free(rgb); return -1; }
    for (int i = 0; i < w * h; i++)
        gray[i] = 0.299f * rgb[3 * i] + 0.587f * rgb[3 * i + 1] + 0.114f * rgb[3 * i + 2];
    stbi_image_free(rgb);

    // Texture (Laplacian)
    stats->texture = compute_texture(gray, w, h);

    // Variance
    stats->variance = compute_variance(gray, w * h);

    // Edge density (Canny) — no pre-blur, matching Python's cv2.Canny()
    float *gx = malloc(w * h * sizeof(float));
    float *gy = malloc(w * h * sizeof(float));
    float *mag = malloc(w * h * sizeof(float));
    unsigned char *edge = malloc(w * h);
    if (!gx || !gy || !mag || !edge) {
        free(gray); free(gx); free(gy); free(mag); free(edge);
        return -1;
    }

    sobel_3x3(gray, gx, gy, w, h);

    int n = w * h;
    for (int i = 0; i < n; i++)
        mag[i] = sqrtf(gx[i] * gx[i] + gy[i] * gy[i]);

    canny_nms_and_thresh(mag, gx, gy, edge, w, h);

    int edge_count = 0;
    for (int i = 0; i < n; i++)
        if (edge[i]) edge_count++;
    stats->edge_density = (float)edge_count / (float)n;

    free(edge); free(mag); free(gy); free(gx);

    // Noise (crop + Gaussian blur + median abs diff)
    stats->noise = compute_noise(gray, w, h);

    free(gray);
    return 0;
}

// ======================= END IMAGE ANALYSIS =======================

// Forward declarations for metadata functions defined after process_file
static MetadataBlock meta_extract_xmp_jpeg(const unsigned char *buf, long size);
static MetadataBlock meta_extract_xmp_png(const unsigned char *buf, long size);
static MetadataBlock meta_extract_xmp_webp(const unsigned char *buf, long size);
static MetadataBlock meta_extract_icc_jpeg(const unsigned char *buf, long size);
static MetadataBlock meta_extract_icc_png(const unsigned char *buf, long size);
static MetadataBlock meta_extract_icc_webp(const unsigned char *buf, long size);
static MetadataBlock meta_extract_iptc_jpeg(const unsigned char *buf, long size);
static int extract_all_metadata(const char *path, MetadataBlock *out, int max_out);
static void free_metadata_blocks(MetadataBlock *blocks, int n);
static int inject_metadata_blocks(const char *jpeg_path, const MetadataBlock *blocks, int num_blocks,
                                  const ExifBlock *exif);

static int process_file(FileEntry *fe, const char *out_dir) {
    const char *path = fe->path;
    char ext[16]; const char *dot = strrchr(path, '.');
    if (!dot) return -1;
    strncpy(ext, dot, 15); ext[15] = 0;
    _strlwr(ext);

    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &fad)) return -1;
    ULARGE_INTEGER origSize; origSize.LowPart = fad.nFileSizeLow; origSize.HighPart = fad.nFileSizeHigh;

    ExifBlock exif = extract_exif(path);
    MetadataBlock meta_blocks[MAX_META_BLOCKS];
    int num_meta = extract_all_metadata(path, meta_blocks, MAX_META_BLOCKS);

    // Auto optimize: analyze image and predict distance
    int use_qual_mode = g_useQuality;
    int quality_val = 0;
    float dist_val = 0.0f;
    if (g_autoOptimize) {
        ImageStats stats;
        if (analyze_image(path, &stats) == 0) {
            float dist = predict_safe_distance(&stats);
            log_msg("Auto: edge=%.4f texture=%.2f var=%.0f noise=%.2f -> distance=%.4f",
                    stats.edge_density, stats.texture, stats.variance, stats.noise, dist);
            use_qual_mode = 0;
            dist_val = dist;
        } else {
            log_msg("Warning: auto-analysis failed, using manual settings");
            use_qual_mode = g_useQuality;
            quality_val = g_qualityVal;
            dist_val = g_distVal;
        }
    } else if (g_useQuality) {
        use_qual_mode = 1;
        quality_val = g_qualityVal;
    } else {
        use_qual_mode = 0;
        dist_val = g_distVal;
    }

    // Build output path: out_dir\basename.jpg
    const char *fname = strrchr(path, '\\');
    fname = fname ? fname + 1 : path;
    char base[512]; strncpy(base, fname, sizeof(base)-5); base[sizeof(base)-5] = 0;
    char *fdot = strrchr(base, '.');
    if (fdot) *fdot = 0;
    char outPath[4096]; snprintf(outPath, sizeof(outPath), "%s\\%s.jpg", out_dir, base);

    // Tentative temp path for cjpegli output
    char tmpPath[4096]; snprintf(tmpPath, sizeof(tmpPath), "%s\\%s.cjpegli_tmp", out_dir, base);

    int use_direct = is_supported_by_cjpegli(ext);
    int need_convert = 0;
    char ppmPath[4096] = {0};

    if (g_resizeEnabled || !use_direct) {
        need_convert = 1;
        snprintf(ppmPath, sizeof(ppmPath), "%s\\%s.ppm_tmp", out_dir, base);
        int w, h, ch;
        unsigned char *pixels = stbi_load(path, &w, &h, &ch, 4);
        if (!pixels) { free(exif.data); free_metadata_blocks(meta_blocks, num_meta); return -1; }
        if (g_resizeEnabled && g_resizeWidth > 0) {
            int ow = w, oh = h;
            if (max(ow, oh) > g_resizeWidth) {
                float ratio = (float)g_resizeWidth / max(ow, oh);
                ow = (int)(ow * ratio); oh = (int)(oh * ratio);
                if (ow < 1) ow = 1; if (oh < 1) oh = 1;
                unsigned char *resized = stbir_resize_uint8_srgb(pixels, w, h, 0, NULL, ow, oh, 0, STBIR_RGBA);
                stbi_image_free(pixels);
                if (!resized) { free(exif.data); free_metadata_blocks(meta_blocks, num_meta); return -1; }
                pixels = resized; w = ow; h = oh;
            }
        }
        if (!write_ppm(ppmPath, w, h, pixels)) {
            stbi_image_free(pixels);
            free(exif.data); free_metadata_blocks(meta_blocks, num_meta);
            return -1;
        }
        stbi_image_free(pixels);
    }

    const char *cjpegli_input = need_convert ? ppmPath : path;
    int ret = run_cjpegli(cjpegli_input, tmpPath, use_qual_mode, quality_val, dist_val, fe);
    if (need_convert && ppmPath[0]) remove(ppmPath);
    if (ret != 0) { free(exif.data); free_metadata_blocks(meta_blocks, num_meta); remove(tmpPath); return -1; }

    // Inject all preserved metadata (EXIF + XMP + ICC + IPTC)
    inject_metadata_blocks(tmpPath, meta_blocks, num_meta, &exif);
    free_metadata_blocks(meta_blocks, num_meta);
    if (exif.data) free(exif.data);

    WIN32_FILE_ATTRIBUTE_DATA outFad;
    if (!GetFileAttributesExA(tmpPath, GetFileExInfoStandard, &outFad)) { remove(tmpPath); return -1; }
    ULARGE_INTEGER newSize; newSize.LowPart = outFad.nFileSizeLow; newSize.HighPart = outFad.nFileSizeHigh;

    if (newSize.QuadPart >= origSize.QuadPart) { remove(tmpPath); return 0; }

    if (g_thresholdEnabled) {
        double pct = 100.0 * (1.0 - (double)newSize.QuadPart / origSize.QuadPart);
        if (pct < g_thresholdPct) { remove(tmpPath); return 0; }
    }

    // Move tmp to final output name
    if (!MoveFileExA(tmpPath, outPath, MOVEFILE_REPLACE_EXISTING)) {
        remove(tmpPath); return -1;
    }

    // Copy original file timestamps to output
    HANDLE hFile = CreateFileA(outPath, GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (hFile != INVALID_HANDLE_VALUE) {
        SetFileTime(hFile, &fad.ftCreationTime, &fad.ftLastAccessTime, &fad.ftLastWriteTime);
        CloseHandle(hFile);
    }

    // Log size comparison
    char origStr[64], newStr[64];
    format_bytes(origStr, sizeof(origStr), origSize.QuadPart);
    format_bytes(newStr, sizeof(newStr), newSize.QuadPart);
    log_msg("OK: %s (%s -> %s)", base, origStr, newStr);
    return 1;
}

// ======================= METADATA EXTRACTION & INJECTION =======================

// Extract XMP from JPEG APP1 (http://ns.adobe.com/xap/1.0/)
static MetadataBlock meta_extract_xmp_jpeg(const unsigned char *buf, long size) {
    MetadataBlock b = {NULL, 0, 0};
    for (long i = 0; i < size - 35; i++) {
        if (buf[i] == 0xFF && buf[i+1] == 0xE1) {
            int len = (buf[i+2] << 8) | buf[i+3];
            if (i + 2 + len > size || len < 30) continue;
            if (memcmp(buf + i + 4, "http://ns.adobe.com/xap/1.0/\0", 29) == 0) {
                b.data = malloc(len);
                memcpy(b.data, buf + i + 2, len);
                b.len = len;
                break;
            }
        }
    }
    if (b.data) b.type = META_TYPE_XMP;
    return b;
}

// Extract XMP from PNG iTXt (keyword "XML:com.adobe.xmp")
static MetadataBlock meta_extract_xmp_png(const unsigned char *buf, long size) {
    MetadataBlock b = {NULL, 0, 0};
    long pos = 8;
    while (pos + 8 <= size) {
        unsigned int clen = (buf[pos]<<24)|(buf[pos+1]<<16)|(buf[pos+2]<<8)|buf[pos+3];
        if (pos + 12 + clen > size) break;
        if (memcmp(buf + pos + 4, "iTXt", 4) == 0 && clen > 20) {
            const char *kw = (const char*)(buf + pos + 8);
            int kwlen = 0;
            while (kwlen < (int)clen && kw[kwlen]) kwlen++;
            if (kwlen == 17 && memcmp(kw, "XML:com.adobe.xmp", 17) == 0) {
                // Find text start: keyword + null + comp_flag(1) + comp_method(1) + lang + null + trans_kw + null
                int off = kwlen + 1;
                if (off + 2 <= (int)clen) {
                    int comp_flag = buf[pos + 8 + off];
                    off += 2; // skip comp_flag and comp_method
                    // skip language tag
                    while (off < (int)clen && buf[pos + 8 + off]) off++;
                    off++; // skip null
                    // skip translated keyword
                    while (off < (int)clen && buf[pos + 8 + off]) off++;
                    off++; // skip null
                    int text_len = clen - off;
                    if (text_len > 0) {
                        if (comp_flag) {
                            // Decompress
                            int deplen;
                            char *dep = stbi_zlib_decode_malloc(
                                (const char*)(buf + pos + 8 + off), text_len, &deplen);
                            if (dep && deplen > 0) {
                                if (deplen > 0x7FFFFFFF - 31) { free(dep); return b; }
                                b.len = deplen + 31; // 2(len) + 29(namespace) + data
                                b.data = malloc(b.len);
                                unsigned char *p = b.data;
                                p[0] = (b.len >> 8) & 0xFF; p[1] = b.len & 0xFF;
                                memcpy(p + 2, "http://ns.adobe.com/xap/1.0/\0", 29);
                                memcpy(p + 31, dep, deplen);
                                free(dep);
                            }
                        } else {
                            if (text_len > 0x7FFFFFFF - 31) return b;
                            b.len = text_len + 31;
                            b.data = malloc(b.len);
                            unsigned char *p = b.data;
                            p[0] = (b.len >> 8) & 0xFF; p[1] = b.len & 0xFF;
                            memcpy(p + 2, "http://ns.adobe.com/xap/1.0/\0", 29);
                            memcpy(p + 31, buf + pos + 8 + off, text_len);
                        }
                    }
                }
                break;
            }
        }
        pos += 8 + clen + (clen & 1);
    }
    if (b.data) b.type = META_TYPE_XMP;
    return b;
}

// Extract XMP from WebP (XMP  chunk - 4-char tag "XMP ")
static MetadataBlock meta_extract_xmp_webp(const unsigned char *buf, long size) {
    MetadataBlock b = {NULL, 0, 0};
    long pos = 12;
    while (pos + 8 <= size) {
        unsigned int clen = buf[pos+4]|(buf[pos+5]<<8)|(buf[pos+6]<<16)|(buf[pos+7]<<24);
        if (pos + 8 + clen > (unsigned long)size) break;
        if (memcmp(buf + pos, "XMP ", 4) == 0 && clen > 0) {
            b.len = clen + 31;
            b.data = malloc(b.len);
            unsigned char *p = b.data;
            p[0] = (b.len >> 8) & 0xFF; p[1] = b.len & 0xFF;
            memcpy(p + 2, "http://ns.adobe.com/xap/1.0/\0", 29);
            memcpy(p + 31, buf + pos + 8, clen);
            break;
        }
        pos += 8 + clen + (clen & 1);
    }
    if (b.data) b.type = META_TYPE_XMP;
    return b;
}

// Extract ICC from JPEG APP2
static MetadataBlock meta_extract_icc_jpeg(const unsigned char *buf, long size) {
    MetadataBlock b = {NULL, 0, 0};
    // Collect all APP2 ICC_PROFILE segments and concatenate by seq number
    unsigned char *segments[256] = {0};
    int seg_lens[256] = {0};
    int total_segs = 0;

    for (long i = 0; i < size - 17; i++) {
        if (buf[i] == 0xFF && buf[i+1] == 0xE2) {
            int len = (buf[i+2] << 8) | buf[i+3];
            if (i + 2 + len > size || len < 14) continue;
            if (memcmp(buf + i + 4, "ICC_PROFILE\0", 12) == 0) {
                int seq = buf[i + 16];
                int total = buf[i + 17];
                int data_len = len - 14;
                if (seq >= 1 && seq <= total && data_len > 0 && !segments[seq]) {
                    segments[seq] = malloc(data_len);
                    memcpy(segments[seq], buf + i + 18, data_len);
                    seg_lens[seq] = data_len;
                    if (seq > total_segs) total_segs = seq;
                }
            }
        }
    }

    if (total_segs == 0) return (MetadataBlock){NULL, 0, 0};

    b.type = META_TYPE_ICC;
    b.len = 0;
    for (int s = 1; s <= total_segs; s++) b.len += seg_lens[s];
    b.data = malloc(b.len);
    int off = 0;
    for (int s = 1; s <= total_segs; s++) {
        if (segments[s]) {
            memcpy(b.data + off, segments[s], seg_lens[s]);
            free(segments[s]);
            off += seg_lens[s];
        }
    }
    return b;
}

// Extract ICC from PNG iCCP chunk (decompress with stb zlib)
static MetadataBlock meta_extract_icc_png(const unsigned char *buf, long size) {
    MetadataBlock b = {NULL, 0, 0};
    long pos = 8;
    while (pos + 12 <= size) {
        unsigned int clen = (buf[pos]<<24)|(buf[pos+1]<<16)|(buf[pos+2]<<8)|buf[pos+3];
        if (pos + 12 + clen > (unsigned long)size) break;
        if (memcmp(buf + pos + 4, "iCCP", 4) == 0 && clen > 3) {
            // Skip name + null + compression method byte = start of zlib stream
            int off = 0;
            while (off < (int)clen && buf[pos + 8 + off]) off++;
            off += 2; // null + compression_method = 2
            if (off < (int)clen) {
                int deplen;
                char *dep = stbi_zlib_decode_malloc(
                    (const char*)(buf + pos + 8 + off), clen - off, &deplen);
                if (dep && deplen > 0) {
                    b.data = malloc(deplen);
                    memcpy(b.data, dep, deplen);
                    b.len = deplen;
                    free(dep);
                }
            }
            break;
        }
        pos += 12 + clen + (clen & 1);
    }
    if (b.data) b.type = META_TYPE_ICC;
    return b;
}

// Extract ICC from WebP ICCP chunk
static MetadataBlock meta_extract_icc_webp(const unsigned char *buf, long size) {
    MetadataBlock b = {NULL, 0, 0};
    long pos = 12;
    while (pos + 8 <= size) {
        unsigned int clen = buf[pos+4]|(buf[pos+5]<<8)|(buf[pos+6]<<16)|(buf[pos+7]<<24);
        if (pos + 8 + clen > (unsigned long)size) break;
        if (memcmp(buf + pos, "ICCP", 4) == 0 && clen > 0) {
            b.data = malloc(clen);
            memcpy(b.data, buf + pos + 8, clen);
            b.len = clen;
            break;
        }
        pos += 8 + clen + (clen & 1);
    }
    if (b.data) b.type = META_TYPE_ICC;
    return b;
}

// Extract IPTC from JPEG APP13 (Photoshop 3.0)
static MetadataBlock meta_extract_iptc_jpeg(const unsigned char *buf, long size) {
    MetadataBlock b = {NULL, 0, 0};
    for (long i = 0; i < size - 17; i++) {
        if (buf[i] == 0xFF && buf[i+1] == 0xED) {
            int len = (buf[i+2] << 8) | buf[i+3];
            if (i + 2 + len > size || len < 14) continue;
            if (memcmp(buf + i + 4, "Photoshop 3.0\0", 14) == 0) {
                b.data = malloc(len);
                memcpy(b.data, buf + i + 2, len);
                b.len = len;
                break;
            }
        }
    }
    if (b.data) b.type = META_TYPE_IPTC;
    return b;
}

// Extract all metadata from any supported input file
static int extract_all_metadata(const char *path, MetadataBlock *out, int max_out) {
    int n = 0;
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz < 12) { fclose(f); return 0; }
    unsigned char *buf = malloc(sz < 262144 ? sz : 262144);
    if (!buf) { fclose(f); return 0; }
    long read_sz = fread(buf, 1, sz < 262144 ? sz : 262144, f);
    fclose(f);
    if (read_sz < 12) { free(buf); return 0; }

    const char *ext = strrchr(path, '.');
    if (!ext) { free(buf); return 0; }

    if (_stricmp(ext, ".jpg") == 0 || _stricmp(ext, ".jpeg") == 0) {
        // Extract XMP (stored like EXIF in APP1, but different identifier)
        if (n < max_out) {
            MetadataBlock b = meta_extract_xmp_jpeg(buf, read_sz);
            if (b.data) out[n++] = b;
        }
        // ICC
        if (n < max_out) {
            MetadataBlock b = meta_extract_icc_jpeg(buf, read_sz);
            if (b.data) out[n++] = b;
        }
        // IPTC
        if (n < max_out) {
            MetadataBlock b = meta_extract_iptc_jpeg(buf, read_sz);
            if (b.data) out[n++] = b;
        }
        // EXIF (already extracted by existing code, but we mark it for injection too)
        // Note: EXIF is handled separately by existing code. We skip it here
        // to avoid duplication.
    } else if (_stricmp(ext, ".png") == 0) {
        if (n < max_out) {
            MetadataBlock b = meta_extract_xmp_png(buf, read_sz);
            if (b.data) out[n++] = b;
        }
        if (n < max_out) {
            MetadataBlock b = meta_extract_icc_png(buf, read_sz);
            if (b.data) out[n++] = b;
        }
        // EXIF and IPTC already handled elsewhere for PNG
    } else if (_stricmp(ext, ".webp") == 0) {
        if (n < max_out) {
            MetadataBlock b = meta_extract_xmp_webp(buf, read_sz);
            if (b.data) out[n++] = b;
        }
        if (n < max_out) {
            MetadataBlock b = meta_extract_icc_webp(buf, read_sz);
            if (b.data) out[n++] = b;
        }
    }

    free(buf);
    return n;
}

static void free_metadata_blocks(MetadataBlock *blocks, int n) {
    for (int i = 0; i < n; i++)
        if (blocks[i].data) free(blocks[i].data);
}

// Inject all metadata blocks into JPEG output file.
// Strips existing APP markers from cjpegli output, inserts our preserved blocks.
static int inject_metadata_blocks(const char *jpeg_path, const MetadataBlock *blocks, int num_blocks,
                                  const ExifBlock *exif) {
    FILE *f = fopen(jpeg_path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    unsigned char *src = malloc(sz);
    if (!src) { fclose(f); return -1; }
    if (fread(src, 1, sz, f) != (size_t)sz) { free(src); fclose(f); return -1; }
    fclose(f);

    // Scan the output JPEG and find where real data starts (after SOI + any existing APP markers)
    long data_start = 2;
    long pos = 2;
    while (pos + 4 < sz) {
        if (src[pos] != 0xFF) { data_start = pos; break; }
        int m = src[pos + 1];
        if (m == 0x00 || (m >= 0xD0 && m <= 0xD7) || m == 0xD9) { data_start = pos; break; }
        if (m >= 0xE0 && m <= 0xEF) {
            int mlen = (src[pos+2] << 8) | src[pos+3];
            if (mlen < 2) { data_start = pos; break; }
            pos += 2 + mlen;
            continue;
        }
        data_start = pos;
        break;
    }

    // Count ICC chunks needed
    int icc_chunks = 0;
    const unsigned char *icc_data = NULL;
    int icc_len = 0;
    for (int i = 0; i < num_blocks; i++) {
        if (blocks[i].data && blocks[i].type == META_TYPE_ICC) {
            icc_data = blocks[i].data;
            icc_len = blocks[i].len;
            break;
        }
    }
    if (icc_data && icc_len > 0) {
        int max_payload = 65519; // 65535 - 16 (APP2 overhead)
        icc_chunks = (icc_len + max_payload - 1) / max_payload;
        if (icc_chunks > 255) icc_chunks = 255;
    }

    char tmpPath[4096];
    snprintf(tmpPath, sizeof(tmpPath), "%s.meta_tmp", jpeg_path);
    FILE *out = fopen(tmpPath, "wb");
    if (!out) { free(src); return -1; }

    // Write SOI
    fwrite(src, 1, 2, out);

    // Write EXIF APP1 if present
    if (exif && exif->data && exif->len > 0) {
        unsigned char hdr[2] = { 0xFF, 0xE1 };
        fwrite(hdr, 1, 2, out);
        fwrite(exif->data, 1, exif->len, out);
    }

    // Write metadata blocks as APP markers
    for (int i = 0; i < num_blocks; i++) {
        if (!blocks[i].data || blocks[i].len <= 0) continue;

        if (blocks[i].type == META_TYPE_XMP) {
            // block.data = [len_hi][len_lo]["http://ns.adobe.com/xap/1.0/\0"][xml_body]
            unsigned char hdr[2] = { 0xFF, 0xE1 };
            fwrite(hdr, 1, 2, out);
            fwrite(blocks[i].data, 1, blocks[i].len, out);
        } else if (blocks[i].type == META_TYPE_IPTC) {
            // block.data = [len_hi][len_lo]["Photoshop 3.0\0"][iptc_body]
            unsigned char hdr[2] = { 0xFF, 0xED };
            fwrite(hdr, 1, 2, out);
            fwrite(blocks[i].data, 1, blocks[i].len, out);
        }
        // META_TYPE_ICC handled separately after the loop
    }

    // Write ICC as multi-segment APP2 chunks
    if (icc_data && icc_len > 0 && icc_chunks > 0) {
        int max_payload = 65519;
        int actual_chunks = (icc_len + max_payload - 1) / max_payload;
        if (actual_chunks > 255) actual_chunks = 255;
        int remaining = icc_len;
        int offset = 0;
        for (int seq = 1; seq <= actual_chunks; seq++) {
            int chunk_size = (remaining > max_payload) ? max_payload : remaining;
            int seg_len = 16 + chunk_size; // 2(len) + 12(ICC_PROFILE\0) + 1(seq) + 1(num) + data
            if (seg_len > 65535) seg_len = 65535;
            unsigned char app2_hdr[18]; // FF E2 + len + ICC_PROFILE\0 + seq + num
            app2_hdr[0] = 0xFF;
            app2_hdr[1] = 0xE2;
            app2_hdr[2] = (unsigned char)(seg_len >> 8);
            app2_hdr[3] = (unsigned char)(seg_len & 0xFF);
            memcpy(app2_hdr + 4, "ICC_PROFILE\0", 12);
            app2_hdr[16] = (unsigned char)seq;
            app2_hdr[17] = (unsigned char)actual_chunks;
            fwrite(app2_hdr, 1, 18, out);
            fwrite(icc_data + offset, 1, chunk_size, out);
            offset += chunk_size;
            remaining -= chunk_size;
        }
    }

    // Write the rest of the original JPEG file (everything after old APP markers)
    if (sz - data_start > 0)
        fwrite(src + data_start, 1, sz - data_start, out);

    int ok = !ferror(out);
    fclose(out);
    free(src);

    if (!ok) { remove(tmpPath); return -1; }

    if (!MoveFileExA(tmpPath, jpeg_path, MOVEFILE_REPLACE_EXISTING)) {
        remove(tmpPath); return -1;
    }
    return 1;
}

// ======================= END METADATA =======================

static LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE: {
            set_dark_title(hWnd);
            g_font = CreateFontA(-13, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, 0, 0, "Segoe UI");
            g_fontMono = CreateFontA(-13, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, 0, 0, "Consolas");
            int y = 10, ch = 22;
            RECT rc; GetClientRect(hWnd, &rc);
            #define MK(x,y,w,h,cls,text,bits,id) CreateWindowExA(0,cls,text,WS_CHILD|WS_VISIBLE|WS_TABSTOP|(bits),x,y,w,h, hWnd,(HMENU)(INT_PTR)id,g_hInst,NULL)

            // Manual Configuration groupbox
            HWND grp = CreateWindowExA(0, "BUTTON", "Manual Configuration", WS_CHILD|WS_VISIBLE|BS_GROUPBOX, 10, y, rc.right-20, 182, hWnd, 0, g_hInst, NULL);
            SendMessage(grp, WM_SETFONT, (WPARAM)g_font, 0);

            // Auto optimize checkbox
            g_hAutoChk = MK(25, y+18, 420, ch, "BUTTON", "Auto-optimize (experimental, may harm noise-like small details)", BS_AUTOCHECKBOX, IDC_AUTO_CHK);
            SendMessage(g_hAutoChk, WM_SETFONT, (WPARAM)g_font, 0);
            SendMessage(g_hAutoChk, BM_SETCHECK, g_autoOptimize ? BST_CHECKED : BST_UNCHECKED, 0);
            y += 28;

            // Fixed Quality radio + slider + value
            g_hQualityRadio = MK(25, y+18, 130, ch, "BUTTON", "Fixed Quality", BS_AUTORADIOBUTTON|WS_GROUP, IDC_QUALITY_RADIO);
            SendMessage(g_hQualityRadio, WM_SETFONT, (WPARAM)g_font, 0);
            g_hQualitySlider = MK(160, y+17, 280, 24, "msctls_trackbar32", "", TBS_AUTOTICKS|TBS_HORZ, IDC_QUALITY_SLIDER);
            SendMessage(g_hQualitySlider, TBM_SETRANGE, TRUE, MAKELPARAM(0, 100));
            SendMessage(g_hQualitySlider, TBM_SETPOS, TRUE, g_qualityVal);
            SendMessage(g_hQualitySlider, TBM_SETPAGESIZE, 0, 5);
            g_hQualityEdit = MK(450, y+18, 45, ch, "EDIT", "", ES_NUMBER|ES_CENTER|WS_BORDER, IDC_QUALITY_EDIT);
            SendMessage(g_hQualityEdit, WM_SETFONT, (WPARAM)g_font, 0);
            char qbuf[16]; snprintf(qbuf, sizeof(qbuf), "%d", g_qualityVal);
            SetWindowTextA(g_hQualityEdit, qbuf);
            y += 28;

            // Fixed Distance radio + slider + value
            g_hDistRadio = MK(25, y+18, 130, ch, "BUTTON", "Fixed Distance", BS_AUTORADIOBUTTON, IDC_DISTANCE_RADIO);
            SendMessage(g_hDistRadio, WM_SETFONT, (WPARAM)g_font, 0);
            g_hDistSlider = MK(160, y+17, 280, 24, "msctls_trackbar32", "", TBS_AUTOTICKS|TBS_HORZ, IDC_DISTANCE_SLIDER);
            SendMessage(g_hDistSlider, TBM_SETRANGE, TRUE, MAKELPARAM(10, 500));
            SendMessage(g_hDistSlider, TBM_SETPOS, TRUE, (int)(g_distVal * 100));
            SendMessage(g_hDistSlider, TBM_SETPAGESIZE, 0, 5);
            g_hDistEdit = MK(450, y+18, 45, ch, "EDIT", "", ES_CENTER|WS_BORDER, IDC_DISTANCE_EDIT);
            SendMessage(g_hDistEdit, WM_SETFONT, (WPARAM)g_font, 0);
            char dbuf[16]; snprintf(dbuf, sizeof(dbuf), "%.2f", g_distVal);
            SetWindowTextA(g_hDistEdit, dbuf);
            y += 28;

            // Copy skipped/errored files checkbox
            g_hCopyFailedChk = MK(25, y+18, 350, ch, "BUTTON", "Copy skipped/error files to output folder too", BS_AUTOCHECKBOX, IDC_COPY_FAILED_CHK);
            SendMessage(g_hCopyFailedChk, WM_SETFONT, (WPARAM)g_font, 0);
            SendMessage(g_hCopyFailedChk, BM_SETCHECK, BST_CHECKED, 0);
            y += 24;

            // Resize max width
            g_hResizeChk = MK(25, y+18, 155, ch, "BUTTON", "Resize max width", BS_AUTOCHECKBOX, IDC_RESIZE_CHK);
            SendMessage(g_hResizeChk, WM_SETFONT, (WPARAM)g_font, 0);
            g_hResizeEdit = MK(185, y+18, 50, ch, "EDIT", "", ES_NUMBER|WS_BORDER, IDC_RESIZE_EDIT);
            SendMessage(g_hResizeEdit, WM_SETFONT, (WPARAM)g_font, 0); SetWindowTextA(g_hResizeEdit, "2000");
            HWND rs = MK(235, y+18, 20, ch, "msctls_updown32", "", 0, IDC_RESIZE_SPIN);
            SendMessage(rs, UDM_SETRANGE, 0, MAKELPARAM(10000, 100));
            SendMessage(rs, UDM_SETPOS, 0, MAKELPARAM(2000, 0));
            SendMessage(rs, UDM_SETBUDDY, (WPARAM)g_hResizeEdit, 0);
            HWND pxLbl = MK(255, y+18, 30, ch, "STATIC", "px", SS_CENTERIMAGE, 0);
            SendMessage(pxLbl, WM_SETFONT, (WPARAM)g_font, 0);
            y += 26;

            // Threshold
            g_hThreshChk = MK(25, y+18, 210, ch, "BUTTON", "Only process if reduction >=", BS_AUTOCHECKBOX, IDC_THRESHOLD_CHK);
            SendMessage(g_hThreshChk, WM_SETFONT, (WPARAM)g_font, 0);
            g_hThreshEdit = MK(235, y+18, 45, ch, "EDIT", "", ES_NUMBER|WS_BORDER, IDC_THRESHOLD_EDIT);
            SendMessage(g_hThreshEdit, WM_SETFONT, (WPARAM)g_font, 0); SetWindowTextA(g_hThreshEdit, "15");
            HWND ts = MK(280, y+18, 20, ch, "msctls_updown32", "", 0, IDC_THRESHOLD_SPIN);
            SendMessage(ts, UDM_SETRANGE, 0, MAKELPARAM(50, 1));
            SendMessage(ts, UDM_SETPOS, 0, MAKELPARAM(15, 0));
            SendMessage(ts, UDM_SETBUDDY, (WPARAM)g_hThreshEdit, 0);
            HWND pctLbl = MK(300, y+18, 30, ch, "STATIC", "%", SS_CENTERIMAGE, 0);
            SendMessage(pctLbl, WM_SETFONT, (WPARAM)g_font, 0);
            y += 30;

            // Select Quality by default, disable Distance
            SendMessage(g_hQualityRadio, BM_SETCHECK, BST_CHECKED, 0);
            EnableWindow(g_hDistSlider, FALSE);
            EnableWindow(g_hDistEdit, FALSE);

            y = 200;

            // Drop zone
            int dropH = 100;
            g_hDropStatic = MK(10, y, rc.right-20, dropH, "STATIC", "Drag-n-Drop Images Here\r\n(JPG/JPEG/PNG, WEBP, GIF, BMP, TGA, PSD, HDR)", SS_CENTER|WS_BORDER, IDC_DROP_STATIC);
            SendMessage(g_hDropStatic, WM_SETFONT, (WPARAM)g_font, 0);
            DragAcceptFiles(hWnd, TRUE);
            y += dropH + 8;

            // Progress bar + Clear + Process buttons
            g_hProgress = MK(10, y, rc.right-240, 22, "msctls_progress32", "", 0, IDC_PROGRESS);
            SendMessage(g_hProgress, PBM_SETRANGE, 0, MAKELPARAM(0, 100));
            g_hProcessBtn = MK(rc.right-220, y-2, 100, ch+4, "BUTTON", "Process", BS_PUSHBUTTON, IDC_PROCESS_BTN);
            SendMessage(g_hProcessBtn, WM_SETFONT, (WPARAM)g_font, 0);
            g_hClearBtn = MK(rc.right-110, y-2, 100, ch+4, "BUTTON", "Clear", BS_PUSHBUTTON, IDC_CLEAR_BTN);
            SendMessage(g_hClearBtn, WM_SETFONT, (WPARAM)g_font, 0);
            ShowWindow(g_hClearBtn, SW_HIDE);
            y += 30;

            // Log
            g_hLog = MK(10, y, rc.right-20, rc.bottom-y-35, "EDIT", "", ES_MULTILINE|ES_READONLY|ES_AUTOVSCROLL|WS_VSCROLL|WS_BORDER, IDC_LOG_EDIT);
            SendMessage(g_hLog, WM_SETFONT, (WPARAM)g_fontMono, 0);

            // Status
            g_hStatus = MK(10, rc.bottom-25, rc.right-20, 20, "STATIC", "Ready", SS_CENTERIMAGE, IDC_STATUS);
            SendMessage(g_hStatus, WM_SETFONT, (WPARAM)g_font, 0);

            // Initial disable state for auto mode
            if (g_autoOptimize) {
                EnableWindow(g_hQualityRadio, FALSE);
                EnableWindow(g_hQualitySlider, FALSE);
                EnableWindow(g_hQualityEdit, FALSE);
                EnableWindow(g_hDistRadio, FALSE);
                EnableWindow(g_hDistSlider, FALSE);
                EnableWindow(g_hDistEdit, FALSE);
            }
            break;
        }
        case WM_SIZE: {
            RECT rc; GetClientRect(hWnd, &rc);
            if (g_hDropStatic) SetWindowPos(g_hDropStatic, 0, 10, 200, rc.right-20, 100, SWP_NOZORDER);
            if (g_hProgress) SetWindowPos(g_hProgress, 0, 10, 308, rc.right-240, 22, SWP_NOZORDER);
            if (g_hProcessBtn) SetWindowPos(g_hProcessBtn, 0, rc.right-220, 306, 100, 26, SWP_NOZORDER);
            if (g_hClearBtn) SetWindowPos(g_hClearBtn, 0, rc.right-110, 306, 100, 26, SWP_NOZORDER);
            if (g_hLog) SetWindowPos(g_hLog, 0, 10, 338, rc.right-20, rc.bottom-373, SWP_NOZORDER);
            if (g_hStatus) SetWindowPos(g_hStatus, 0, 10, rc.bottom-25, rc.right-20, 20, SWP_NOZORDER);
            break;
        }
        case WM_CTLCOLORSTATIC: case WM_CTLCOLOREDIT: case WM_CTLCOLORLISTBOX: {
            HDC hdc = (HDC)wParam;
            if (g_dark) {
                SetBkColor(hdc, RGB(0x20,0x20,0x20));
                SetTextColor(hdc, RGB(0xDC,0xDC,0xDC));
                if (msg == WM_CTLCOLOREDIT || msg == WM_CTLCOLORLISTBOX)
                    SetBkColor(hdc, RGB(0x2D,0x2D,0x2D));
                return (LRESULT)((msg == WM_CTLCOLOREDIT || msg == WM_CTLCOLORLISTBOX) ? g_editBrush : g_bgBrush);
            }
            return (LRESULT)g_bgBrush;
        }
        case WM_CTLCOLORBTN: {
            HDC hdc = (HDC)wParam;
            if (g_dark) {
                SetBkColor(hdc, RGB(0x20,0x20,0x20));
                SetTextColor(hdc, RGB(0xDC,0xDC,0xDC));
                return (LRESULT)g_bgBrush;
            }
            return (LRESULT)g_bgBrush;
        }
        case WM_HSCROLL: {
            HWND hwnd = (HWND)lParam;
            if (hwnd == g_hQualitySlider) {
                g_qualityVal = (int)SendMessage(g_hQualitySlider, TBM_GETPOS, 0, 0);
                char b[16]; snprintf(b, sizeof(b), "%d", g_qualityVal);
                g_updating = 1; SetWindowTextA(g_hQualityEdit, b); g_updating = 0;
            } else if (hwnd == g_hDistSlider) {
                g_distVal = (int)SendMessage(g_hDistSlider, TBM_GETPOS, 0, 0) / 100.0f;
                char b[16]; snprintf(b, sizeof(b), "%.2f", g_distVal);
                g_updating = 1; SetWindowTextA(g_hDistEdit, b); g_updating = 0;
            }
            break;
        }
        case WM_DROPFILES: {
            HDROP hDrop = (HDROP)wParam;
            int count = DragQueryFileA(hDrop, 0xFFFFFFFF, NULL, 0);
            for (int i = 0; i < count; i++) {
                char path[4096]; DragQueryFileA(hDrop, i, path, sizeof(path));
                const char *ext = strrchr(path, '.');
                if (!ext) continue;
                char le[16]; strncpy(le, ext, 15); le[15] = 0; _strlwr(le);
                if (strcmp(le, ".jpg") == 0 || strcmp(le, ".jpeg") == 0 ||
                    strcmp(le, ".png") == 0 || strcmp(le, ".webp") == 0 ||
                    strcmp(le, ".gif") == 0 || strcmp(le, ".bmp") == 0 ||
                    strcmp(le, ".tga") == 0 || strcmp(le, ".psd") == 0 ||
                    strcmp(le, ".hdr") == 0 || strcmp(le, ".ppm") == 0 ||
                    strcmp(le, ".pgm") == 0 || strcmp(le, ".pbm") == 0 ||
                    strcmp(le, ".pnm") == 0 || strcmp(le, ".pfm") == 0 ||
                    strcmp(le, ".pam") == 0) {
                    add_file(path);
                    WCHAR wpath[4096];
                    DragQueryFileW(hDrop, i, wpath, 4096);
                    g_files[g_fileCount-1].wpath = _wcsdup(wpath);
                    FileEntry *fe = &g_files[g_fileCount-1];
                    if (fe->is_jpeg && fe->s_h > 0) {
                        char tag[8];
                        if (fe->s_h == 2 && fe->s_v == 2 && fe->cb_h == 1 && fe->cb_v == 1)
                            snprintf(tag,8,"4:2:0");
                        else if (fe->s_h == 2 && fe->s_v == 2 && fe->cb_h == 1 && fe->cb_v == 2)
                            snprintf(tag,8,"4:4:0");
                        else if (fe->s_h == 2 && fe->s_v == 1) snprintf(tag,8,"4:2:2");
                        else if (fe->s_h == 1 && fe->s_v == 1) snprintf(tag,8,"4:4:4");
                        else snprintf(tag,8,"%dx%d", fe->s_h, fe->s_v);
                        log_msg("Added: %s (%s, %s)", path, tag, fe->prog ? "progressive" : "baseline");
                    } else {
                        log_msg("Added: %s", path);
                    }
                }
            }
            DragFinish(hDrop);
            ShowWindow(g_hClearBtn, g_fileCount > 0 ? SW_SHOW : SW_HIDE);
            char buf[64]; snprintf(buf, sizeof(buf), "Total: %d files | Ready", g_fileCount);
            set_status(buf);
            break;
        }
        case WM_COMMAND: {
            int id = LOWORD(wParam);
            int code = HIWORD(wParam);
            if (id == IDC_QUALITY_RADIO && code == BN_CLICKED) {
                g_useQuality = 1;
                EnableWindow(g_hQualitySlider, TRUE);
                EnableWindow(g_hQualityEdit, TRUE);
                EnableWindow(g_hDistSlider, FALSE);
                EnableWindow(g_hDistEdit, FALSE);
            }
            if (id == IDC_DISTANCE_RADIO && code == BN_CLICKED) {
                g_useQuality = 0;
                EnableWindow(g_hQualitySlider, FALSE);
                EnableWindow(g_hQualityEdit, FALSE);
                EnableWindow(g_hDistSlider, TRUE);
                EnableWindow(g_hDistEdit, TRUE);
            }
            if (id == IDC_AUTO_CHK && code == BN_CLICKED) {
                g_autoOptimize = (int)SendMessage(g_hAutoChk, BM_GETCHECK, 0, 0) == BST_CHECKED;
                int en = !g_autoOptimize;
                EnableWindow(g_hQualityRadio, en);
                EnableWindow(g_hQualitySlider, en);
                EnableWindow(g_hQualityEdit, en);
                EnableWindow(g_hDistRadio, en);
                EnableWindow(g_hDistSlider, en);
                EnableWindow(g_hDistEdit, en);
                if (en && g_useQuality) {
                    EnableWindow(g_hDistSlider, FALSE);
                    EnableWindow(g_hDistEdit, FALSE);
                } else if (en && !g_useQuality) {
                    EnableWindow(g_hQualitySlider, FALSE);
                    EnableWindow(g_hQualityEdit, FALSE);
                }
            }
            if (id == IDC_RESIZE_CHK && code == BN_CLICKED) {
                g_resizeEnabled = (int)SendMessage(g_hResizeChk, BM_GETCHECK, 0, 0);
            }
            if (id == IDC_THRESHOLD_CHK && code == BN_CLICKED) {
                g_thresholdEnabled = (int)SendMessage(g_hThreshChk, BM_GETCHECK, 0, 0);
            }
            if (id == IDC_COPY_FAILED_CHK && code == BN_CLICKED) {
                g_copyFailed = (int)SendMessage(g_hCopyFailedChk, BM_GETCHECK, 0, 0);
            }
            if (id == IDC_RESIZE_EDIT && code == EN_CHANGE) {
                char buf[16]; GetWindowTextA(g_hResizeEdit, buf, sizeof(buf));
                g_resizeWidth = atoi(buf); if (g_resizeWidth < 100) g_resizeWidth = 100;
            }
            if (id == IDC_THRESHOLD_EDIT && code == EN_CHANGE) {
                char buf[16]; GetWindowTextA(g_hThreshEdit, buf, sizeof(buf));
                g_thresholdPct = atoi(buf); if (g_thresholdPct < 1) g_thresholdPct = 1; if (g_thresholdPct > 50) g_thresholdPct = 50;
            }
            if (id == IDC_QUALITY_EDIT && code == EN_CHANGE && !g_updating) {
                char buf[16]; GetWindowTextA(g_hQualityEdit, buf, sizeof(buf));
                int v = atoi(buf);
                if (v >= 0 && v <= 100) {
                    g_qualityVal = v;
                    SendMessage(g_hQualitySlider, TBM_SETPOS, TRUE, v);
                }
            }
            if (id == IDC_DISTANCE_EDIT && code == EN_CHANGE && !g_updating) {
                char buf[16]; GetWindowTextA(g_hDistEdit, buf, sizeof(buf));
                float d = (float)atof(buf);
                if (d > 0) {
                    g_distVal = d;
                    int tick = (int)(d * 100.0f + 0.5f);
                    if (tick >= 10 && tick <= 500) {
                        g_updating = 1;
                        SendMessage(g_hDistSlider, TBM_SETPOS, TRUE, tick);
                        g_updating = 0;
                    }
                }
            }
            if (id == IDC_CLEAR_BTN && code == BN_CLICKED) {
                if (g_processing) break;
                for (int i = 0; i < g_fileCount; i++) { free(g_files[i].path); free(g_files[i].wpath); }
                g_fileCount = 0;
                set_status("Ready");
                ShowWindow(g_hClearBtn, SW_HIDE);
                SendMessage(g_hLog, WM_SETTEXT, 0, (LPARAM)"");
            }
            if (id == IDC_PROCESS_BTN) {
                if (g_processing) { g_cancel = 1; SetWindowTextA(g_hProcessBtn, "Cancelling..."); EnableWindow(g_hProcessBtn, FALSE); break; }
                if (g_fileCount == 0) { log_msg("No files to process."); break; }

                // Warn if animated GIFs
                int animGifIdx = -1;
                for (int i = 0; i < g_fileCount; i++)
                    if (g_files[i].is_anim_gif) { animGifIdx = i; break; }
                if (animGifIdx >= 0) {
                    char msg[4096];
                    snprintf(msg, sizeof(msg), "Animated GIF detected:\n%s\n\nOnly the first frame will be encoded.\nContinue?", g_files[animGifIdx].path);
                    if (MessageBoxA(hWnd, msg, "Animated GIF", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) == IDNO) {
                        free(g_files[animGifIdx].path);
                        free(g_files[animGifIdx].wpath);
                        for (int j = animGifIdx; j < g_fileCount - 1; j++) g_files[j] = g_files[j+1];
                        g_fileCount--;
                    }
                }

                // Determine output directory based on first file
                char firstDir[MAX_PATH];
                strncpy(firstDir, g_files[0].path, sizeof(firstDir)-1); firstDir[sizeof(firstDir)-1] = 0;
                char *p = strrchr(firstDir, '\\');
                if (p) *p = 0;

                // Check if all files are in the same directory
                int sameDir = 1;
                for (int i = 1; i < g_fileCount; i++) {
                    char d[MAX_PATH];
                    strncpy(d, g_files[i].path, sizeof(d)-1); d[sizeof(d)-1] = 0;
                    char *q = strrchr(d, '\\');
                    if (q) *q = 0;
                    if (_stricmp(d, firstDir) != 0) { sameDir = 0; break; }
                }

                if (!sameDir) {
                    char warnMsg[4096];
                    snprintf(warnMsg, sizeof(warnMsg),
                        "Files are located in different directories.\n\n"
                        "All output will be saved to the 'processed' folder next to the first file:\n%s",
                        firstDir);
                    MessageBoxA(hWnd, warnMsg, "Different Directories", MB_OK | MB_ICONINFORMATION);
                }

                // Ensure output directory exists
                char outDir[MAX_PATH];
                snprintf(outDir, sizeof(outDir), "%s\\processed", firstDir);
                if (!CreateDirectoryA(outDir, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) {
                    char em[512]; snprintf(em, sizeof(em),
                        "Cannot create output folder:\n%s\n\n"
                        "Check permissions or disk space.", outDir);
                    MessageBoxA(hWnd, em, "Error", MB_OK | MB_ICONERROR);
                    log_msg("ERROR: cannot create output directory");
                    break;
                }

                log_msg("Output folder: %s", outDir);

                g_processing = 1; g_cancel = 0;
                ShowWindow(g_hClearBtn, SW_HIDE);
                SetWindowTextA(g_hProcessBtn, "Cancel");
                SendMessage(g_hLog, WM_SETTEXT, 0, (LPARAM)"");
                set_progress(0);
                int total = g_fileCount, succeeded = 0, skipped = 0, failed = 0;

                for (int i = 0; i < total && !g_cancel; i++) {
                    const char *fname = strrchr(g_files[i].path, '\\');
                    fname = fname ? fname + 1 : g_files[i].path;
                    SetWindowTextA(g_hDropStatic, fname); UpdateWindow(g_hDropStatic);
                    set_progress((i * 100) / total);
                    int result = process_file(&g_files[i], outDir);
                    if (result > 0) {
                        succeeded++;
                    } else if (result == 0) {
                        skipped++;
                        log_msg("SKIP: %s (no reduction)", fname);
                    } else {
                        failed++;
                        log_msg("ERROR: %s", fname);
                    }
                    if (g_copyFailed && result <= 0) {
                        WCHAR outDirW[MAX_PATH], destW[4096];
                        MultiByteToWideChar(CP_ACP, 0, outDir, -1, outDirW, MAX_PATH);
                        const WCHAR *wfname = wcsrchr(g_files[i].wpath, L'\\');
                        wfname = wfname ? wfname + 1 : g_files[i].wpath;
                        if (result < 0) {
                            wcscpy(destW, outDirW);
                            wcscat(destW, L"\\errors");
                            CreateDirectoryW(destW, NULL);
                            wcscat(destW, L"\\");
                            wcscat(destW, wfname);
                        } else {
                            wcscpy(destW, outDirW);
                            wcscat(destW, L"\\");
                            wcscat(destW, wfname);
                        }
                        if (CopyFileW(g_files[i].wpath, destW, FALSE))
                            log_msg("  -> copied original to output%s", result < 0 ? " (errors)" : "");
                        else
                            log_msg("  -> FAILED to copy original to output");
                    }
                    char s[128]; snprintf(s, sizeof(s), "[%d/%d] OK:%d SKIP:%d ERR:%d", i+1, total, succeeded, skipped, failed);
                    set_status(s);
                    MSG msg;
                    while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
                        TranslateMessage(&msg); DispatchMessage(&msg);
                    }
                }
                set_progress(g_cancel ? 0 : 100);
                char summary[256];
                snprintf(summary, sizeof(summary), "Done. Total:%d  OK:%d  Skipped:%d  Errors:%d%s",
                         total, succeeded, skipped, failed, g_cancel ? " (cancelled)" : "");
                log_msg("--- %s ---", summary);
                set_status(summary);
                SetWindowTextA(g_hDropStatic, "Drag-n-Drop Images Here\r\n(JPG/JPEG/PNG, WEBP, GIF, BMP, TGA, PSD, HDR)");
                g_processing = 0; g_cancel = 0;
                SetWindowTextA(g_hProcessBtn, "Process");
                EnableWindow(g_hProcessBtn, TRUE);
                // Clear file list
                for (int i = 0; i < g_fileCount; i++) { free(g_files[i].path); free(g_files[i].wpath); }
                g_fileCount = 0;
                ShowWindow(g_hClearBtn, SW_HIDE);
                // Clean up any leftover temp files in outDir
                char pattern[4096]; snprintf(pattern, sizeof(pattern), "%s\\*.cjpegli_tmp", outDir);
                WIN32_FIND_DATAA fd; HANDLE hf = FindFirstFileA(pattern, &fd);
                if (hf != INVALID_HANDLE_VALUE) {
                    do { char tp[4096]; snprintf(tp, sizeof(tp), "%s\\%s", outDir, fd.cFileName); remove(tp); }
                    while (FindNextFileA(hf, &fd));
                    FindClose(hf);
                }
            }
            break;
        }
        case WM_NOTIFY: {
            NMHDR *nm = (NMHDR*)lParam;
            if (nm->code == UDN_DELTAPOS) {
                NMUPDOWN *ud = (NMUPDOWN*)nm;
                if (nm->idFrom == IDC_RESIZE_SPIN) {
                    int v = ud->iPos + ud->iDelta;
                    if (v >= 100 && v <= 10000) { g_resizeWidth = v; char b[16]; snprintf(b, sizeof(b), "%d", v); SetWindowTextA(g_hResizeEdit, b); }
                }
                if (nm->idFrom == IDC_THRESHOLD_SPIN) {
                    int v = ud->iPos + ud->iDelta;
                    if (v >= 1 && v <= 50) { g_thresholdPct = v; char b[16]; snprintf(b, sizeof(b), "%d", v); SetWindowTextA(g_hThreshEdit, b); }
                }
            }
            break;
        }
        case WM_DESTROY: PostQuitMessage(0); break;
        default: return DefWindowProcW(hWnd, msg, wParam, lParam);
    }
    return 0;
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int) {
    g_hInst = hInst;
    GetModuleFileNameA(NULL, g_selfDir, sizeof(g_selfDir));
    char *p = strrchr(g_selfDir, '\\'); if (p) *p = 0;
    snprintf(g_cjpegliPath, sizeof(g_cjpegliPath), "%s\\cjpegli.exe", g_selfDir);

    if (GetFileAttributesA(g_cjpegliPath) == INVALID_FILE_ATTRIBUTES) {
        char msg[1024];
        snprintf(msg, sizeof(msg),
            "cjpegli.exe not found in the program folder.\n\n"
            "Please download it from:\n"
            "https://github.com/libjxl/libjxl/releases\n\n"
            "Extract cjpegli.exe from the jxl-x64-windows-static.zip archive\n"
            "and place it alongside this application.\n\n"
            "The program will close now.");
        MessageBoxA(NULL, msg, "Missing cjpegli.exe", MB_OK | MB_ICONERROR);
        return 1;
    }

    INITCOMMONCONTROLSEX icc = {sizeof(icc), ICC_STANDARD_CLASSES | ICC_UPDOWN_CLASS | ICC_PROGRESS_CLASS | ICC_BAR_CLASSES};
    InitCommonControlsEx(&icc);

    g_dark = is_dark_mode();
    g_bgBrush = CreateSolidBrush(g_dark ? RGB(0x20,0x20,0x20) : RGB(0xF0,0xF0,0xF0));
    g_editBrush = CreateSolidBrush(g_dark ? RGB(0x2D,0x2D,0x2D) : RGB(0xFF,0xFF,0xFF));

    WNDCLASSW wc = {0};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = g_bgBrush;
    wc.lpszClassName = L"JPEGLIOptimizerClass";
    RegisterClassW(&wc);

    RECT wr = {0, 0, 620, 624};
    AdjustWindowRect(&wr, WS_OVERLAPPEDWINDOW, FALSE);
    g_hWnd = CreateWindowExW(0, wc.lpszClassName, L"JPEGLI Optimizer",
        WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX & ~WS_THICKFRAME,
        CW_USEDEFAULT, CW_USEDEFAULT, wr.right-wr.left, wr.bottom-wr.top,
        NULL, NULL, hInst, NULL);
    ShowWindow(g_hWnd, SW_SHOW);
    UpdateWindow(g_hWnd);

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg); DispatchMessage(&msg);
    }
    return 0;
}
