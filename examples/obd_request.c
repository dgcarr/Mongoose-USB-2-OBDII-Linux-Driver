/* Reads the supported-PID bitmask from every ECU that answers, over ISO15765, using only the J2534 API:
 *   obd_request [serial:SERIAL]
 * It sends one read-only OBD-II request (mode 01, PID 00) to the functional address 0x7DF and prints each reply.
 * The adapter must be on a vehicle at 500 kbit with the ignition on. See docs/USING.md. */
#include "mongoose/j2534.h"
#include <stdio.h>
#include <string.h>

static void fail(const char *what, int32_t status) {
    char text[80] = {0};
    PassThruGetLastError(text);
    fprintf(stderr, "%s failed: status %d: %s\n", what, status, text);
}
/* An ISO15765 message is the four-byte big-endian CAN ID followed by the service bytes. */
static void message(PASSTHRU_MSG *m, uint32_t id, const uint8_t *data, uint32_t size) {
    memset(m, 0, sizeof(*m));
    m->ProtocolID = ISO15765;
    m->TxFlags = ISO15765_FRAME_PAD;  /* required by the ECUs this was tested against: they ignore short frames */
    m->Data[0] = (uint8_t)(id >> 24); m->Data[1] = (uint8_t)(id >> 16); m->Data[2] = (uint8_t)(id >> 8); m->Data[3] = (uint8_t)id;
    if (size) memcpy(&m->Data[4], data, size);
    m->DataSize = 4 + size;
}
int main(int argc, char **argv) {
    uint32_t device = 0, channel = 0, filter = 0;
    int32_t status = PassThruOpen(argc > 1 ? argv[1] : NULL, &device);
    if (status) { fail("PassThruOpen", status); return 1; }
    int result = 1;
    if ((status = PassThruConnect(device, ISO15765, 0, 500000, &channel))) { fail("PassThruConnect", status); goto close_device; }
    /* A flow-control filter tells the adapter which ECU replies to answer with flow control, and where to send it.
     * The mask covers the whole 11-bit ID. Add one per ECU; 0x7E8/0x7E0 is the usual engine ECU pair. */
    PASSTHRU_MSG mask, pattern, flow;
    message(&mask, 0xffffffffu, NULL, 0); message(&pattern, 0x7e8, NULL, 0); message(&flow, 0x7e0, NULL, 0);
    if ((status = PassThruStartMsgFilter(channel, FLOW_CONTROL_FILTER, &mask, &pattern, &flow, &filter))) {
        fail("PassThruStartMsgFilter", status); goto disconnect;
    }
    static const uint8_t request[2] = {0x01, 0x00};
    PASSTHRU_MSG out;
    message(&out, 0x7df, request, 2);
    uint32_t count = 1;
    /* A timed write blocks until the adapter reports the frame was sent, and returns ERR_TIMEOUT if it never is. */
    if ((status = PassThruWriteMsgs(channel, &out, &count, 250)) || count != 1) { fail("PassThruWriteMsgs", status); goto stop; }
    for (int waited = 0; waited < 20; ++waited) {
        static PASSTHRU_MSG in[8];
        count = 8;
        status = PassThruReadMsgs(channel, in, &count, 100);
        if (status && status != ERR_TIMEOUT && status != ERR_BUFFER_EMPTY) { fail("PassThruReadMsgs", status); goto stop; }
        for (uint32_t i = 0; i < count; ++i) {
            /* RxStatus TX_DONE|TX_MSG_TYPE is our own request being confirmed; START_OF_MESSAGE announces a multi-frame reply. */
            if (in[i].RxStatus & (TX_DONE | START_OF_MESSAGE)) continue;
            printf("reply from 0x%02x%02x:", in[i].Data[2], in[i].Data[3]);
            for (uint32_t b = 4; b < in[i].DataSize; ++b) printf(" %02x", in[i].Data[b]);
            printf("\n");
            result = 0;
        }
    }
    if (result) fprintf(stderr, "no ECU answered\n");
stop:
    PassThruStopMsgFilter(channel, filter);
disconnect:
    PassThruDisconnect(channel);
close_device:
    PassThruClose(device);
    return result;
}
