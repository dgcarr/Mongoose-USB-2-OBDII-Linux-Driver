#include "mongoose/j2534.h"
#include "script_runner.h"
#include <stdio.h>
#include <stdlib.h>
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
// ERR_TIMEOUT, because nothing was confirmed as transmitted. A raw CAN channel carries no
// ISO-TP layer, so the single frame is built by hand: PCI 0x02 (two data bytes), service,
// PID, padded to 8 bytes. Without the PCI byte an ECU on a real bus ignores the request.
static int32_t check_transmit(uint32_t channel, uint32_t flags) {
    PASSTHRU_MSG message = {0};
    message.ProtocolID = CAN;
    message.TxFlags = flags;  // the identifier width must match the channel's
    message.DataSize = 12;
    message.Data[2] = 0x07; message.Data[3] = 0xdf;
    message.Data[4] = 0x02; message.Data[5] = 0x01; message.Data[6] = 0x00;
    memset(&message.Data[7], 0x55, 5);
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
static int cycle_once(const char *name, int raw, double *reply_ms) {
    uint32_t device = 0, channel = 0, filter = 0;
    int32_t status = PassThruOpen((void *)name, &device);
    if (status) { report("PassThruOpen", status); return 1; }
    int result = 0;
    char firmware[80], driver[80], api[80];
    if (PassThruReadVersion(device, firmware, driver, api)) { report("PassThruReadVersion", 1); result = 2; }
    if (!result && (status = PassThruConnect(device, raw ? CAN : ISO15765, 0, 500000, &channel))) { report("PassThruConnect", status); result = 3; }
    if (!result && raw) {
        // Raw CAN: a pass filter for the ECU reply IDs only (mask 0x7F8 covers 0x7E8-0x7EF).
        PASSTHRU_MSG mask = {0}, pattern = {0};
        mask.ProtocolID = pattern.ProtocolID = CAN; mask.DataSize = pattern.DataSize = 4;
        mask.Data[2] = 0x07; mask.Data[3] = 0xf8; pattern.Data[2] = 0x07; pattern.Data[3] = 0xe8;
        if ((status = PassThruStartMsgFilter(channel, PASS_FILTER, &mask, &pattern, NULL, &filter))) { report("PassThruStartMsgFilter", status); result = 4; }
    } else if (!result) {
        PASSTHRU_MSG mask, pattern, flow;
        iso_message(&mask, 0xffffffffu, NULL, 0); iso_message(&pattern, 0x7e8, NULL, 0); iso_message(&flow, 0x7e0, NULL, 0);
        if ((status = PassThruStartMsgFilter(channel, FLOW_CONTROL_FILTER, &mask, &pattern, &flow, &filter))) { report("PassThruStartMsgFilter", status); result = 4; }
    }
    if (!result) {
        static const uint8_t pid[2] = {0x01, 0x00};
        PASSTHRU_MSG request;
        if (raw) {
            // The single frame is built by hand on a raw channel: PCI 0x02, service, PID, padded to 8.
            memset(&request, 0, sizeof(request));
            request.ProtocolID = CAN; request.DataSize = 12;
            request.Data[2] = 0x07; request.Data[3] = 0xdf;
            request.Data[4] = 0x02; request.Data[5] = pid[0]; request.Data[6] = pid[1];
            memset(&request.Data[7], 0x55, 5);
        } else iso_message(&request, 0x7df, pid, 2);
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
            // ISO15765 delivers service+PID straight after the ID (and a transmit-done message first,
            // RxStatus 9, which must not count); raw CAN carries the PCI length byte ahead of them.
            const unsigned at = raw ? 5 : 4;
            if (in.RxStatus == 0 && in.DataSize >= at + 2 && in.Data[2] == 0x07 && in.Data[3] == 0xe8 &&
                in.Data[at] == 0x41 && in.Data[at + 1] == 0x00) replied = 1;
        }
        clock_gettime(CLOCK_MONOTONIC, &t1);
        *reply_ms = (double)(t1.tv_sec - t0.tv_sec) * 1000.0 + (double)(t1.tv_nsec - t0.tv_nsec) / 1e6;
    }
    if (filter && (status = PassThruStopMsgFilter(channel, filter))) { report("PassThruStopMsgFilter", status); if (!result) result = 8; }
    if (channel && (status = PassThruDisconnect(channel))) { report("PassThruDisconnect", status); if (!result) result = 9; }
    if ((status = PassThruClose(device))) { report("PassThruClose", status); if (!result) result = 10; }
    return result;
}
static int check_vehicle_cycles(const char *name, unsigned cycles, int raw) {
    double total = 0, worst = 0, best = 1e9;
    struct timespec s0, s1;
    clock_gettime(CLOCK_MONOTONIC, &s0);
    for (unsigned i = 1; i <= cycles; ++i) {
        double ms = 0;
        const int failed = cycle_once(name, raw, &ms);
        if (failed) { printf("cycle %u FAILED at step %d\n", i, failed); return 1; }
        total += ms; if (ms > worst) worst = ms; if (ms < best) best = ms;
        if (i % 10 == 0) printf("cycle %u ok\n", i);
    }
    clock_gettime(CLOCK_MONOTONIC, &s1);
    printf("cycles=%u all passed in %.1f s; request->reply mean %.2f ms best %.2f ms worst %.2f ms\n", cycles,
           (double)(s1.tv_sec - s0.tv_sec) + (double)(s1.tv_nsec - s0.tv_nsec) / 1e9, total / cycles, best, worst);
    return 0;
}
// Diagnostic soak on a live bus: continuous wildcard receive plus one read-only mode 01
// request per second, rotating through PIDs 00, 05, 0C and 0D that the ECM reports. Each
// reply must arrive within a second. Raw CAN, so the single-frame PCI byte is built here.
// Duration comes from MONGOOSE_SOAK_SECONDS (default 3600); progress is logged each minute.
static long rss_kb(void) {
    FILE *file = fopen("/proc/self/status", "r");
    char line[128]; long kb = -1;
    if (!file) return -1;
    while (fgets(line, sizeof(line), file)) if (sscanf(line, "VmRSS: %ld kB", &kb) == 1) break;
    fclose(file);
    return kb;
}
static double now_ms(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec * 1000.0 + (double)t.tv_nsec / 1e6;
}
static int check_vehicle_hour(uint32_t channel, unsigned seconds) {
    PASSTHRU_MSG mask = {0}, pattern = {0};
    mask.ProtocolID = pattern.ProtocolID = CAN;
    mask.DataSize = pattern.DataSize = 4;
    uint32_t filter = 0;
    int32_t status = PassThruStartMsgFilter(channel, PASS_FILTER, &mask, &pattern, NULL, &filter);
    report("PassThruStartMsgFilter wildcard", status);
    if (status) return 1;
    static const uint8_t pids[4] = {0x00, 0x05, 0x0c, 0x0d};
    static PASSTHRU_MSG batch[256];
    const double start = now_ms(), end = start + seconds * 1000.0;
    double next_request = start, sent_at = 0, worst_reply = 0, reply_total = 0, next_log = start + 60000.0;
    unsigned long frames = 0, overflows = 0, requests = 0, replies = 0, unanswered = 0, write_failures = 0;
    unsigned long gaps_over_10ms = 0; uint32_t last_ts = 0, max_gap = 0; int have_ts = 0, outstanding = 0;
    uint8_t wanted = 0; long rss_first = rss_kb(); int failed = 0;
    printf("hour soak starts: %u s, rss_start=%ld kB\n", seconds, rss_first); fflush(stdout);
    while (now_ms() < end && !failed) {
        const double now = now_ms();
        if (now >= next_request) {
            if (outstanding) { ++unanswered; printf("UNANSWERED request %lu (pid %02x)\n", requests, wanted); fflush(stdout); }
            PASSTHRU_MSG query = {0};
            wanted = pids[requests % 4];
            query.ProtocolID = CAN; query.DataSize = 12;
            query.Data[2] = 0x07; query.Data[3] = 0xdf; query.Data[4] = 0x02; query.Data[5] = 0x01; query.Data[6] = wanted;
            memset(&query.Data[7], 0x55, 5);
            uint32_t sent = 1;
            const int32_t wr = PassThruWriteMsgs(channel, &query, &sent, 250);
            if (wr || sent != 1) { ++write_failures; report("PassThruWriteMsgs", wr); }
            ++requests; outstanding = 1; sent_at = now_ms(); next_request += 1000.0;
        }
        uint32_t count = 256;
        const int32_t rd = PassThruReadMsgs(channel, batch, &count, 5);
        if (rd == ERR_BUFFER_OVERFLOW) ++overflows;
        else if (rd && rd != ERR_BUFFER_EMPTY && rd != ERR_TIMEOUT) { report("PassThruReadMsgs", rd); failed = 1; break; }
        for (uint32_t i = 0; i < count; ++i) {
            const PASSTHRU_MSG *m = &batch[i];
            if (have_ts) { const uint32_t gap = m->Timestamp - last_ts; if (gap > max_gap) max_gap = gap; if (gap > 10000) ++gaps_over_10ms; }
            last_ts = m->Timestamp; have_ts = 1;
            if (outstanding && m->DataSize >= 7 && m->Data[2] == 0x07 && (m->Data[3] & 0xf8) == 0xe8 &&
                m->Data[5] == 0x41 && m->Data[6] == wanted) {
                const double ms = now_ms() - sent_at;
                ++replies; reply_total += ms; if (ms > worst_reply) worst_reply = ms; outstanding = 0;
            }
        }
        frames += count;
        if (now_ms() >= next_log) {
            const double elapsed = (now_ms() - start) / 1000.0;
            printf("t=%.0fs frames=%lu rate=%.0f/s requests=%lu replies=%lu unanswered=%lu write_failures=%lu overflows=%lu max_gap_us=%u gaps_over_10ms=%lu rss=%ld kB\n",
                   elapsed, frames, (double)frames / elapsed, requests, replies, unanswered, write_failures, overflows, max_gap, gaps_over_10ms, rss_kb());
            fflush(stdout); next_log += 60000.0;
        }
    }
    if (outstanding) ++unanswered;  /* a request still pending at the deadline had under a second */
    if (outstanding && now_ms() - sent_at < 1000.0) --unanswered;
    const double elapsed = (now_ms() - start) / 1000.0;
    printf("hour soak done: %.0f s frames=%lu rate=%.0f/s requests=%lu replies=%lu unanswered=%lu write_failures=%lu overflows=%lu\n",
           elapsed, frames, (double)frames / elapsed, requests, replies, unanswered, write_failures, overflows);
    printf("device max_gap_us=%u gaps_over_10ms=%lu reply mean %.2f ms worst %.2f ms rss_start=%ld end=%ld kB\n",
           max_gap, gaps_over_10ms, replies ? reply_total / (double)replies : 0.0, worst_reply, rss_first, rss_kb());
    const int32_t stopped = PassThruStopMsgFilter(channel, filter);
    report("PassThruStopMsgFilter", stopped);
    return failed || stopped || unanswered || write_failures || overflows;
}
// Script mode runs a tools/scripts/*.txt file, the same one the Windows harness runs:
//   mongoose-client [serial:SERIAL] --script FILE [--gap MS]
//   mongoose-client --script-check FILE...   (parse only: no adapter, no library calls)
// Returns -1 when argv is not a script invocation.
static int script_main(int argc, char **argv) {
    if (argc >= 3 && strcmp(argv[1], "--script-check") == 0) {
        int result = 0;
        for (int i = 2; i < argc; ++i) {
            const int r = script_run(argv[i], NULL, 0, 1);
            if (r) { fprintf(stderr, "%s: not a valid script\n", argv[i]); result = r; }
        }
        return result;
    }
    int i = 1;
    const char *name = NULL;
    if (i < argc && strncmp(argv[i], "serial:", 7) == 0) name = argv[i++];
    if (i >= argc || strcmp(argv[i], "--script") != 0) return -1;
    if (++i >= argc) { fprintf(stderr, "--script needs a file\n"); return 2; }
    const char *path = argv[i++];
    uint32_t gap = 0;
    if (i < argc && strcmp(argv[i], "--gap") == 0) {
        if (++i >= argc) { fprintf(stderr, "--gap needs milliseconds\n"); return 2; }
        gap = (uint32_t)strtoul(argv[i++], NULL, 10);
    }
    if (i != argc) { fprintf(stderr, "unexpected argument: %s\n", argv[i]); return 2; }
    return script_run(path, name, gap, 0);
}
// Diagnostic soak on an ISO15765 channel: one read-only functional request a second, rotating through
// mode 01 PIDs 00, 05, 0C, 0D, and every tenth second mode 09 PID 04, whose two ECUs each answer with an
// interleaved multi-frame reply (39 bytes from 0x7E8, 23 from 0x7E9) that the reassembler must keep
// apart. A single-frame reply must arrive from 0x7E8 within a second, both multi-frame ones within two.
// Duration from MONGOOSE_SOAK_SECONDS (default 3600); progress is logged each minute.
static int check_vehicle_hour_iso(uint32_t channel, unsigned seconds) {
    PASSTHRU_MSG mask, pattern, flow;
    iso_message(&mask, 0xffffffffu, NULL, 0); iso_message(&pattern, 0x7e8, NULL, 0); iso_message(&flow, 0x7e0, NULL, 0);
    uint32_t filter8 = 0, filter9 = 0;
    int32_t status = PassThruStartMsgFilter(channel, FLOW_CONTROL_FILTER, &mask, &pattern, &flow, &filter8);
    report("PassThruStartMsgFilter flow-control 7E8/7E0", status);
    if (status) return 1;
    iso_message(&pattern, 0x7e9, NULL, 0); iso_message(&flow, 0x7e1, NULL, 0);
    status = PassThruStartMsgFilter(channel, FLOW_CONTROL_FILTER, &mask, &pattern, &flow, &filter9);
    report("PassThruStartMsgFilter flow-control 7E9/7E1", status);
    if (status) { PassThruStopMsgFilter(channel, filter8); return 1; }
    static const uint8_t pids[4] = {0x00, 0x05, 0x0c, 0x0d};
    static const uint8_t calibration[2] = {0x09, 0x04};
    const double start = now_ms(), end = start + seconds * 1000.0;
    double next_request = start, sent_at = 0, next_log = start + 60000.0, worst = 0, total_ms = 0;
    unsigned long requests = 0, singles = 0, multis = 0, single_ok = 0, multi_ok = 0, single_bad = 0, multi_bad = 0;
    unsigned long write_failures = 0, read_errors = 0, overflows = 0, tx_done = 0, indications = 0, stray = 0, second_ecu = 0;
    int outstanding = 0, multi = 0, got8 = 0, got9 = 0;
    uint8_t wanted = 0;
    const long rss_first = rss_kb();
    printf("iso hour soak starts: %u s, rss_start=%ld kB\n", seconds, rss_first); fflush(stdout);
    while (now_ms() < end) {
        const double now = now_ms();
        if (now >= next_request) {
            if (outstanding) {
                if (multi) { ++multi_bad; printf("MULTI-FRAME INCOMPLETE request %lu (7E8=%d 7E9=%d)\n", requests, got8, got9); }
                else { ++single_bad; printf("UNANSWERED request %lu (pid %02x)\n", requests, wanted); }
                fflush(stdout);
            }
            multi = requests % 10 == 9;
            wanted = pids[requests % 4];
            uint8_t service[2] = {0x01, wanted};
            PASSTHRU_MSG request;
            iso_message(&request, 0x7df, multi ? calibration : service, 2);
            uint32_t sent = 1;
            const int32_t wr = PassThruWriteMsgs(channel, &request, &sent, 250);
            if (wr || sent != 1) { ++write_failures; report("PassThruWriteMsgs", wr); }
            ++requests; if (multi) ++multis; else ++singles;
            outstanding = 1; got8 = got9 = 0; sent_at = now_ms(); next_request += 1000.0;
        }
        static PASSTHRU_MSG batch[16];
        uint32_t count = 16;
        const int32_t rd = PassThruReadMsgs(channel, batch, &count, 5);
        if (rd == ERR_BUFFER_OVERFLOW) ++overflows;
        else if (rd && rd != ERR_BUFFER_EMPTY && rd != ERR_TIMEOUT) { ++read_errors; report("PassThruReadMsgs", rd); }
        for (uint32_t i = 0; i < count; ++i) {
            const PASSTHRU_MSG *m = &batch[i];
            if (m->RxStatus & TX_DONE) { ++tx_done; continue; }
            if (m->RxStatus & START_OF_MESSAGE) { ++indications; continue; }
            if (m->RxStatus != 0 || m->DataSize < 6) { ++stray; continue; }
            const unsigned ecu = m->Data[3];
            if (outstanding && !multi && ecu == 0xe8 && m->Data[4] == 0x41 && m->Data[5] == wanted) {
                const double ms = now_ms() - sent_at;
                ++single_ok; total_ms += ms; if (ms > worst) worst = ms; outstanding = 0;
            } else if (outstanding && multi && m->Data[4] == 0x49 && m->Data[5] == 0x04) {
                // DataSize counts the four ID bytes: 35 payload bytes from 0x7E8, 19 from 0x7E9.
                if (ecu == 0xe8 && m->DataSize == 39) got8 = 1;
                else if (ecu == 0xe9 && m->DataSize == 23) got9 = 1;
                if (got8 && got9) { ++multi_ok; outstanding = 0; }
            } else if (ecu == 0xe9 && m->Data[4] == 0x41) ++second_ecu;  // the other ECU's normal reply to a functional request
            else ++stray;
        }
        if (outstanding && now_ms() - sent_at > (multi ? 2000.0 : 1000.0)) {
            if (multi) { ++multi_bad; printf("MULTI-FRAME INCOMPLETE request %lu (7E8=%d 7E9=%d)\n", requests, got8, got9); }
            else { ++single_bad; printf("UNANSWERED request %lu (pid %02x)\n", requests, wanted); }
            fflush(stdout); outstanding = 0;
        }
        if (now_ms() >= next_log) {
            printf("t=%.0fs requests=%lu single_ok=%lu/%lu multi_ok=%lu/%lu write_failures=%lu read_errors=%lu overflows=%lu rss=%ld kB\n",
                   (now_ms() - start) / 1000.0, requests, single_ok, singles, multi_ok, multis, write_failures, read_errors, overflows, rss_kb());
            fflush(stdout); next_log += 60000.0;
        }
    }
    if (outstanding && now_ms() - sent_at >= (multi ? 2000.0 : 1000.0)) { if (multi) ++multi_bad; else ++single_bad; }
    printf("iso hour soak done: %.0f s requests=%lu single=%lu ok=%lu bad=%lu | multi-frame=%lu ok=%lu bad=%lu\n",
           (now_ms() - start) / 1000.0, requests, singles, single_ok, single_bad, multis, multi_ok, multi_bad);
    printf("write_failures=%lu read_errors=%lu overflows=%lu tx_done_messages=%lu start_indications=%lu second_ecu_replies=%lu stray=%lu\n",
           write_failures, read_errors, overflows, tx_done, indications, second_ecu, stray);
    printf("single-frame request->reply mean %.2f ms worst %.2f ms (5 ms read granularity) rss_start=%ld end=%ld kB\n",
           single_ok ? total_ms / (double)single_ok : 0.0, worst, rss_first, rss_kb());
    const int32_t stopped9 = PassThruStopMsgFilter(channel, filter9), stopped8 = PassThruStopMsgFilter(channel, filter8);
    report("PassThruStopMsgFilter", stopped8 ? stopped8 : stopped9);
    return single_bad || multi_bad || write_failures || read_errors || overflows || stopped8 || stopped9;
}
// Ignition and bus-sleep observer. Passive on a raw 500 kbit channel with a wildcard filter, and it logs a line
// each second: frame rate, whether the bus is active or quiet (no frame for a second), battery voltage from
// READ_VBATT, and every read error by code. So that the ignition can be switched off and on while it runs, it
// sends one read-only mode 01 PID 00 request every ten seconds and only while frames are arriving: it never
// transmits onto a bus that has gone quiet, so a sleeping vehicle is not woken by it. Even so, a request every ten
// seconds can keep a network from going to sleep, so after two requests in a row go unanswered (the engine ECU
// has shut down) it stops transmitting altogether and only listens; it starts again when the bus has been quiet
// and comes back, or the frame rate returns to ignition-on levels. Duration comes from MONGOOSE_OBSERVE_SECONDS
// (default 600).
static int check_vehicle_ignition(uint32_t device, uint32_t channel, unsigned seconds) {
    PASSTHRU_MSG mask = {0}, pattern = {0};
    mask.ProtocolID = pattern.ProtocolID = CAN; mask.DataSize = pattern.DataSize = 4;
    uint32_t filter = 0;
    int32_t status = PassThruStartMsgFilter(channel, PASS_FILTER, &mask, &pattern, NULL, &filter);
    report("PassThruStartMsgFilter wildcard", status);
    if (status) return 1;
    static PASSTHRU_MSG batch[256];
    const double start = now_ms(), end = start + seconds * 1000.0;
    double next_tick = start + 1000.0, last_frame = start, last_request = start - 10000.0, sent_at = 0;
    unsigned long total = 0, tick_frames = 0, requests = 0, replies = 0, unanswered = 0, write_failures = 0, overflows = 0, other_errors = 0;
    unsigned long errors_by_code[32] = {0};
    uint32_t last_ts = 0, max_gap_us = 0; int have_ts = 0, quiet = 0, outstanding = 0;
    unsigned transitions = 0, streak = 0; int passive = 0;
    printf("observer starts: %u s. Switch the ignition off and on when you like.\n", seconds); fflush(stdout);
    while (now_ms() < end) {
        const double now = now_ms();
        uint32_t count = 256;
        const int32_t rd = PassThruReadMsgs(channel, batch, &count, 20);
        if (rd == ERR_BUFFER_OVERFLOW) ++overflows;
        else if (rd && rd != ERR_BUFFER_EMPTY && rd != ERR_TIMEOUT) {
            ++other_errors; if (rd > 0 && rd < 32) ++errors_by_code[rd];
            printf("READ ERROR at t=%.1fs: ", (now - start) / 1000.0); report("PassThruReadMsgs", rd);
            if (rd == ERR_DEVICE_NOT_CONNECTED || rd == ERR_INVALID_CHANNEL_ID) break;
        }
        for (uint32_t i = 0; i < count; ++i) {
            const PASSTHRU_MSG *m = &batch[i];
            if (have_ts) { const uint32_t gap = m->Timestamp - last_ts; if (gap > max_gap_us && gap < 0x80000000u) max_gap_us = gap; }
            last_ts = m->Timestamp; have_ts = 1;
            if (outstanding && m->DataSize >= 7 && m->Data[2] == 0x07 && m->Data[3] == 0xe8 && m->Data[5] == 0x41 && m->Data[6] == 0x00) {
                ++replies; outstanding = 0; streak = 0;
                printf("reply from 7E8 after %.1f ms\n", now_ms() - sent_at);
            }
        }
        if (count) { total += count; tick_frames += count; last_frame = now; }
        const int now_quiet = now - last_frame > 1000.0;
        if (now_quiet != quiet) {
            quiet = now_quiet; ++transitions;
            printf("*** t=%.1fs BUS %s (last device timestamp %u us)\n", (now - start) / 1000.0, quiet ? "QUIET" : "ACTIVE", last_ts);
            if (!quiet && passive) { passive = 0; streak = 0; printf("*** t=%.1fs bus woke: requests resume\n", (now - start) / 1000.0); }
            fflush(stdout);
        }
        if (outstanding && now - sent_at > 1000.0) {
            ++unanswered; outstanding = 0;
            printf("UNANSWERED request at t=%.1fs\n", (sent_at - start) / 1000.0);
            if (++streak >= 2 && !passive) { passive = 1; printf("*** t=%.1fs two requests unanswered: listening only from here\n", (now - start) / 1000.0); }
        }
        if (!quiet && !outstanding && !passive && now - last_request >= 10000.0) {
            PASSTHRU_MSG query = {0};
            query.ProtocolID = CAN; query.DataSize = 12;
            query.Data[2] = 0x07; query.Data[3] = 0xdf; query.Data[4] = 0x02; query.Data[5] = 0x01; query.Data[6] = 0x00;
            memset(&query.Data[7], 0x55, 5);
            uint32_t sent = 1;
            const int32_t wr = PassThruWriteMsgs(channel, &query, &sent, 250);
            if (wr || sent != 1) { ++write_failures; printf("WRITE at t=%.1fs: ", (now - start) / 1000.0); report("PassThruWriteMsgs", wr); }
            ++requests; outstanding = 1; sent_at = last_request = now_ms();
        }
        if (now >= next_tick) {
            uint32_t mv = 0;
            const int32_t vs = PassThruIoctl(device, READ_VBATT, NULL, &mv);
            printf("t=%4.0fs bus=%-6s frames/s=%-5lu total=%-8lu vbatt=%5u mV%s req=%lu rep=%lu unans=%lu wfail=%lu ovf=%lu err=%lu\n",
                   (now - start) / 1000.0, quiet ? "QUIET" : "active", tick_frames, total, mv, vs ? "(read failed)" : "",
                   requests, replies, unanswered, write_failures, overflows, other_errors);
            fflush(stdout);
            // Ignition-on traffic (well over 1800 frames/s) means the vehicle is awake again even if the bus never
            // went quiet: start asking again.
            if (passive && tick_frames > 1800) { passive = 0; streak = 0; printf("*** t=%.1fs traffic back to %lu frames/s: requests resume\n", (now - start) / 1000.0, tick_frames); }
            tick_frames = 0; next_tick += 1000.0;
        }
    }
    printf("observer done: %.0f s frames=%lu bus transitions=%u requests=%lu replies=%lu unanswered=%lu write_failures=%lu overflows=%lu read_errors=%lu max_gap_us=%u\n",
           (now_ms() - start) / 1000.0, total, transitions, requests, replies, unanswered, write_failures, overflows, other_errors, max_gap_us);
    for (int c = 1; c < 32; ++c) if (errors_by_code[c]) printf("  read error code %d: %lu times\n", c, errors_by_code[c]);
    const int32_t stopped = PassThruStopMsgFilter(channel, filter);
    report("PassThruStopMsgFilter", stopped);
    return 0;
}
int main(int argc, char **argv) {
    uint32_t device = 0;
    const int scripted = script_main(argc, argv);
    if (scripted < 0 && argc == 3 && strcmp(argv[2], "--vehicle-ignition") == 0) {
        const char *text = getenv("MONGOOSE_OBSERVE_SECONDS");
        const unsigned seconds = text ? (unsigned)strtoul(text, NULL, 10) : 600u;
        uint32_t observed_device = 0, channel = 0;
        if (PassThruOpen(argv[1], &observed_device)) { report("PassThruOpen", 1); return 1; }
        int result = 1;
        if (!PassThruConnect(observed_device, CAN, 0, 500000, &channel)) {
            result = check_vehicle_ignition(observed_device, channel, seconds ? seconds : 600u);
            PassThruDisconnect(channel);
        } else report("PassThruConnect", 1);
        PassThruClose(observed_device);
        return result;
    }
    if (scripted < 0 && argc == 3 && strcmp(argv[2], "--vehicle-hour-iso") == 0) {
        const char *text = getenv("MONGOOSE_SOAK_SECONDS");
        const unsigned seconds = text ? (unsigned)strtoul(text, NULL, 10) : 3600u;
        uint32_t iso_device = 0, channel = 0;
        if (PassThruOpen(argv[1], &iso_device)) { report("PassThruOpen", 1); return 1; }
        int result = 1;
        if (!PassThruConnect(iso_device, ISO15765, 0, 500000, &channel)) {
            result = check_vehicle_hour_iso(channel, seconds ? seconds : 3600u);
            PassThruDisconnect(channel);
        } else report("PassThruConnect", 1);
        PassThruClose(iso_device);
        return result;
    }
    if (scripted >= 0) return scripted;
    const int can_lifecycle = argc == 3 && strcmp(argv[2], "--can-lifecycle") == 0;
    const int can_receive = argc == 3 && strcmp(argv[2], "--can-receive-check") == 0;
    const int can_transmit = argc == 3 && strcmp(argv[2], "--can-transmit-check") == 0;
    const int vehicle_listen = argc == 3 && strcmp(argv[2], "--vehicle-listen") == 0;
    const int vehicle_check = argc == 3 && strcmp(argv[2], "--vehicle-check") == 0;
    if (argc == 3 && strcmp(argv[2], "--vehicle-hour") == 0) {
        const char *text = getenv("MONGOOSE_SOAK_SECONDS");
        const unsigned seconds = text ? (unsigned)strtoul(text, NULL, 10) : 3600u;
        uint32_t hour_device = 0, channel = 0;
        if (PassThruOpen(argv[1], &hour_device)) { report("PassThruOpen", 1); return 1; }
        int result = 1;
        if (!PassThruConnect(hour_device, CAN, 0, 500000, &channel)) {
            result = check_vehicle_hour(channel, seconds ? seconds : 3600u);
            PassThruDisconnect(channel);
        } else report("PassThruConnect", 1);
        PassThruClose(hour_device);
        return result;
    }
    if (argc == 3 && strcmp(argv[2], "--vehicle-cycles") == 0)
        return check_vehicle_cycles(argv[1], 100, 0);
    if (argc == 3 && strcmp(argv[2], "--vehicle-cycles-raw") == 0)
        return check_vehicle_cycles(argv[1], 100, 1);
    const int vehicle_soak = argc == 3 && strcmp(argv[2], "--vehicle-soak") == 0;
    const int vehicle_iso = argc == 3 && strcmp(argv[2], "--vehicle-iso") == 0;
    if (argc > 3 || (argc >= 2 && strncmp(argv[1], "serial:", 7)) ||
        (argc == 3 && !can_lifecycle && !can_receive && !can_transmit &&
         !vehicle_listen && !vehicle_check && !vehicle_iso && !vehicle_soak)) {
        fprintf(stderr, "Usage: mongoose-client [serial:SERIAL [--can-lifecycle|--can-receive-check|"
                        "--can-transmit-check|--vehicle-listen|--vehicle-check|--vehicle-iso|--vehicle-soak|--vehicle-cycles|--vehicle-cycles-raw|--vehicle-hour|--vehicle-hour-iso|--vehicle-ignition]]\n"
                        "       mongoose-client [serial:SERIAL] --script FILE [--gap MS]\n"
                        "       mongoose-client --script-check FILE...\n"); return 2;
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
