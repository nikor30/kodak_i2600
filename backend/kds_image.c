/* Host-side image work: find the sheet, colour correction, gray and black/white output.
 * Source: docs/protocol/image-processing.md (steps 2, 4, 5 and 6, and "Processing in the C backend").
 */
#define _GNU_SOURCE
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

/* first..last of the longest run of entries with count >= min, or -1 */
static void longest_run(const int *count, int n, int min, int *first, int *last)
{
    int best = 0, start = -1;
    *first = *last = -1;
    for (int i = 0; i <= n; i++) {
        int on = i < n && count[i] >= min && count[i] > 0;
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

static int cmp_double(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

/* Robust line through the points (pos[i], val[i]), in cells: median slope, then least
 * squares on the points within 2 cells of that line. The outer 5 % at each end are
 * ignored. Returns the number of inliers (0 = no usable edge). */
static int fit_edge(const double *pos, const double *val, int n, double *slope)
{
    int skip = n / 20;
    pos += skip;
    val += skip;
    n -= 2 * skip;
    if (n < 20)
        return 0;
    double *tmp = malloc((size_t)n * sizeof(double));
    if (!tmp)
        return 0;
    int half = n / 2, m = 0;
    for (int i = 0; i + half < n; i++)
        tmp[m++] = (val[i + half] - val[i]) / (pos[i + half] - pos[i]);
    qsort(tmp, (size_t)m, sizeof(double), cmp_double);
    double s = tmp[m / 2];
    for (int i = 0; i < n; i++)
        tmp[i] = val[i] - s * pos[i];
    qsort(tmp, (size_t)n, sizeof(double), cmp_double);
    double c = tmp[n / 2];
    free(tmp);
    double sx = 0, sy = 0, sxx = 0, sxy = 0;
    int in = 0;
    for (int i = 0; i < n; i++) {
        if (fabs(val[i] - s * pos[i] - c) > 2)
            continue;
        in++;
        sx += pos[i];
        sy += val[i];
        sxx += pos[i] * pos[i];
        sxy += pos[i] * val[i];
    }
    double den = in * sxx - sx * sx;
    if (in < 20 || den == 0)
        return 0;
    *slope = (in * sxy - sx * sy) / den;
    return in;
}

void kds_full_frame(int lines, struct kds_sheet *s)
{
    s->w = KDS_LINE_PX;
    s->h = lines;
    s->x0 = s->y0 = 0;
    s->cs = 1;
    s->sn = 0;
    s->angle = 0;
}

/* Skew of the sheet in radians (positive = top edge falls to the right), from its four
 * edges inside the cell box bx0..bx1, by0..by1. */
static double find_skew(const uint8_t *mask, int cw, int ch, int bx0, int bx1, int by0, int by1)
{
    int nmax = cw > ch ? cw : ch;
    double *pos = malloc((size_t)nmax * sizeof(double)), *val = malloc((size_t)nmax * sizeof(double));
    double angle[4], weight[4], total = 0;
    int edges = 0;
    if (!pos || !val) {
        free(pos);
        free(val);
        return 0;
    }
    for (int e = 0; e < 4; e++) {       /* top, bottom (per column); left, right (per row) */
        int per_col = e < 2, last = e & 1, n = 0;
        int a0 = per_col ? bx0 : by0, a1 = per_col ? bx1 : by1;     /* along the edge */
        int c0 = per_col ? by0 : bx0, c1 = per_col ? by1 : bx1;     /* across it */
        int border = last ? (per_col ? ch : cw) - 1 : 0;
        for (int a = a0; a <= a1; a++) {
            int found = -1;
            for (int k = 0; k <= c1 - c0 && found < 0; k++) {
                int c = last ? c1 - k : c0 + k;
                if (per_col ? mask[c * cw + a] : mask[a * cw + c])
                    found = c;
            }
            if (found < 0 || found == border)   /* the sheet leaves the image here */
                continue;
            pos[n] = a;
            val[n++] = found;
        }
        double slope;
        int in = fit_edge(pos, val, n, &slope);
        if (!in)
            continue;
        angle[edges] = per_col ? atan(slope) : atan(-slope);
        weight[edges] = in;
        total += in;
        edges++;
    }
    free(pos);
    free(val);
    if (!edges)
        return 0;
    for (int i = 0; i < edges; i++)     /* weighted median */
        for (int j = i + 1; j < edges; j++)
            if (angle[j] < angle[i]) {
                double t = angle[i]; angle[i] = angle[j]; angle[j] = t;
                t = weight[i]; weight[i] = weight[j]; weight[j] = t;
            }
    double acc = 0;
    for (int i = 0; i < edges; i++) {
        acc += weight[i];
        if (acc >= total / 2)
            return angle[i];
    }
    return angle[edges - 1];
}

void kds_find_sheet(const uint8_t *raw, int lines, int deskew, struct kds_sheet *s)
{
    int cw = KDS_LINE_PX / CELL, ch = lines / CELL;
    kds_full_frame(lines, s);
    if (ch < 1)
        return;
    int nb = 2 * (cw + ch);             /* histogram bins of the rotated projection */
    uint8_t *mask = calloc((size_t)cw, (size_t)ch);
    int *cols = calloc((size_t)cw, sizeof(int)), *rows = calloc((size_t)ch, sizeof(int));
    int *hu = calloc((size_t)nb, sizeof(int)), *hv = calloc((size_t)nb, sizeof(int));
    if (!mask || !cols || !rows || !hu || !hv)
        goto out;
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
                mask[cy * cw + cx] = 1;
                cols[cx]++;
                rows[cy]++;
            }
        }
    }
    /* Rough box: the longest unbroken run of columns and of rows with at least 10 % paper.
     * Single bright columns at the sensor edges must not count as sheet. */
    int bx0, bx1, by0, by1;
    longest_run(cols, cw, (ch + 9) / 10, &bx0, &bx1);
    longest_run(rows, ch, (cw + 9) / 10, &by0, &by1);
    if (bx0 < 0 || by0 < 0)
        goto out;                       /* no paper found: keep the full frame */

    double angle = deskew ? find_skew(mask, cw, ch, bx0, bx1, by0, by1) : 0;
    if (fabs(angle) < 0.15 * M_PI / 180)
        angle = 0;
    double cs = cos(angle), sn = sin(angle);

    /* Project the paper cells of the box onto the sheet's own axes (u along a row, v down)
     * and keep the rows and columns that are more than half paper. */
    int xc = (bx0 + bx1 + 1) * CELL / 2, yc = (by0 + by1 + 1) * CELL / 2, off = nb / 2;
    int umax = 0, vmax = 0;
    for (int cy = by0; cy <= by1; cy++)
        for (int cx = bx0; cx <= bx1; cx++) {
            if (!mask[cy * cw + cx])
                continue;
            double x = cx * CELL + CELL / 2.0 - xc, y = cy * CELL + CELL / 2.0 - yc;
            int bu = (int)floor((x * cs + y * sn) / CELL) + off, bv = (int)floor((-x * sn + y * cs) / CELL) + off;
            if (bu < 0 || bu >= nb || bv < 0 || bv >= nb)
                continue;
            if (++hu[bu] > umax)
                umax = hu[bu];
            if (++hv[bv] > vmax)
                vmax = hv[bv];
        }
    int u0, u1, v0, v1;
    longest_run(hu, nb, umax / 2 + 1, &u0, &u1);
    longest_run(hv, nb, vmax / 2 + 1, &v0, &v1);
    if (u0 < 0 || v0 < 0)
        goto out;
    int trim = 3;
    double ua = (u0 - off) * CELL + trim, va = (v0 - off) * CELL + trim;
    int w = (u1 - u0 + 1) * CELL - 2 * trim, h = (v1 - v0 + 1) * CELL - 2 * trim;
    if (w < 16 || h < 16)
        goto out;
    s->w = w;
    s->h = h;
    s->cs = cs;
    s->sn = sn;
    s->angle = angle;
    s->x0 = xc + ua * cs - va * sn;
    s->y0 = yc + ua * sn + va * cs;
out:
    free(mask);
    free(cols);
    free(rows);
    free(hu);
    free(hv);
}

/* One row of the sheet as raw RGB (s->w pixels); bilinear when the sheet is rotated. */
void kds_sheet_line(const uint8_t *raw, int lines, const struct kds_sheet *s, int row, uint8_t *out)
{
    if (s->sn == 0) {
        int x = (int)s->x0, y = (int)s->y0 + row;
        if (x >= 0 && y >= 0 && y < lines && x + s->w <= KDS_LINE_PX) {
            memcpy(out, raw + ((size_t)y * KDS_LINE_PX + (size_t)x) * 3, (size_t)s->w * 3);
            return;
        }
    }
    float fx = (float)(s->x0 - row * s->sn), fy = (float)(s->y0 + row * s->cs);
    float dx = (float)s->cs, dy = (float)s->sn;
    for (int i = 0; i < s->w; i++, fx += dx, fy += dy, out += 3) {
        int ix = (int)floorf(fx), iy = (int)floorf(fy);
        if (ix < 0 || iy < 0 || ix >= KDS_LINE_PX - 1 || iy >= lines - 1) {
            out[0] = out[1] = out[2] = 0;
            continue;
        }
        int ax = (int)((fx - ix) * 256), ay = (int)((fy - iy) * 256);
        const uint8_t *p = raw + ((size_t)iy * KDS_LINE_PX + (size_t)ix) * 3, *q = p + KDS_LINE;
        for (int c = 0; c < 3; c++) {
            int top = p[c] * (256 - ax) + p[c + 3] * ax, bot = q[c] * (256 - ax) + q[c + 3] * ax;
            out[c] = (uint8_t)((top * (256 - ay) + bot * ay + 32768) >> 16);
        }
    }
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
