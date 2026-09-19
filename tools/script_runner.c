/* Linux step interpreter for the tools/scripts step files, kept in step with windows_reference.c so
 * one script file drives both stacks and the two logs (and the wire captured beneath them)
 * can be compared line for line. The verbs, argument order, symbol tables, limits and
 * "key=value" output deliberately match that file; if you change one, change the other.
 *
 * Differences from the Windows harness, all deliberate:
 *   - it calls the native library directly instead of loading monpj432.dll;
 *   - the timestamp is the same unit (100 ns since 1601) taken from CLOCK_REALTIME;
 *   - dry mode parses without calling the library, so the script corpus can be checked
 *     in CTest with no adapter;
 *   - parse errors name the file and line.
 * PassThruSetProgrammingVoltage has no step here either: driving voltage onto an OBD pin
 * of a live vehicle is not a read-only act. */
#include "script_runner.h"
#include "mongoose/j2534.h"
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const char *script_path;
static int script_line;
static int dry_run;
static const char *open_name;

static void complain(const char *format, ...) __attribute__((format(printf, 1, 2)));
static void complain(const char *format, ...) {
    va_list args;
    fprintf(stderr, "%s:%d: ", script_path, script_line);
    va_start(args, format); vfprintf(stderr, format, args); va_end(args);
    fputc('\n', stderr);
}
static void die(const char *format, ...) __attribute__((format(printf, 1, 2), noreturn));
static void die(const char *format, ...) {
    va_list args;
    fprintf(stderr, "%s:%d: ", script_path, script_line);
    va_start(args, format); vfprintf(stderr, format, args); va_end(args);
    fputc('\n', stderr);
    exit(2);
}

static uint64_t monotonic_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000u + (uint64_t)t.tv_nsec / 1000000u;
}
static void sleep_ms(uint32_t ms) {
    struct timespec t = {(time_t)(ms / 1000u), (long)(ms % 1000u) * 1000000L};
    while (nanosleep(&t, &t) == -1) {}
}
static void timestamp(void) {
    struct timespec t;
    clock_gettime(CLOCK_REALTIME, &t);
    /* Windows FILETIME: 100 ns ticks since 1601-01-01. */
    const uint64_t ticks = ((uint64_t)t.tv_sec + 11644473600ull) * 10000000ull + (uint64_t)t.tv_nsec / 100u;
    printf("utc_100ns=%" PRIu64 " ", ticks);
}
static void log_result(const char *operation, int32_t result) {
    char error[80] = {0};
    if (result) PassThruGetLastError(error);
    error[79] = 0;
    timestamp();
    printf("%s result=%" PRId32 " error=%s\n", operation, result, error);
    fflush(stdout);
}

static uint32_t number(const char *text) {
    char *end;
    const unsigned long long value = strtoull(text, &end, 0);
    if (!*text || *end || value > UINT32_MAX) die("invalid number: %s", text);
    return (uint32_t)value;
}
static int digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
static int eq(const char *a, const char *b) {
    for (; *a && *b; ++a, ++b) {
        const int ca = (*a >= 'A' && *a <= 'Z') ? *a + 32 : *a;
        const int cb = (*b >= 'A' && *b <= 'Z') ? *b + 32 : *b;
        if (ca != cb) return 0;
    }
    return *a == *b;
}

struct named { const char *name; uint32_t value; };
/* CAN_PS/ISO15765_PS are the adapter's own protocol IDs for the pair relevant to the XC60
 * (PROTOCOL.md section 9); they are not the SAE numbers and must not be mixed. */
static const struct named protocols[] = {
    {"J1850VPW", J1850VPW}, {"J1850PWM", J1850PWM}, {"ISO9141", ISO9141},
    {"ISO14230", ISO14230}, {"CAN", CAN}, {"ISO15765", ISO15765},
    {"CAN_PS", 0x8004}, {"ISO15765_PS", 0x8005}, {NULL, 0}
};
static const struct named filter_types[] = {
    {"PASS", PASS_FILTER}, {"BLOCK", BLOCK_FILTER}, {"FLOW", FLOW_CONTROL_FILTER}, {NULL, 0}
};
static const struct named ioctls[] = {
    {"GET_CONFIG", GET_CONFIG}, {"SET_CONFIG", SET_CONFIG}, {"READ_VBATT", READ_VBATT},
    {"CLEAR_TX_BUFFER", CLEAR_TX_BUFFER}, {"CLEAR_RX_BUFFER", CLEAR_RX_BUFFER},
    {"CLEAR_PERIODIC_MSGS", CLEAR_PERIODIC_MSGS}, {"CLEAR_MSG_FILTERS", CLEAR_MSG_FILTERS},
    {"READ_PROG_VOLTAGE", READ_PROG_VOLTAGE}, {NULL, 0}
};
static const struct named tx_flags[] = {
    {"NONE", 0}, {"CAN_29BIT_ID", CAN_29BIT_ID},
    {"ISO15765_FRAME_PAD", ISO15765_FRAME_PAD}, {"ISO15765_ADDR_TYPE", ISO15765_ADDR_TYPE},
    {NULL, 0}
};
static uint32_t lookup(const struct named *table, const char *text) {
    for (const struct named *entry = table; entry->name; ++entry)
        if (eq(entry->name, text)) return entry->value;
    return number(text);
}
/* Flag words may be OR-ed with a vertical bar. */
static uint32_t lookup_flags(const struct named *table, const char *text) {
    char buffer[160];
    uint32_t value = 0;
    const size_t length = strlen(text);
    if (length >= sizeof(buffer)) die("flags too long: %s", text);
    memcpy(buffer, text, length + 1);
    char *next;
    for (char *part = buffer; part; part = next) {
        next = strchr(part, '|');
        if (next) *next++ = 0;
        value |= lookup(table, part);
    }
    return value;
}

#define SLOTS 64
static struct { char name[24]; uint32_t handle; } slots[SLOTS];
static int slot_count;
static void slot_set(const char *name, uint32_t handle) {
    if (strlen(name) >= sizeof(slots[0].name)) die("handle name too long: %s", name);
    for (int i = 0; i < slot_count; ++i)
        if (eq(slots[i].name, name)) { slots[i].handle = handle; return; }
    if (slot_count == SLOTS) die("too many handles");
    strcpy(slots[slot_count].name, name);
    slots[slot_count].handle = handle;
    ++slot_count;
}
/* A bare number is used verbatim, so error-path scripts can hand the library a
 * deliberately invalid device, channel or filter ID. */
static uint32_t slot_get(const char *name) {
    for (int i = 0; i < slot_count; ++i)
        if (eq(slots[i].name, name)) return slots[i].handle;
    if (name[0] >= '0' && name[0] <= '9') return number(name);
    die("unknown handle: %s", name);
}

/* First four bytes of a CAN or ISO15765 message are the big-endian identifier, so the
 * script supplies the whole Data field as hex. */
static void set_data(PASSTHRU_MSG *message, const char *hex) {
    const size_t length = strlen(hex);
    if (length % 2 || length / 2 > sizeof(message->Data)) die("bad hex: %s", hex);
    for (size_t i = 0; i < length; i += 2) {
        const int hi = digit(hex[i]), lo = digit(hex[i + 1]);
        if (hi < 0 || lo < 0) die("bad hex: %s", hex);
        message->Data[i / 2] = (uint8_t)((hi << 4) | lo);
    }
    message->DataSize = (uint32_t)(length / 2);
}

/* Sustained-load accounting for "readstats". Printing every message at these rates is
 * useless, so summarise: throughput, whether anything is dropped, which RxStatus appears. */
#define STATUS_SLOTS 32
#define ID_SLOTS 4096  /* power of two, open addressing */
struct stats {
    uint64_t total, rounds, empty_rounds;
    uint32_t status_value[STATUS_SLOTS], status_count[STATUS_SLOTS];
    unsigned statuses;
    uint32_t ids[ID_SLOTS];
    int id_used[ID_SLOTS];
    unsigned unique_ids;
    uint32_t first_timestamp, last_timestamp, max_gap_us;
    uint64_t gaps_over_10ms;
    int seen_timestamp;
};
static void note_status(struct stats *s, uint32_t value) {
    for (unsigned i = 0; i < s->statuses; ++i)
        if (s->status_value[i] == value) { ++s->status_count[i]; return; }
    if (s->statuses < STATUS_SLOTS) {
        s->status_value[s->statuses] = value;
        s->status_count[s->statuses] = 1;
        ++s->statuses;
    }
}
static void note_id(struct stats *s, uint32_t id) {
    if (s->unique_ids >= ID_SLOTS / 2) return;  /* keep the table sparse */
    uint32_t slot = (id * 2654435761u) & (ID_SLOTS - 1);
    while (s->id_used[slot]) {
        if (s->ids[slot] == id) return;
        slot = (slot + 1) & (ID_SLOTS - 1);
    }
    s->id_used[slot] = 1;
    s->ids[slot] = id;
    ++s->unique_ids;
}
static void note_message(struct stats *s, const PASSTHRU_MSG *m) {
    ++s->total;
    note_status(s, m->RxStatus);
    if (m->DataSize >= 4)
        note_id(s, ((uint32_t)m->Data[0] << 24) | ((uint32_t)m->Data[1] << 16) |
                   ((uint32_t)m->Data[2] << 8) | m->Data[3]);
    if (!s->seen_timestamp) { s->first_timestamp = m->Timestamp; s->seen_timestamp = 1; }
    else {
        /* Unsigned microseconds, monotonic within a session, so a plain difference is
         * meaningful; a large step means traffic was dropped in between. */
        const uint32_t gap = m->Timestamp - s->last_timestamp;
        if (gap > s->max_gap_us) s->max_gap_us = gap;
        if (gap > 10000) ++s->gaps_over_10ms;
    }
    s->last_timestamp = m->Timestamp;
}
static void print_stats(const struct stats *s, uint32_t seconds) {
    const uint32_t span = s->seen_timestamp ? (s->last_timestamp - s->first_timestamp) : 0;
    printf("stats_total=%" PRIu64 " rounds=%" PRIu64 " empty_rounds=%" PRIu64 " requested_seconds=%" PRIu32 "\n",
           s->total, s->rounds, s->empty_rounds, seconds);
    printf("stats_device_span_us=%" PRIu32 " rate_msg_per_s=%.1f\n",
           span, span ? (double)s->total * 1e6 / (double)span : 0.0);
    printf("stats_unique_can_ids=%u\n", s->unique_ids);
    printf("stats_max_gap_us=%" PRIu32 " gaps_over_10ms=%" PRIu64 "\n", s->max_gap_us, s->gaps_over_10ms);
    for (unsigned i = 0; i < s->statuses; ++i)
        printf("stats_rxstatus 0x%08" PRIx32 " = %" PRIu32 "\n", s->status_value[i], s->status_count[i]);
    fflush(stdout);
}
static void print_messages(const PASSTHRU_MSG *messages, uint32_t count) {
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t size = messages[i].DataSize;
        if (size > sizeof(messages[i].Data)) size = (uint32_t)sizeof(messages[i].Data);
        printf("message protocol=%" PRIu32 " rx=%" PRIu32 " tx=%" PRIu32 " timestamp=%" PRIu32
               " size=%" PRIu32 " extra=%" PRIu32 " data=",
               messages[i].ProtocolID, messages[i].RxStatus, messages[i].TxFlags,
               messages[i].Timestamp, messages[i].DataSize, messages[i].ExtraDataIndex);
        for (uint32_t j = 0; j < size; ++j) printf("%02x", messages[i].Data[j]);
        printf("\n");
    }
    fflush(stdout);
}

/* In a dry run every library call becomes a success that produced nothing. */
#define CALL(expression) (dry_run ? 0 : (expression))
#define NEED(n) do { if (count < (n)) { complain("step %s needs %d arguments", token[0], (n) - 1); return 1; } } while (0)
#define MAX_TOKENS 12

static int run_step(char **token, int count, uint32_t gap) {
    int32_t result = 0;
    const char *verb = token[0];

    if (eq(verb, "sleep")) { NEED(2); const uint32_t ms = number(token[1]); if (!dry_run) sleep_ms(ms); return 0; }
    if (eq(verb, "mark")) return 0;  /* already echoed by the step line */

    if (eq(verb, "open")) {  /* open HANDLE */
        uint32_t device = 1;
        NEED(2);
        result = CALL(PassThruOpen((void *)open_name, &device));
        if (!dry_run) log_result("Open", result);
        if (result) return 1;
        if (!dry_run) printf("device=%" PRIu32 "\n", device);
        slot_set(token[1], device);
    } else if (eq(verb, "close")) {  /* close HANDLE */
        NEED(2);
        const uint32_t device = slot_get(token[1]);
        result = CALL(PassThruClose(device));
        if (!dry_run) log_result("Close", result);
        if (result) return 1;
    } else if (eq(verb, "version")) {  /* version HANDLE */
        char firmware[80] = {0}, driver[80] = {0}, api[80] = {0};
        NEED(2);
        const uint32_t device = slot_get(token[1]);
        result = CALL(PassThruReadVersion(device, firmware, driver, api));
        if (!dry_run) log_result("ReadVersion", result);
        if (result) return 1;
        firmware[79] = driver[79] = api[79] = 0;
        if (!dry_run) printf("firmware=%s driver=%s api=%s\n", firmware, driver, api);
    } else if (eq(verb, "connect")) {  /* connect DEV PROTOCOL FLAGS BAUD CHAN */
        uint32_t channel = 1;
        NEED(6);
        const uint32_t protocol = lookup(protocols, token[2]);
        const uint32_t flags = lookup_flags(tx_flags, token[3]);
        const uint32_t baud = number(token[4]);
        const uint32_t device = slot_get(token[1]);
        if (!dry_run) printf("connect protocol=%" PRIu32 " flags=%" PRIu32 " baud=%" PRIu32 "\n", protocol, flags, baud);
        result = CALL(PassThruConnect(device, protocol, flags, baud, &channel));
        if (!dry_run) log_result("Connect", result);
        if (result) return 1;
        if (!dry_run) printf("channel=%" PRIu32 "\n", channel);
        slot_set(token[5], channel);
    } else if (eq(verb, "disconnect")) {  /* disconnect CHAN */
        NEED(2);
        const uint32_t channel = slot_get(token[1]);
        result = CALL(PassThruDisconnect(channel));
        if (!dry_run) log_result("Disconnect", result);
        if (result) return 1;
    } else if (eq(verb, "filter")) {  /* filter CHAN TYPE PROTOCOL FLAGS MASK PATTERN [FLOW] NAME */
        uint32_t id = 1;
        static PASSTHRU_MSG mask, pattern, flow;  /* 4 KiB each; keep them off the stack */
        memset(&mask, 0, sizeof(mask)); memset(&pattern, 0, sizeof(pattern)); memset(&flow, 0, sizeof(flow));
        NEED(8);
        const uint32_t type = lookup(filter_types, token[2]);
        const uint32_t protocol = lookup(protocols, token[3]);
        const uint32_t flags = lookup_flags(tx_flags, token[4]);
        const int has_flow = (type == FLOW_CONTROL_FILTER);
        if (has_flow) NEED(9);
        mask.ProtocolID = pattern.ProtocolID = flow.ProtocolID = protocol;
        mask.TxFlags = pattern.TxFlags = flow.TxFlags = flags;
        set_data(&mask, token[5]);
        set_data(&pattern, token[6]);
        if (has_flow) set_data(&flow, token[7]);
        const uint32_t channel = slot_get(token[1]);
        result = CALL(PassThruStartMsgFilter(channel, type, &mask, &pattern, has_flow ? &flow : NULL, &id));
        if (!dry_run) log_result("StartMsgFilter", result);
        if (result) return 1;
        if (!dry_run) printf("filter=%" PRIu32 "\n", id);
        slot_set(token[has_flow ? 8 : 7], id);
    } else if (eq(verb, "stopfilter")) {  /* stopfilter CHAN FILTER */
        NEED(3);
        const uint32_t channel = slot_get(token[1]), filter = slot_get(token[2]);
        result = CALL(PassThruStopMsgFilter(channel, filter));
        if (!dry_run) log_result("StopMsgFilter", result);
        if (result) return 1;
    } else if (eq(verb, "readstats")) {  /* readstats CHAN COUNT TIMEOUT SECONDS */
        NEED(5);
        const uint32_t channel = slot_get(token[1]);
        const uint32_t wanted = number(token[2]), timeout = number(token[3]), seconds = number(token[4]);
        if (!wanted || wanted > 1024) { complain("readstats count must be 1..1024"); return 1; }
        if (!dry_run) {
            PASSTHRU_MSG *buffer = calloc(wanted, sizeof(*buffer));
            struct stats *s = calloc(1, sizeof(*s));
            if (!buffer || !s) { free(buffer); free(s); complain("out of memory"); return 1; }
            const uint64_t started = monotonic_ms();
            int samples = 0, failed_step = 0;
            do {
                uint32_t got = wanted;
                result = PassThruReadMsgs(channel, buffer, &got, timeout);
                if (got > wanted) { complain("ReadMsgs returned %" PRIu32, got); failed_step = 1; break; }
                ++s->rounds;
                if (!got) ++s->empty_rounds;
                for (uint32_t i = 0; i < got; ++i) {
                    note_message(s, &buffer[i]);
                    if (samples < 8) { print_messages(&buffer[i], 1); ++samples; }
                }
                if (result && result != ERR_BUFFER_EMPTY && result != ERR_TIMEOUT) {
                    log_result("ReadMsgs", result); failed_step = 1; break;
                }
            } while ((monotonic_ms() - started) < (uint64_t)seconds * 1000u);
            print_stats(s, seconds);
            free(buffer); free(s);
            if (failed_step) return 1;
        }
    } else if (eq(verb, "read") || eq(verb, "readfor")) {
        /* read CHAN COUNT TIMEOUT | readfor CHAN COUNT TIMEOUT SECONDS */
        uint32_t seconds = 0, rounds = 0;
        uint64_t total = 0;
        NEED(4);
        const uint32_t channel = slot_get(token[1]);
        const uint32_t wanted = number(token[2]), timeout = number(token[3]);
        if (eq(verb, "readfor")) { NEED(5); seconds = number(token[4]); }
        if (!wanted || wanted > 256) { complain("read count must be 1..256"); return 1; }
        if (!dry_run) {
            PASSTHRU_MSG *buffer = calloc(wanted, sizeof(*buffer));
            if (!buffer) { complain("out of memory"); return 1; }
            const uint64_t started = monotonic_ms();
            do {
                uint32_t got = wanted;
                memset(buffer, 0, wanted * sizeof(*buffer));
                result = PassThruReadMsgs(channel, buffer, &got, timeout);
                log_result("ReadMsgs", result);
                if (got > wanted) { complain("ReadMsgs returned %" PRIu32, got); free(buffer); return 1; }
                printf("received=%" PRIu32 "\n", got);
                print_messages(buffer, got);
                total += got; ++rounds;
                /* An empty buffer is a normal outcome for a timed read, not a failure. */
                if (result && result != ERR_BUFFER_EMPTY && result != ERR_TIMEOUT) { free(buffer); return 1; }
            } while (seconds && (monotonic_ms() - started) < (uint64_t)seconds * 1000u);
            printf("read_total=%" PRIu64 " rounds=%" PRIu32 "\n", total, rounds);
            free(buffer);
        }
    } else if (eq(verb, "write")) {  /* write CHAN PROTOCOL FLAGS HEX TIMEOUT */
        static PASSTHRU_MSG message;
        memset(&message, 0, sizeof(message));
        uint32_t sent = 1;
        NEED(6);
        message.ProtocolID = lookup(protocols, token[2]);
        message.TxFlags = lookup_flags(tx_flags, token[3]);
        set_data(&message, token[4]);
        const uint32_t timeout = number(token[5]);
        const uint32_t channel = slot_get(token[1]);
        result = CALL(PassThruWriteMsgs(channel, &message, &sent, timeout));
        if (!dry_run) {
            log_result("WriteMsgs", result);
            printf("written=%" PRIu32 "\n", sent);
        }
        if (result) return 1;
    } else if (eq(verb, "periodic")) {  /* periodic CHAN PROTOCOL FLAGS HEX INTERVAL_MS NAME */
        static PASSTHRU_MSG message;
        memset(&message, 0, sizeof(message));
        uint32_t id = 1;
        NEED(7);
        message.ProtocolID = lookup(protocols, token[2]);
        message.TxFlags = lookup_flags(tx_flags, token[3]);
        set_data(&message, token[4]);
        const uint32_t interval = number(token[5]);
        const uint32_t channel = slot_get(token[1]);
        result = CALL(PassThruStartPeriodicMsg(channel, &message, &id, interval));
        if (!dry_run) log_result("StartPeriodicMsg", result);
        if (result) return 1;
        if (!dry_run) printf("periodic=%" PRIu32 "\n", id);
        slot_set(token[6], id);
    } else if (eq(verb, "stopperiodic")) {  /* stopperiodic CHAN NAME */
        NEED(3);
        const uint32_t channel = slot_get(token[1]), periodic = slot_get(token[2]);
        result = CALL(PassThruStopPeriodicMsg(channel, periodic));
        if (!dry_run) log_result("StopPeriodicMsg", result);
        if (result) return 1;
    } else if (eq(verb, "getconfig")) {  /* getconfig CHAN PARAM[,PARAM...] */
        SCONFIG parameters[64];
        uint32_t total = 0;
        char buffer[512];
        NEED(3);
        const size_t length = strlen(token[2]);
        if (length >= sizeof(buffer)) { complain("parameter list too long"); return 1; }
        memcpy(buffer, token[2], length + 1);
        char *next;
        for (char *part = buffer; part && total < 64; part = next) {
            next = strchr(part, ',');
            if (next) *next++ = 0;
            parameters[total].Parameter = number(part);
            parameters[total].Value = 0;
            ++total;
        }
        SCONFIG_LIST list = {total, parameters};
        const uint32_t channel = slot_get(token[1]);
        result = CALL(PassThruIoctl(channel, GET_CONFIG, &list, NULL));
        if (!dry_run) {
            log_result("Ioctl GET_CONFIG", result);
            for (uint32_t i = 0; i < total; ++i)
                printf("config parameter=0x%" PRIx32 " value=0x%" PRIx32 "\n", parameters[i].Parameter, parameters[i].Value);
            fflush(stdout);
        }
        /* A rejected selector is itself a finding; keep going. */
    } else if (eq(verb, "setconfig")) {  /* setconfig CHAN PARAM=VALUE[,...] */
        SCONFIG parameters[64];
        uint32_t total = 0;
        char buffer[512];
        NEED(3);
        const size_t length = strlen(token[2]);
        if (length >= sizeof(buffer)) { complain("parameter list too long"); return 1; }
        memcpy(buffer, token[2], length + 1);
        char *next;
        for (char *part = buffer; part && total < 64; part = next) {
            next = strchr(part, ',');
            if (next) *next++ = 0;
            char *equals = strchr(part, '=');
            if (!equals) { complain("setconfig needs PARAM=VALUE"); return 1; }
            *equals++ = 0;
            parameters[total].Parameter = number(part);
            parameters[total].Value = number(equals);
            ++total;
        }
        SCONFIG_LIST list = {total, parameters};
        const uint32_t channel = slot_get(token[1]);
        result = CALL(PassThruIoctl(channel, SET_CONFIG, &list, NULL));
        if (!dry_run) log_result("Ioctl SET_CONFIG", result);
        if (result) return 1;
    } else if (eq(verb, "vbatt")) {  /* vbatt DEV */
        uint32_t millivolts = 0;
        NEED(2);
        const uint32_t device = slot_get(token[1]);
        result = CALL(PassThruIoctl(device, READ_VBATT, NULL, &millivolts));
        if (!dry_run) log_result("Ioctl READ_VBATT", result);
        if (result) return 1;
        if (!dry_run) printf("vbatt_mv=%" PRIu32 "\n", millivolts);
    } else if (eq(verb, "ioctl")) {  /* ioctl HANDLE NAME - argument-free ioctls only */
        NEED(3);
        const uint32_t id = lookup(ioctls, token[2]);
        if (id == GET_CONFIG || id == SET_CONFIG || id == READ_VBATT || id == READ_PROG_VOLTAGE) {
            complain("use getconfig/setconfig/vbatt for %s", token[2]);
            return 1;
        }
        const uint32_t handle = slot_get(token[1]);
        result = CALL(PassThruIoctl(handle, id, NULL, NULL));
        if (!dry_run) log_result("Ioctl", result);
        if (result) return 1;
    } else {
        complain("unknown step: %s", verb);
        return 1;
    }
    if (gap && !dry_run) sleep_ms(gap);
    return 0;
}

int script_run(const char *path, const char *device_name, uint32_t gap_ms, int dry) {
    FILE *file = fopen(path, "r");
    char line[512];
    int failed = 0;
    if (!file) { fprintf(stderr, "cannot open script %s\n", path); return 2; }
    script_path = path; script_line = 0; dry_run = dry; open_name = device_name; slot_count = 0;
    while (fgets(line, sizeof(line), file)) {
        char *tokens[MAX_TOKENS];
        int count = 0;
        ++script_line;
        // A line longer than the buffer would be read as several, and the tail of a long comment would then run
        // as a command. Refuse it, and a NUL that would hide the rest of a line, before anything is executed.
        const size_t length = strlen(line);
        if (length == sizeof(line) - 1 && line[length - 1] != '\n' && !feof(file)) {
            complain("line too long (over %d characters)", (int)sizeof(line) - 2);
            failed = 1; break;
        }
        if (length && line[length - 1] != '\n' && !feof(file)) { complain("line has an embedded NUL"); failed = 1; break; }
        char *hash = strchr(line, '#');
        if (hash) *hash = 0;
        for (char *cursor = strtok(line, " \t\r\n"); cursor && count < MAX_TOKENS; cursor = strtok(NULL, " \t\r\n"))
            tokens[count++] = cursor;
        if (!count) continue;
        if (!dry) {
            timestamp();
            printf("step line=%d:", script_line);
            for (int i = 0; i < count; ++i) printf(" %s", tokens[i]);
            printf("\n");
            fflush(stdout);
        }
        if (run_step(tokens, count, gap_ms)) { failed = 1; break; }
    }
    fclose(file);
    return failed;
}
