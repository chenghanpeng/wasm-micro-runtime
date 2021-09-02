/*
 * Copyright (C) 2021 Ant Group.  All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

#include "packets.h"

#include <stdbool.h>

#include "bh_log.h"
#include "gdbserver.h"

void
pktbuf_insert(WASMGDBServer *gdbserver, const uint8_t *buf, ssize_t len)
{
    WasmDebugPacket *pkt = &gdbserver->pkt;
    if ((unsigned long)(pkt->end + len) >= sizeof(pkt->buf)) {
        LOG_ERROR("Packet buffer overflow");
        exit(-2);
    }
    memcpy(pkt->buf + pkt->end, buf, len);
    pkt->end += len;
}

void
pktbuf_erase_head(WASMGDBServer *gdbserver, ssize_t end)
{
    WasmDebugPacket *pkt = &gdbserver->pkt;
    memmove(pkt->buf, pkt->buf + end, pkt->end - end);
    pkt->end -= end;
}

void
inbuf_erase_head(WASMGDBServer *gdbserver, ssize_t end)
{
    pktbuf_erase_head(gdbserver, end);
}

void
pktbuf_clear(WASMGDBServer *gdbserver)
{
    WasmDebugPacket *pkt = &gdbserver->pkt;
    pkt->end = 0;
}

int
read_data_once(WASMGDBServer *gdbserver)
{
    ssize_t nread;
    uint8_t buf[4096];
    nread = read(gdbserver->socket_fd, buf, sizeof(buf));
    if (nread <= 0) {
        LOG_ERROR("Connection closed");
        return -1;
    }
    pktbuf_insert(gdbserver, buf, nread);
    return nread;
}

void
write_data_raw(WASMGDBServer *gdbserver, const uint8_t *data, ssize_t len)
{
    ssize_t nwritten;
    nwritten = write(gdbserver->socket_fd, data, len);
    if (nwritten < 0) {
        LOG_ERROR("Write error\n");
        exit(-2);
    }
}

void
write_hex(WASMGDBServer *gdbserver, unsigned long hex)
{
    char buf[32];
    size_t len;

    len = snprintf(buf, sizeof(buf) - 1, "%02lx", hex);
    write_data_raw(gdbserver, (uint8_t *)buf, len);
}

void
write_packet_bytes(WASMGDBServer *gdbserver,
                   const uint8_t *data,
                   size_t num_bytes)
{
    uint8_t checksum;
    size_t i;

    write_data_raw(gdbserver, (uint8_t *)"$", 1);
    for (i = 0, checksum = 0; i < num_bytes; ++i)
        checksum += data[i];
    write_data_raw(gdbserver, (uint8_t *)data, num_bytes);
    write_data_raw(gdbserver, (uint8_t *)"#", 1);
    write_hex(gdbserver, checksum);
}

void
write_packet(WASMGDBServer *gdbserver, const char *data)
{
    LOG_VERBOSE("send replay:%s", data);
    write_packet_bytes(gdbserver, (const uint8_t *)data, strlen(data));
}

void
write_binary_packet(WASMGDBServer *gdbserver,
                    const char *pfx,
                    const uint8_t *data,
                    ssize_t num_bytes)
{
    uint8_t *buf;
    ssize_t pfx_num_chars = strlen(pfx);
    ssize_t buf_num_bytes = 0;
    int i;

    buf = malloc(2 * num_bytes + pfx_num_chars);
    memcpy(buf, pfx, pfx_num_chars);
    buf_num_bytes += pfx_num_chars;

    for (i = 0; i < num_bytes; ++i) {
        uint8_t b = data[i];
        switch (b) {
            case '#':
            case '$':
            case '}':
            case '*':
                buf[buf_num_bytes++] = '}';
                buf[buf_num_bytes++] = b ^ 0x20;
                break;
            default:
                buf[buf_num_bytes++] = b;
                break;
        }
    }
    write_packet_bytes(gdbserver, buf, buf_num_bytes);
    free(buf);
}

bool
skip_to_packet_start(WASMGDBServer *gdbserver)
{
    ssize_t end = -1;
    for (size_t i = 0; i < gdbserver->pkt.end; ++i)
        if (gdbserver->pkt.buf[i] == '$') {
            end = i;
            break;
        }

    if (end < 0) {
        pktbuf_clear(gdbserver);
        return false;
    }

    pktbuf_erase_head(gdbserver, end);
    bh_assert(1 <= gdbserver->pkt.end);
    bh_assert('$' == gdbserver->pkt.buf[0]);
    return true;
}

bool
read_packet(WASMGDBServer *gdbserver)
{
    while (!skip_to_packet_start(gdbserver)) {
        if(read_data_once(gdbserver) < 0)
            return false;
    }
    if (!gdbserver->noack)
        write_data_raw(gdbserver, (uint8_t *)"+", 1);
    return true;
}