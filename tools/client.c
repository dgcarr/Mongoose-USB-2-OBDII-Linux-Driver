#include "mongoose/j2534.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
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
// One OBD-II mode 01 PID 00 frame, the standard read-only capability query. Sent twice:
// once queue-and-return, once blocking. With no bus the blocking write must report
// ERR_TIMEOUT, because nothing was confirmed as transmitted.
static int32_t check_transmit(uint32_t channel, uint32_t flags) {
    PASSTHRU_MSG message = {0};
    message.ProtocolID = CAN;
    message.TxFlags = flags;  // the identifier width must match the channel's
    message.DataSize = 6;
    message.Data[2] = 0x07; message.Data[3] = 0xdf; message.Data[4] = 0x09; message.Data[5] = 0x02;
    uint32_t count = 1;
    int32_t status = PassThruWriteMsgs(channel, &message, &count, 0);
    report("PassThruWriteMsgs queued", status);
    printf("queued=%u\n", count);
    if (status) return status;
    count = 1;
    status = PassThruWriteMsgs(channel, &message, &count, 250);
    report("PassThruWriteMsgs blocking", status);
    printf("confirmed=%u\n", count);
    if (status == ERR_TIMEOUT && count == 0) {
        printf("no iMsgTxDone without a bus: queued but not transmitted, as expected\n");
        return 0;
    }
    return status ? status : 0;
}
// Live-vehicle check on one 500 kbit 11-bit channel. Listening never transmits. With
// request set it then sends exactly one read-only OBD-II mode 09 PID 02 (VIN) query to
// the functional address and reports whether the adapter confirmed it reached the bus.
static int32_t check_vehicle(uint32_t channel, int request) {
    PASSTHRU_MSG mask = {0}, pattern = {0};
    mask.ProtocolID = pattern.ProtocolID = CAN;
    mask.DataSize = pattern.DataSize = 4;
    uint32_t filter = 0;
    int32_t status = PassThruStartMsgFilter(channel, PASS_FILTER, &mask, &pattern, NULL, &filter);
    report("PassThruStartMsgFilter wildcard", status);
    if (status) return status;
    for (int phase = 0; phase < (request ? 2 : 1) && !status; ++phase) {
        if (phase == 1) {
            // Frames queued by the wildcard phase would look like replies; discard them.
            unsigned drained = 0;
            for (;;) {
                PASSTHRU_MSG stale = {0};
                uint32_t count = 1;
                if (PassThruReadMsgs(channel, &stale, &count, 0) || !count) break;
                drained += count;
            }
            printf("drained=%u stale frames\n", drained);
            static const uint8_t queries[2][2] = {{0x01, 0x00}, {0x09, 0x02}};
            for (int q = 0; q < 2; ++q) {
                PASSTHRU_MSG query = {0};
                // Raw CAN carries no ISO-TP layer, so build the single frame by hand: PCI
                // 0x02 (two data bytes), service, PID, padded to 8 bytes. The ISO15765 API
                // takes only service+PID and the adapter adds the PCI, which is why the same
                // request without it (an earlier draft) drew no reply.
                query.ProtocolID = CAN; query.DataSize = 12;
                query.Data[2] = 0x07; query.Data[3] = 0xdf;
                query.Data[4] = 0x02; query.Data[5] = queries[q][0]; query.Data[6] = queries[q][1];
                memset(&query.Data[7], 0x55, 5);
                uint32_t sent = 1;
                status = PassThruWriteMsgs(channel, &query, &sent, 250);
                printf("request 7DF %02x %02x: ", queries[q][0], queries[q][1]);
                report("PassThruWriteMsgs (blocking)", status);
                printf("confirmed=%u\n", sent);
                if (status == ERR_TIMEOUT) status = 0;  // report it; the ECUs may simply be asleep
            }
        }
        printf("-- %s, 4 s --\n", phase ? "after request" : "passive listen");
        const time_t end = time(NULL) + 4;
        unsigned total = 0, diagnostic = 0;
        while (time(NULL) < end) {
            PASSTHRU_MSG in = {0};
            uint32_t count = 1;
            const int32_t rd = PassThruReadMsgs(channel, &in, &count, 100);
            if (rd && rd != ERR_BUFFER_EMPTY && rd != ERR_TIMEOUT) {
                report("PassThruReadMsgs", rd); status = rd; break;
            }
            for (uint32_t i = 0; i < count; ++i, ++total) {
                const uint32_t id = in.DataSize >= 4
                    ? (uint32_t)in.Data[0] << 24 | (uint32_t)in.Data[1] << 16 | (uint32_t)in.Data[2] << 8 | in.Data[3] : 0;
                const int obd = id >= 0x700 && id <= 0x7ff;
                diagnostic += obd ? 1u : 0u;
                if (!obd && (phase || total >= 20)) continue;  // print OBD range always, others only when listening
                printf("rx rxstatus=0x%x ts=%u data=", in.RxStatus, in.Timestamp);
                for (uint32_t b = 0; b < in.DataSize; ++b) printf("%02x", in.Data[b]);
                printf("\n");
            }
        }
        printf("frames=%u obd_range=%u\n", total, diagnostic);
    }
    const int32_t stopped = PassThruStopMsgFilter(channel, filter);
    report("PassThruStopMsgFilter", stopped);
    return status ? status : stopped;
}
// Live-vehicle ISO15765 check, the same exchange as Windows capture E2: a flow-control
// filter for response 0x7E8 / request 0x7E0, then one read-only mode 09 PID 02 (VIN)
// request to the functional address. The adapter segments flow control and the library
// reassembles, so the caller sees one message per reply.
static void iso_message(PASSTHRU_MSG *message, uint32_t id, const uint8_t *service, uint32_t size) {
    memset(message, 0, sizeof(*message));
    message->ProtocolID = ISO15765; message->TxFlags = ISO15765_FRAME_PAD;
    message->Data[0] = (uint8_t)(id >> 24); message->Data[1] = (uint8_t)(id >> 16);
    message->Data[2] = (uint8_t)(id >> 8); message->Data[3] = (uint8_t)id;
    if (size) memcpy(&message->Data[4], service, size);
    message->DataSize = 4 + size;
}
static int32_t check_vehicle_iso(uint32_t channel) {
    PASSTHRU_MSG mask, pattern, flow;
    iso_message(&mask, 0xffffffffu, NULL, 0);
    iso_message(&pattern, 0x7e8, NULL, 0);
    iso_message(&flow, 0x7e0, NULL, 0);
    uint32_t filter = 0;
    int32_t status = PassThruStartMsgFilter(channel, FLOW_CONTROL_FILTER, &mask, &pattern, &flow, &filter);
    report("PassThruStartMsgFilter flow-control 7E8/7E0", status);
    if (status) return status;
    static const uint8_t vin[2] = {0x09, 0x02};
    PASSTHRU_MSG request;
    iso_message(&request, 0x7df, vin, 2);
    uint32_t sent = 1;
    status = PassThruWriteMsgs(channel, &request, &sent, 250);
    report("PassThruWriteMsgs 7DF 09 02 (blocking)", status);
    printf("confirmed=%u\n", sent);
    if (status == ERR_TIMEOUT) status = 0;
    const time_t end = time(NULL) + 4;
    unsigned messages = 0;
    while (time(NULL) < end && !status) {
        PASSTHRU_MSG in;
        memset(&in, 0, sizeof(in));
        uint32_t count = 1;
        const int32_t rd = PassThruReadMsgs(channel, &in, &count, 100);
        if (rd && rd != ERR_BUFFER_EMPTY && rd != ERR_TIMEOUT) { report("PassThruReadMsgs", rd); status = rd; break; }
        for (uint32_t i = 0; i < count; ++i, ++messages) {
            printf("rx rxstatus=0x%x size=%u data=", in.RxStatus, in.DataSize);
            for (uint32_t b = 0; b < in.DataSize; ++b) printf("%02x", in.Data[b]);
            printf("  ascii=");
            for (uint32_t b = 4; b < in.DataSize; ++b) putchar(in.Data[b] >= 32 && in.Data[b] < 127 ? in.Data[b] : '.');
            printf("\n");
        }
    }
    printf("messages=%u\n", messages);
    const int32_t stopped = PassThruStopMsgFilter(channel, filter);
    report("PassThruStopMsgFilter", stopped);
    return status ? status : stopped;
}
// Passive sustained-receive measurement on a live bus: one wildcard filter, no transmit.
// Reports the read errors that would show loss, and gaps between device timestamps.
static int32_t check_vehicle_soak(uint32_t channel, unsigned seconds) {
    PASSTHRU_MSG mask = {0}, pattern = {0};
    mask.ProtocolID = pattern.ProtocolID = CAN;
    mask.DataSize = pattern.DataSize = 4;
    uint32_t filter = 0;
    int32_t status = PassThruStartMsgFilter(channel, PASS_FILTER, &mask, &pattern, NULL, &filter);
    report("PassThruStartMsgFilter wildcard", status);
    if (status) return status;
    const time_t end = time(NULL) + seconds;
    unsigned long total = 0, overflows = 0, empty_rounds = 0, gaps_over_10ms = 0;
    uint32_t last = 0, max_gap = 0, first = 0;
    int have_last = 0;
    static PASSTHRU_MSG batch[256];
    while (time(NULL) < end) {
        uint32_t count = 256;
        const int32_t rd = PassThruReadMsgs(channel, batch, &count, 100);
        if (rd == ERR_BUFFER_OVERFLOW) { ++overflows; }
        else if (rd == ERR_BUFFER_EMPTY) { ++empty_rounds; }
        else if (rd && rd != ERR_TIMEOUT) { report("PassThruReadMsgs", rd); status = rd; break; }
        for (uint32_t i = 0; i < count; ++i) {
            const uint32_t ts = batch[i].Timestamp;
            if (!have_last) { first = ts; have_last = 1; }
            else { const uint32_t gap = ts - last; if (gap > max_gap) max_gap = gap; if (gap > 10000) ++gaps_over_10ms; }
            last = ts;
        }
        total += count;
    }
    printf("soak seconds=%u frames=%lu rate=%.1f/s overflows=%lu empty_rounds=%lu\n",
           seconds, total, (double)total / seconds, overflows, empty_rounds);
    printf("device_span_us=%u max_gap_us=%u gaps_over_10ms=%lu\n", last - first, max_gap, gaps_over_10ms);
    const int32_t stopped = PassThruStopMsgFilter(channel, filter);
    report("PassThruStopMsgFilter", stopped);
    return status ? status : stopped;
}
// One hardware lifecycle cycle: open, version, ISO15765 connect, flow-control filter, one
// read-only mode 01 PID 00 request that must draw a positive 0x41 reply from 0x7E8, then
// the full teardown. Returns 0 on success; a nonzero result names the step that failed.
static int cycle_once(const char *name, double *reply_ms) {
    uint32_t device = 0, channel = 0, filter = 0;
    int32_t status = PassThruOpen((void *)name, &device);
    if (status) { report("PassThruOpen", status); return 1; }
    int result = 0;
    char firmware[80], driver[80], api[80];
    if (PassThruReadVersion(device, firmware, driver, api)) { report("PassThruReadVersion", 1); result = 2; }
    if (!result && (status = PassThruConnect(device, ISO15765, 0, 500000, &channel))) { report("PassThruConnect", status); result = 3; }
    if (!result) {
        PASSTHRU_MSG mask, pattern, flow;
        iso_message(&mask, 0xffffffffu, NULL, 0); iso_message(&pattern, 0x7e8, NULL, 0); iso_message(&flow, 0x7e0, NULL, 0);
        if ((status = PassThruStartMsgFilter(channel, FLOW_CONTROL_FILTER, &mask, &pattern, &flow, &filter))) { report("PassThruStartMsgFilter", status); result = 4; }
    }
    if (!result) {
        static const uint8_t pid[2] = {0x01, 0x00};
        PASSTHRU_MSG request;
        iso_message(&request, 0x7df, pid, 2);
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        uint32_t sent = 1;
        if ((status = PassThruWriteMsgs(channel, &request, &sent, 250)) || sent != 1) { report("PassThruWriteMsgs", status); result = 5; }
        int replied = 0;
        while (!result && !replied) {
            PASSTHRU_MSG in;
            memset(&in, 0, sizeof(in));
            uint32_t count = 1;
            status = PassThruReadMsgs(channel, &in, &count, 1000);
            if (status && status != ERR_BUFFER_EMPTY && status != ERR_TIMEOUT) { report("PassThruReadMsgs", status); result = 6; break; }
            if (!count) { printf("no reply within 1000 ms\n"); result = 7; break; }
            if (in.RxStatus == 0 && in.DataSize >= 6 && in.Data[2] == 0x07 && in.Data[3] == 0xe8 &&
                in.Data[4] == 0x41 && in.Data[5] == 0x00) replied = 1;
        }
        clock_gettime(CLOCK_MONOTONIC, &t1);
        *reply_ms = (double)(t1.tv_sec - t0.tv_sec) * 1000.0 + (double)(t1.tv_nsec - t0.tv_nsec) / 1e6;
    }
    if (filter && (status = PassThruStopMsgFilter(channel, filter))) { report("PassThruStopMsgFilter", status); if (!result) result = 8; }
    if (channel && (status = PassThruDisconnect(channel))) { report("PassThruDisconnect", status); if (!result) result = 9; }
    if ((status = PassThruClose(device))) { report("PassThruClose", status); if (!result) result = 10; }
    return result;
}
static int check_vehicle_cycles(const char *name, unsigned cycles) {
    double total = 0, worst = 0, best = 1e9;
    struct timespec s0, s1;
    clock_gettime(CLOCK_MONOTONIC, &s0);
    for (unsigned i = 1; i <= cycles; ++i) {
        double ms = 0;
        const int failed = cycle_once(name, &ms);
        if (failed) { printf("cycle %u FAILED at step %d\n", i, failed); return 1; }
        total += ms; if (ms > worst) worst = ms; if (ms < best) best = ms;
        if (i % 10 == 0) printf("cycle %u ok\n", i);
    }
    clock_gettime(CLOCK_MONOTONIC, &s1);
    printf("cycles=%u all passed in %.1f s; request->reply mean %.2f ms best %.2f ms worst %.2f ms\n", cycles,
           (double)(s1.tv_sec - s0.tv_sec) + (double)(s1.tv_nsec - s0.tv_nsec) / 1e9, total / cycles, best, worst);
    return 0;
}
int main(int argc, char **argv) {
    uint32_t device = 0;
    const int can_lifecycle = argc == 3 && strcmp(argv[2], "--can-lifecycle") == 0;
    const int can_receive = argc == 3 && strcmp(argv[2], "--can-receive-check") == 0;
    const int can_transmit = argc == 3 && strcmp(argv[2], "--can-transmit-check") == 0;
    const int vehicle_listen = argc == 3 && strcmp(argv[2], "--vehicle-listen") == 0;
    const int vehicle_check = argc == 3 && strcmp(argv[2], "--vehicle-check") == 0;
    if (argc == 3 && strcmp(argv[2], "--vehicle-cycles") == 0)
        return check_vehicle_cycles(argv[1], 100);
    const int vehicle_soak = argc == 3 && strcmp(argv[2], "--vehicle-soak") == 0;
    const int vehicle_iso = argc == 3 && strcmp(argv[2], "--vehicle-iso") == 0;
    if (argc > 3 || (argc >= 2 && strncmp(argv[1], "serial:", 7)) ||
        (argc == 3 && !can_lifecycle && !can_receive && !can_transmit &&
         !vehicle_listen && !vehicle_check && !vehicle_iso && !vehicle_soak)) {
        fprintf(stderr, "Usage: mongoose-client [serial:SERIAL [--can-lifecycle|--can-receive-check|"
                        "--can-transmit-check|--vehicle-listen|--vehicle-check|--vehicle-iso|--vehicle-soak|--vehicle-cycles]]\n"); return 2;
    }
    int32_t status = PassThruOpen(argc >= 2 ? argv[1] : NULL, &device);
    report("PassThruOpen", status);
    if (status) return 1;
    char firmware[80], driver[80], api[80];
    int32_t version = PassThruReadVersion(device, firmware, driver, api);
    report("PassThruReadVersion", version);
    if (!version) printf("firmware=%s driver=%s api=%s\n", firmware, driver, api);
    int32_t channel_status = 0;
    if ((vehicle_listen || vehicle_check) && !version) {
        uint32_t channel = 0;
        channel_status = PassThruConnect(device, CAN, 0, 500000, &channel);
        report("PassThruConnect 500000", channel_status);
        if (!channel_status) {
            channel_status = check_vehicle(channel, vehicle_check);
            const int32_t disconnected = PassThruDisconnect(channel);
            report("PassThruDisconnect", disconnected);
            if (!channel_status) channel_status = disconnected;
        }
    }
    if (vehicle_soak && !version) {
        uint32_t channel = 0;
        channel_status = PassThruConnect(device, CAN, 0, 500000, &channel);
        report("PassThruConnect 500000", channel_status);
        if (!channel_status) {
            channel_status = check_vehicle_soak(channel, 300);
            const int32_t disconnected = PassThruDisconnect(channel);
            report("PassThruDisconnect", disconnected);
            if (!channel_status) channel_status = disconnected;
        }
    }
    if (vehicle_iso && !version) {
        uint32_t channel = 0;
        channel_status = PassThruConnect(device, ISO15765, 0, 500000, &channel);
        report("PassThruConnect ISO15765 500000", channel_status);
        if (!channel_status) {
            channel_status = check_vehicle_iso(channel);
            const int32_t disconnected = PassThruDisconnect(channel);
            report("PassThruDisconnect", disconnected);
            if (!channel_status) channel_status = disconnected;
        }
    }
    if ((can_lifecycle || can_receive || can_transmit) && !version) {
        // Reconnect within one process also exercises handle retirement. Receive mode
        // adds pass filters and reads; only transmit mode calls WriteMsgs, and it sends
        // one read-only OBD query per channel.
        const uint32_t bauds[] = {500000, 250000, 500000};
        const uint32_t flags[] = {0, 0, CAN_29BIT_ID};
        for (unsigned i = 0; i < 3; ++i) {
            uint32_t channel = 0;
            channel_status = PassThruConnect(device, CAN, flags[i], bauds[i], &channel);
            report("PassThruConnect", channel_status);
            if (channel_status) break;
            printf("device=%u channel=%u baud=%u flags=0x%x\n", device, channel, bauds[i], flags[i]);
            if (can_receive) channel_status = check_receive(channel, flags[i]);
            if (can_transmit) channel_status = check_transmit(channel, flags[i]);
            const int32_t disconnected = PassThruDisconnect(channel);
            report("PassThruDisconnect", disconnected);
            if (!channel_status) channel_status = disconnected;
            if (channel_status) break;
        }
    }
    status = PassThruClose(device); report("PassThruClose", status);
    return status || version || channel_status ? 1 : 0;
}
