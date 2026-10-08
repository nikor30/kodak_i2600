/* Kodak i2x00 native driver: device layer shared by the SANE backend and the tests.
 *
 * Written from docs/protocol/ only (clean room). Section numbers in comments refer to
 * docs/protocol/commands.md.
 */
#ifndef KDS_H
#define KDS_H

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

#define KDS_LINE_PX 2580                /* pixels per raw line (section 8) */
#define KDS_LINE (KDS_LINE_PX * 3)      /* bytes per raw line: 8-bit RGB */
#define KDS_FRONT 0
#define KDS_REAR 1

/* Results of the device layer. Positive values are batch errors reported by the scanner. */
enum {
    KDS_OK = 0,
    KDS_END = 1,            /* no more pages in this batch */
    KDS_E_IO = -1,
    KDS_E_BUSY = -2,
    KDS_E_ACCESS = -3,
    KDS_E_NO_FIRMWARE = -4, /* scanner was power-cycled and not initialised */
    KDS_E_NO_DOCS = -5,
    KDS_E_COVER = -6,
    KDS_E_JAM = -7,
    KDS_E_MULTIFEED = -8,
    KDS_E_OVERFLOW = -9,
    KDS_E_SCANNER = -10,
    KDS_E_TIMEOUT = -11,
    KDS_E_TRUNCATED = -12,
    KDS_E_NOMEM = -13,
    KDS_E_SEQUENCE = -14,
};
const char *kds_strerror(int err);

extern int kds_debug_level;
void kds_dbg(int level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

/* ---- page splitter (kds_split.c) ------------------------------------------- */
/* Called with a malloc'ed buffer of lines * KDS_LINE bytes; the callee owns it. */
typedef void (*kds_page_cb)(void *arg, int number, uint8_t *data, int lines);

struct kds_splitter {
    uint8_t *buf;
    size_t len, cap;
    size_t line;            /* next line boundary to test for a trailer */
    int pages;
    kds_page_cb cb;
    void *arg;
};
void kds_split_init(struct kds_splitter *s, kds_page_cb cb, void *arg);
uint8_t *kds_split_reserve(struct kds_splitter *s, size_t n);   /* room for n more bytes, or NULL */
int kds_split_commit(struct kds_splitter *s, size_t n);         /* n bytes were written; 0 or KDS_E_NOMEM */
size_t kds_split_leftover_lines(const struct kds_splitter *s);
void kds_split_free(struct kds_splitter *s);

/* ---- image (kds_image.c) ---------------------------------------------------- */
enum { KDS_MODE_COLOR, KDS_MODE_GRAY, KDS_MODE_LINEART, KDS_MODE_RAW };
/* Where the page is in a raw frame. Output pixel (i, j) is taken from the raw position
 * (x0 + i*cs - j*sn, y0 + i*sn + j*cs). */
struct kds_sheet {
    int w, h;
    double x0, y0, cs, sn;
    double angle;           /* skew in radians, 0 = taken as it is */
};
void kds_full_frame(int lines, struct kds_sheet *s);
void kds_find_sheet(const uint8_t *raw, int lines, int deskew, struct kds_sheet *s);
void kds_sheet_line(const uint8_t *raw, int lines, const struct kds_sheet *s, int row, uint8_t *out);
int kds_bytes_per_line(int mode, int w);
/* w raw pixels -> one output line (kds_bytes_per_line() bytes) */
void kds_convert_line(const uint8_t *raw, int w, int mode, int threshold, uint8_t *out);

/* ---- scan start sequence (kds_dev.c) ---------------------------------------- */
struct kds_step {
    uint8_t out, req;
    uint16_t val, idx, len;
    uint8_t *data;          /* OUT payload */
    double gap;             /* pause before the request, seconds */
};
struct kds_seq {
    struct kds_step *steps;
    int n;
};
int kds_seq_load(const char *path, struct kds_seq *seq);
void kds_seq_free(struct kds_seq *seq);

/* ---- device (kds_dev.c) ------------------------------------------------------ */
struct kds_page {
    struct kds_page *next;
    int number, lines;
    uint8_t *data;          /* NULL while the page is parked in fd */
    int fd;                 /* unlinked temporary file, or -1 */
    int counted;            /* counts against the pages-in-memory limit */
};

struct kds_panel {
    int start_pressed;      /* Start was pressed since the last call */
    int function;           /* function number on the LCD */
    int paper;              /* paper in the feeder */
    int cover_open;
};

struct libusb_device;
struct kds_dev;

int kds_open(struct libusb_device *usbdev, struct kds_dev **out);
void kds_close(struct kds_dev *d);
void kds_set_spool(struct kds_dev *d, int max_pages_in_memory, const char *dir);
int kds_panel(struct kds_dev *d, struct kds_panel *p);

/* A batch is everything in the feeder: the scanner feeds the whole stack by itself. */
int kds_batch_start(struct kds_dev *d, const struct kds_seq *seq, int duplex);
int kds_batch_next(struct kds_dev *d, int side, struct kds_page **page);   /* KDS_OK, KDS_END or error */
void kds_batch_end(struct kds_dev *d);
int kds_batch_sheets(struct kds_dev *d);
int kds_page_load(struct kds_page *p);
void kds_page_free(struct kds_dev *d, struct kds_page *p);

#endif
