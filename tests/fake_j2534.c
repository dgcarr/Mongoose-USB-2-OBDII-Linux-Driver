/* A stand-in for libmongoose_j2534, linked into a test build of mongoose-socketcan only. It plays an ECU on the
 * "vehicle" side: it delivers three frames once (11-bit, 29-bit, no data), answers a mode 01 PID 00 request to 0x7DF
 * with a reply from 0x7E8, plays the engine ECU at 0x7E0/0x7E8 for the requests docs/VOLVO.md shows (ISO-TP, with
 * a multi-frame reply continuing only once the host's flow-control frame comes back through the bridge), and appends every frame written to it to the file named by FAKE_J2534_TX_LOG. */
#include "mongoose/j2534.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static PASSTHRU_MSG pending[16];
static unsigned pending_count;
static int filtered;
static void queue(uint32_t id, uint32_t status, const uint8_t *data, uint32_t size) {
    if (pending_count == 16) return;
    PASSTHRU_MSG *m = &pending[pending_count++];
    memset(m, 0, sizeof *m);
    m->ProtocolID = CAN; m->RxStatus = status;
    m->Data[0] = (uint8_t)(id >> 24); m->Data[1] = (uint8_t)(id >> 16); m->Data[2] = (uint8_t)(id >> 8); m->Data[3] = (uint8_t)id;
    memcpy(&m->Data[4], data, size);
    m->DataSize = 4 + size;
}
static uint8_t segmented[64];  /* the rest of a multi-frame reply, sent when flow control arrives */
static size_t segmented_size, segmented_sent;

static void reply_isotp(const uint8_t *payload, size_t size) {
    uint8_t frame[8] = {0};
    if (size <= 7) {
        frame[0] = (uint8_t)size; memcpy(&frame[1], payload, size);
        queue(0x7e8, 0, frame, 8);
        return;
    }
    frame[0] = 0x10; frame[1] = (uint8_t)size; memcpy(&frame[2], payload, 6);
    queue(0x7e8, 0, frame, 8);
    memcpy(segmented, payload, size); segmented_size = size; segmented_sent = 6;
}
/* One padded ISO-TP frame from the host to 0x7E0: a single-frame request or a flow control. */
static void answer_engine(const uint8_t *frame) {
    if ((frame[0] & 0xf0) == 0x30) {
        for (uint8_t index = 1; segmented_sent < segmented_size; ++index) {
            uint8_t consecutive[8] = {(uint8_t)(0x20 | (index & 15))};
            const size_t chunk = segmented_size - segmented_sent < 7 ? segmented_size - segmented_sent : 7;
            memcpy(&consecutive[1], &segmented[segmented_sent], chunk);
            queue(0x7e8, 0, consecutive, 8);
            segmented_sent += chunk;
        }
        return;
    }
    static const struct { uint8_t request[3], request_size; const char *reply; uint8_t reply_size; } table[] = {
        {{0x01, 0x0c}, 2, "\x41\x0c\x0b\xb8", 4},                          /* 750 rpm */
        {{0x01, 0x05}, 2, "\x41\x05\x7b", 3},                               /* 83 C */
        {{0x09, 0x02}, 2, "\x49\x02\x01YV1TESTVIN0000000", 20},
        {{0x03}, 1, "\x43\x02\x01\x71\x44\x20", 6},                         /* P0171, C0420 */
        {{0x22, 0xf1, 0x90}, 3, "\x62\xf1\x90YV1TESTVIN0000000", 20},
    };
    for (size_t i = 0; i < sizeof table / sizeof table[0]; ++i)
        if (frame[0] == table[i].request_size && !memcmp(&frame[1], table[i].request, table[i].request_size))
            reply_isotp((const uint8_t *)table[i].reply, table[i].reply_size);
}

int32_t PassThruOpen(void *name, uint32_t *device) { (void)name; *device = 1; return 0; }
int32_t PassThruClose(uint32_t device) { (void)device; return 0; }
int32_t PassThruConnect(uint32_t device, uint32_t protocol, uint32_t flags, uint32_t baud, uint32_t *channel) {
    (void)device; (void)flags;
    if (protocol != CAN || baud != 500000) return ERR_INVALID_BAUDRATE;
    *channel = 2;
    return 0;
}
int32_t PassThruDisconnect(uint32_t channel) { (void)channel; return 0; }
int32_t PassThruStartMsgFilter(uint32_t channel, uint32_t type, PASSTHRU_MSG *mask, PASSTHRU_MSG *pattern,
                               PASSTHRU_MSG *flow, uint32_t *filter) {
    (void)channel; (void)flow;
    if (type != PASS_FILTER || mask->DataSize != 4 || pattern->DataSize != 4) return ERR_INVALID_MSG;
    pthread_mutex_lock(&lock);
    filtered = 1;
    static const uint8_t engine[8] = {0x03, 0x41, 0x0c, 0x1a, 0xf8, 0, 0, 0}, body[3] = {0xde, 0xad, 0x01};
    queue(0x7e8, 0, engine, 8);
    queue(0x18daf110, CAN_29BIT_ID, body, 3);
    queue(0x123, 0, NULL, 0);
    pthread_mutex_unlock(&lock);
    *filter = 3;
    return 0;
}
int32_t PassThruStopMsgFilter(uint32_t channel, uint32_t filter) { (void)channel; (void)filter; return 0; }
int32_t PassThruReadMsgs(uint32_t channel, PASSTHRU_MSG *messages, uint32_t *count, uint32_t timeout) {
    (void)channel;
    const uint32_t wanted = *count;
    *count = 0;
    for (;;) {
        pthread_mutex_lock(&lock);
        while (*count < wanted && pending_count) {
            messages[(*count)++] = pending[0];
            memmove(pending, pending + 1, --pending_count * sizeof pending[0]);
        }
        pthread_mutex_unlock(&lock);
        if (*count || !timeout) break;
        nanosleep(&(struct timespec){0, 10000000}, NULL);
        timeout = timeout > 10 ? timeout - 10 : 0;
    }
    return *count ? (*count < wanted && timeout ? ERR_TIMEOUT : 0) : ERR_BUFFER_EMPTY;
}
int32_t PassThruWriteMsgs(uint32_t channel, PASSTHRU_MSG *messages, uint32_t *count, uint32_t timeout) {
    (void)channel; (void)timeout;
    const char *path = getenv("FAKE_J2534_TX_LOG");
    FILE *log = path ? fopen(path, "a") : NULL;
    pthread_mutex_lock(&lock);
    for (uint32_t i = 0; i < *count; ++i) {
        const PASSTHRU_MSG *m = &messages[i];
        if (log) {
            fprintf(log, "%s %02x%02x%02x%02x", m->TxFlags & CAN_29BIT_ID ? "ext" : "std", m->Data[0], m->Data[1], m->Data[2], m->Data[3]);
            for (uint32_t b = 4; b < m->DataSize; ++b) fprintf(log, " %02x", m->Data[b]);
            fprintf(log, "\n");
        }
        static const uint8_t request[4] = {0x00, 0x00, 0x07, 0xdf}, reply[8] = {0x06, 0x41, 0x00, 0xbe, 0x3e, 0xb8, 0x11, 0};
        if (filtered && m->DataSize >= 7 && !memcmp(m->Data, request, 4) && m->Data[4] == 0x02 && m->Data[5] == 0x01 && m->Data[6] == 0x00)
            queue(0x7e8, 0, reply, 8);
        if (filtered && m->DataSize == 12 && m->Data[2] == 0x07 && m->Data[3] == 0xe0) answer_engine(&m->Data[4]);
    }
    pthread_mutex_unlock(&lock);
    if (log) fclose(log);
    return 0;
}
int32_t PassThruGetLastError(char *text) { strcpy(text, "fake"); return 0; }
