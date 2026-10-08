/* Host-side image work: find the sheet, colour correction, gray and black/white output.
 * Source: docs/protocol/image-processing.md (steps 2 and 6, and "Simplified processing").
 */
#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "kds.h"

#define CELL 4
#define PAPER_LEVEL 14                  /* mean of max(R, G, B) over a cell above this = paper */

/* out = 255 * clip(M * raw/255 + o)^(1/1.6) */
static const float M[3][3] = {
    { 2.734f, -1.091f, -0.309f },
    { -0.294f, 1.694f, -0.036f },
    { -0.025f, -0.110f, 1.377f },
};
static const float O[3] = { -0.037f, -0.040f, -0.041f };

#define LUT_N 1024
static uint8_t gamma_lut[LUT_N];
static pthread_once_t lut_once = PTHREAD_ONCE_INIT;

static void lut_init(void)
{
    for (int i = 0; i < LUT_N; i++)
        gamma_lut[i] = (uint8_t)(255.0 * pow(i / (double)(LUT_N - 1), 1 / 1.6) + 0.5);
}

/* first..last of the longest run of entries with count >= 10 % of `full`, or -1 */
static void longest_run(const int *count, int n, int full, int *first, int *last)
{
    int best = 0, start = -1;
    *first = *last = -1;
    for (int i = 0; i <= n; i++) {
        int on = i < n && count[i] * 10 >= full;
        if (on && start < 0)
            start = i;
        if (!on && start >= 0) {
            if (i - start > best) {
                best = i - start;
                *first = start;
                *last = i - 1;
            }
            start = -1;
        }
    }
}

void kds_find_sheet(const uint8_t *raw, int lines, struct kds_crop *crop)
{
    int cw = KDS_LINE_PX / CELL, ch = lines / CELL;
    crop->x = crop->y = 0;
    crop->w = KDS_LINE_PX;
    crop->h = lines;
    if (ch < 1)
        return;
    int *cols = calloc((size_t)cw, sizeof(int));
    int *rows = calloc((size_t)ch, sizeof(int));
    if (!cols || !rows) {
        free(cols);
        free(rows);
        return;
    }
    for (int cy = 0; cy < ch; cy++) {
        for (int cx = 0; cx < cw; cx++) {
            int sum = 0;
            for (int y = 0; y < CELL; y++) {
                const uint8_t *p = raw + ((size_t)(cy * CELL + y) * KDS_LINE_PX + (size_t)cx * CELL) * 3;
                for (int x = 0; x < CELL; x++, p += 3) {
                    int m = p[0] > p[1] ? p[0] : p[1];
                    sum += m > p[2] ? m : p[2];
                }
            }
            if (sum > PAPER_LEVEL * CELL * CELL) {
                cols[cx]++;
                rows[cy]++;
            }
        }
    }
    /* The sheet is the longest unbroken run of counted columns and of counted rows:
     * single bright columns at the sensor edges must not widen the box. */
    int x0, x1, y0, y1;
    longest_run(cols, cw, ch, &x0, &x1);
    longest_run(rows, ch, cw, &y0, &y1);
    free(cols);
    free(rows);
    if (x0 < 0 || y0 < 0)
        return;                         /* no paper found: keep the full frame */
    crop->x = x0 * CELL;
    crop->y = y0 * CELL;
    crop->w = (x1 - x0 + 1) * CELL;
    crop->h = (y1 - y0 + 1) * CELL;
}

int kds_bytes_per_line(int mode, int w)
{
    switch (mode) {
    case KDS_MODE_GRAY:
        return w;
    case KDS_MODE_LINEART:
        return (w + 7) / 8;
    default:
        return w * 3;
    }
}

static inline uint8_t correct(const uint8_t *p, int c)
{
    float v = (M[c][0] * p[0] + M[c][1] * p[1] + M[c][2] * p[2]) * (1.0f / 255) + O[c];
    int i = (int)(v * (LUT_N - 1) + 0.5f);
    return gamma_lut[i < 0 ? 0 : i >= LUT_N ? LUT_N - 1 : i];
}

void kds_convert_line(const uint8_t *raw, int w, int mode, int threshold, uint8_t *out)
{
    pthread_once(&lut_once, lut_init);
    if (mode == KDS_MODE_RAW) {
        memcpy(out, raw, (size_t)w * 3);
        return;
    }
    if (mode == KDS_MODE_LINEART)
        memset(out, 0, (size_t)(w + 7) / 8);
    for (int x = 0; x < w; x++, raw += 3) {
        uint8_t r = correct(raw, 0), g = correct(raw, 1), b = correct(raw, 2);
        if (mode == KDS_MODE_COLOR) {
            out[3 * x] = r;
            out[3 * x + 1] = g;
            out[3 * x + 2] = b;
            continue;
        }
        int gray = (299 * r + 587 * g + 114 * b + 500) / 1000;
        if (mode == KDS_MODE_GRAY)
            out[x] = (uint8_t)gray;
        else if (gray < threshold)
            out[x >> 3] |= 0x80 >> (x & 7);     /* SANE lineart: 1 = black, first pixel in the top bit */
    }
}
