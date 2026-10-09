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
#define EP_BULK_OUT 0x02

#define REQ_GET_STATUS 0x00
#define REQ_OPERATION 0x10
#define REQ_SET_LAMP 0x11
#define REQ_SET_SEQUENCE_NUMBER 0x16
#define REQ_SET_TIME 0x1f
#define REQ_START_CAPTURE 0x17
#define REQ_VRAM 0x37
#define REQ_SET_POWER 0x39      /* wValue 2: leave standby */
#define REQ_EVENT_CONTROL 0x3a
#define REQ_BATCH_DATA 0x45
#define REQ_LCD_POPULATE 0x62

#define PWR_STANDBY 1           /* bPowerState / Power State event; 3 = idle, 4 = operating */
#define EV_END_OF_OPERATION 0x01
#define EV_POWER 0x10
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
#define PWR_TIMEOUT_MS 5000
#define KODAK_EPOCH 978307200L       /* 2001-01-01 00:00:00 UTC, for SetTime */

/* The only requests this driver sends (section 8, "Replay rules"), besides LCDPopulate
 * for function labels in kds_lcd_label(). */
static const uint8_t OUT_ALLOWED[] = { 0x3a, 0x1b, 0x32, 0x31, 0x11, 0x45, 0x37, 0xa3, 0xe0, 0x30, 0x10, 0x17 };
static const uint8_t IN_ALLOWED[] = { 0x00, 0x32, 0x35, 0x37, 0xa3, 0xe0 };
/* Requests of the power-up replay (section 6). All of it is volatile: firmware and FPGA
 * image go into RAM. Writes to permanent storage (NVRam 35, EEPROM a2, firmware update
 * 23/24) are in neither list, so a power-up file containing one is refused. */
static const uint8_t PWR_OUT_ALLOWED[] = { 0x21, 0xa0, 0x20, 0xf1, 0xa3, 0x37, 0x1f, 0x18, 0x11, 0xe0, 0x30, 0x17 };
static const uint8_t PWR_IN_ALLOWED[] = { 0x00, 0xf2, 0xa3, 0x02, 0x34, 0x36, 0x37, 0x03, 0x35, 0xe3, 0xe2, 0xe0, 0x32, 0x33 };

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
    int start_pressed, function, tray, interlock, power, quiet;
    int woke;               /* left standby: read the status once, even when quiet */

    /* batch */
    int active;             /* a batch exists (supervisor thread to join) */
    int started;            /* the operation is running on the scanner */
    int done;               /* readers have to stop */
    int finished;           /* supervisor is through */
    int cancel, ends, sheets, error, duplex;
    int batch_events;       /* events since the start sequence was sent */
    double last_event, done_at, status_at;
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

static int bulk_read(struct kds_dev *d, uint8_t ep, uint8_t *buf, int size, int timeout_ms, int *got);
static int power_up(struct kds_dev *d, const char *seq_path);

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
    int fw_id, power, tray, interlock, button, error;
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
    st->power = b[12];
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
    d->batch_events++;
    switch (ev[0]) {
    case EV_POWER:              /* 1 = standby, 3 = idle, 4 = operating */
        if (ev[2] != d->power && ((ev[2] != 3 && ev[2] != 4) || (d->power != 3 && d->power != 4 && d->power != 0)))
            kds_dbg(1, "power state %d -> %d", d->power, ev[2]);
        if (d->power == PWR_STANDBY && ev[2] != PWR_STANDBY)
            d->woke = 1;
        d->power = ev[2];
        break;
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
int kds_open(struct libusb_device *usbdev, const char *powerup_path, int functions, struct kds_dev **out)
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
    kds_dbg(2, "status: firmware id %d, power %d, tray %d, interlock %d, function %d, error %d",
            st.fw_id, st.power, st.tray, st.interlock, st.button, st.error);
    if (st.fw_id == 1 && powerup_path && *powerup_path) {   /* boot firmware after power-on */
        double t0 = now();
        kds_dbg(1, "scanner was power-cycled: loading its firmware from %s", powerup_path);
        rc = power_up(d, powerup_path);
        if (rc == KDS_OK)
            rc = get_status(d, &st);
        if (rc < 0) {
            kds_dbg(1, "power-up failed: %s", kds_strerror(rc));
            goto fail;
        }
        kds_dbg(1, "power-up done in %.1f s, firmware id %d", now() - t0, st.fw_id);
    }
    if (st.fw_id != 3) {
        rc = KDS_E_NO_FIRMWARE;
        goto fail;
    }
    if (st.button == 0 || functions > 0) {
        /* SetSequenceNumber (section 7): show number 1; wIndex is a bit mask of the numbers the
         * arrow buttons offer. After the power-up the panel has none (blank LCD, Start reports
         * 0); the vendor driver sends 7 (numbers 1-3) on every open. */
        int count = functions >= 1 && functions <= 9 ? functions : 3;
        int urc = libusb_control_transfer(d->h, 0x40, REQ_SET_SEQUENCE_NUMBER, 1, (uint16_t)((1 << count) - 1),
                                          NULL, 0, CTRL_TIMEOUT_MS);
        rc = urc < 0 ? usb_err(urc) : get_status(d, &st);
        if (rc < 0)
            goto fail;
        kds_dbg(2, "panel offers function numbers 1-%d (now %d)", count, st.button);
    }
    d->power = st.power;
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
    /* No control requests while a batch runs (section 8). Otherwise GetStatus at most every
     * 2 s (the vendor's idle poll rate): frontends poll the sensors far more often, and
     * button, paper and cover changes arrive as events in between anyway. */
    int idle = !d->active && !d->dead && (d->woke || (!d->quiet && now() - d->status_at >= 2.0));
    pthread_mutex_unlock(&d->lock);
    struct status st = { 0 };
    if (idle)
        rc = get_status(d, &st);
    pthread_mutex_lock(&d->lock);
    if (idle && rc == KDS_OK) {
        d->status_at = now();
        d->woke = 0;
        d->power = st.power;
        d->tray = st.tray;
        d->interlock = st.interlock;
        if (st.button)
            d->function = st.button;
    }
    p->start_pressed = d->start_pressed;
    d->start_pressed = 0;
    p->function = d->function;
    p->paper = d->tray == 2;
    /* In standby the scanner reports interlock 2 with the cover closed (section 3). */
    p->cover_open = d->interlock == 2 && d->power != PWR_STANDBY;
    if (d->dead)
        rc = KDS_E_IO;
    pthread_mutex_unlock(&d->lock);
    return rc;
}

void kds_set_quiet(struct kds_dev *d, int quiet)
{
    pthread_mutex_lock(&d->lock);
    d->quiet = quiet;
    d->status_at = 0;           /* a fresh status as soon as we may ask again */
    pthread_mutex_unlock(&d->lock);
}

/* LCDPopulate (section 7): message type 1 = function label, id = function number.
 * The only use of request 62 in this driver; it is not allowed in a scan sequence. */
int kds_lcd_label(struct kds_dev *d, int number, const uint8_t bitmap[KDS_LCD_BYTES])
{
    if (number < 1 || number > 9)
        return KDS_E_SEQUENCE;
    pthread_mutex_lock(&d->lock);
    int idle = !d->active && !d->dead;
    pthread_mutex_unlock(&d->lock);
    if (!idle)
        return KDS_E_BUSY;
    int rc = libusb_control_transfer(d->h, 0x40, REQ_LCD_POPULATE, (uint16_t)(number << 8 | 1),
                                     (KDS_LCD_H / 8) << 8 | KDS_LCD_W, (unsigned char *)bitmap,
                                     KDS_LCD_BYTES, CTRL_TIMEOUT_MS);
    if (rc < 0) {
        kds_dbg(1, "LCD label %d: %s", number, libusb_error_name(rc));
        return usb_err(rc);
    }
    return KDS_OK;
}

/* ---- sequence files ---------------------------------------------------------- */
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

static int parse_hex(const char *arg, struct kds_step *st)
{
    size_t n = strcmp(arg, "-") ? strlen(arg) : 0;
    if (n % 2 || n / 2 > 0xffff)
        return KDS_E_SEQUENCE;
    st->len = (uint16_t)(n / 2);
    st->data = malloc(st->len ? st->len : 1);
    if (!st->data)
        return KDS_E_NOMEM;
    for (size_t i = 0; i < st->len; i++) {
        int hi = hexval(arg[2 * i]), lo = hexval(arg[2 * i + 1]);
        if (hi < 0 || lo < 0)
            return KDS_E_SEQUENCE;
        st->data[i] = (uint8_t)(hi << 4 | lo);
    }
    return KDS_OK;
}

/* One step per line; `#` starts a comment.
 *   out RR VVVV IIII HEXDATA|- PAUSE
 *   in  RR VVVV IIII LENGTH PAUSE
 * and in a power-up file also
 *   outblob RR VVVV IIII OFFSET LENGTH PAUSE     payload from the .bin file
 *   bulk OFFSET LENGTH PAUSE                     bulk OUT on EP 02 from the .bin file
 *   wait ID PAUSE                                poll GetStatus until the firmware id is ID
 * A file with a request outside the list for its kind is refused as a whole. */
static int load(const char *path, struct kds_seq *seq, int powerup)
{
    const uint8_t *out_ok = powerup ? PWR_OUT_ALLOWED : OUT_ALLOWED, *in_ok = powerup ? PWR_IN_ALLOWED : IN_ALLOWED;
    size_t n_out = powerup ? sizeof(PWR_OUT_ALLOWED) : sizeof(OUT_ALLOWED);
    size_t n_in = powerup ? sizeof(PWR_IN_ALLOWED) : sizeof(IN_ALLOWED);
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
    while (rc == KDS_OK && getline(&line, &cap, f) >= 0) {
        lineno++;
        char kind[12], *arg = NULL;
        unsigned req = 0, val = 0, idx = 0, off = 0, len = 0;
        double gap = 0;
        int pos = 0;
        char *hash = strchr(line, '#');
        if (hash)
            *hash = 0;
        if (sscanf(line, "%11s %n", kind, &pos) < 1)
            continue;
        const char *rest = line + pos;
        struct kds_step st = { 0 };
        rc = KDS_E_SEQUENCE;
        if (!strcmp(kind, "out")) {
            if (sscanf(rest, "%x %x %x %ms %lf", &req, &val, &idx, &arg, &gap) == 5)
                rc = parse_hex(arg, &st);
            st.kind = KDS_STEP_OUT;
        } else if (!strcmp(kind, "in")) {
            if (sscanf(rest, "%x %x %x %u %lf", &req, &val, &idx, &len, &gap) == 5 && len <= 4096)
                rc = KDS_OK;
            st.kind = KDS_STEP_IN;
            st.len = (uint16_t)len;
        } else if (powerup && !strcmp(kind, "outblob")) {
            if (sscanf(rest, "%x %x %x %u %u %lf", &req, &val, &idx, &off, &len, &gap) == 6 && len <= 0xffff)
                rc = KDS_OK;
            st.kind = KDS_STEP_OUTBLOB;
            st.off = off;
            st.blob_len = len;
        } else if (powerup && !strcmp(kind, "bulk")) {
            if (sscanf(rest, "%u %u %lf", &off, &len, &gap) == 3 && len > 0)
                rc = KDS_OK;
            st.kind = KDS_STEP_BULK;
            st.off = off;
            st.blob_len = len;
        } else if (powerup && !strcmp(kind, "wait")) {
            if (sscanf(rest, "%u %lf", &val, &gap) == 2 && val <= 0xff)
                rc = KDS_OK;
            st.kind = KDS_STEP_WAIT;
            st.want = (uint8_t)val;
            val = 0;
        }
        free(arg);
        if (rc == KDS_OK && (req > 0xff || val > 0xffff || idx > 0xffff || !(gap >= 0)))
            rc = KDS_E_SEQUENCE;
        st.out = st.kind == KDS_STEP_OUT;
        st.req = (uint8_t)req;
        st.val = (uint16_t)val;
        st.idx = (uint16_t)idx;
        st.gap = gap;
        int is_out = st.kind == KDS_STEP_OUT || st.kind == KDS_STEP_OUTBLOB;
        if (rc == KDS_OK && (is_out || st.kind == KDS_STEP_IN)
            && !allowed(is_out ? out_ok : in_ok, is_out ? n_out : n_in, st.req))
            rc = KDS_E_SEQUENCE;
        if (rc == KDS_OK && is_out && st.req == 0xf1 && st.val != 3)    /* the one diagnostic request of the power-up */
            rc = KDS_E_SEQUENCE;
        struct kds_step *ns = rc == KDS_OK ? realloc(seq->steps, (size_t)(seq->n + 1) * sizeof(*ns)) : NULL;
        if (rc == KDS_OK && !ns)
            rc = KDS_E_NOMEM;
        if (rc != KDS_OK) {
            kds_dbg(1, "%s:%d: refused (%s %02x)", path, lineno, kind, req);
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

int kds_seq_load(const char *path, struct kds_seq *seq)
{
    return load(path, seq, 0);
}

int kds_powerup_load(const char *path, struct kds_seq *seq)
{
    return load(path, seq, 1);
}

/* ---- power-up (section 6, "Native replay of the power-up open") ---------------- */
static int read_file(const char *path, uint8_t **data, size_t *size)
{
    FILE *f = fopen(path, "rb");
    *data = NULL;
    *size = 0;
    if (!f) {
        kds_dbg(1, "%s: %s", path, strerror(errno));
        return KDS_E_SEQUENCE;
    }
    long n = -1;
    if (fseek(f, 0, SEEK_END) == 0)
        n = ftell(f);
    rewind(f);
    if (n > 0 && n < (64L << 20) && (*data = malloc((size_t)n)) && fread(*data, 1, (size_t)n, f) == (size_t)n) {
        *size = (size_t)n;
        fclose(f);
        return KDS_OK;
    }
    fclose(f);
    free(*data);
    *data = NULL;
    return KDS_E_SEQUENCE;
}

static int drain_image_pipes(struct kds_dev *d, int timeout_ms)
{
    uint8_t *buf = malloc(16384);
    int rc = buf ? KDS_OK : KDS_E_NOMEM;
    for (int ep = 0; ep < 2 && rc == KDS_OK; ep++) {
        int got;
        do
            rc = bulk_read(d, ep ? EP_IMAGE_REAR : EP_IMAGE_FRONT, buf, 16384, timeout_ms, &got);
        while (rc == KDS_OK && got == 16384);
    }
    free(buf);
    return rc;
}

/* Load firmware and FPGA image into a freshly powered scanner by replaying the vendor
 * driver's own initialisation from a locally extracted file (PATH.seq + PATH.bin). */
static int power_up(struct kds_dev *d, const char *seq_path)
{
    struct kds_seq seq;
    uint8_t *bin = NULL, reply[4096];
    size_t bin_size = 0;
    char bin_path[600];
    size_t plen = strlen(seq_path);
    if (plen > 4 && !strcmp(seq_path + plen - 4, ".seq"))
        plen -= 4;
    snprintf(bin_path, sizeof(bin_path), "%.*s.bin", (int)(plen < 590 ? plen : 590), seq_path);

    int rc = kds_powerup_load(seq_path, &seq);
    if (rc != KDS_OK)
        return rc;
    rc = read_file(bin_path, &bin, &bin_size);
    for (int i = 0; i < seq.n && rc == KDS_OK; i++) {   /* check everything before sending anything */
        const struct kds_step *st = &seq.steps[i];
        if ((st->kind == KDS_STEP_OUTBLOB || st->kind == KDS_STEP_BULK)
            && ((size_t)st->off > bin_size || st->blob_len > bin_size - st->off)) {
            kds_dbg(1, "%s: step %d points outside %s", seq_path, i + 1, bin_path);
            rc = KDS_E_SEQUENCE;
        }
    }
    kds_dbg(2, "power-up: %d steps, %zu payload bytes", seq.n, bin_size);
    for (int i = 0; i < seq.n && rc == KDS_OK; i++) {
        const struct kds_step *st = &seq.steps[i];
        int n, got = 0;
        if (st->gap > 0)
            pause_s(st->gap < 2.0 ? st->gap : 2.0);
        switch (st->kind) {
        case KDS_STEP_BULK:
            n = libusb_bulk_transfer(d->h, EP_BULK_OUT, bin + st->off, (int)st->blob_len, &got, PWR_TIMEOUT_MS);
            if (n < 0 || (uint32_t)got != st->blob_len) {
                kds_dbg(1, "power-up step %d: bulk out: %s, %d of %u bytes", i + 1, n < 0 ? libusb_error_name(n) : "short", got, st->blob_len);
                rc = KDS_E_IO;
            }
            break;
        case KDS_STEP_WAIT: {       /* a just-loaded firmware needs time to come up */
            double deadline = now() + 15;
            for (;;) {
                n = libusb_control_transfer(d->h, 0xc0, REQ_GET_STATUS, 0, 0, reply, 32, 3000);
                if (n >= 1 && reply[0] == st->want)
                    break;
                if (now() > deadline) {
                    kds_dbg(1, "power-up step %d: firmware id %d did not appear", i + 1, st->want);
                    rc = KDS_E_NO_FIRMWARE;
                    break;
                }
                pause_s(0.1);
            }
            break;
        }
        case KDS_STEP_IN:
            n = libusb_control_transfer(d->h, 0xc0, st->req, st->val, st->idx, reply, st->len, PWR_TIMEOUT_MS);
            if (n < 0) {
                kds_dbg(1, "power-up step %d: get %02x: %s", i + 1, st->req, libusb_error_name(n));
                rc = KDS_E_IO;
            }
            break;
        default: {                  /* OUT, with the payload in the line or in the .bin file */
            const uint8_t *data = st->kind == KDS_STEP_OUTBLOB ? bin + st->off : st->data;
            uint16_t len = st->kind == KDS_STEP_OUTBLOB ? (uint16_t)st->blob_len : st->len;
            uint16_t val = st->val, idx = st->idx;
            if (st->req == REQ_SET_TIME) {      /* now, not the captured moment */
                uint32_t secs = (uint32_t)(time(NULL) - KODAK_EPOCH);
                val = (uint16_t)(secs >> 16);
                idx = (uint16_t)secs;
            }
            n = libusb_control_transfer(d->h, 0x40, st->req, val, idx, (unsigned char *)data, len, PWR_TIMEOUT_MS);
            if (n < 0) {
                kds_dbg(1, "power-up step %d: set %02x: %s", i + 1, st->req, libusb_error_name(n));
                rc = KDS_E_IO;
            } else if (st->req == REQ_START_CAPTURE) {  /* calibration capture, discarded */
                rc = drain_image_pipes(d, 3000);
            }
        }
        }
    }
    free(bin);
    kds_seq_free(&seq);
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
    kds_dbg(3, "image %d parked in %s", p->number, d->spool_dir);
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
    /* The scanner reports a Start press in standby but stays asleep, and its interlock and
     * tray bytes say nothing meanwhile (section 3). Wake it as the vendor driver does on open. */
    if (rc == KDS_OK && st.power == PWR_STANDBY) {
        double t0 = now();
        kds_dbg(1, "scanner is in standby: waking it");
        rc = ctl_out(d, REQ_SET_POWER, 2, 0, NULL, 0);
        for (int i = 0; rc == KDS_OK && i < 25; i++) {
            pause_s(0.2);
            rc = get_status(d, &st);
            if (rc != KDS_OK || st.power != PWR_STANDBY)
                break;
        }
        if (rc == KDS_OK && st.power != PWR_STANDBY) {
            kds_dbg(1, "awake after %.1f s: power %d, tray %d, interlock %d", now() - t0, st.power, st.tray, st.interlock);
            pause_s(1.0);       /* let the paper sensor settle */
            rc = get_status(d, &st);
        }
    }
    if (rc == KDS_OK) {
        if (st.power == PWR_STANDBY) {
            kds_dbg(1, "scanner is in standby and did not wake up");
            rc = KDS_E_SCANNER;
        } else if (st.interlock != 1)
            rc = KDS_E_COVER;
        else if (st.fw_id != 3)
            rc = KDS_E_NO_FIRMWARE;
        else if (st.error) {
            kds_dbg(1, "scanner reports error code %d", st.error);
            rc = KDS_E_SCANNER;
        } else if (st.tray != 2)
            rc = KDS_E_NO_DOCS;
    }
    if (rc != KDS_OK)
        return rc;

    pthread_mutex_lock(&d->lock);
    d->active = 1;
    d->started = d->done = d->finished = d->cancel = d->ends = d->sheets = d->error = 0;
    d->duplex = duplex;
    d->last_event = now();
    pthread_mutex_unlock(&d->lock);

    rc = replay(d, seq);
    pthread_mutex_lock(&d->lock);
    d->batch_events = 0;        /* what comes now belongs to the feed (Transport State, Start of Operation) */
    pthread_mutex_unlock(&d->lock);
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

/* After kds_batch_start(): 1 as soon as the scanner shows that it feeds (an event, a page or
 * the end of the batch), 0 if nothing came for `seconds`. A started scan reports Transport
 * State and Start of Operation within about a second. */
int kds_batch_wait_feed(struct kds_dev *d, double seconds)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    ts.tv_sec += (time_t)seconds;
    ts.tv_nsec += (long)((seconds - (time_t)seconds) * 1e9);
    if (ts.tv_nsec >= 1000000000) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000;
    }
    pthread_mutex_lock(&d->lock);
    int rc = 0;
    while (d->active && !d->batch_events && !d->done && !d->finished && !d->head[0] && !d->head[1] && rc == 0)
        rc = pthread_cond_timedwait(&d->cond, &d->lock, &ts);
    int fed = !d->active || d->batch_events || d->done || d->finished || d->head[0] || d->head[1];
    pthread_mutex_unlock(&d->lock);
    return fed;
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
