/* SANE backend for the Kodak i2400/i2600/i2800 sheet-fed scanners (USB 040a:601d = i2600).
 *
 * Written from docs/protocol/ only (clean room). This file is the SANE API; the device
 * layer is in kds_dev.c, kds_split.c and kds_image.c.
 *
 * Scanning: the scanner feeds the whole stack by itself once started (a "batch"). The
 * first sane_start() starts the batch, every sane_start() returns one side of one sheet
 * (front, then rear in duplex), and SANE_STATUS_NO_DOCS ends the batch.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libusb.h>
#include <sane/sane.h>
#include <sane/saneopts.h>

#include "kds.h"

#define BACKEND_NAME "kodak_i2x00"
#define BUILD 1
#define EXPORT __attribute__((visibility("default")))

#ifndef KDS_DATADIR
#define KDS_DATADIR "/usr/share/sane/kodak_i2x00"
#endif
#ifndef KDS_CONFDIR
#define KDS_CONFDIR "/etc/sane.d"
#endif

/* Size of an A4 sheet in raw pixels, for sane_get_parameters() before a scan. */
#define A4_W 2448
#define A4_H 3464
#define NUM_LABELS 7        /* function numbers the panel offers */
#define LABEL_SIZE 160

enum {
    OPT_NUM,
    OPT_GROUP_MODE,
    OPT_MODE,
    OPT_SOURCE,
    OPT_RESOLUTION,
    OPT_THRESHOLD,
    OPT_GROUP_ADVANCED,
    OPT_SWDESKEW,
    OPT_RAW,
    OPT_GROUP_SENSORS,
    OPT_SCAN,
    OPT_FUNCTION,
    OPT_PAGE_LOADED,
    OPT_COVER_OPEN,
    OPT_GROUP_DISPLAY,
    OPT_LABEL_1,            /* ... OPT_LABEL_1 + NUM_LABELS - 1 */
    OPT_QUIET = OPT_LABEL_1 + 7,
    NUM_OPTIONS
};

static const SANE_String_Const mode_list[] = {
    SANE_VALUE_SCAN_MODE_COLOR, SANE_VALUE_SCAN_MODE_GRAY, SANE_VALUE_SCAN_MODE_LINEART, NULL
};
static const SANE_String_Const source_list[] = { "ADF Front", "ADF Duplex", NULL };
static const SANE_Word resolution_list[] = { 1, 300 };
static const SANE_Range threshold_range = { 0, 255, 1 };
static const SANE_Range function_range = { 1, 9, 1 };

struct usb_id {
    unsigned vid, pid;
};

struct device {
    SANE_Device sane;
    char name[32];
    libusb_device *usb;
};

struct handle {
    struct kds_dev *dev;
    SANE_Option_Descriptor opt[NUM_OPTIONS];
    int mode, source, threshold, raw, deskew;
    int pressed;            /* Start press not yet reported through the scan option */
    int quiet;
    char label[NUM_LABELS][LABEL_SIZE];
    char label_name[NUM_LABELS][12], label_title[NUM_LABELS][32];

    struct kds_seq seq;
    int batch;              /* a batch is running on the device */
    int next_side;
    struct kds_page *page;  /* the frame being read */
    struct kds_sheet sheet;
    uint8_t *rawline;       /* one row of the sheet as raw RGB */
    int frame_mode, line;
    int eof;                /* the frame was read to its end */
    uint8_t *linebuf;
    int linebuf_len, linebuf_pos;
};

static libusb_context *usb_ctx;
static struct device *devices;
static int num_devices;
static const SANE_Device **device_list;

static struct usb_id ids[16] = { { 0x040a, 0x601d } };
static int num_ids = 1;
static char cfg_sequence[512] = KDS_DATADIR "/color300-duplex.seq";
static char cfg_spool_dir[256];
static char cfg_powerup[512];
static int cfg_memory_pages = -1;
static int cfg_functions;               /* function numbers the panel offers, 0 = unchanged */
static char cfg_label[10][64];          /* LCD text per function number, "" = leave alone */

/* ---- configuration ----------------------------------------------------------- */
static void read_config_file(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return;
    kds_dbg(2, "reading %s", path);
    char line[1024], val[512];   /* longer values are cut by the %Ns limits below */
    while (fgets(line, sizeof(line), f)) {
        unsigned vid, pid;
        int n;
        char *p = line + strspn(line, " \t");
        if (*p == '#' || *p == '\n' || !*p)
            continue;
        if (sscanf(p, "usb %x %x", &vid, &pid) == 2) {
            int known = 0;
            for (int i = 0; i < num_ids; i++)
                known |= ids[i].vid == vid && ids[i].pid == pid;
            if (!known && num_ids < (int)(sizeof(ids) / sizeof(ids[0])))
                ids[num_ids++] = (struct usb_id){ vid, pid };
        } else if (sscanf(p, "sequence %511s", val) == 1) {
            snprintf(cfg_sequence, sizeof(cfg_sequence), "%s", val);
        } else if (sscanf(p, "powerup %511s", val) == 1) {
            snprintf(cfg_powerup, sizeof(cfg_powerup), "%s", val);
        } else if (sscanf(p, "spool-dir %250s", val) == 1) {
            snprintf(cfg_spool_dir, sizeof(cfg_spool_dir), "%.250s", val);
        } else if (sscanf(p, "memory-pages %d", &n) == 1) {
            cfg_memory_pages = n;
        } else if (sscanf(p, "functions %d", &n) == 1 && n >= 1 && n <= 9) {
            cfg_functions = n;
        } else if (sscanf(p, "label %d %63[^\n]", &n, val) == 2 && n >= 1 && n <= 9) {
            snprintf(cfg_label[n], sizeof(cfg_label[n]), "%.63s", val);
        } else {
            kds_dbg(1, "%s: unknown line: %s", path, p);
        }
    }
    fclose(f);
}

static void read_config(void)
{
    const char *dirs = getenv("SANE_CONFIG_DIR");
    char buf[1024];
    snprintf(buf, sizeof(buf), "%s", dirs && *dirs ? dirs : KDS_CONFDIR);
    for (char *save, *dir = strtok_r(buf, ":", &save); dir; dir = strtok_r(NULL, ":", &save)) {
        char path[1200];
        snprintf(path, sizeof(path), "%s/" BACKEND_NAME ".conf", dir);
        read_config_file(path);
    }
}

/* ---- helpers ------------------------------------------------------------------ */
static SANE_Status to_sane(int err)
{
    switch (err) {
    case KDS_OK: return SANE_STATUS_GOOD;
    case KDS_END:
    case KDS_E_NO_DOCS: return SANE_STATUS_NO_DOCS;
    case KDS_E_BUSY: return SANE_STATUS_DEVICE_BUSY;
    case KDS_E_ACCESS: return SANE_STATUS_ACCESS_DENIED;
    case KDS_E_COVER: return SANE_STATUS_COVER_OPEN;
    case KDS_E_JAM:
    case KDS_E_MULTIFEED: return SANE_STATUS_JAMMED;
    case KDS_E_NOMEM: return SANE_STATUS_NO_MEM;
    case KDS_E_SEQUENCE: return SANE_STATUS_INVAL;
    default: return SANE_STATUS_IO_ERROR;
    }
}

static void free_devices(void)
{
    for (int i = 0; i < num_devices; i++)
        libusb_unref_device(devices[i].usb);
    free(devices);
    free(device_list);
    devices = NULL;
    device_list = NULL;
    num_devices = 0;
}

static void release_page(struct handle *h)
{
    if (h->page)
        kds_page_free(h->dev, h->page);
    h->page = NULL;
    free(h->linebuf);
    free(h->rawline);
    h->linebuf = h->rawline = NULL;
    h->linebuf_len = h->linebuf_pos = 0;
}

static void end_batch(struct handle *h)
{
    release_page(h);
    if (h->batch)
        kds_batch_end(h->dev);
    h->batch = 0;
    h->eof = 0;
}

static int scan_mode(const struct handle *h)
{
    return h->raw ? KDS_MODE_RAW : h->mode;
}

static size_t max_string_size(const SANE_String_Const *list)
{
    size_t max = 0;
    for (; *list; list++)
        if (strlen(*list) + 1 > max)
            max = strlen(*list) + 1;
    return max;
}

static void init_options(struct handle *h)
{
    SANE_Option_Descriptor *o;
    for (int i = 0; i < NUM_OPTIONS; i++) {
        o = &h->opt[i];
        o->name = o->title = o->desc = "";
        o->type = SANE_TYPE_INT;
        o->unit = SANE_UNIT_NONE;
        o->size = sizeof(SANE_Word);
        o->cap = SANE_CAP_SOFT_SELECT | SANE_CAP_SOFT_DETECT;
        o->constraint_type = SANE_CONSTRAINT_NONE;
    }

    o = &h->opt[OPT_NUM];
    o->title = SANE_TITLE_NUM_OPTIONS;
    o->desc = SANE_DESC_NUM_OPTIONS;
    o->cap = SANE_CAP_SOFT_DETECT;

    static const struct { int opt; const char *title; int cap; } groups[] = {
        { OPT_GROUP_MODE, SANE_TITLE_STANDARD, 0 },
        { OPT_GROUP_ADVANCED, "Advanced", SANE_CAP_ADVANCED },
        { OPT_GROUP_SENSORS, SANE_TITLE_SENSORS, SANE_CAP_ADVANCED },
        { OPT_GROUP_DISPLAY, "Display and power", SANE_CAP_ADVANCED },
    };
    for (size_t i = 0; i < sizeof(groups) / sizeof(groups[0]); i++) {
        o = &h->opt[groups[i].opt];
        o->title = groups[i].title;
        o->type = SANE_TYPE_GROUP;
        o->size = 0;
        o->cap = groups[i].cap;
    }

    o = &h->opt[OPT_MODE];
    o->name = SANE_NAME_SCAN_MODE;
    o->title = SANE_TITLE_SCAN_MODE;
    o->desc = SANE_DESC_SCAN_MODE;
    o->type = SANE_TYPE_STRING;
    o->size = (SANE_Int)max_string_size(mode_list);
    o->constraint_type = SANE_CONSTRAINT_STRING_LIST;
    o->constraint.string_list = mode_list;

    o = &h->opt[OPT_SOURCE];
    o->name = SANE_NAME_SCAN_SOURCE;
    o->title = SANE_TITLE_SCAN_SOURCE;
    o->desc = SANE_DESC_SCAN_SOURCE;
    o->type = SANE_TYPE_STRING;
    o->size = (SANE_Int)max_string_size(source_list);
    o->constraint_type = SANE_CONSTRAINT_STRING_LIST;
    o->constraint.string_list = source_list;

    o = &h->opt[OPT_RESOLUTION];
    o->name = SANE_NAME_SCAN_RESOLUTION;
    o->title = SANE_TITLE_SCAN_RESOLUTION;
    o->desc = SANE_DESC_SCAN_RESOLUTION;
    o->unit = SANE_UNIT_DPI;
    o->constraint_type = SANE_CONSTRAINT_WORD_LIST;
    o->constraint.word_list = resolution_list;

    o = &h->opt[OPT_THRESHOLD];
    o->name = SANE_NAME_THRESHOLD;
    o->title = SANE_TITLE_THRESHOLD;
    o->desc = "Gray level below which a pixel becomes black in Lineart mode (higher = more black).";
    o->constraint_type = SANE_CONSTRAINT_RANGE;
    o->constraint.range = &threshold_range;
    o->cap |= SANE_CAP_INACTIVE;

    o = &h->opt[OPT_SWDESKEW];
    o->name = "swdeskew";
    o->title = "Software deskew";
    o->desc = "Straighten a sheet that was fed at an angle.";
    o->type = SANE_TYPE_BOOL;
    o->cap |= SANE_CAP_ADVANCED;

    o = &h->opt[OPT_RAW];
    o->name = "raw";
    o->title = "Raw sensor image";
    o->desc = "Deliver the image as the scanner sends it: full sensor width, no crop, no deskew, no colour correction.";
    o->type = SANE_TYPE_BOOL;
    o->cap |= SANE_CAP_ADVANCED;

    const int sensor = SANE_CAP_SOFT_DETECT | SANE_CAP_HARD_SELECT | SANE_CAP_ADVANCED;
    o = &h->opt[OPT_SCAN];
    o->name = SANE_NAME_SCAN;
    o->title = SANE_TITLE_SCAN;
    o->desc = "The Start button was pressed since this option was last read.";
    o->type = SANE_TYPE_BOOL;
    o->cap = sensor;

    o = &h->opt[OPT_FUNCTION];
    o->name = "function-number";
    o->title = "Function number";
    o->desc = "The function number selected with the arrow buttons and shown on the scanner's display.";
    o->constraint_type = SANE_CONSTRAINT_RANGE;
    o->constraint.range = &function_range;
    o->cap = sensor;

    o = &h->opt[OPT_PAGE_LOADED];
    o->name = SANE_NAME_PAGE_LOADED;
    o->title = SANE_TITLE_PAGE_LOADED;
    o->desc = SANE_DESC_PAGE_LOADED;
    o->type = SANE_TYPE_BOOL;
    o->cap = sensor;

    o = &h->opt[OPT_COVER_OPEN];
    o->name = SANE_NAME_COVER_OPEN;
    o->title = SANE_TITLE_COVER_OPEN;
    o->desc = SANE_DESC_COVER_OPEN;
    o->type = SANE_TYPE_BOOL;
    o->cap = sensor;

    for (int i = 0; i < NUM_LABELS; i++) {
        snprintf(h->label_name[i], sizeof(h->label_name[i]), "label-%d", i + 1);
        snprintf(h->label_title[i], sizeof(h->label_title[i]), "Display text for function %d", i + 1);
        snprintf(h->label[i], LABEL_SIZE, "%s", cfg_label[i + 1]);
        o = &h->opt[OPT_LABEL_1 + i];
        o->name = h->label_name[i];
        o->title = h->label_title[i];
        o->desc = "Text on the scanner's display next to this function number (ASCII). A line break starts "
                  "small info lines below the title. The scanner forgets it when switched off.";
        o->type = SANE_TYPE_STRING;
        o->size = LABEL_SIZE;
        o->cap |= SANE_CAP_ADVANCED;
    }
    o = &h->opt[OPT_QUIET];
    o->name = "quiet";
    o->title = "Quiet while idle";
    o->desc = "Send nothing to the scanner while it is idle, so that it may go to standby. "
              "Buttons, paper and cover are still reported.";
    o->type = SANE_TYPE_BOOL;
    o->cap |= SANE_CAP_ADVANCED;

    h->mode = KDS_MODE_COLOR;
    h->source = 1;
    h->threshold = 200;
    h->raw = 0;
    h->deskew = 1;
}

/* ---- SANE API ------------------------------------------------------------------ */
EXPORT SANE_Status sane_kodak_i2x00_init(SANE_Int *version_code, SANE_Auth_Callback authorize)
{
    (void)authorize;
    const char *dbg = getenv("SANE_DEBUG_KODAK_I2X00");
    kds_debug_level = dbg ? atoi(dbg) : 0;
    if (version_code)
        *version_code = SANE_VERSION_CODE(SANE_CURRENT_MAJOR, 0, BUILD);
    if (usb_ctx)
        return SANE_STATUS_GOOD;
    read_config();
    int rc = libusb_init(&usb_ctx);
    if (rc < 0) {
        kds_dbg(1, "libusb_init: %s", libusb_error_name(rc));
        usb_ctx = NULL;
        return SANE_STATUS_IO_ERROR;
    }
    return SANE_STATUS_GOOD;
}

EXPORT void sane_kodak_i2x00_exit(void)
{
    free_devices();
    if (usb_ctx)
        libusb_exit(usb_ctx);
    usb_ctx = NULL;
}

EXPORT SANE_Status sane_kodak_i2x00_get_devices(const SANE_Device ***list, SANE_Bool local_only)
{
    (void)local_only;
    if (!usb_ctx)
        return SANE_STATUS_INVAL;
    free_devices();
    libusb_device **all;
    ssize_t n = libusb_get_device_list(usb_ctx, &all);
    if (n < 0)
        return SANE_STATUS_IO_ERROR;
    devices = calloc((size_t)n + 1, sizeof(*devices));
    device_list = calloc((size_t)n + 1, sizeof(*device_list));
    if (!devices || !device_list) {
        libusb_free_device_list(all, 1);
        free_devices();
        return SANE_STATUS_NO_MEM;
    }
    for (ssize_t i = 0; i < n; i++) {
        struct libusb_device_descriptor dd;
        if (libusb_get_device_descriptor(all[i], &dd) < 0)
            continue;
        int match = 0;
        for (int k = 0; k < num_ids; k++)
            match |= ids[k].vid == dd.idVendor && ids[k].pid == dd.idProduct;
        if (!match)
            continue;
        struct device *dev = &devices[num_devices];
        snprintf(dev->name, sizeof(dev->name), "usb:%03d:%03d",
                 libusb_get_bus_number(all[i]), libusb_get_device_address(all[i]));
        dev->usb = libusb_ref_device(all[i]);
        dev->sane.name = dev->name;
        dev->sane.vendor = "Kodak";
        dev->sane.model = dd.idProduct == 0x601d ? "i2600" : "i2x00";
        dev->sane.type = "sheetfed scanner";
        device_list[num_devices++] = &dev->sane;
    }
    libusb_free_device_list(all, 1);
    kds_dbg(2, "%d scanner(s) found", num_devices);
    if (list)
        *list = device_list;
    return SANE_STATUS_GOOD;
}

EXPORT SANE_Status sane_kodak_i2x00_open(SANE_String_Const name, SANE_Handle *handle)
{
    if (!usb_ctx || !handle)
        return SANE_STATUS_INVAL;
    if (!num_devices) {
        SANE_Status st = sane_kodak_i2x00_get_devices(NULL, SANE_FALSE);
        if (st != SANE_STATUS_GOOD)
            return st;
    }
    struct device *dev = NULL;
    for (int i = 0; i < num_devices && !dev; i++)
        if (!name || !*name || !strcmp(name, devices[i].name))
            dev = &devices[i];
    if (!dev)
        return SANE_STATUS_INVAL;

    struct handle *h = calloc(1, sizeof(*h));
    if (!h)
        return SANE_STATUS_NO_MEM;
    int rc = kds_open(dev->usb, cfg_powerup, cfg_functions, &h->dev);
    if (rc != KDS_OK) {
        kds_dbg(1, "open %s: %s", dev->name, kds_strerror(rc));
        free(h);
        return to_sane(rc);
    }
    kds_set_spool(h->dev, cfg_memory_pages, cfg_spool_dir);
    for (int n = 1; n <= 9; n++) {      /* the scanner forgets its labels at power-off */
        uint8_t bitmap[KDS_LCD_BYTES];
        if (!cfg_label[n][0])
            continue;
        kds_lcd_text(cfg_label[n], bitmap);
        rc = kds_lcd_label(h->dev, n, bitmap);
        kds_dbg(rc == KDS_OK ? 2 : 1, "LCD label %d \"%s\": %s", n, cfg_label[n], kds_strerror(rc));
    }
    init_options(h);
    *handle = h;
    return SANE_STATUS_GOOD;
}

EXPORT void sane_kodak_i2x00_close(SANE_Handle handle)
{
    struct handle *h = handle;
    if (!h)
        return;
    end_batch(h);
    kds_close(h->dev);
    kds_seq_free(&h->seq);
    free(h);
}

EXPORT const SANE_Option_Descriptor *sane_kodak_i2x00_get_option_descriptor(SANE_Handle handle, SANE_Int n)
{
    struct handle *h = handle;
    return n >= 0 && n < NUM_OPTIONS ? &h->opt[n] : NULL;
}

static int find_string(const SANE_String_Const *list, const char *s)
{
    for (int i = 0; list[i]; i++)
        if (!strcmp(list[i], s))
            return i;
    return -1;
}

EXPORT SANE_Status sane_kodak_i2x00_control_option(SANE_Handle handle, SANE_Int n, SANE_Action action,
                                                   void *value, SANE_Int *info)
{
    struct handle *h = handle;
    if (info)
        *info = 0;
    if (n < 0 || n >= NUM_OPTIONS || h->opt[n].type == SANE_TYPE_GROUP)
        return SANE_STATUS_INVAL;
    if (h->opt[n].cap & SANE_CAP_INACTIVE)
        return SANE_STATUS_INVAL;

    if (action == SANE_ACTION_GET_VALUE) {
        struct kds_panel panel;
        if (!value)
            return SANE_STATUS_INVAL;
        switch (n) {
        case OPT_NUM: *(SANE_Word *)value = NUM_OPTIONS; break;
        case OPT_MODE: strcpy(value, mode_list[h->mode]); break;
        case OPT_SOURCE: strcpy(value, source_list[h->source]); break;
        case OPT_RESOLUTION: *(SANE_Word *)value = 300; break;
        case OPT_THRESHOLD: *(SANE_Word *)value = h->threshold; break;
        case OPT_RAW: *(SANE_Word *)value = h->raw; break;
        case OPT_QUIET: *(SANE_Word *)value = h->quiet; break;
        case OPT_SWDESKEW: *(SANE_Word *)value = h->deskew; break;
        default:
            if (n >= OPT_LABEL_1 && n < OPT_LABEL_1 + NUM_LABELS) {
                strcpy(value, h->label[n - OPT_LABEL_1]);
                break;
            }
            /* sensors */
            if (kds_panel(h->dev, &panel) != KDS_OK)
                return SANE_STATUS_IO_ERROR;
            h->pressed |= panel.start_pressed;          /* kept until the scan option is read */
            if (n == OPT_SCAN) {
                *(SANE_Word *)value = h->pressed;
                h->pressed = 0;
            } else if (n == OPT_FUNCTION) {
                *(SANE_Word *)value = panel.function;
            } else if (n == OPT_PAGE_LOADED) {
                *(SANE_Word *)value = panel.paper;
            } else {
                *(SANE_Word *)value = panel.cover_open;
            }
        }
        return SANE_STATUS_GOOD;
    }

    if (action != SANE_ACTION_SET_VALUE && action != SANE_ACTION_SET_AUTO)
        return SANE_STATUS_INVAL;
    if (!(h->opt[n].cap & SANE_CAP_SOFT_SELECT) || action == SANE_ACTION_SET_AUTO)
        return SANE_STATUS_INVAL;
    if (!value)
        return SANE_STATUS_INVAL;
    if (h->batch)
        return SANE_STATUS_DEVICE_BUSY;
    int i;
    switch (n) {
    case OPT_MODE:
        if ((i = find_string(mode_list, value)) < 0)
            return SANE_STATUS_INVAL;
        h->mode = i;
        if (i == KDS_MODE_LINEART)
            h->opt[OPT_THRESHOLD].cap &= ~SANE_CAP_INACTIVE;
        else
            h->opt[OPT_THRESHOLD].cap |= SANE_CAP_INACTIVE;
        if (info)
            *info |= SANE_INFO_RELOAD_PARAMS | SANE_INFO_RELOAD_OPTIONS;
        break;
    case OPT_SOURCE:
        if ((i = find_string(source_list, value)) < 0)
            return SANE_STATUS_INVAL;
        h->source = i;
        break;
    case OPT_RESOLUTION:
        if (*(SANE_Word *)value != 300) {
            *(SANE_Word *)value = 300;
            if (info)
                *info |= SANE_INFO_INEXACT;
        }
        break;
    case OPT_THRESHOLD:
        i = *(SANE_Word *)value;
        if (i < 0 || i > 255)
            return SANE_STATUS_INVAL;
        h->threshold = i;
        break;
    case OPT_SWDESKEW:
        h->deskew = *(SANE_Word *)value != 0;
        break;
    case OPT_QUIET:
        h->quiet = *(SANE_Word *)value != 0;
        kds_set_quiet(h->dev, h->quiet);
        break;
    case OPT_RAW:
        h->raw = *(SANE_Word *)value != 0;
        if (info)
            *info |= SANE_INFO_RELOAD_PARAMS;
        break;
    default:
        if (n >= OPT_LABEL_1 && n < OPT_LABEL_1 + NUM_LABELS) {
            uint8_t bitmap[KDS_LCD_BYTES];
            const char *text = value;
            if (strnlen(text, LABEL_SIZE) >= LABEL_SIZE)
                return SANE_STATUS_INVAL;
            kds_lcd_text(text, bitmap);
            int rc = kds_lcd_label(h->dev, n - OPT_LABEL_1 + 1, bitmap);
            if (rc != KDS_OK) {
                kds_dbg(1, "LCD label %d: %s", n - OPT_LABEL_1 + 1, kds_strerror(rc));
                return to_sane(rc);
            }
            strcpy(h->label[n - OPT_LABEL_1], text);
            break;
        }
        return SANE_STATUS_INVAL;
    }
    return SANE_STATUS_GOOD;
}

EXPORT SANE_Status sane_kodak_i2x00_get_parameters(SANE_Handle handle, SANE_Parameters *p)
{
    struct handle *h = handle;
    if (!p)
        return SANE_STATUS_INVAL;
    int mode = h->page ? h->frame_mode : scan_mode(h);
    int w = h->page ? h->sheet.w : h->raw ? KDS_LINE_PX : A4_W;
    p->format = mode == KDS_MODE_GRAY || mode == KDS_MODE_LINEART ? SANE_FRAME_GRAY : SANE_FRAME_RGB;
    p->depth = mode == KDS_MODE_LINEART ? 1 : 8;
    p->last_frame = SANE_TRUE;
    p->pixels_per_line = w;
    p->bytes_per_line = kds_bytes_per_line(mode, w);
    p->lines = h->page ? h->sheet.h : A4_H;      /* an estimate until the page has arrived */
    return SANE_STATUS_GOOD;
}

EXPORT SANE_Status sane_kodak_i2x00_start(SANE_Handle handle)
{
    struct handle *h = handle;
    int rc;
    release_page(h);
    h->eof = 0;
    if (!h->batch) {
        if (!h->seq.n && (rc = kds_seq_load(cfg_sequence, &h->seq)) != KDS_OK) {
            kds_dbg(1, "scan sequence %s: %s", cfg_sequence, kds_strerror(rc));
            return to_sane(rc);
        }
        for (int attempt = 0;; attempt++) {
            rc = kds_batch_start(h->dev, &h->seq, h->source == 1);
            if (rc != KDS_OK) {
                kds_dbg(1, "start: %s", kds_strerror(rc));
                return to_sane(rc);
            }
            /* Seen once after a long idle time: the start sequence is accepted, but the scanner
             * neither feeds nor reports anything. Do not wait 30 s for that; stop and start again. */
            if (attempt == 1 || kds_batch_wait_feed(h->dev, 6.0))
                break;
            kds_dbg(1, "the scanner did not react to the scan start: stopping and starting once more");
            kds_batch_end(h->dev);
        }
        h->batch = 1;
        h->next_side = KDS_FRONT;
    }
    rc = kds_batch_next(h->dev, h->next_side, &h->page);
    if (rc == KDS_OK)
        rc = kds_page_load(h->page);
    if (rc != KDS_OK) {
        if (rc != KDS_END)
            kds_dbg(1, "batch: %s", kds_strerror(rc));
        else
            kds_dbg(2, "batch complete: %d sheet(s)", kds_batch_sheets(h->dev));
        end_batch(h);
        return to_sane(rc);
    }
    if (h->source == 1)
        h->next_side ^= 1;
    h->frame_mode = scan_mode(h);
    if (h->frame_mode == KDS_MODE_RAW) {
        kds_full_frame(h->page->lines, &h->sheet);
    } else {
        kds_find_sheet(h->page->data, h->page->lines, h->deskew, &h->sheet);
        kds_dbg(3, "sheet at %.0f,%.0f size %dx%d, skew %.2f deg", h->sheet.x0, h->sheet.y0,
                h->sheet.w, h->sheet.h, h->sheet.angle * 57.29578);
    }
    h->line = 0;
    h->linebuf_len = kds_bytes_per_line(h->frame_mode, h->sheet.w);
    h->linebuf_pos = h->linebuf_len;
    h->linebuf = malloc((size_t)h->linebuf_len);
    h->rawline = malloc((size_t)h->sheet.w * 3);
    if (!h->linebuf || !h->rawline) {
        end_batch(h);
        return SANE_STATUS_NO_MEM;
    }
    return SANE_STATUS_GOOD;
}

EXPORT SANE_Status sane_kodak_i2x00_read(SANE_Handle handle, SANE_Byte *buf, SANE_Int max_len, SANE_Int *len)
{
    struct handle *h = handle;
    if (len)
        *len = 0;
    if (!h->page || !buf || !len)
        return h->eof ? SANE_STATUS_EOF : SANE_STATUS_CANCELLED;
    while (*len < max_len) {
        if (h->linebuf_pos == h->linebuf_len) {
            if (h->line >= h->sheet.h)
                break;
            kds_sheet_line(h->page->data, h->page->lines, &h->sheet, h->line, h->rawline);
            kds_convert_line(h->rawline, h->sheet.w, h->frame_mode, h->threshold, h->linebuf);
            h->linebuf_pos = 0;
            h->line++;
        }
        int n = h->linebuf_len - h->linebuf_pos;
        if (n > max_len - *len)
            n = max_len - *len;
        memcpy(buf + *len, h->linebuf + h->linebuf_pos, (size_t)n);
        h->linebuf_pos += n;
        *len += n;
    }
    if (*len == 0) {
        release_page(h);
        h->eof = 1;
        return SANE_STATUS_EOF;
    }
    return SANE_STATUS_GOOD;
}

/* After a completely read page the batch goes on: the scanner has fed the remaining
 * sheets already, and the next sane_start() delivers them. Cancelling inside a page
 * (or before the first one) ends the batch. */
EXPORT void sane_kodak_i2x00_cancel(SANE_Handle handle)
{
    struct handle *h = handle;
    if (h->page || !h->eof)
        end_batch(h);
}

EXPORT SANE_Status sane_kodak_i2x00_set_io_mode(SANE_Handle handle, SANE_Bool non_blocking)
{
    (void)handle;
    return non_blocking ? SANE_STATUS_UNSUPPORTED : SANE_STATUS_GOOD;
}

EXPORT SANE_Status sane_kodak_i2x00_get_select_fd(SANE_Handle handle, SANE_Int *fd)
{
    (void)handle;
    (void)fd;
    return SANE_STATUS_UNSUPPORTED;
}
