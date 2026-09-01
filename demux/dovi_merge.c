/*
 * This file is part of mpv.
 *
 * mpv is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * mpv is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with mpv.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <libavcodec/packet.h>
#include <libavutil/avutil.h>
#include <libavutil/error.h>
#include <libavutil/mem.h>
#include <libavutil/mathematics.h>

#include "common/msg.h"
#include "mpv_talloc.h"

#include "dovi_merge.h"

#define DOVI_MERGE_MAX_PENDING 16
#define DOVI_MERGE_MAX_BYTES (64U * 1024U * 1024U)

#define HEVC_NAL_AUD 35
#define HEVC_NAL_DOVI_RPU 62

struct packet_node {
    AVPacket *pkt;
    struct packet_node *next;
};

struct packet_queue {
    struct packet_node *head;
    struct packet_node *tail;
    unsigned count;
    size_t bytes;
};

struct mp_dovi_merge {
    struct mp_log *log;
    int bl_stream;
    int el_stream;
    AVRational bl_tb;
    AVRational el_tb;
    struct packet_queue bl;
    struct packet_queue el;
    bool warned_malformed;
    bool warned_pressure;
};

struct annexb_nal {
    const uint8_t *data;
    size_t size;
    int type;
};

struct annexb_iter {
    const uint8_t *data;
    size_t size;
    size_t next;
    bool invalid;
};

static bool find_start_code(const uint8_t *data, size_t size, size_t from,
                            size_t *offset, size_t *length)
{
    for (size_t n = from; n + 3 <= size; n++) {
        if (data[n] != 0 || data[n + 1] != 0)
            continue;
        if (data[n + 2] == 1) {
            *offset = n;
            *length = 3;
            return true;
        }
        if (n + 4 <= size && data[n + 2] == 0 && data[n + 3] == 1) {
            *offset = n;
            *length = 4;
            return true;
        }
    }
    return false;
}

static bool annexb_next(struct annexb_iter *it, struct annexb_nal *nal)
{
    size_t start, start_len;
    if (!find_start_code(it->data, it->size, it->next, &start, &start_len))
        return false;

    if (it->next == 0) {
        for (size_t n = 0; n < start; n++) {
            if (it->data[n] != 0) {
                it->invalid = true;
                return false;
            }
        }
    }

    size_t data_start = start + start_len;
    size_t next_start, next_len;
    bool has_next = find_start_code(it->data, it->size, data_start,
                                    &next_start, &next_len);
    size_t data_end = has_next ? next_start : it->size;
    it->next = has_next ? next_start : it->size;

    if (data_end - data_start < 2) {
        it->invalid = true;
        return false;
    }

    nal->data = it->data + data_start;
    nal->size = data_end - data_start;
    nal->type = (nal->data[0] >> 1) & 0x3f;
    if ((nal->data[1] & 0x07) == 0) {
        it->invalid = true;
        return false;
    }
    return true;
}

static bool add_size(size_t *total, size_t value)
{
    if (value > INT_MAX || *total > INT_MAX - value)
        return false;
    *total += value;
    return true;
}

static bool measure_el(const AVPacket *el, size_t *wrapped_size,
                       unsigned *wrapped_nals, unsigned *rpu_nals)
{
    struct annexb_iter it = {el->data, el->size};
    struct annexb_nal nal;
    size_t total = 0;
    unsigned wrapped = 0;
    unsigned rpus = 0;

    while (annexb_next(&it, &nal)) {
        if (nal.type == HEVC_NAL_AUD)
            continue;
        if (!add_size(&total, 4 + nal.size +
                              (nal.type == HEVC_NAL_DOVI_RPU ? 0 : 2)))
            return false;
        if (nal.type == HEVC_NAL_DOVI_RPU)
            rpus++;
        else
            wrapped++;
    }

    if (it.invalid || it.next != it.size || !wrapped || !rpus)
        return false;

    *wrapped_size = total;
    *wrapped_nals = wrapped;
    *rpu_nals = rpus;
    return true;
}

static uint8_t *write_start_code(uint8_t *dst)
{
    *dst++ = 0;
    *dst++ = 0;
    *dst++ = 0;
    *dst++ = 1;
    return dst;
}

static uint8_t *write_el_pass(uint8_t *dst, const AVPacket *el, bool rpu_pass)
{
    struct annexb_iter it = {el->data, el->size};
    struct annexb_nal nal;

    while (annexb_next(&it, &nal)) {
        bool is_rpu = nal.type == HEVC_NAL_DOVI_RPU;
        if (nal.type == HEVC_NAL_AUD || is_rpu != rpu_pass)
            continue;

        dst = write_start_code(dst);
        if (!is_rpu) {
            // Dolby's single-track Profile 7 representation wraps every EL
            // NAL in an HEVC UNSPEC63 NAL. The embedded NAL keeps its header.
            *dst++ = 0x7e;
            *dst++ = 0x01;
        }
        memcpy(dst, nal.data, nal.size);
        dst += nal.size;
    }
    return dst;
}

static AVPacket *merge_packets(struct mp_dovi_merge *s, AVPacket *bl,
                               AVPacket *el)
{
    size_t extra = 0;
    unsigned wrapped_nals = 0;
    unsigned rpu_nals = 0;
    if (bl->size < 0 || el->size <= 0 ||
        !measure_el(el, &extra, &wrapped_nals, &rpu_nals) ||
        !add_size(&extra, bl->size))
    {
        if (!s->warned_malformed) {
            MP_WARN(s, "Dolby Vision M2TS: malformed or oversized EL access "
                       "unit; passing the corresponding base layer alone.\n");
            s->warned_malformed = true;
        }
        av_packet_free(&el);
        return bl;
    }

    AVPacket *out = av_packet_alloc();
    if (!out || av_new_packet(out, extra) < 0 ||
        av_packet_copy_props(out, bl) < 0)
    {
        MP_WARN(s, "Dolby Vision M2TS: unable to allocate a merged access "
                   "unit; passing the base layer alone.\n");
        av_packet_free(&out);
        av_packet_free(&el);
        return bl;
    }

    uint8_t *dst = out->data;
    memcpy(dst, bl->data, bl->size);
    dst += bl->size;
    dst = write_el_pass(dst, el, false);
    dst = write_el_pass(dst, el, true);
    if ((size_t)(dst - out->data) != extra) {
        // measure_el() and write_el_pass() share the same iterator. Reaching
        // this means an internal invariant was violated; preserve playback.
        MP_ERR(s, "Dolby Vision M2TS: internal merged-size mismatch.\n");
        av_packet_free(&out);
        av_packet_free(&el);
        return bl;
    }

    MP_TRACE(s, "Dolby Vision M2TS: merged BL=%d EL=%d (%u wrapped NALs, "
                "%u RPU NALs).\n",
             bl->size, el->size, wrapped_nals, rpu_nals);
    av_packet_free(&bl);
    av_packet_free(&el);
    return out;
}

static bool queue_push(struct packet_queue *q, AVPacket *pkt)
{
    struct packet_node *node = av_mallocz(sizeof(*node));
    if (!node)
        return false;
    node->pkt = pkt;
    if (q->tail)
        q->tail->next = node;
    else
        q->head = node;
    q->tail = node;
    q->count++;
    if (pkt->size > 0)
        q->bytes += pkt->size;
    return true;
}

static AVPacket *queue_pop(struct packet_queue *q)
{
    struct packet_node *node = q->head;
    if (!node)
        return NULL;
    q->head = node->next;
    if (!q->head)
        q->tail = NULL;
    q->count--;
    if (node->pkt->size > 0)
        q->bytes -= node->pkt->size;
    AVPacket *pkt = node->pkt;
    av_free(node);
    return pkt;
}

static void queue_clear(struct packet_queue *q)
{
    AVPacket *pkt;
    while ((pkt = queue_pop(q)))
        av_packet_free(&pkt);
}

static bool packet_times_match(struct mp_dovi_merge *s, const AVPacket *bl,
                               const AVPacket *el)
{
    if (bl->pts != AV_NOPTS_VALUE && el->pts != AV_NOPTS_VALUE)
        return av_compare_ts(bl->pts, s->bl_tb, el->pts, s->el_tb) == 0;
    if (bl->dts != AV_NOPTS_VALUE && el->dts != AV_NOPTS_VALUE)
        return av_compare_ts(bl->dts, s->bl_tb, el->dts, s->el_tb) == 0;
    return true;
}

static int packet_time_order(struct mp_dovi_merge *s, const AVPacket *bl,
                             const AVPacket *el)
{
    if (bl->dts != AV_NOPTS_VALUE && el->dts != AV_NOPTS_VALUE)
        return av_compare_ts(bl->dts, s->bl_tb, el->dts, s->el_tb);
    if (bl->pts != AV_NOPTS_VALUE && el->pts != AV_NOPTS_VALUE)
        return av_compare_ts(bl->pts, s->bl_tb, el->pts, s->el_tb);
    return 0;
}

static AVPacket *produce_packet(struct mp_dovi_merge *s)
{
    while (s->bl.head && s->el.head) {
        AVPacket *bl = s->bl.head->pkt;
        AVPacket *el = s->el.head->pkt;
        if (packet_times_match(s, bl, el)) {
            bl = queue_pop(&s->bl);
            el = queue_pop(&s->el);
            return merge_packets(s, bl, el);
        }

        int order = packet_time_order(s, bl, el);
        if (order < 0)
            return queue_pop(&s->bl);
        if (order > 0) {
            el = queue_pop(&s->el);
            av_packet_free(&el);
            continue;
        }

        // No comparable timestamps remain. Pair in FIFO order rather than
        // allowing an unbounded queue.
        bl = queue_pop(&s->bl);
        el = queue_pop(&s->el);
        return merge_packets(s, bl, el);
    }

    size_t total = s->bl.bytes + s->el.bytes;
    if (s->bl.count > DOVI_MERGE_MAX_PENDING ||
        (total > DOVI_MERGE_MAX_BYTES && s->bl.head))
    {
        if (!s->warned_pressure) {
            MP_WARN(s, "Dolby Vision M2TS: packet pairing queue limit "
                       "reached; passing an unmatched base layer.\n");
            s->warned_pressure = true;
        }
        return queue_pop(&s->bl);
    }

    while (s->el.count > DOVI_MERGE_MAX_PENDING ||
           s->bl.bytes + s->el.bytes > DOVI_MERGE_MAX_BYTES)
    {
        AVPacket *el = queue_pop(&s->el);
        if (!el)
            break;
        av_packet_free(&el);
    }
    return NULL;
}

static void merge_destructor(void *ptr)
{
    mp_dovi_merge_reset(ptr);
}

struct mp_dovi_merge *mp_dovi_merge_create(void *ta_parent, struct mp_log *log,
                                           int bl_stream, int el_stream,
                                           AVRational bl_tb, AVRational el_tb)
{
    if (bl_stream < 0 || el_stream < 0 || bl_stream == el_stream ||
        bl_tb.num <= 0 || bl_tb.den <= 0 || el_tb.num <= 0 || el_tb.den <= 0)
        return NULL;

    struct mp_dovi_merge *s = talloc_zero(ta_parent, struct mp_dovi_merge);
    if (!s)
        return NULL;
    talloc_set_destructor(s, merge_destructor);
    s->log = log;
    s->bl_stream = bl_stream;
    s->el_stream = el_stream;
    s->bl_tb = bl_tb;
    s->el_tb = el_tb;
    return s;
}

void mp_dovi_merge_reset(struct mp_dovi_merge *s)
{
    if (!s)
        return;
    queue_clear(&s->bl);
    queue_clear(&s->el);
}

int mp_dovi_merge_bl_stream(const struct mp_dovi_merge *s)
{
    return s ? s->bl_stream : -1;
}

int mp_dovi_merge_el_stream(const struct mp_dovi_merge *s)
{
    return s ? s->el_stream : -1;
}

AVPacket *mp_dovi_merge_push(struct mp_dovi_merge *s, AVPacket *pkt)
{
    if (!s || !pkt)
        return pkt;

    struct packet_queue *q = NULL;
    bool is_bl = pkt->stream_index == s->bl_stream;
    if (is_bl)
        q = &s->bl;
    else if (pkt->stream_index == s->el_stream)
        q = &s->el;
    else
        return pkt;

    if (!queue_push(q, pkt)) {
        MP_WARN(s, "Dolby Vision M2TS: unable to queue an access unit.\n");
        if (is_bl)
            return pkt;
        av_packet_free(&pkt);
        return NULL;
    }
    return produce_packet(s);
}

AVPacket *mp_dovi_merge_drain(struct mp_dovi_merge *s)
{
    if (!s)
        return NULL;

    AVPacket *pkt = produce_packet(s);
    if (pkt)
        return pkt;
    pkt = queue_pop(&s->bl);
    if (pkt)
        return pkt;
    queue_clear(&s->el);
    return NULL;
}
