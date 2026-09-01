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

#pragma once

#include <libavutil/rational.h>

struct AVPacket;
struct mp_log;

struct mp_dovi_merge;

// Create an access-unit merger for a separate Dolby Vision base layer and
// enhancement layer. The returned object is attached to ta_parent.
struct mp_dovi_merge *mp_dovi_merge_create(void *ta_parent, struct mp_log *log,
                                           int bl_stream, int el_stream,
                                           AVRational bl_tb, AVRational el_tb);

void mp_dovi_merge_reset(struct mp_dovi_merge *s);

int mp_dovi_merge_bl_stream(const struct mp_dovi_merge *s);
int mp_dovi_merge_el_stream(const struct mp_dovi_merge *s);

// Takes ownership of pkt. Returns an output packet owned by the caller when a
// pair is complete or a base-layer packet must be released without an EL.
// Returns NULL while waiting for the matching packet.
struct AVPacket *mp_dovi_merge_push(struct mp_dovi_merge *s,
                                    struct AVPacket *pkt);

// Release one queued base-layer packet at EOF. Enhancement-only packets are
// discarded once no base-layer packets remain.
struct AVPacket *mp_dovi_merge_drain(struct mp_dovi_merge *s);
