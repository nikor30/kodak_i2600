/* Cuts one side's image stream into pages at the trailers (section 8, "Pages and trailers"). */
#include <stdlib.h>
#include <string.h>

#include "kds.h"

#define TAGS 32                         /* tag bytes at the start of a trailer */
#define FIRST_CAP ((size_t)4200 * KDS_LINE)   /* a little more than an A4 page */

void kds_split_init(struct kds_splitter *s, kds_page_cb cb, void *arg)
{
    memset(s, 0, sizeof(*s));
    s->cb = cb;
    s->arg = arg;
}

void kds_split_free(struct kds_splitter *s)
{
    free(s->buf);
    s->buf = NULL;
    s->len = s->cap = s->line = 0;
}

uint8_t *kds_split_reserve(struct kds_splitter *s, size_t n)
{
    if (s->len + n > s->cap) {
        size_t cap = s->cap ? s->cap : FIRST_CAP;
        while (cap < s->len + n)
            cap += cap / 2;
        uint8_t *nb = realloc(s->buf, cap);
        if (!nb)
            return NULL;
        s->buf = nb;
        s->cap = cap;
    }
    return s->buf + s->len;
}

/* `00 01 00 00 nn nn 02 f5`, then six tags `vv vv 01 tt` with tt = f6 f9 fa fc fd fe */
static int tags_at(const uint8_t *p)
{
    static const uint8_t tag[6] = { 0xf6, 0xf9, 0xfa, 0xfc, 0xfd, 0xfe };
    if (p[0] != 0 || p[1] != 1 || p[2] != 0 || p[3] != 0 || p[6] != 2 || p[7] != 0xf5)
        return 0;
    for (int i = 0; i < 6; i++)
        if (p[8 + 4 * i + 2] != 1 || p[8 + 4 * i + 3] != tag[i])
            return 0;
    return 1;
}

int kds_split_commit(struct kds_splitter *s, size_t n)
{
    s->len += n;
    for (;;) {
        size_t pos = s->line * KDS_LINE;
        if (pos + TAGS > s->len)
            return KDS_OK;
        const uint8_t *b = s->buf;
        if (!tags_at(b + pos)) {
            s->line++;
            continue;
        }
        size_t end = 0;
        for (int k = 0; k < 256; k++) {     /* 2k filler bytes, then `00 k 01 ff` */
            size_t q = pos + TAGS + 2 * (size_t)k;
            if (q + 4 > s->len)
                return KDS_OK;              /* trailer not complete yet */
            if (b[q] == 0 && b[q + 1] == k && b[q + 2] == 1 && b[q + 3] == 0xff) {
                end = q + 4;
                break;
            }
        }
        if (!end) {                         /* tag bytes by coincidence in pixel data */
            s->line++;
            continue;
        }
        int number = b[pos + 4] << 8 | b[pos + 5];
        size_t rest = s->len - end;
        if (s->line) {
            size_t cap = rest > FIRST_CAP ? rest : FIRST_CAP;
            uint8_t *nb = malloc(cap);
            if (!nb)
                return KDS_E_NOMEM;
            memcpy(nb, b + end, rest);
            uint8_t *page = realloc(s->buf, pos);
            if (!page)
                page = s->buf;
            s->buf = nb;
            s->cap = cap;
            s->pages++;
            s->cb(s->arg, number, page, (int)s->line);
        } else {
            memmove(s->buf, b + end, rest);
        }
        s->len = rest;
        s->line = 0;
    }
}

size_t kds_split_leftover_lines(const struct kds_splitter *s)
{
    return s->len / KDS_LINE;
}
