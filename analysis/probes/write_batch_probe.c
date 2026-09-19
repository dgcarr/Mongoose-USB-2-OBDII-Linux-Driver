/* Three one-message PassThruWriteMsgs back to back, then one three-message call, then the singles again, all
 * queue-and-return (timeout 0) on a raw CAN channel. Safe with no vehicle: nothing on the bus acknowledges the
 * frames. Before the one-message-transaction fix the three-message call timed out. Build from the repository root:
 *   cc -Iinclude analysis/probes/write_batch_probe.c -Lbuild -lmongoose_j2534 -Wl,-rpath,$PWD/build -o write_batch_probe */
#include "mongoose/j2534.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }
static void run(int batched) {
    uint32_t device, channel, filter; char text[80];
    PassThruOpen(NULL, &device); PassThruConnect(device, CAN, 0, 500000, &channel);
    PASSTHRU_MSG mask = {0}, pattern = {0}; mask.ProtocolID = pattern.ProtocolID = CAN; mask.DataSize = pattern.DataSize = 4;
    PassThruStartMsgFilter(channel, PASS_FILTER, &mask, &pattern, NULL, &filter);
    PASSTHRU_MSG m[3] = {0};
    for (int i = 0; i < 3; ++i) { m[i].ProtocolID = CAN; m[i].DataSize = 12; m[i].Data[2] = 7; m[i].Data[3] = 0xdf; m[i].Data[4] = 2; m[i].Data[5] = 1; memset(&m[i].Data[7], 0x55, 5); }
    const double start = now();
    if (batched) { uint32_t n = 3; int32_t s = PassThruWriteMsgs(channel, m, &n, 0); PassThruGetLastError(text); printf("batched: status %d count %u %.0f ms %s\n", s, n, now() - start, s ? text : ""); }
    else for (int i = 0; i < 3; ++i) { uint32_t n = 1; int32_t s = PassThruWriteMsgs(channel, &m[i], &n, 0); PassThruGetLastError(text); printf("single %d: status %d count %u %.0f ms %s\n", i, s, n, now() - start, s ? text : ""); }
    PassThruStopMsgFilter(channel, filter); PassThruDisconnect(channel); PassThruClose(device);
}
int main(void) { run(0); run(1); run(0); return 0; }
