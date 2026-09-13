#include "mongoose/j2534.h"
#include <stdio.h>
#include <string.h>
static void report(const char *operation, int32_t status) {
    char error[80] = {0};
    if (status) PassThruGetLastError(error);
    printf("%s status=%d %s\n", operation, status, error);
}
static int32_t check_receive(uint32_t channel, uint32_t flags) {
    PASSTHRU_MSG mask = {0}, pattern = {0}, received = {0};
    uint32_t filters[2] = {0}, count = 1;
    mask.ProtocolID = pattern.ProtocolID = CAN;
    mask.TxFlags = pattern.TxFlags = flags;
    mask.DataSize = pattern.DataSize = 4;
    int32_t status = 0;
    for (unsigned i = 0; i < 2; ++i) {
        status = PassThruStartMsgFilter(channel, PASS_FILTER, &mask, &pattern, NULL, &filters[i]);
        report("PassThruStartMsgFilter", status);
        if (status) break;
        printf("filter=%u\n", filters[i]);
        mask.Data[2] = 7; mask.Data[3] = 0xff;
        pattern.Data[2] = 7; pattern.Data[3] = 0xe8;
    }
    if (!status) {
        status = PassThruReadMsgs(channel, &received, &count, 25);
        report("PassThruReadMsgs", status);
        printf("received=%u\n", count);
        if (status == ERR_BUFFER_EMPTY) status = 0; // expected on an unconnected bus
    }
    for (unsigned i = 0; i < 2; ++i) {
        if (filters[i]) {
            const int32_t stopped = PassThruStopMsgFilter(channel, filters[i]);
            report("PassThruStopMsgFilter", stopped);
            if (!status) status = stopped;
        }
    }
    return status;
}
int main(int argc, char **argv) {
    uint32_t device = 0;
    const int can_lifecycle = argc == 3 && strcmp(argv[2], "--can-lifecycle") == 0;
    const int can_receive = argc == 3 && strcmp(argv[2], "--can-receive-check") == 0;
    if (argc > 3 || (argc >= 2 && strncmp(argv[1], "serial:", 7)) || (argc == 3 && !can_lifecycle && !can_receive)) {
        fprintf(stderr, "Usage: mongoose-client [serial:SERIAL [--can-lifecycle|--can-receive-check]]\n"); return 2;
    }
    int32_t status = PassThruOpen(argc >= 2 ? argv[1] : NULL, &device);
    report("PassThruOpen", status);
    if (status) return 1;
    char firmware[80], driver[80], api[80];
    int32_t version = PassThruReadVersion(device, firmware, driver, api);
    report("PassThruReadVersion", version);
    if (!version) printf("firmware=%s driver=%s api=%s\n", firmware, driver, api);
    int32_t channel_status = 0;
    if ((can_lifecycle || can_receive) && !version) {
        // Neither mode calls WriteMsgs. Reconnect within one process also
        // exercises handle retirement; receive mode adds pass filters and reads.
        const uint32_t bauds[] = {500000, 250000, 500000};
        const uint32_t flags[] = {0, 0, CAN_29BIT_ID};
        for (unsigned i = 0; i < 3; ++i) {
            uint32_t channel = 0;
            channel_status = PassThruConnect(device, CAN, flags[i], bauds[i], &channel);
            report("PassThruConnect", channel_status);
            if (channel_status) break;
            printf("device=%u channel=%u baud=%u flags=0x%x\n", device, channel, bauds[i], flags[i]);
            if (can_receive) channel_status = check_receive(channel, flags[i]);
            const int32_t disconnected = PassThruDisconnect(channel);
            report("PassThruDisconnect", disconnected);
            if (!channel_status) channel_status = disconnected;
            if (channel_status) break;
        }
    }
    status = PassThruClose(device); report("PassThruClose", status);
    return status || version || channel_status ? 1 : 0;
}
