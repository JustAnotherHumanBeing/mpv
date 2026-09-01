/*
 * This file is part of mpv.
 *
 * mpv is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <stdint.h>
#include <string.h>

#include <libavcodec/packet.h>
#include <libavutil/avutil.h>

#include "demux/dovi_merge.h"
#include "mpv_talloc.h"
#include "test_utils.h"

static AVPacket *make_packet(int stream, int64_t pts,
                             const uint8_t *data, size_t size)
{
    AVPacket *pkt = av_packet_alloc();
    assert_true(pkt);
    assert_int_equal(av_new_packet(pkt, size), 0);
    memcpy(pkt->data, data, size);
    pkt->stream_index = stream;
    pkt->pts = pts;
    pkt->dts = pts;
    pkt->duration = 1001;
    pkt->pos = 1234;
    pkt->flags = AV_PKT_FLAG_KEY;
    return pkt;
}

static void test_wrapping(void *ta_ctx)
{
    static const uint8_t bl_data[] = {
        0x00, 0x00, 0x00, 0x01, 0x26, 0x01, 0xaa,
    };
    static const uint8_t el_data[] = {
        0x00, 0x00, 0x00, 0x01, 0x46, 0x01, 0x10,
        0x00, 0x00, 0x01, 0x40, 0x01, 0xaa, 0xbb,
        0x00, 0x00, 0x01, 0x7c, 0x01, 0xcc,
    };
    static const uint8_t expected[] = {
        0x00, 0x00, 0x00, 0x01, 0x26, 0x01, 0xaa,
        0x00, 0x00, 0x00, 0x01, 0x7e, 0x01, 0x40, 0x01, 0xaa, 0xbb,
        0x00, 0x00, 0x00, 0x01, 0x7c, 0x01, 0xcc,
    };

    struct mp_dovi_merge *m =
        mp_dovi_merge_create(ta_ctx, NULL, 0, 1,
                             (AVRational){1, 90000},
                             (AVRational){1, 90000});
    assert_true(m);
    assert_true(!mp_dovi_merge_push(m,
        make_packet(0, 90000, bl_data, sizeof(bl_data))));
    AVPacket *out = mp_dovi_merge_push(m,
        make_packet(1, 90000, el_data, sizeof(el_data)));
    assert_true(out);
    assert_int_equal(out->size, sizeof(expected));
    assert_memcmp(out->data, expected, sizeof(expected));
    assert_int_equal(out->stream_index, 0);
    assert_int_equal(out->pts, 90000);
    assert_int_equal(out->dts, 90000);
    assert_int_equal(out->duration, 1001);
    assert_int_equal(out->pos, 1234);
    assert_true(out->flags & AV_PKT_FLAG_KEY);
    av_packet_free(&out);
    assert_true(!mp_dovi_merge_drain(m));
}

static void test_reverse_and_missing_timestamps(void *ta_ctx)
{
    static const uint8_t bl_data[] = {
        0x00, 0x00, 0x01, 0x02, 0x01, 0x80,
    };
    static const uint8_t el_data[] = {
        0x00, 0x00, 0x01, 0x02, 0x01, 0x80,
        0x00, 0x00, 0x01, 0x7c, 0x01, 0x01,
    };
    struct mp_dovi_merge *m =
        mp_dovi_merge_create(ta_ctx, NULL, 4, 9,
                             (AVRational){1, 1000},
                             (AVRational){1, 1000});
    assert_true(m);

    AVPacket *el = make_packet(9, AV_NOPTS_VALUE, el_data, sizeof(el_data));
    AVPacket *bl = make_packet(4, AV_NOPTS_VALUE, bl_data, sizeof(bl_data));
    assert_true(!mp_dovi_merge_push(m, el));
    AVPacket *out = mp_dovi_merge_push(m, bl);
    assert_true(out);
    assert_int_equal(out->stream_index, 4);
    assert_int_equal(out->pts, AV_NOPTS_VALUE);
    av_packet_free(&out);
}

static void test_mismatch_and_malformed_fallback(void *ta_ctx)
{
    static const uint8_t bl_data[] = {
        0x00, 0x00, 0x01, 0x02, 0x01, 0x80,
    };
    static const uint8_t el_data[] = {
        0x00, 0x00, 0x01, 0x02, 0x01, 0x80,
        0x00, 0x00, 0x01, 0x7c, 0x01, 0x01,
    };
    static const uint8_t malformed[] = {0x02, 0x01, 0x80};
    struct mp_dovi_merge *m =
        mp_dovi_merge_create(ta_ctx, NULL, 0, 1,
                             (AVRational){1, 1}, (AVRational){1, 1});
    assert_true(m);

    assert_true(!mp_dovi_merge_push(m,
        make_packet(0, 10, bl_data, sizeof(bl_data))));
    AVPacket *out = mp_dovi_merge_push(m,
        make_packet(1, 20, el_data, sizeof(el_data)));
    assert_true(out);
    assert_int_equal(out->pts, 10);
    assert_int_equal(out->size, sizeof(bl_data));
    assert_memcmp(out->data, bl_data, sizeof(bl_data));
    av_packet_free(&out);
    assert_true(!mp_dovi_merge_drain(m));

    assert_true(!mp_dovi_merge_push(m,
        make_packet(0, 30, bl_data, sizeof(bl_data))));
    out = mp_dovi_merge_push(m,
        make_packet(1, 30, malformed, sizeof(malformed)));
    assert_true(out);
    assert_int_equal(out->size, sizeof(bl_data));
    assert_memcmp(out->data, bl_data, sizeof(bl_data));
    av_packet_free(&out);

    assert_true(!mp_dovi_merge_push(m,
        make_packet(0, 40, bl_data, sizeof(bl_data))));
    mp_dovi_merge_reset(m);
    assert_true(!mp_dovi_merge_drain(m));
}

int main(void)
{
    void *ta_ctx = talloc_new(NULL);
    test_wrapping(ta_ctx);
    test_reverse_and_missing_timestamps(ta_ctx);
    test_mismatch_and_malformed_fallback(ta_ctx);
    talloc_free(ta_ctx);
    return 0;
}
