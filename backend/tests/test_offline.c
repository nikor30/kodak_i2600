/* Offline tests of the parts that need no scanner: page splitter, sheet detection,
 * pixel conversion, sequence file parser.
 *
 *   test_offline SEQUENCE.seq [FRONT.raw REAR.raw]
 *
 * The optional raw files are image streams saved from a real scan (local captures).
 */
#define _GNU_SOURCE
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kds.h"

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

struct got {
    int n, number[16], lines[16];
    uint8_t first[16];
    struct kds_sheet sheet[16];
    int find_sheet;
};

static void on_page(void *arg, int number, uint8_t *data, int lines)
{
    struct got *g = arg;
    if (g->n < 16) {
        g->number[g->n] = number;
        g->lines[g->n] = lines;
        g->first[g->n] = data[0];
        if (g->find_sheet)
            kds_find_sheet(data, lines, 1, &g->sheet[g->n]);
    }
    g->n++;
    free(data);
}

static size_t put_trailer(uint8_t *p, int number, int k)
{
    static const uint8_t tags[6] = { 0xf6, 0xf9, 0xfa, 0xfc, 0xfd, 0xfe };
    uint8_t *q = p;
    *q++ = 0; *q++ = 1; *q++ = 0; *q++ = 0; *q++ = (uint8_t)(number >> 8); *q++ = (uint8_t)number; *q++ = 2; *q++ = 0xf5;
    for (int i = 0; i < 6; i++) { *q++ = 5; *q++ = 10; *q++ = 1; *q++ = tags[i]; }
    for (int i = 0; i < 2 * k; i++) *q++ = 0x55;
    *q++ = 0; *q++ = (uint8_t)k; *q++ = 1; *q++ = 0xff;
    return (size_t)(q - p);
}

static void feed(struct kds_splitter *s, const uint8_t *data, size_t n, size_t chunk)
{
    for (size_t off = 0; off < n; off += chunk) {
        size_t m = n - off < chunk ? n - off : chunk;
        uint8_t *p = kds_split_reserve(s, m);
        memcpy(p, data + off, m);
        CHECK(kds_split_commit(s, m) == KDS_OK, "commit");
    }
}

static void test_splitter(void)
{
    /* pages of 7, 3 and 5 lines; the second page has trailer-like tag bytes inside its data */
    static const int lines[3] = { 7, 3, 5 }, ks[3] = { 0, 126, 255 };
    size_t cap = 20 * KDS_LINE, n = 0;
    uint8_t *stream = malloc(cap);
    for (int p = 0; p < 3; p++) {
        memset(stream + n, 0x10 + p, (size_t)lines[p] * KDS_LINE);
        if (p == 1)     /* tags at a line boundary, but no valid end marker after them */
            put_trailer(stream + n + KDS_LINE, 99, 0), stream[n + KDS_LINE + 32 + 3] = 0;
        n += (size_t)lines[p] * KDS_LINE;
        n += put_trailer(stream + n, p + 2, ks[p]);
    }
    static const size_t chunks[] = { 1 << 18, 16384, 7741, 13 };
    for (size_t c = 0; c < sizeof(chunks) / sizeof(chunks[0]); c++) {
        struct got g = { 0 };
        struct kds_splitter s;
        kds_split_init(&s, on_page, &g);
        feed(&s, stream, n, chunks[c]);
        CHECK(g.n == 3, "chunk %zu: %d pages", chunks[c], g.n);
        for (int p = 0; p < 3 && p < g.n; p++)
            CHECK(g.number[p] == p + 2 && g.lines[p] == lines[p] && g.first[p] == 0x10 + p,
                  "chunk %zu page %d: number %d, %d lines, first byte %02x", chunks[c], p, g.number[p], g.lines[p], g.first[p]);
        CHECK(kds_split_leftover_lines(&s) == 0, "leftover");
        kds_split_free(&s);
    }
    free(stream);
}

/* a white sheet of sw x sh pixels, centre (cx, cy), turned by deg, on a dark background */
static uint8_t *draw_sheet(int lines, double cx, double cy, int sw, int sh, double deg)
{
    uint8_t *raw = calloc((size_t)lines, KDS_LINE);
    double cs = cos(deg * M_PI / 180), sn = sin(deg * M_PI / 180);
    for (int y = 0; y < lines; y++)
        for (int x = 0; x < KDS_LINE_PX; x++) {
            uint8_t *p = raw + ((size_t)y * KDS_LINE_PX + x) * 3;
            double u = (x - cx) * cs + (y - cy) * sn, v = -(x - cx) * sn + (y - cy) * cs;
            int paper = fabs(u) < sw / 2.0 && fabs(v) < sh / 2.0;
            int mark = paper && u > -sw / 2.0 + 100 && u < -sw / 2.0 + 140 && v > -sh / 2.0 + 100 && v < -sh / 2.0 + 140;
            p[0] = mark ? 60 : paper ? 229 : 3;
            p[1] = mark ? 60 : paper ? 225 : 4;
            p[2] = mark ? 60 : paper ? 245 : 2;
        }
    return raw;
}

static void test_image(void)
{
    /* a "sheet" of 400 x 240 pixels at (800, 100) on a dark background */
    int lines = 480;
    uint8_t *raw = draw_sheet(lines, 1000, 220, 400, 240, 0);
    raw[(5 * KDS_LINE_PX + 5) * 3] = 255;      /* a speck must not widen the box */
    for (int y = 0; y < 200; y++)               /* nor a bright stripe beside the sheet */
        raw[((size_t)y * KDS_LINE_PX + 41) * 3 + 1] = 200;
    struct kds_sheet c;
    kds_find_sheet(raw, lines, 1, &c);
    CHECK(c.x0 == 803 && c.y0 == 103 && c.w == 394 && c.h == 234 && c.angle == 0,
          "sheet %.1f,%.1f %dx%d angle %f", c.x0, c.y0, c.w, c.h, c.angle);
    uint8_t *row = malloc(KDS_LINE);
    kds_sheet_line(raw, lines, &c, 0, row);
    CHECK(row[0] == 229 && row[(c.w - 1) * 3 + 2] == 245, "unrotated row");
    free(raw);

    /* skewed A4-like sheets: the angle is found, the size is right, the mark lands where it was drawn */
    static const double angles[] = { 0.8, -0.5, 2.0, 0.1 };
    lines = 3900;
    for (size_t i = 0; i < sizeof(angles) / sizeof(angles[0]); i++) {
        raw = draw_sheet(lines, 1300, 1950, 2448, 3464, angles[i]);
        kds_find_sheet(raw, lines, 1, &c);
        double found = c.angle * 180 / M_PI, want = fabs(angles[i]) < 0.15 ? 0 : angles[i];
        printf("skew %+.2f: found %+.3f, %dx%d\n", angles[i], found, c.w, c.h);
        CHECK(fabs(found - want) < 0.03, "skew %.2f found %.3f", angles[i], found);
        CHECK(abs(c.w - 2442) <= 8 && abs(c.h - 3458) <= 8, "skew %.2f: size %dx%d", angles[i], c.w, c.h);
        if (want != 0) {
            kds_sheet_line(raw, lines, &c, 117, row);
            CHECK(row[3 * 117] < 100 && row[3 * 60] > 200 && row[3 * 170] > 200 && row[0] > 200 && row[3 * (c.w - 1)] > 200,
                  "skew %.2f: row 117 = %d %d %d, ends %d %d", angles[i], row[3 * 60], row[3 * 117], row[3 * 170], row[0], row[3 * (c.w - 1)]);
        }
        kds_find_sheet(raw, lines, 0, &c);
        CHECK(c.angle == 0, "deskew off");
        free(raw);
    }
    free(row);

    uint8_t out[16], px[4 * 3] = { 229, 225, 245, 3, 4, 2, 60, 60, 60, 229, 225, 245 };
    kds_convert_line(px, 4, KDS_MODE_COLOR, 0, out);
    CHECK(out[0] >= 250 && out[1] >= 250 && out[2] >= 250, "paper white -> %d %d %d", out[0], out[1], out[2]);
    CHECK(out[3] <= 5 && out[4] <= 5 && out[5] <= 5, "background -> %d %d %d", out[3], out[4], out[5]);
    kds_convert_line(px, 4, KDS_MODE_GRAY, 0, out);
    CHECK(out[0] >= 250 && out[1] <= 5 && out[2] > out[1] && out[2] < out[0], "gray %d %d %d", out[0], out[1], out[2]);
    kds_convert_line(px, 4, KDS_MODE_LINEART, 200, out);
    CHECK(out[0] == 0x60, "lineart %02x (want 60: white, black, black, white)", out[0]);
    CHECK(kds_bytes_per_line(KDS_MODE_LINEART, 2449) == 307 && kds_bytes_per_line(KDS_MODE_COLOR, 10) == 30, "bytes per line");
    kds_convert_line(px, 4, KDS_MODE_RAW, 0, out);
    CHECK(!memcmp(out, px, 12), "raw");
}

static int lcd_pixel(const uint8_t *bm, int x, int y)
{
    return bm[(y / 8) * KDS_LCD_W + x] >> (y % 8) & 1;
}

static int lcd_rows_used(const uint8_t *bm, int y0, int y1)
{
    for (int y = y0; y < y1; y++)
        for (int x = 0; x < KDS_LCD_W; x++)
            if (lcd_pixel(bm, x, y))
                return 1;
    return 0;
}

static void test_lcd(int show)
{
    uint8_t bm[KDS_LCD_BYTES];
    kds_lcd_text("L", bm);              /* double size, glyph at x = 4: a 10 x 14 "L" */
    CHECK(lcd_pixel(bm, 4, 0) && lcd_pixel(bm, 5, 13) && lcd_pixel(bm, 13, 12) && lcd_pixel(bm, 13, 13)
          && !lcd_pixel(bm, 13, 0) && !lcd_pixel(bm, 6, 11) && !lcd_pixel(bm, 4, 14) && !lcd_pixel(bm, 3, 0), "glyph L");
    int n = 0;
    for (int i = 0; i < KDS_LCD_BYTES; i++)
        n += __builtin_popcount(bm[i]);
    CHECK(n == 4 * (7 + 4), "L has %d pixels", n);
    kds_lcd_text("Paperless Color", bm);    /* two large lines: rows 0..15 and 16..31 */
    CHECK(lcd_rows_used(bm, 0, 16) && lcd_rows_used(bm, 16, 32) && !lcd_rows_used(bm, 32, 48), "two large lines expected");
    if (show)
        for (int y = 0; y < 34; y++) {
            for (int x = 0; x < KDS_LCD_W; x++)
                putchar(lcd_pixel(bm, x, y) ? '#' : '.');
            putchar('\n');
        }
    kds_lcd_text("a text that is far too long for three large lines of ten", bm);
    CHECK(lcd_rows_used(bm, 0, 8) && lcd_rows_used(bm, 16, 24) && !lcd_rows_used(bm, 24, 48), "three small lines expected");
    kds_lcd_text("", bm);
    CHECK(!lcd_rows_used(bm, 0, 48), "empty text");
    kds_lcd_text("\xc3\xa4", bm);       /* non-ASCII becomes '?' and must not crash */
    CHECK(lcd_rows_used(bm, 0, 16), "non-ASCII");
}

static void write_file(const char *path, const char *text)
{
    FILE *f = fopen(path, "w");
    fputs(text, f);
    fclose(f);
}

static void test_sequence(const char *path)
{
    struct kds_seq seq;
    CHECK(kds_seq_load(path, &seq) == KDS_OK, "load %s", path);
    int out = 0, in = 0, start = 0;
    for (int i = 0; i < seq.n; i++) {
        out += seq.steps[i].out;
        in += !seq.steps[i].out;
        start += seq.steps[i].out && seq.steps[i].req == 0x10 && seq.steps[i].val == 1;
    }
    printf("sequence: %d steps (%d out, %d in), %d operation starts\n", seq.n, out, in, start);
    CHECK(seq.n == 319 && start == 2, "expected 319 steps with 2 operation starts");
    CHECK(seq.steps[0].out && seq.steps[0].req == 0x3a && seq.steps[0].val == 1 && seq.steps[0].idx == 1,
          "first step is not `events on`");
    kds_seq_free(&seq);

    char tmp[] = "/tmp/kds-test-XXXXXX";
    int fd = mkstemp(tmp);
    if (fd < 0)
        return;
    static const char *bad[] = {
        "out 35 0000 0000 00 0\n",              /* NVRam write */
        "out 62 0104 0680 00 0\n",              /* LCDPopulate is not part of a scan */
        "out 21 0000 0000 - 0\n",               /* FirmwareDownload */
        "in 02 0000 0000 56 0\n",               /* a read outside the scan list */
        "out 3a 0001 0001 0 0\n",               /* odd hex length */
        "bulk 3a 0001 0001 - 0\n",
        "# only a comment\n",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        write_file(tmp, bad[i]);
        CHECK(kds_seq_load(tmp, &seq) == KDS_E_SEQUENCE, "accepted: %s", bad[i]);
    }
    /* power-up files: more kinds of steps, their own request lists */
    static const char *pwr_bad[] = {
        "out 35 0000 0000 00 0\n",              /* NVRam write */
        "out 62 0104 0680 00 0\n",              /* LCDPopulate */
        "out 23 0000 0000 - 0\n",               /* SubsystemFwUpdate */
        "out 24 0000 0000 - 0\n",               /* BulkDownload */
        "outblob a2 0000 0000 0 16 0\n",        /* EEPROM */
        "out f1 0001 0000 - 0\n",               /* a diagnostic request other than v=3 */
        "out 16 0001 0007 - 0\n",               /* not part of the replay */
        "in 09 0000 0000 32 0\n",
        "bulk 0 0 0\n",
        "wait 300 0\n",
    };
    for (size_t i = 0; i < sizeof(pwr_bad) / sizeof(pwr_bad[0]); i++) {
        write_file(tmp, pwr_bad[i]);
        CHECK(kds_powerup_load(tmp, &seq) == KDS_E_SEQUENCE, "power-up file accepted: %s", pwr_bad[i]);
    }
    write_file(tmp, "wait 1 0\nout 21 0000 0000 - 0\noutblob a0 0000 0000 0 2048 0.1\nbulk 16384 16384 0\n"
                    "out f1 0003 0000 - 0\nin f2 0000 0000 512 0\nwait 3 0.05\nout 1f 3077 7e46 - 0\n");
    CHECK(kds_powerup_load(tmp, &seq) == KDS_OK && seq.n == 8 && seq.steps[0].kind == KDS_STEP_WAIT && seq.steps[0].want == 1
          && seq.steps[2].kind == KDS_STEP_OUTBLOB && seq.steps[2].blob_len == 2048 && seq.steps[2].gap == 0.1
          && seq.steps[3].kind == KDS_STEP_BULK && seq.steps[3].off == 16384 && seq.steps[6].want == 3, "good power-up file");
    kds_seq_free(&seq);
    write_file(tmp, "bulk 0 16 0\n");            /* power-up steps are not valid in a scan sequence */
    CHECK(kds_seq_load(tmp, &seq) == KDS_E_SEQUENCE, "bulk accepted in a scan sequence");
    write_file(tmp, "out 21 0000 0000 - 0\n");
    CHECK(kds_seq_load(tmp, &seq) == KDS_E_SEQUENCE, "FirmwareDownload accepted in a scan sequence");
    const char *real = getenv("KDS_TEST_POWERUP");  /* a real local power-up file, if there is one */
    if (real) {
        int rc = kds_powerup_load(real, &seq), kinds[5] = { 0 };
        for (int i = 0; rc == KDS_OK && i < seq.n; i++)
            kinds[seq.steps[i].kind]++;
        printf("%s: %s, %d steps: %d in, %d out, %d outblob, %d bulk, %d wait\n", real, kds_strerror(rc), seq.n,
               kinds[KDS_STEP_IN], kinds[KDS_STEP_OUT], kinds[KDS_STEP_OUTBLOB], kinds[KDS_STEP_BULK], kinds[KDS_STEP_WAIT]);
        CHECK(rc == KDS_OK, "real power-up file refused");
        kds_seq_free(&seq);
    }

    write_file(tmp, "out 3a 0001 0001 - 0\n  # comment\nin 00 0000 0000 32 0.25\nout 45 0000 0000 010002 0\n");
    CHECK(kds_seq_load(tmp, &seq) == KDS_OK && seq.n == 3 && seq.steps[2].len == 3 && seq.steps[2].data[2] == 2
          && seq.steps[1].gap == 0.25 && seq.steps[1].len == 32, "good file");
    kds_seq_free(&seq);
    remove(tmp);
}

static void test_stream(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        printf("%s: not found, skipped\n", path);
        return;
    }
    struct got g = { .find_sheet = 1 };
    struct kds_splitter s;
    kds_split_init(&s, on_page, &g);
    size_t n, total = 0;
    uint8_t *p;
    while ((p = kds_split_reserve(&s, 1 << 18)) && (n = fread(p, 1, 1 << 18, f)) > 0) {
        total += n;
        CHECK(kds_split_commit(&s, n) == KDS_OK, "commit");
    }
    fclose(f);
    printf("%s: %zu bytes, %d pages, %zu leftover lines\n", path, total, g.n, kds_split_leftover_lines(&s));
    for (int i = 0; i < g.n && i < 16; i++)
        printf("  image %d: %d lines, sheet at %.0f,%.0f size %dx%d, skew %+.2f deg\n", g.number[i], g.lines[i],
               g.sheet[i].x0, g.sheet[i].y0, g.sheet[i].w, g.sheet[i].h, g.sheet[i].angle * 180 / M_PI);
    CHECK(g.n > 0, "no page found");
    CHECK(kds_split_leftover_lines(&s) <= 50, "stream ends inside a page");
    kds_split_free(&s);
}

int main(int argc, char **argv)
{
    test_splitter();
    test_image();
    test_lcd(getenv("KDS_TEST_SHOW_LCD") != NULL);
    if (argc > 1)
        test_sequence(argv[1]);
    for (int i = 2; i < argc; i++)
        test_stream(argv[i]);
    printf(failures ? "%d FAILED\n" : "all offline tests passed\n", failures);
    return failures != 0;
}
