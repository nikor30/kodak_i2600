/* Kodak i2x00 device layer: vendor control requests, events, scan batches.
 *
 * Written from docs/protocol/ only (clean room): commands.md sections 1, 3, 4 and 8
 * ("Driver procedure for a scan"). Only the requests listed there are ever sent.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <libusb.h>

#include "kds.h"

#define EP_EVENTS 0x88
#define EP_IMAGE_FRONT 0x82
#define EP_IMAGE_REAR 0x86

#define REQ_GET_STATUS 0x00
#define REQ_OPERATION 0x10
#define REQ_SET_LAMP 0x11
#define REQ_START_CAPTURE 0x17
#define REQ_VRAM 0x37
#define REQ_EVENT_CONTROL 0x3a
#define REQ_BATCH_DATA 0x45

#define EV_END_OF_OPERATION 0x01
#define EV_TRAY 0x13
#define EV_INTERLOCK 0x16
#define EV_BUTTON 0x20
#define EV_PAPER_JAM 0x30
#define EV_MULTIFEED 0x31
#define EV_BUFFER_OVERFLOW 0x32
#define EV_OTHER_ERROR 0x34
#define EV_FUNCTION 0x60

#define READ_SIZE (1 << 18)
#define READ_TIMEOUT_MS 500
#define EVENT_SILENCE_S 30.0
#define CTRL_TIMEOUT_MS 2000

/* The only requests this driver sends (section 8, "Replay rules"). */
static const uint8_t OUT_ALLOWED[] = { 0x3a, 0x1b, 0x32, 0x31, 0x11, 0x45, 0x37, 0xa3, 0xe0, 0x30, 0x10, 0x17 };
static const uint8_t IN_ALLOWED[] = { 0x00, 0x32, 0x35, 0x37, 0xa3, 0xe0 };

struct reader {
    struct kds_dev *dev;
    int side;
    pthread_t thread;
    struct kds_splitter split;
};

struct kds_dev {
    libusb_device_handle *h;
    pthread_mutex_t lock;
    pthread_cond_t cond;

    pthread_t ev_thread;
    int ev_running, ev_stop, dead;

    /* panel, from events and GetStatus */
    int start_pressed, function, tray, interlock;

    /* batch */
    int active;             /* a batch exists (supervisor thread to join) */
    int started;            /* the operation is running on the scanner */
    int done;               /* readers have to stop */
    int finished;           /* supervisor is through */
    int cancel, ends, sheets, error, duplex;
    double last_event, done_at;
    pthread_t sup_thread;
    struct reader rd[2];
    struct kds_page *head[2], *tail[2];

    int mem_pages, max_mem_pages;
    char spool_dir[256];
};

int kds_debug_level;

void kds_dbg(int level, const char *fmt, ...)
{
    if (level > kds_debug_level)
        return;
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[kodak_i2x00] ");
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

const char *kds_strerror(int err)
{
    switch (err) {
    case KDS_OK: return "ok";
    case KDS_END: return "no more pages";
    case KDS_E_IO: return "USB error";
    case KDS_E_BUSY: return "scanner is in use by another program";
    case KDS_E_ACCESS: return "no permission to open the scanner";
    case KDS_E_NO_FIRMWARE: return "scanner was power-cycled and has no firmware loaded";
    case KDS_E_NO_DOCS: return "feeder is empty";
    case KDS_E_COVER: return "cover is open";
    case KDS_E_JAM: return "paper jam";
    case KDS_E_MULTIFEED: return "multifeed";
    case KDS_E_OVERFLOW: return "scanner buffer overflow";
    case KDS_E_SCANNER: return "scanner error";
    case KDS_E_TIMEOUT: return "no event from the scanner for too long";
    case KDS_E_TRUNCATED: return "image data ended inside a page";
    case KDS_E_NOMEM: return "out of memory";
    case KDS_E_SEQUENCE: return "bad scan sequence file";
    }
    return "unknown error";
}

static double now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void pause_s(double s)
{
    struct timespec ts = { (time_t)s, (long)((s - (time_t)s) * 1e9) };
    nanosleep(&ts, NULL);
}

static int allowed(const uint8_t *list, size_t n, uint8_t req)
{
    for (size_t i = 0; i < n; i++)
        if (list[i] == req)
            return 1;
    return 0;
}

static int usb_err(int rc)
{
    switch (rc) {
    case LIBUSB_ERROR_BUSY: return KDS_E_BUSY;
    case LIBUSB_ERROR_ACCESS: return KDS_E_ACCESS;
    case LIBUSB_ERROR_NO_MEM: return KDS_E_NOMEM;
    default: return KDS_E_IO;
    }
}

static int ctl_out(struct kds_dev *d, uint8_t req, uint16_t val, uint16_t idx, const uint8_t *data, uint16_t len)
{
    if (!allowed(OUT_ALLOWED, sizeof(OUT_ALLOWED), req)) {
        kds_dbg(1, "refused: OUT request %02x is not allowed", req);
        return KDS_E_SEQUENCE;
    }
    int rc = libusb_control_transfer(d->h, 0x40, req, val, idx, (unsigned char *)data, len, CTRL_TIMEOUT_MS);
    if (rc < 0) {
        kds_dbg(1, "set %02x v=%04x i=%04x: %s", req, val, idx, libusb_error_name(rc));
        return usb_err(rc);
    }
    return KDS_OK;
}

/* returns the number of bytes read or an error */
static int ctl_in(struct kds_dev *d, uint8_t req, uint16_t val, uint16_t idx, uint8_t *buf, uint16_t len)
{
    if (!allowed(IN_ALLOWED, sizeof(IN_ALLOWED), req)) {
        kds_dbg(1, "refused: IN request %02x is not allowed", req);
        return KDS_E_SEQUENCE;
    }
    int rc = libusb_control_transfer(d->h, 0xc0, req, val, idx, buf, len, CTRL_TIMEOUT_MS);
    if (rc < 0) {
        kds_dbg(1, "get %02x v=%04x i=%04x: %s", req, val, idx, libusb_error_name(rc));
        return usb_err(rc);
    }
    return rc;
}

/* ---- GetStatus (section 3) --------------------------------------------------- */
struct status {
    int fw_id, tray, interlock, button, error;
};

static int get_status(struct kds_dev *d, struct status *st)
{
    uint8_t b[32];
    int n = ctl_in(d, REQ_GET_STATUS, 0, 0, b, sizeof(b));
    if (n < 0)
        return n;
    if (n < 28)
        return KDS_E_IO;
    st->fw_id = b[0];
    st->tray = b[15];
    st->interlock = b[19];
    st->button = b[20];
    st->error = b[27];
    return KDS_OK;
}

/* ---- events (section 4) ------------------------------------------------------ */
static void fail_batch(struct kds_dev *d, int err)      /* lock held */
{
    if (!d->error)
        d->error = err;
    if (!d->done) {
        d->done = 1;
        d->done_at = now();
    }
}

static void handle_event(struct kds_dev *d, const uint8_t *ev)
{
    kds_dbg(4, "event %02x %02x %02x %02x %02x", ev[0], ev[1], ev[2], ev[3], ev[4]);
    pthread_mutex_lock(&d->lock);
    d->last_event = now();
    switch (ev[0]) {
    case EV_BUTTON:
        d->start_pressed = 1;
        d->function = ev[2];
        break;
    case EV_FUNCTION:
        d->function = ev[2];
        break;
    case EV_TRAY:
        d->tray = ev[2];
        break;
    case EV_INTERLOCK:
        d->interlock = ev[2];
        if (d->active && !d->done && ev[2] == 2)
            fail_batch(d, KDS_E_COVER);
        break;
    case EV_END_OF_OPERATION:
        if (d->active && !d->done && ++d->ends >= 2) {  /* the first one ends the pre-scan */
            d->sheets = ev[4];
            d->started = 0;
            d->done = 1;
            d->done_at = now();
        }
        break;
    case EV_PAPER_JAM:
    case EV_MULTIFEED:
    case EV_BUFFER_OVERFLOW:
    case EV_OTHER_ERROR:
        if (d->active && !d->error)
            d->error = ev[0] == EV_PAPER_JAM ? KDS_E_JAM : ev[0] == EV_MULTIFEED ? KDS_E_MULTIFEED
                     : ev[0] == EV_BUFFER_OVERFLOW ? KDS_E_OVERFLOW : KDS_E_SCANNER;
        break;
    }
    pthread_cond_broadcast(&d->cond);
    pthread_mutex_unlock(&d->lock);
}

static void *event_thread(void *arg)
{
    struct kds_dev *d = arg;
    for (;;) {
        pthread_mutex_lock(&d->lock);
        int stop = d->ev_stop;
        pthread_mutex_unlock(&d->lock);
        if (stop)
            break;
        uint8_t ev[8];
        int got = 0;
        int rc = libusb_interrupt_transfer(d->h, EP_EVENTS, ev, sizeof(ev), &got, 250);
        if (rc == LIBUSB_ERROR_TIMEOUT)
            continue;
        if (rc < 0) {
            kds_dbg(1, "event pipe: %s", libusb_error_name(rc));
            pthread_mutex_lock(&d->lock);
            d->dead = 1;
            if (d->active)
                fail_batch(d, KDS_E_IO);
            pthread_cond_broadcast(&d->cond);
            pthread_mutex_unlock(&d->lock);
            break;
        }
        if (got == 8)
            handle_event(d, ev);
    }
    return NULL;
}

/* ---- open / close ------------------------------------------------------------ */
int kds_open(struct libusb_device *usbdev, struct kds_dev **out)
{
    struct kds_dev *d = calloc(1, sizeof(*d));
    if (!d)
        return KDS_E_NOMEM;
    pthread_mutex_init(&d->lock, NULL);
    pthread_condattr_t ca;
    pthread_condattr_init(&ca);
    pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
    pthread_cond_init(&d->cond, &ca);
    pthread_condattr_destroy(&ca);
    d->max_mem_pages = 4;
    snprintf(d->spool_dir, sizeof(d->spool_dir), "/var/tmp");

    int rc = libusb_open(usbdev, &d->h);
    if (rc < 0) {
        kds_dbg(1, "open: %s", libusb_error_name(rc));
        rc = usb_err(rc);
        goto fail;
    }
    rc = libusb_claim_interface(d->h, 0);
    if (rc < 0) {
        kds_dbg(1, "claim interface: %s", libusb_error_name(rc));
        rc = usb_err(rc);
        goto fail;
    }
    struct status st;
    rc = get_status(d, &st);
    if (rc < 0)
        goto fail;
    kds_dbg(2, "status: firmware id %d, tray %d, interlock %d, function %d, error %d",
            st.fw_id, st.tray, st.interlock, st.button, st.error);
    if (st.fw_id != 3) {    /* 1 = boot firmware after power-up (section 6) */
        rc = KDS_E_NO_FIRMWARE;
        goto fail;
    }
    d->tray = st.tray;
    d->interlock = st.interlock;
    d->function = st.button ? st.button : 1;
    rc = ctl_out(d, REQ_EVENT_CONTROL, 1, 1, NULL, 0);
    if (rc < 0)
        goto fail;
    if (pthread_create(&d->ev_thread, NULL, event_thread, d)) {
        rc = KDS_E_NOMEM;
        goto fail;
    }
    d->ev_running = 1;
    *out = d;
    return KDS_OK;
fail:
    kds_close(d);
    return rc;
}

void kds_close(struct kds_dev *d)
{
    if (!d)
        return;
    kds_batch_end(d);
    if (d->ev_running) {
        pthread_mutex_lock(&d->lock);
        d->ev_stop = 1;
        pthread_mutex_unlock(&d->lock);
        pthread_join(d->ev_thread, NULL);
        if (!d->dead)
            ctl_out(d, REQ_EVENT_CONTROL, 1, 0, NULL, 0);
    }
    if (d->h) {
        libusb_release_interface(d->h, 0);
        libusb_close(d->h);
    }
    pthread_cond_destroy(&d->cond);
    pthread_mutex_destroy(&d->lock);
    free(d);
}

void kds_set_spool(struct kds_dev *d, int max_pages_in_memory, const char *dir)
{
    if (max_pages_in_memory >= 0)
        d->max_mem_pages = max_pages_in_memory;
    if (dir && *dir)
        snprintf(d->spool_dir, sizeof(d->spool_dir), "%s", dir);
}

int kds_panel(struct kds_dev *d, struct kds_panel *p)
{
    int rc = KDS_OK;
    pthread_mutex_lock(&d->lock);
    int idle = !d->active && !d->dead;
    pthread_mutex_unlock(&d->lock);
    struct status st = { 0 };
    if (idle)               /* no control requests while a batch runs (section 8) */
        rc = get_status(d, &st);
    pthread_mutex_lock(&d->lock);
    if (idle && rc == KDS_OK) {
        d->tray = st.tray;
        d->interlock = st.interlock;
        if (st.button)
            d->function = st.button;
    }
    p->start_pressed = d->start_pressed;
    d->start_pressed = 0;
    p->function = d->function;
    p->paper = d->tray == 2;
    p->cover_open = d->interlock == 2;
    if (d->dead)
        rc = KDS_E_IO;
    pthread_mutex_unlock(&d->lock);
    return rc;
}

/* ---- sequence file ------------------------------------------------------------ */
void kds_seq_free(struct kds_seq *seq)
{
    for (int i = 0; i < seq->n; i++)
        free(seq->steps[i].data);
    free(seq->steps);
    seq->steps = NULL;
    seq->n = 0;
}

static int hexval(int c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

/* Lines: `out RR VVVV IIII HEXDATA|- PAUSE` or `in RR VVVV IIII LENGTH PAUSE` */
int kds_seq_load(const char *path, struct kds_seq *seq)
{
    FILE *f = fopen(path, "r");
    seq->steps = NULL;
    seq->n = 0;
    if (!f) {
        kds_dbg(1, "%s: %s", path, strerror(errno));
        return KDS_E_SEQUENCE;
    }
    char *line = NULL;
    size_t cap = 0;
    int lineno = 0, rc = KDS_OK;
    while (getline(&line, &cap, f) >= 0) {
        lineno++;
        char dir[8], *arg = NULL;
        unsigned req = 0, val = 0, idx = 0;
        double gap = 0;
        char *hash = strchr(line, '#');
        if (hash)
            *hash = 0;
        if (line[strspn(line, " \t\r\n")] == 0)
            continue;
        if (sscanf(line, "%7s %x %x %x %ms %lf", dir, &req, &val, &idx, &arg, &gap) != 6
            || req > 0xff || val > 0xffff || idx > 0xffff || gap < 0) {
            kds_dbg(1, "%s:%d: cannot parse", path, lineno);
            rc = KDS_E_SEQUENCE;
        }
        struct kds_step st = { .req = (uint8_t)req, .val = (uint16_t)val, .idx = (uint16_t)idx, .gap = gap };
        if (rc == KDS_OK && !strcmp(dir, "out")) {
            st.out = 1;
            size_t n = strcmp(arg, "-") ? strlen(arg) : 0;
            if (n % 2 || n / 2 > 0xffff)
                rc = KDS_E_SEQUENCE;
            st.len = (uint16_t)(n / 2);
            st.data = malloc(st.len ? st.len : 1);
            if (!st.data)
                rc = KDS_E_NOMEM;
            for (size_t i = 0; rc == KDS_OK && i < st.len; i++) {
                int hi = hexval(arg[2 * i]), lo = hexval(arg[2 * i + 1]);
                if (hi < 0 || lo < 0)
                    rc = KDS_E_SEQUENCE;
                st.data[i] = (uint8_t)(hi << 4 | lo);
            }
        } else if (rc == KDS_OK && !strcmp(dir, "in")) {
            char *e;
            long n = strtol(arg, &e, 10);
            if (*e || n < 0 || n > 4096)
                rc = KDS_E_SEQUENCE;
            st.len = (uint16_t)n;
        } else if (rc == KDS_OK) {
            rc = KDS_E_SEQUENCE;
        }
        free(arg);
        if (rc == KDS_OK && !allowed(st.out ? OUT_ALLOWED : IN_ALLOWED,
                                     st.out ? sizeof(OUT_ALLOWED) : sizeof(IN_ALLOWED), st.req)) {
            kds_dbg(1, "%s:%d: request %s %02x is not allowed in a scan sequence", path, lineno, dir, st.req);
            rc = KDS_E_SEQUENCE;
        }
        struct kds_step *ns = rc == KDS_OK ? realloc(seq->steps, (size_t)(seq->n + 1) * sizeof(*ns)) : NULL;
        if (!ns) {
            if (rc == KDS_OK)
                rc = KDS_E_NOMEM;
            else
                kds_dbg(1, "%s:%d: refused", path, lineno);
            free(st.data);
            break;
        }
        seq->steps = ns;
        seq->steps[seq->n++] = st;
    }
    free(line);
    fclose(f);
    if (rc == KDS_OK && seq->n == 0)
        rc = KDS_E_SEQUENCE;
    if (rc != KDS_OK)
        kds_seq_free(seq);
    return rc;
}

/* ---- batch --------------------------------------------------------------------- */
static int bulk_read(struct kds_dev *d, uint8_t ep, uint8_t *buf, int size, int timeout_ms, int *got)
{
    *got = 0;
    int rc = libusb_bulk_transfer(d->h, ep, buf, size, got, (unsigned)timeout_ms);
    if (rc < 0 && rc != LIBUSB_ERROR_TIMEOUT) {
        kds_dbg(1, "bulk read ep %02x: %s", ep, libusb_error_name(rc));
        return usb_err(rc);
    }
    return KDS_OK;
}

static int replay(struct kds_dev *d, const struct kds_seq *seq)
{
    uint8_t vram[64], reply[4096], *drain = NULL;
    int vram_len = -1, rc = KDS_OK;
    for (int i = 0; i < seq->n && rc == KDS_OK; i++) {
        const struct kds_step *st = &seq->steps[i];
        if (st->gap > 0)
            pause_s(st->gap < 1.0 ? st->gap : 1.0);
        if (!st->out) {
            int n = ctl_in(d, st->req, st->val, st->idx, reply, st->len);
            if (n < 0)
                rc = n;
            else if (st->req == REQ_VRAM && n <= (int)sizeof(vram)) {
                memcpy(vram, reply, (size_t)n);
                vram_len = n;
            }
            continue;
        }
        if (st->req == REQ_VRAM && vram_len >= 0)   /* write back what this scanner just reported */
            rc = ctl_out(d, st->req, st->val, st->idx, vram, (uint16_t)vram_len);
        else
            rc = ctl_out(d, st->req, st->val, st->idx, st->data, st->len);
        if (rc == KDS_OK && st->req == REQ_OPERATION) {
            pthread_mutex_lock(&d->lock);
            d->started = st->val != 0;
            pthread_mutex_unlock(&d->lock);
        }
        if (rc == KDS_OK && st->req == REQ_START_CAPTURE) {     /* pre-scan block, discarded */
            if (!drain && !(drain = malloc(16384)))
                rc = KDS_E_NOMEM;
            for (int ep = 0; ep < 2 && rc == KDS_OK; ep++) {
                int got;
                do
                    rc = bulk_read(d, ep ? EP_IMAGE_REAR : EP_IMAGE_FRONT, drain, 16384, 2000, &got);
                while (rc == KDS_OK && got == 16384);
            }
        }
    }
    free(drain);
    return rc;
}

static void park(struct kds_dev *d, struct kds_page *p)
{
    char path[320];
    snprintf(path, sizeof(path), "%s/kodak_i2x00-XXXXXX", d->spool_dir);
    int fd = mkstemp(path);
    if (fd < 0) {
        kds_dbg(1, "%s: %s (page stays in memory)", path, strerror(errno));
        return;
    }
    unlink(path);
    size_t total = (size_t)p->lines * KDS_LINE, off = 0;
    while (off < total) {
        ssize_t n = write(fd, p->data + off, total - off);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            kds_dbg(1, "spool write: %s (page stays in memory)", strerror(errno));
            close(fd);
            return;
        }
        off += (size_t)n;
    }
    free(p->data);
    p->data = NULL;
    p->fd = fd;
}

static void on_page(void *arg, int number, uint8_t *data, int lines)
{
    struct reader *r = arg;
    struct kds_dev *d = r->dev;
    kds_dbg(3, "%s page, image %d, %d lines", r->side ? "rear" : "front", number, lines);
    if (r->side == KDS_REAR && !d->duplex) {
        free(data);
        return;
    }
    struct kds_page *p = calloc(1, sizeof(*p));
    if (!p) {
        free(data);
        pthread_mutex_lock(&d->lock);
        fail_batch(d, KDS_E_NOMEM);
        pthread_mutex_unlock(&d->lock);
        return;
    }
    p->number = number;
    p->lines = lines;
    p->data = data;
    p->fd = -1;
    pthread_mutex_lock(&d->lock);
    int spill = d->mem_pages >= d->max_mem_pages;
    if (!spill) {
        d->mem_pages++;
        p->counted = 1;
    }
    pthread_mutex_unlock(&d->lock);
    if (spill)              /* long stacks: raw pages wait on disk, not in memory */
        park(d, p);
    pthread_mutex_lock(&d->lock);
    if (d->tail[r->side])
        d->tail[r->side]->next = p;
    else
        d->head[r->side] = p;
    d->tail[r->side] = p;
    pthread_cond_broadcast(&d->cond);
    pthread_mutex_unlock(&d->lock);
}

static void *reader_thread(void *arg)
{
    struct reader *r = arg;
    struct kds_dev *d = r->dev;
    uint8_t ep = r->side ? EP_IMAGE_REAR : EP_IMAGE_FRONT;
    int idle = 0;
    for (;;) {
        pthread_mutex_lock(&d->lock);
        int done = d->done;
        double since = done ? now() - d->done_at : 0;
        pthread_mutex_unlock(&d->lock);
        if (done && (idle >= 2 || since > 5.0))
            break;
        int got, rc;
        uint8_t *buf = kds_split_reserve(&r->split, READ_SIZE);
        if (!buf)
            rc = KDS_E_NOMEM;
        else
            rc = bulk_read(d, ep, buf, READ_SIZE, READ_TIMEOUT_MS, &got);
        if (rc == KDS_OK && got > 0)
            rc = kds_split_commit(&r->split, (size_t)got);
        if (rc != KDS_OK) {
            pthread_mutex_lock(&d->lock);
            fail_batch(d, rc);
            pthread_cond_broadcast(&d->cond);
            pthread_mutex_unlock(&d->lock);
            break;
        }
        idle = got > 0 ? 0 : idle + 1;
    }
    return NULL;
}

static void *supervisor_thread(void *arg)
{
    struct kds_dev *d = arg;
    pthread_mutex_lock(&d->lock);
    while (!d->done) {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        ts.tv_nsec += 500000000;
        if (ts.tv_nsec >= 1000000000) {
            ts.tv_sec++;
            ts.tv_nsec -= 1000000000;
        }
        pthread_cond_timedwait(&d->cond, &d->lock, &ts);
        if (!d->done && d->cancel) {
            d->done = 1;
            d->done_at = now();
        }
        if (!d->done && now() - d->last_event > EVENT_SILENCE_S)
            fail_batch(d, KDS_E_TIMEOUT);
    }
    int stop = d->started && !d->dead;
    d->started = 0;
    int dead = d->dead;
    pthread_mutex_unlock(&d->lock);

    if (stop) {             /* the batch did not end by itself */
        kds_dbg(2, "stopping the operation");
        ctl_out(d, REQ_OPERATION, 0, 0, NULL, 0);
    }
    for (int i = 0; i < 2; i++)
        pthread_join(d->rd[i].thread, NULL);
    if (!dead) {
        static const uint8_t batch_end[3] = { 2, 0, 0 };
        ctl_out(d, REQ_SET_LAMP, 0, 0, NULL, 0);
        ctl_out(d, REQ_BATCH_DATA, 0, 0, batch_end, sizeof(batch_end));
    }
    pthread_mutex_lock(&d->lock);
    for (int i = 0; i < 2; i++)
        if (kds_split_leftover_lines(&d->rd[i].split) > 50 && !d->error && !d->cancel)
            d->error = KDS_E_TRUNCATED;
    kds_dbg(2, "batch finished: %d sheets, front %d, rear %d pages, %s", d->sheets,
            d->rd[0].split.pages, d->rd[1].split.pages, kds_strerror(d->error));
    d->finished = 1;
    pthread_cond_broadcast(&d->cond);
    pthread_mutex_unlock(&d->lock);
    return NULL;
}

int kds_batch_start(struct kds_dev *d, const struct kds_seq *seq, int duplex)
{
    struct status st;
    if (d->active)
        return KDS_E_BUSY;
    if (d->dead)
        return KDS_E_IO;
    int rc = get_status(d, &st);
    if (rc < 0)
        return rc;
    if (st.interlock != 1)
        return KDS_E_COVER;
    if (st.fw_id != 3)
        return KDS_E_NO_FIRMWARE;
    if (st.error) {
        kds_dbg(1, "scanner reports error code %d", st.error);
        return KDS_E_SCANNER;
    }
    if (st.tray != 2)
        return KDS_E_NO_DOCS;

    pthread_mutex_lock(&d->lock);
    d->active = 1;
    d->started = d->done = d->finished = d->cancel = d->ends = d->sheets = d->error = 0;
    d->duplex = duplex;
    d->last_event = now();
    pthread_mutex_unlock(&d->lock);

    rc = replay(d, seq);
    if (rc == KDS_OK) {
        for (int i = 0; i < 2; i++) {
            d->rd[i].dev = d;
            d->rd[i].side = i;
            kds_split_init(&d->rd[i].split, on_page, &d->rd[i]);
        }
        if (pthread_create(&d->rd[0].thread, NULL, reader_thread, &d->rd[0]))
            rc = KDS_E_NOMEM;
        else if (pthread_create(&d->rd[1].thread, NULL, reader_thread, &d->rd[1])) {
            pthread_mutex_lock(&d->lock);
            fail_batch(d, KDS_E_NOMEM);
            pthread_mutex_unlock(&d->lock);
            pthread_join(d->rd[0].thread, NULL);
            rc = KDS_E_NOMEM;
        } else if (pthread_create(&d->sup_thread, NULL, supervisor_thread, d)) {
            pthread_mutex_lock(&d->lock);
            fail_batch(d, KDS_E_NOMEM);
            pthread_mutex_unlock(&d->lock);
            pthread_join(d->rd[0].thread, NULL);
            pthread_join(d->rd[1].thread, NULL);
            rc = KDS_E_NOMEM;
        }
    }
    if (rc != KDS_OK) {     /* nothing is running on our side: leave the scanner idle */
        static const uint8_t batch_end[3] = { 2, 0, 0 };
        kds_dbg(1, "scan start failed: %s", kds_strerror(rc));
        if (d->started)
            ctl_out(d, REQ_OPERATION, 0, 0, NULL, 0);
        ctl_out(d, REQ_SET_LAMP, 0, 0, NULL, 0);
        ctl_out(d, REQ_BATCH_DATA, 0, 0, batch_end, sizeof(batch_end));
        pthread_mutex_lock(&d->lock);
        d->active = d->started = 0;
        pthread_mutex_unlock(&d->lock);
    }
    return rc;
}

int kds_batch_next(struct kds_dev *d, int side, struct kds_page **page)
{
    int rc;
    pthread_mutex_lock(&d->lock);
    if (!d->active) {
        pthread_mutex_unlock(&d->lock);
        return KDS_END;
    }
    while (!d->head[side] && !d->finished)
        pthread_cond_wait(&d->cond, &d->lock);
    struct kds_page *p = d->head[side];
    if (p) {
        d->head[side] = p->next;
        if (!d->head[side])
            d->tail[side] = NULL;
        p->next = NULL;
        *page = p;
        rc = KDS_OK;
    } else {
        rc = d->error ? d->error : KDS_END;
    }
    pthread_mutex_unlock(&d->lock);
    return rc;
}

int kds_batch_sheets(struct kds_dev *d)
{
    pthread_mutex_lock(&d->lock);
    int n = d->sheets;
    pthread_mutex_unlock(&d->lock);
    return n;
}

void kds_batch_end(struct kds_dev *d)
{
    pthread_mutex_lock(&d->lock);
    int active = d->active;
    d->cancel = 1;
    pthread_cond_broadcast(&d->cond);
    pthread_mutex_unlock(&d->lock);
    if (!active)
        return;
    pthread_join(d->sup_thread, NULL);
    for (int i = 0; i < 2; i++) {
        kds_split_free(&d->rd[i].split);
        while (d->head[i]) {
            struct kds_page *p = d->head[i];
            d->head[i] = p->next;
            kds_page_free(d, p);
        }
        d->tail[i] = NULL;
    }
    pthread_mutex_lock(&d->lock);
    d->active = 0;
    pthread_mutex_unlock(&d->lock);
}

int kds_page_load(struct kds_page *p)
{
    if (p->data)
        return KDS_OK;
    size_t total = (size_t)p->lines * KDS_LINE, off = 0;
    uint8_t *data = malloc(total);
    if (!data)
        return KDS_E_NOMEM;
    while (off < total) {
        ssize_t n = pread(p->fd, data + off, total - off, (off_t)off);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            free(data);
            return KDS_E_IO;
        }
        off += (size_t)n;
    }
    close(p->fd);
    p->fd = -1;
    p->data = data;
    return KDS_OK;
}

void kds_page_free(struct kds_dev *d, struct kds_page *p)
{
    if (!p)
        return;
    if (p->fd >= 0)
        close(p->fd);
    if (p->counted) {
        pthread_mutex_lock(&d->lock);
        d->mem_pages--;
        pthread_mutex_unlock(&d->lock);
    }
    free(p->data);
    free(p);
}
