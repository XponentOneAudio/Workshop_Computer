/*
WorkshopLink.h - Workshop Link protocol, version 1

A small SysEx protocol that lets a Workshop Computer card (the USB host) put a
live readout on the screen of a USB-MIDI instrument with a display (the USB
device), and receive that instrument's encoders and buttons back. The first
implementation is the M-VAVE FM1 (fm1_open firmware); the protocol is not
FM1-specific, so other firmwares with a screen can implement it too.

This file is shared, byte for byte, by both ends of the link:
  - Workshop_Computer: Demonstrations+HelloWorlds/PicoSDK/ComputerCard/WorkshopLink.h
  - fm1_open:          app/WorkshopLink.h
Keep the copies identical. It is plain C99, has no dependencies beyond
<stdint.h>/<stddef.h>, does no allocation, and is safe to include from C or C++.

Frame layout
------------
    F0 7D 'W' 'L' <version> <command> <sequence> <packed payload...> <checksum> F7

  - 0x7D is the MIDI "non-commercial / educational" manufacturer ID. The two
    ID bytes 'W' 'L' keep this protocol apart from other users of 0x7D (the
    FM1's own 'F' 'M' transfer protocol and the 8mu's 00 00 messages).
  - <version> is WL_VERSION. A receiver ignores frames of another version.
  - <sequence> is a free-running 7-bit counter. Receivers may use it to spot
    loss; nothing is retransmitted.
  - The payload is up to WL_MAX_PAYLOAD 8-bit bytes, packed 7 bits per MIDI
    data byte in groups of up to seven: one byte holding the top bits
    (bit i = top bit of byte i of the group), then the low seven bits of each
    byte of the group.
  - <checksum> makes the sum of every byte from 7D through the checksum
    itself zero modulo 128 (the same rule as the FM1 transfer protocol).

The largest frame is WL_MAX_FRAME (119) bytes, which fits the 128-byte
outgoing-message limit of the FM1 SDK, so either end can send any frame.

Session
-------
  host -> device  HELLO       Start (or restart) a session. Clears the layout.
  device -> host  HELLO_ACK   Device capabilities. The host then sends LAYOUT
                              and one WIDGET per widget, then live updates.
  host -> device  PING        At least once a second when nothing else is sent.
                              The device treats the link as gone after
                              WL_TIMEOUT_MS without a frame.
  device -> host  NEED_LAYOUT The device got an update without a session
                              (e.g. it restarted). The host sends HELLO again.

All multi-byte numbers in payloads are little-endian. Strings are a length
byte followed by that many ASCII bytes (no terminator). Coordinates are
absolute screen pixels; the device clips widgets to its usable area.

MIT License

Copyright (c) 2026 Workshop Link contributors

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/
#ifndef WORKSHOP_LINK_H
#define WORKSHOP_LINK_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WL_VERSION 1U
#define WL_MANUFACTURER 0x7DU
#define WL_ID0 0x57U /* 'W' */
#define WL_ID1 0x4CU /* 'L' */

#define WL_HEADER_BYTES 7U /* F0 7D W L version command sequence */
#define WL_MAX_PAYLOAD 96U
#define WL_PACKED_BYTES(n) ((n) + ((n) + 6U) / 7U)
#define WL_MAX_FRAME (WL_HEADER_BYTES + WL_PACKED_BYTES(WL_MAX_PAYLOAD) + 2U)

#define WL_TIMEOUT_MS 3000U
#define WL_PING_MS 1000U

/* Limits a host may rely on for version 1 devices. A device may report
 * smaller numbers in HELLO_ACK; it never needs to accept more. */
#define WL_MAX_WIDGETS 16U
#define WL_MAX_LABEL 15U       /* widget labels */
#define WL_MAX_TEXT 23U        /* TEXT widget contents and the page title */
#define WL_MAX_NAME 16U        /* HELLO / HELLO_ACK names */
#define WL_MAX_SCOPES 2U       /* SCOPE widgets per layout */
#define WL_MAX_SCOPE_POINTS 120U
#define WL_MAX_VALUES_PER_FRAME 32U /* 3 bytes each */
#define WL_MAX_SCOPE_PER_FRAME 90U

/* Commands. Host -> device use 0x00-0x3F, device -> host 0x40-0x7F.
 * 0x20-0x3F and 0x60-0x7F are reserved for card/firmware extensions and
 * are never used by this protocol itself. */
enum wl_command {
    /* host -> device */
    WL_HELLO = 0x01,       /* u8 version, u8 flags (WL_HELLO_*), str name */
    WL_PING = 0x02,        /* empty */
    WL_BYE = 0x03,         /* empty: the host is going away */
    WL_LAYOUT = 0x10,      /* str title. Removes every widget. */
    WL_WIDGET = 0x11,      /* u8 id, type, x, y, w, h, color; s16 min, max; str label */
    WL_VALUES = 0x12,      /* repeated { u8 id; s16 value } */
    WL_TEXT = 0x13,        /* u8 id, str text */
    WL_SCOPE = 0x14,       /* u8 id, u8 offset, u8 count, s8 samples[count] */
    WL_SHOW = 0x15,        /* u8 show: 1 asks the device to open its link page */
    WL_HOST_USER_FIRST = 0x20,
    WL_HOST_USER_LAST = 0x3F,
    /* device -> host */
    WL_HELLO_ACK = 0x41,   /* u8 version, caps (WL_CAP_*), screen w, screen h,
                              max widgets, max scope points; str name */
    WL_EVENT = 0x42,       /* u8 visible, u16 buttons, s8 encoder deltas[7] */
    WL_NEED_LAYOUT = 0x43, /* empty */
    WL_DEVICE_USER_FIRST = 0x60,
    WL_DEVICE_USER_LAST = 0x7F
};

enum wl_hello_flags {
    WL_HELLO_SHOW = 1U /* open the link page now, if the device allows it */
};

enum wl_caps {
    WL_CAP_SCREEN = 1U,
    WL_CAP_ENCODERS = 2U,
    WL_CAP_BUTTONS = 4U
};

/* Widget types. Every widget has a label; what else it shows: */
enum wl_widget_type {
    WL_W_LABEL = 0, /* only its label */
    WL_W_VALUE = 1, /* label and a number */
    WL_W_BAR = 2,   /* label, number and a horizontal bar from min to max */
    WL_W_LED = 3,   /* label and an indicator, lit when value != 0 */
    WL_W_TEXT = 4,  /* label and a text string set by WL_TEXT */
    WL_W_SCOPE = 5, /* a trace of signed 8-bit points set by WL_SCOPE */
    WL_W_COUNT
};

/* Colour roles. Each device maps them onto its own palette. */
enum wl_color {
    WL_C_TEXT = 0, WL_C_ACCENT, WL_C_DIM, WL_C_GREEN, WL_C_AMBER,
    WL_C_RED, WL_C_BLUE, WL_C_VIOLET, WL_C_COUNT
};

/* Error codes returned by wl_decode. */
enum wl_error {
    WL_E_NOT_LINK = -1, /* not a Workshop Link frame (someone else's SysEx) */
    WL_E_VERSION = -2,
    WL_E_FORMAT = -3,   /* truncated, bad framing or bad packing */
    WL_E_CHECKSUM = -4,
    WL_E_SPACE = -5     /* payload larger than the caller's buffer */
};

/* True when a complete SysEx message belongs to this protocol (any version). */
static inline int wl_is_link_frame(const uint8_t *m, size_t n)
{
    return m && n >= 9U && m[0] == 0xF0U && m[1] == WL_MANUFACTURER &&
           m[2] == WL_ID0 && m[3] == WL_ID1 && m[n - 1U] == 0xF7U;
}

/* Build a frame. Returns its length, or 0 if the arguments are invalid or
 * the frame would not fit in out_capacity. */
static inline size_t wl_encode(uint8_t *out, size_t out_capacity, unsigned command,
                               unsigned sequence, const uint8_t *payload, size_t bytes)
{
    if (!out || command > 0x7FU || bytes > WL_MAX_PAYLOAD || (bytes && !payload))
        return 0;
    size_t total = WL_HEADER_BYTES + WL_PACKED_BYTES(bytes) + 2U;
    if (total > out_capacity)
        return 0;
    size_t at = 0;
    out[at++] = 0xF0U;
    out[at++] = WL_MANUFACTURER;
    out[at++] = WL_ID0;
    out[at++] = WL_ID1;
    out[at++] = WL_VERSION;
    out[at++] = (uint8_t)command;
    out[at++] = (uint8_t)(sequence & 0x7FU);
    for (size_t group = 0; group < bytes; group += 7U) {
        size_t count = bytes - group < 7U ? bytes - group : 7U;
        uint8_t high = 0;
        for (size_t i = 0; i < count; ++i)
            high = (uint8_t)(high | ((payload[group + i] >> 7) << i));
        out[at++] = high;
        for (size_t i = 0; i < count; ++i)
            out[at++] = (uint8_t)(payload[group + i] & 0x7FU);
    }
    unsigned sum = 0;
    for (size_t i = 1; i < at; ++i)
        sum += out[i];
    out[at++] = (uint8_t)((128U - (sum & 0x7FU)) & 0x7FU);
    out[at++] = 0xF7U;
    return at;
}

/* Check and unpack a complete SysEx frame. Returns the payload length
 * (0..WL_MAX_PAYLOAD) or a negative wl_error. command/sequence may be NULL. */
static inline int wl_decode(const uint8_t *m, size_t n, uint8_t *command, uint8_t *sequence,
                            uint8_t *payload, size_t capacity)
{
    if (!wl_is_link_frame(m, n))
        return WL_E_NOT_LINK;
    if (m[4] != WL_VERSION)
        return WL_E_VERSION;
    if (n > WL_MAX_FRAME)
        return WL_E_FORMAT;
    unsigned sum = 0;
    for (size_t i = 1; i < n - 1U; ++i) {
        if (m[i] & 0x80U)
            return WL_E_FORMAT;
        sum += m[i];
    }
    if (sum & 0x7FU)
        return WL_E_CHECKSUM;
    size_t packed = n - WL_HEADER_BYTES - 2U, at = WL_HEADER_BYTES, out = 0;
    const size_t end = WL_HEADER_BYTES + packed;
    while (at < end) {
        size_t count = end - at - 1U;
        if (count == 0)
            return WL_E_FORMAT; /* a group header with no bytes */
        if (count > 7U)
            count = 7U;
        uint8_t high = m[at++];
        if (high >> count)
            return WL_E_FORMAT; /* top bits set for bytes that are absent */
        if (out + count > capacity || out + count > WL_MAX_PAYLOAD)
            return WL_E_SPACE;
        for (size_t i = 0; i < count; ++i)
            payload[out++] = (uint8_t)(m[at++] | (((high >> i) & 1U) << 7));
    }
    if (command)
        *command = m[5];
    if (sequence)
        *sequence = m[6];
    return (int)out;
}

/* Payload builder. Any overflow sets ok = 0 and further writes are ignored,
 * so a sequence of puts needs one check at the end. */
struct wl_writer {
    uint8_t *data;
    size_t length, capacity;
    int ok;
};
static inline void wl_writer_init(struct wl_writer *w, uint8_t *data, size_t capacity)
{
    w->data = data; w->length = 0; w->capacity = capacity; w->ok = 1;
}
static inline void wl_put8(struct wl_writer *w, unsigned v)
{
    if (!w->ok || w->length >= w->capacity) { w->ok = 0; return; }
    w->data[w->length++] = (uint8_t)v;
}
static inline void wl_put16(struct wl_writer *w, int v)
{
    wl_put8(w, (unsigned)v & 0xFFU);
    wl_put8(w, ((unsigned)v >> 8) & 0xFFU);
}
/* Strings longer than max_chars are truncated, not rejected. */
static inline void wl_put_str(struct wl_writer *w, const char *s, size_t max_chars)
{
    size_t n = 0;
    while (s && s[n] && n < max_chars)
        ++n;
    wl_put8(w, (unsigned)n);
    for (size_t i = 0; i < n; ++i)
        wl_put8(w, (uint8_t)s[i]);
}

/* Payload reader. Reading past the end sets ok = 0 and returns zeros. */
struct wl_reader {
    const uint8_t *data;
    size_t length, at;
    int ok;
};
static inline void wl_reader_init(struct wl_reader *r, const uint8_t *data, size_t length)
{
    r->data = data; r->length = length; r->at = 0; r->ok = 1;
}
static inline size_t wl_remaining(const struct wl_reader *r)
{
    return r->ok ? r->length - r->at : 0;
}
static inline unsigned wl_get8(struct wl_reader *r)
{
    if (!r->ok || r->at >= r->length) { r->ok = 0; return 0; }
    return r->data[r->at++];
}
static inline int wl_get16(struct wl_reader *r)
{
    unsigned lo = wl_get8(r), hi = wl_get8(r);
    return (int)(int16_t)(uint16_t)(lo | (hi << 8));
}
/* Copies at most capacity-1 characters and always terminates the output.
 * Non-printable characters become '?'. */
static inline void wl_get_str(struct wl_reader *r, char *out, size_t capacity)
{
    size_t n = wl_get8(r), kept = 0;
    for (size_t i = 0; i < n; ++i) {
        unsigned c = wl_get8(r);
        if (kept + 1U < capacity)
            out[kept++] = (char)(c >= 0x20U && c < 0x7FU ? c : '?');
    }
    if (capacity)
        out[kept] = 0;
}

#ifdef __cplusplus
}
#endif

#endif /* WORKSHOP_LINK_H */
