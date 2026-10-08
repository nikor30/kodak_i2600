/* Offline tests of the parts that need no scanner: page splitter, sheet detection,
 * pixel conversion, sequence file parser.
 *
 *   test_offline SEQUENCE.seq [FRONT.raw REAR.raw]
 *
 * The optional raw files are image streams saved from a real scan (local captures).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kds.h"

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

struct got {
    int n, number[16], lines[16];
    uint8_t first[16];
    struct kds_crop crop[16];
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
            kds_find_sheet(data, lines, &g->crop[g->n]);
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

static void test_image(void)
{
    /* a "sheet" of 400 x 240 pixels at (800, 100) on a dark background */
    int lines = 480;
    uint8_t *raw = calloc((size_t)lines, KDS_LINE);
    for (int y = 0; y < lines; y++)
        for (int x = 0; x < KDS_LINE_PX; x++) {
            uint8_t *p = raw + ((size_t)y * KDS_LINE_PX + x) * 3;
            int paper = x >= 800 && x < 1200 && y >= 100 && y < 340;
            p[0] = paper ? 229 : 3;
            p[1] = paper ? 225 : 4;
            p[2] = paper ? 245 : 2;
        }
    raw[(5 * KDS_LINE_PX + 5) * 3] = 255;      /* a speck must not widen the box */
    struct kds_crop c;
    kds_find_sheet(raw, lines, &c);
    CHECK(c.x == 800 && c.y == 100 && c.w == 400 && c.h == 240, "crop %d,%d %dx%d", c.x, c.y, c.w, c.h);

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
    free(raw);
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
        printf("  image %d: %d lines, sheet at %d,%d size %dx%d\n", g.number[i], g.lines[i],
               g.crop[i].x, g.crop[i].y, g.crop[i].w, g.crop[i].h);
    CHECK(g.n > 0, "no page found");
    CHECK(kds_split_leftover_lines(&s) <= 50, "stream ends inside a page");
    kds_split_free(&s);
}

int main(int argc, char **argv)
{
    test_splitter();
    test_image();
    if (argc > 1)
        test_sequence(argv[1]);
    for (int i = 2; i < argc; i++)
        test_stream(argv[i]);
    printf(failures ? "%d FAILED\n" : "all offline tests passed\n", failures);
    return failures != 0;
}
