/* Build for Win32: supplied monpj432.dll is a 32-bit PE, even in x64 installer.
 * stdout is a timestamped reference log. USBPcap capture runs separately.
 *
 * Two modes:
 *   legacy  - the original --cycles/--can/--baud/--flags/--tx flags, preserved
 *             in behaviour so earlier baselines stay reproducible.
 *   script  - --script FILE runs one J2534 call per line from a step file, so
 *             each experiment is a small diffable data file under tools/scripts/
 *             and the API log pairs cleanly with the USBPcap capture.
 *
 * PassThruSetProgrammingVoltage is deliberately never loaded or called: driving
 * voltage onto an OBD pin of a live vehicle is not a read-only act. */
/* strcpy/strtok are used on bounded, locally-owned buffers; MSVC's _s variants
 * are not available to the mingw cross-build documented in docs/CAPTURING.md. */
#define _CRT_SECURE_NO_WARNINGS 1
#include "mongoose/j2534.h"
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

typedef int32_t (J2534_CALL *OpenFn)(void *, uint32_t *);
typedef int32_t (J2534_CALL *CloseFn)(uint32_t);
typedef int32_t (J2534_CALL *VersionFn)(uint32_t, char *, char *, char *);
typedef int32_t (J2534_CALL *ErrorFn)(char *);
typedef int32_t (J2534_CALL *ConnectFn)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t *);
typedef int32_t (J2534_CALL *FilterFn)(uint32_t, uint32_t, PASSTHRU_MSG *, PASSTHRU_MSG *, PASSTHRU_MSG *, uint32_t *);
typedef int32_t (J2534_CALL *StopFilterFn)(uint32_t, uint32_t);
typedef int32_t (J2534_CALL *MessagesFn)(uint32_t, PASSTHRU_MSG *, uint32_t *, uint32_t);
typedef int32_t (J2534_CALL *IoctlFn)(uint32_t, uint32_t, void *, void *);
typedef int32_t (J2534_CALL *PeriodicFn)(uint32_t, PASSTHRU_MSG *, uint32_t *, uint32_t);

static ErrorFn get_error;
static OpenFn api_open;
static CloseFn api_close;
static VersionFn api_version;
static ConnectFn api_connect;
static CloseFn api_disconnect;
static FilterFn api_filter;
static StopFilterFn api_stop_filter;
static MessagesFn api_read, api_write;
static IoctlFn api_ioctl;
static PeriodicFn api_start_periodic;
static StopFilterFn api_stop_periodic;   /* same signature: (channel, id) */

static void timestamp(void) {
    FILETIME time; ULARGE_INTEGER ticks;
    GetSystemTimeAsFileTime(&time); ticks.LowPart = time.dwLowDateTime; ticks.HighPart = time.dwHighDateTime;
    printf("utc_100ns=%" PRIu64 " ", (uint64_t)ticks.QuadPart);
}
static void log_result(const char *operation, int32_t result) {
    char error[80] = {0};
    if (result && get_error) get_error(error);
    error[79] = 0; timestamp(); printf("%s result=%" PRId32 " error=%s\n", operation, result, error); fflush(stdout);
}
static void load(HMODULE library, const char *name, void *destination, size_t size) {
    FARPROC symbol = GetProcAddress(library, name);
    if (!symbol || size != sizeof(symbol)) { fprintf(stderr, "Cannot load %s\n", name); exit(2); }
    memcpy(destination, &symbol, size);
}
#define LOAD(variable, symbol) load(library, symbol, &variable, sizeof(variable))
static uint32_t number(const char *text) {
    char *end; unsigned long long value = strtoull(text, &end, 0);
    if (!*text || *end || value > UINT32_MAX) { fprintf(stderr, "Invalid number: %s\n", text); exit(2); }
    return (uint32_t)value;
}
static int digit(char c) {
    if (c >= '0' && c <= '9') return c-'0';
    if (c >= 'a' && c <= 'f') return c-'a'+10;
    if (c >= 'A' && c <= 'F') return c-'A'+10;
    return -1;
}
/* Case-insensitive compare; avoids _stricmp/strcasecmp so MSVC and mingw agree. */
static int eq(const char *a, const char *b) {
    for (; *a && *b; ++a, ++b) {
        int ca = (*a >= 'A' && *a <= 'Z') ? *a + 32 : *a;
        int cb = (*b >= 'A' && *b <= 'Z') ? *b + 32 : *b;
        if (ca != cb) return 0;
    }
    return *a == *b;
}

/* ---- symbolic argument tables ------------------------------------------- */
struct named { const char *name; uint32_t value; };
/* Adapter protocol IDs. CAN_PS/ISO15765_PS are the pair relevant to the XC60
 * (PROTOCOL.md section 9); they are NOT the SAE numbers and must not be mixed. */
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
/* Symbol, or a plain number. Flag words may be OR-ed with a vertical bar. */
static uint32_t lookup(const struct named *table, const char *text) {
    const struct named *entry;
    for (entry = table; entry->name; ++entry)
        if (eq(entry->name, text)) return entry->value;
    return number(text);
}
static uint32_t lookup_flags(const struct named *table, const char *text) {
    char buffer[160]; uint32_t value = 0; size_t length = strlen(text);
    char *part, *next;
    if (length >= sizeof(buffer)) { fprintf(stderr, "Flags too long: %s\n", text); exit(2); }
    memcpy(buffer, text, length + 1);
    for (part = buffer; part; part = next) {
        next = strchr(part, 0x7c);
        if (next) *next++ = 0;
        value |= lookup(table, part);
    }
    return value;
}

/* ---- named handle slots -------------------------------------------------- */
#define SLOTS 64
static struct { char name[24]; uint32_t handle; } slots[SLOTS];
static int slot_count;
static void slot_set(const char *name, uint32_t handle) {
    int i;
    if (strlen(name) >= sizeof(slots[0].name)) { fprintf(stderr, "Handle name too long: %s\n", name); exit(2); }
    for (i = 0; i < slot_count; ++i)
        if (eq(slots[i].name, name)) { slots[i].handle = handle; return; }
    if (slot_count == SLOTS) { fprintf(stderr, "Too many handles\n"); exit(2); }
    strcpy(slots[slot_count].name, name); slots[slot_count].handle = handle; ++slot_count;
}
/* A bare number is used verbatim, so error-path scripts can hand the DLL a
 * deliberately invalid device, channel or filter ID. */
static uint32_t slot_get(const char *name) {
    int i;
    for (i = 0; i < slot_count; ++i) if (eq(slots[i].name, name)) return slots[i].handle;
    if (name[0] >= '0' && name[0] <= '9') return number(name);
    fprintf(stderr, "Unknown handle: %s\n", name); exit(2);
}

/* Parse hex into a PASSTHRU_MSG. For CAN and ISO15765 the first four bytes are
 * the big-endian identifier, so the caller supplies the whole field as hex. */
static void set_data(PASSTHRU_MSG *message, const char *hex) {
    size_t length = strlen(hex), i;
    if (length % 2 || length / 2 > sizeof(message->Data)) { fprintf(stderr, "Bad hex: %s\n", hex); exit(2); }
    for (i = 0; i < length; i += 2) {
        int hi = digit(hex[i]), lo = digit(hex[i+1]);
        if (hi < 0 || lo < 0) { fprintf(stderr, "Bad hex: %s\n", hex); exit(2); }
        message->Data[i/2] = (uint8_t)((hi << 4) | lo);
    }
    message->DataSize = (uint32_t)(length / 2);
}
/* Sustained-load accounting (step "readstats"). Printing every message is
 * useless at these rates - a five minute run produced 736512 of them and a 69 MB
 * log - and the questions D4 actually asks are throughput, whether anything is
 * dropped, and which RxStatus bits appear. So summarise instead. */
#define STATUS_SLOTS 32
#define ID_SLOTS 4096          /* power of two, open addressing */
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
        s->status_value[s->statuses] = value; s->status_count[s->statuses] = 1; ++s->statuses;
    }
}
static void note_id(struct stats *s, uint32_t id) {
    if (s->unique_ids >= ID_SLOTS / 2) return;          /* keep the table sparse */
    uint32_t slot = (id * 2654435761u) & (ID_SLOTS - 1);
    while (s->id_used[slot]) {
        if (s->ids[slot] == id) return;
        slot = (slot + 1) & (ID_SLOTS - 1);
    }
    s->id_used[slot] = 1; s->ids[slot] = id; ++s->unique_ids;
}
static void note_message(struct stats *s, const PASSTHRU_MSG *m) {
    ++s->total;
    note_status(s, m->RxStatus);
    if (m->DataSize >= 4)
        note_id(s, ((uint32_t)m->Data[0] << 24) | ((uint32_t)m->Data[1] << 16) |
                   ((uint32_t)m->Data[2] << 8) | m->Data[3]);
    if (!s->seen_timestamp) { s->first_timestamp = m->Timestamp; s->seen_timestamp = 1; }
    else {
        /* The device counter is unsigned microseconds and monotonic within a
         * session, so a plain difference is meaningful. A large step means the
         * adapter or the DLL dropped traffic in between. */
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
    uint32_t i, j;
    for (i = 0; i < count; ++i) {
        uint32_t size = messages[i].DataSize;
        if (size > sizeof(messages[i].Data)) size = (uint32_t)sizeof(messages[i].Data);
        printf("message protocol=%" PRIu32 " rx=%" PRIu32 " tx=%" PRIu32 " timestamp=%" PRIu32
               " size=%" PRIu32 " extra=%" PRIu32 " data=",
               messages[i].ProtocolID, messages[i].RxStatus, messages[i].TxFlags,
               messages[i].Timestamp, messages[i].DataSize, messages[i].ExtraDataIndex);
        for (j = 0; j < size; ++j) printf("%02x", messages[i].Data[j]);
        printf("\n");
    }
    fflush(stdout);
}

/* ---- script mode --------------------------------------------------------- */
#define MAX_TOKENS 12
static int run_step(char **token, int count, uint32_t gap);

static int run_script(const char *path, uint32_t gap) {
    FILE *file = fopen(path, "r");
    char line[512]; int failed = 0, line_number = 0;
    if (!file) { fprintf(stderr, "Cannot open script %s\n", path); return 2; }
    while (fgets(line, sizeof(line), file)) {
        char *tokens[MAX_TOKENS]; int count = 0, i;
        char *hash, *cursor;
        ++line_number;
        hash = strchr(line, 0x23); if (hash) *hash = 0;
        for (cursor = strtok(line, " \t\r\n"); cursor && count < MAX_TOKENS; cursor = strtok(NULL, " \t\r\n"))
            tokens[count++] = cursor;
        if (!count) continue;
        timestamp(); printf("step line=%d:", line_number);
        for (i = 0; i < count; ++i) printf(" %s", tokens[i]);
        printf("\n"); fflush(stdout);
        if (run_step(tokens, count, gap)) { failed = 1; break; }
    }
    fclose(file);
    return failed;
}

#define NEED(n) do { if (count < (n)) { fprintf(stderr, "Step %s needs %d arguments\n", token[0], (n)-1); return 1; } } while (0)

static int run_step(char **token, int count, uint32_t gap) {
    int32_t result = 0;
    const char *verb = token[0];

    if (eq(verb, "sleep")) { NEED(2); Sleep(number(token[1])); return 0; }
    if (eq(verb, "mark"))  { return 0; } /* already echoed by the step line */

    if (eq(verb, "open")) {              /* open HANDLE */
        uint32_t device = 0;
        NEED(2);
        result = api_open(NULL, &device); log_result("Open", result);
        if (result) return 1;
        printf("device=%" PRIu32 "\n", device); slot_set(token[1], device);
    } else if (eq(verb, "close")) {      /* close HANDLE */
        NEED(2);
        result = api_close(slot_get(token[1])); log_result("Close", result);
        if (result) return 1;
    } else if (eq(verb, "version")) {    /* version HANDLE */
        char fw[80] = {0}, dll[80] = {0}, api[80] = {0};
        NEED(2);
        result = api_version(slot_get(token[1]), fw, dll, api); log_result("ReadVersion", result);
        if (result) return 1;
        fw[79] = dll[79] = api[79] = 0;
        printf("firmware=%s driver=%s api=%s\n", fw, dll, api);
    } else if (eq(verb, "connect")) {    /* connect DEV PROTOCOL FLAGS BAUD CHAN */
        uint32_t channel = 0, protocol, flags, baud;
        NEED(6);
        protocol = lookup(protocols, token[2]);
        flags = lookup_flags(tx_flags, token[3]);
        baud = number(token[4]);
        printf("connect protocol=%" PRIu32 " flags=%" PRIu32 " baud=%" PRIu32 "\n", protocol, flags, baud);
        result = api_connect(slot_get(token[1]), protocol, flags, baud, &channel);
        log_result("Connect", result);
        if (result) return 1;
        printf("channel=%" PRIu32 "\n", channel); slot_set(token[5], channel);
    } else if (eq(verb, "disconnect")) { /* disconnect CHAN */
        NEED(2);
        result = api_disconnect(slot_get(token[1])); log_result("Disconnect", result);
        if (result) return 1;
    } else if (eq(verb, "filter")) {     /* filter CHAN TYPE PROTOCOL FLAGS MASK PATTERN [FLOW] NAME */
        uint32_t type, protocol, flags, id = 0;
        int has_flow;
        PASSTHRU_MSG mask = {0}, pattern = {0}, flow = {0};
        NEED(8);
        type = lookup(filter_types, token[2]);
        protocol = lookup(protocols, token[3]);
        flags = lookup_flags(tx_flags, token[4]);
        has_flow = (type == FLOW_CONTROL_FILTER);
        if (has_flow) NEED(9);
        mask.ProtocolID = pattern.ProtocolID = flow.ProtocolID = protocol;
        mask.TxFlags = pattern.TxFlags = flow.TxFlags = flags;
        set_data(&mask, token[5]); set_data(&pattern, token[6]);
        if (has_flow) set_data(&flow, token[7]);
        result = api_filter(slot_get(token[1]), type, &mask, &pattern, has_flow ? &flow : NULL, &id);
        log_result("StartMsgFilter", result);
        if (result) return 1;
        printf("filter=%" PRIu32 "\n", id); slot_set(token[has_flow ? 8 : 7], id);
    } else if (eq(verb, "stopfilter")) { /* stopfilter CHAN FILTER */
        NEED(3);
        result = api_stop_filter(slot_get(token[1]), slot_get(token[2]));
        log_result("StopMsgFilter", result);
        if (result) return 1;
    } else if (eq(verb, "readstats")) {  /* readstats CHAN COUNT TIMEOUT SECONDS */
        NEED(5);
        uint32_t channel = slot_get(token[1]);
        uint32_t wanted = number(token[2]), timeout = number(token[3]), seconds = number(token[4]);
        if (!wanted || wanted > 1024) { fprintf(stderr, "readstats count must be 1..1024\n"); return 1; }
        PASSTHRU_MSG *buffer = calloc(wanted, sizeof(*buffer));
        struct stats *s = calloc(1, sizeof(*s));
        if (!buffer || !s) { free(buffer); free(s); fprintf(stderr, "Out of memory\n"); return 1; }
        const DWORD started = GetTickCount();
        int samples = 0, failed_step = 0;
        do {
            uint32_t got = wanted;
            result = api_read(channel, buffer, &got, timeout);
            if (got > wanted) { fprintf(stderr, "ReadMsgs returned %" PRIu32 "\n", got); failed_step = 1; break; }
            ++s->rounds;
            if (!got) ++s->empty_rounds;
            for (uint32_t i = 0; i < got; ++i) {
                note_message(s, &buffer[i]);
                if (samples < 8) { print_messages(&buffer[i], 1); ++samples; }
            }
            if (result && result != ERR_BUFFER_EMPTY && result != ERR_TIMEOUT) {
                log_result("ReadMsgs", result); failed_step = 1; break;
            }
        } while ((GetTickCount() - started) < seconds * 1000u);
        print_stats(s, seconds);
        free(buffer); free(s);
        if (failed_step) return 1;
    } else if (eq(verb, "read") || eq(verb, "readfor")) {
        /* read CHAN COUNT TIMEOUT | readfor CHAN COUNT TIMEOUT SECONDS */
        uint32_t channel, wanted, timeout, seconds = 0, rounds = 0;
        uint64_t total = 0;
        PASSTHRU_MSG *buffer;
        DWORD started;
        NEED(4);
        channel = slot_get(token[1]);
        wanted = number(token[2]); timeout = number(token[3]);
        if (eq(verb, "readfor")) { NEED(5); seconds = number(token[4]); }
        if (!wanted || wanted > 256) { fprintf(stderr, "read count must be 1..256\n"); return 1; }
        buffer = calloc(wanted, sizeof(*buffer));
        if (!buffer) { fprintf(stderr, "Out of memory\n"); return 1; }
        started = GetTickCount();
        do {
            uint32_t got = wanted;
            memset(buffer, 0, wanted * sizeof(*buffer));
            result = api_read(channel, buffer, &got, timeout);
            log_result("ReadMsgs", result);
            if (got > wanted) { fprintf(stderr, "ReadMsgs returned %" PRIu32 "\n", got); free(buffer); return 1; }
            printf("received=%" PRIu32 "\n", got);
            print_messages(buffer, got);
            total += got; ++rounds;
            /* An empty buffer is a normal outcome for a timed read, not a failure. */
            if (result && result != ERR_BUFFER_EMPTY && result != ERR_TIMEOUT) { free(buffer); return 1; }
        } while (seconds && (GetTickCount() - started) < seconds * 1000u);
        printf("read_total=%" PRIu64 " rounds=%" PRIu32 "\n", total, rounds);
        free(buffer);
    } else if (eq(verb, "write")) {      /* write CHAN PROTOCOL FLAGS HEX TIMEOUT */
        PASSTHRU_MSG message = {0};
        uint32_t sent = 1;
        NEED(6);
        message.ProtocolID = lookup(protocols, token[2]);
        message.TxFlags = lookup_flags(tx_flags, token[3]);
        set_data(&message, token[4]);
        result = api_write(slot_get(token[1]), &message, &sent, number(token[5]));
        log_result("WriteMsgs", result);
        printf("written=%" PRIu32 "\n", sent);
        if (result) return 1;
    } else if (eq(verb, "periodic")) {   /* periodic CHAN PROTOCOL FLAGS HEX INTERVAL_MS NAME */
        PASSTHRU_MSG message = {0};
        uint32_t id = 0;
        NEED(7);
        message.ProtocolID = lookup(protocols, token[2]);
        message.TxFlags = lookup_flags(tx_flags, token[3]);
        set_data(&message, token[4]);
        result = api_start_periodic(slot_get(token[1]), &message, &id, number(token[5]));
        log_result("StartPeriodicMsg", result);
        if (result) return 1;
        printf("periodic=%" PRIu32 "\n", id); slot_set(token[6], id);
    } else if (eq(verb, "stopperiodic")) { /* stopperiodic CHAN NAME */
        NEED(3);
        result = api_stop_periodic(slot_get(token[1]), slot_get(token[2]));
        log_result("StopPeriodicMsg", result);
        if (result) return 1;
    } else if (eq(verb, "getconfig")) {  /* getconfig CHAN PARAM[,PARAM...] */
        SCONFIG parameters[64]; SCONFIG_LIST list;
        uint32_t total = 0, i;
        char buffer[512], *part, *next;
        size_t length;
        NEED(3);
        length = strlen(token[2]);
        if (length >= sizeof(buffer)) { fprintf(stderr, "Parameter list too long\n"); return 1; }
        memcpy(buffer, token[2], length + 1);
        for (part = buffer; part && total < 64; part = next) {
            next = strchr(part, 0x2c); if (next) *next++ = 0;
            parameters[total].Parameter = number(part); parameters[total].Value = 0; ++total;
        }
        list.NumOfParams = total; list.ConfigPtr = parameters;
        result = api_ioctl(slot_get(token[1]), GET_CONFIG, &list, NULL);
        log_result("Ioctl GET_CONFIG", result);
        for (i = 0; i < total; ++i)
            printf("config parameter=0x%" PRIx32 " value=0x%" PRIx32 "\n", parameters[i].Parameter, parameters[i].Value);
        fflush(stdout);
        /* A rejected selector is itself a finding; keep going. */
    } else if (eq(verb, "setconfig")) {  /* setconfig CHAN PARAM=VALUE[,...] */
        SCONFIG parameters[64]; SCONFIG_LIST list;
        uint32_t total = 0;
        char buffer[512], *part, *next;
        size_t length;
        NEED(3);
        length = strlen(token[2]);
        if (length >= sizeof(buffer)) { fprintf(stderr, "Parameter list too long\n"); return 1; }
        memcpy(buffer, token[2], length + 1);
        for (part = buffer; part && total < 64; part = next) {
            char *equals;
            next = strchr(part, 0x2c); if (next) *next++ = 0;
            equals = strchr(part, 0x3d);
            if (!equals) { fprintf(stderr, "setconfig needs PARAM=VALUE\n"); return 1; }
            *equals++ = 0;
            parameters[total].Parameter = number(part); parameters[total].Value = number(equals); ++total;
        }
        list.NumOfParams = total; list.ConfigPtr = parameters;
        result = api_ioctl(slot_get(token[1]), SET_CONFIG, &list, NULL);
        log_result("Ioctl SET_CONFIG", result);
        if (result) return 1;
    } else if (eq(verb, "vbatt")) {      /* vbatt DEV */
        uint32_t millivolts = 0;
        NEED(2);
        result = api_ioctl(slot_get(token[1]), READ_VBATT, NULL, &millivolts);
        log_result("Ioctl READ_VBATT", result);
        if (result) return 1;
        printf("vbatt_mv=%" PRIu32 "\n", millivolts);
    } else if (eq(verb, "ioctl")) {      /* ioctl HANDLE NAME - argument-free ioctls only */
        uint32_t id;
        NEED(3);
        id = lookup(ioctls, token[2]);
        if (id == GET_CONFIG || id == SET_CONFIG || id == READ_VBATT || id == READ_PROG_VOLTAGE) {
            fprintf(stderr, "Use getconfig/setconfig/vbatt for %s\n", token[2]); return 1;
        }
        result = api_ioctl(slot_get(token[1]), id, NULL, NULL);
        log_result("Ioctl", result);
        if (result) return 1;
    } else {
        fprintf(stderr, "Unknown step: %s\n", verb); return 1;
    }
    if (gap) Sleep(gap);
    return 0;
}

/* ---- legacy mode (unchanged behaviour) ----------------------------------- */
static int run_legacy(uint32_t cycles, uint32_t baud, uint32_t flags, int can, const char *tx) {
    int failed = 0;
    uint32_t cycle;
    PASSTHRU_MSG transmit = {0}; transmit.ProtocolID = CAN; transmit.TxFlags = flags;
    if (tx) set_data(&transmit, tx);
    for (cycle = 0; cycle < cycles; ++cycle) {
        uint32_t device = 0, channel = 0, filter_id = 0;
        int connected = 0, filtered = 0;
        int32_t result;
        char fw[80] = {0}, dll[80] = {0}, api[80] = {0};
        timestamp(); printf("cycle=%" PRIu32 " Open name=NULL\n", cycle);
        result = api_open(NULL, &device); log_result("Open", result);
        if (result) { failed = 1; break; }
        printf("device=%" PRIu32 "\n", device);
        result = api_version(device, fw, dll, api); log_result("ReadVersion", result);
        if (result) failed = 1;
        else { fw[79] = dll[79] = api[79] = 0; printf("firmware=%s driver=%s api=%s\n", fw, dll, api); }
        if (can) {
            timestamp(); printf("Connect protocol=%u flags=%" PRIu32 " baud=%" PRIu32 "\n", CAN, flags, baud);
            result = api_connect(device, CAN, flags, baud, &channel); log_result("Connect", result);
            if (result) failed = 1;
            else {
                PASSTHRU_MSG mask = {0}, pattern = {0};
                PASSTHRU_MSG received[16] = {0};
                uint32_t count;
                connected = 1; printf("channel=%" PRIu32 "\n", channel);
                mask.ProtocolID = pattern.ProtocolID = CAN;
                mask.TxFlags = pattern.TxFlags = flags;
                mask.DataSize = pattern.DataSize = 4;
                timestamp(); printf("StartMsgFilter type=PASS mask=00000000 pattern=00000000\n");
                result = api_filter(channel, PASS_FILTER, &mask, &pattern, NULL, &filter_id);
                log_result("StartMsgFilter", result);
                if (result) failed = 1;
                else { filtered = 1; printf("filter=%" PRIu32 "\n", filter_id); }
                if (tx && filtered) {
                    count = 1; timestamp(); printf("WriteMsgs count=1 timeout=1000 data=%s\n", tx);
                    result = api_write(channel, &transmit, &count, 1000); log_result("WriteMsgs", result);
                    printf("written=%" PRIu32 "\n", count); if (result) failed = 1;
                }
                count = 16;
                timestamp(); printf("ReadMsgs count=16 timeout=1000\n");
                result = api_read(channel, received, &count, 1000); log_result("ReadMsgs", result);
                printf("received=%" PRIu32 "\n", count);
                if (count > 16) { failed = 1; count = 16; }
                print_messages(received, count);
            }
        }
        if (filtered) { result = api_stop_filter(channel, filter_id); log_result("StopMsgFilter", result); if (result) failed = 1; }
        if (connected) { result = api_disconnect(channel); log_result("Disconnect", result); if (result) failed = 1; }
        result = api_close(device); log_result("Close", result); if (result) failed = 1;
        if (failed) break;
    }
    return failed;
}

int main(int argc, char **argv) {
    uint32_t cycles = 1, baud = 500000, flags = 0, gap = 0;
    int can = 0, failed, i;
    const char *tx = NULL, *script = NULL;
    HMODULE library;
    if (argc < 2) {
        fprintf(stderr, "Usage: mongoose-reference C:\\absolute\\monpj432.dll\n"
                        "         [--cycles N] [--can] [--baud N] [--flags N] [--tx HEX]\n"
                        "         [--script FILE] [--gap MS]\n");
        return 2;
    }
    for (i = 2; i < argc; ++i) {
        if (!strcmp(argv[i], "--can")) can = 1;
        else if (i+1 < argc && !strcmp(argv[i], "--cycles")) cycles = number(argv[++i]);
        else if (i+1 < argc && !strcmp(argv[i], "--baud")) baud = number(argv[++i]);
        else if (i+1 < argc && !strcmp(argv[i], "--flags")) flags = number(argv[++i]);
        else if (i+1 < argc && !strcmp(argv[i], "--tx")) tx = argv[++i];
        else if (i+1 < argc && !strcmp(argv[i], "--script")) script = argv[++i];
        else if (i+1 < argc && !strcmp(argv[i], "--gap")) gap = number(argv[++i]);
        else { fprintf(stderr, "Invalid option: %s\n", argv[i]); return 2; }
    }
    if (script) {
        if (can || tx || cycles != 1) { fprintf(stderr, "--script is exclusive of the legacy flags\n"); return 2; }
    } else {
        if (!cycles || cycles > 100 || (tx && !can) || !baud || (flags & ~((uint32_t)CAN_29BIT_ID))) return 2;
        if (tx) { size_t length = strlen(tx); if (length < 8 || length > 24 || length % 2) return 2; }
    }
    library = LoadLibraryExA(argv[1], NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!library) { fprintf(stderr, "LoadLibrary error=%lu; use absolute path and Win32 executable\n", GetLastError()); return 2; }
    LOAD(api_open, "PassThruOpen"); LOAD(api_close, "PassThruClose");
    LOAD(api_version, "PassThruReadVersion"); LOAD(get_error, "PassThruGetLastError");
    LOAD(api_connect, "PassThruConnect"); LOAD(api_disconnect, "PassThruDisconnect");
    LOAD(api_filter, "PassThruStartMsgFilter"); LOAD(api_stop_filter, "PassThruStopMsgFilter");
    LOAD(api_read, "PassThruReadMsgs"); LOAD(api_write, "PassThruWriteMsgs");
    LOAD(api_ioctl, "PassThruIoctl");
    LOAD(api_start_periodic, "PassThruStartPeriodicMsg"); LOAD(api_stop_periodic, "PassThruStopPeriodicMsg");
    failed = script ? run_script(script, gap) : run_legacy(cycles, baud, flags, can, tx);
    FreeLibrary(library);
    return failed ? 1 : 0;
}
