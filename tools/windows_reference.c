/* Build for Win32: supplied monpj432.dll is a 32-bit PE, even in x64 installer.
 * stdout is a timestamped reference log. USBPcap capture runs separately. */
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
static ErrorFn get_error;
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
int main(int argc, char **argv) {
    uint32_t cycles = 1, baud = 500000, flags = 0;
    int can = 0, failed = 0;
    const char *tx = NULL;
    if (argc < 2) {
        fprintf(stderr, "Usage: mongoose-reference C:\\absolute\\monpj432.dll [--cycles N] [--can] [--baud N] [--flags N] [--tx HEX]\n"); return 2;
    }
    for (int i = 2; i < argc; ++i) {
        if (!strcmp(argv[i], "--can")) can = 1;
        else if (i+1 < argc && !strcmp(argv[i], "--cycles")) cycles = number(argv[++i]);
        else if (i+1 < argc && !strcmp(argv[i], "--baud")) baud = number(argv[++i]);
        else if (i+1 < argc && !strcmp(argv[i], "--flags")) flags = number(argv[++i]);
        else if (i+1 < argc && !strcmp(argv[i], "--tx")) tx = argv[++i];
        else { fprintf(stderr, "Invalid option: %s\n", argv[i]); return 2; }
    }
    if (!cycles || cycles > 100 || (tx && !can) || !baud || (flags & ~((uint32_t)CAN_29BIT_ID))) return 2;
    PASSTHRU_MSG transmit = {0}; transmit.ProtocolID = CAN; transmit.TxFlags = flags;
    if (tx) {
        size_t length = strlen(tx);
        if (length < 8 || length > 24 || length % 2) return 2;
        for (size_t i = 0; i < length; i += 2) {
            int hi = digit(tx[i]), lo = digit(tx[i+1]);
            if (hi < 0 || lo < 0) return 2;
            transmit.Data[i/2] = (uint8_t)((hi << 4) | lo);
        }
        transmit.DataSize = (uint32_t)(length/2);
    }
    HMODULE library = LoadLibraryExA(argv[1], NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!library) { fprintf(stderr, "LoadLibrary error=%lu; use absolute path and Win32 executable\n", GetLastError()); return 2; }
    OpenFn open_device; CloseFn close_device, disconnect;
    VersionFn version; ConnectFn connect; FilterFn filter; StopFilterFn stop_filter;
    MessagesFn read_messages, write_messages;
    LOAD(open_device, "PassThruOpen"); LOAD(close_device, "PassThruClose");
    LOAD(version, "PassThruReadVersion"); LOAD(get_error, "PassThruGetLastError");
    LOAD(connect, "PassThruConnect"); LOAD(disconnect, "PassThruDisconnect");
    LOAD(filter, "PassThruStartMsgFilter"); LOAD(stop_filter, "PassThruStopMsgFilter");
    LOAD(read_messages, "PassThruReadMsgs"); LOAD(write_messages, "PassThruWriteMsgs");
    for (uint32_t cycle = 0; cycle < cycles; ++cycle) {
        uint32_t device = 0, channel = 0, filter_id = 0;
        int connected = 0, filtered = 0;
        timestamp(); printf("cycle=%" PRIu32 " Open name=NULL\n", cycle);
        int32_t result = open_device(NULL, &device); log_result("Open", result);
        if (result) { failed = 1; break; }
        printf("device=%" PRIu32 "\n", device);
        char fw[80] = {0}, dll[80] = {0}, api[80] = {0};
        result = version(device, fw, dll, api); log_result("ReadVersion", result);
        if (result) failed = 1;
        else { fw[79] = dll[79] = api[79] = 0; printf("firmware=%s driver=%s api=%s\n", fw, dll, api); }
        if (can) {
            timestamp(); printf("Connect protocol=%u flags=%" PRIu32 " baud=%" PRIu32 "\n", CAN, flags, baud);
            result = connect(device, CAN, flags, baud, &channel); log_result("Connect", result);
            if (result) failed = 1;
            else {
                connected = 1; printf("channel=%" PRIu32 "\n", channel);
                PASSTHRU_MSG mask = {0}, pattern = {0};
                mask.ProtocolID = pattern.ProtocolID = CAN;
                mask.TxFlags = pattern.TxFlags = flags;
                mask.DataSize = pattern.DataSize = 4;
                timestamp(); printf("StartMsgFilter type=PASS mask=00000000 pattern=00000000\n");
                result = filter(channel, PASS_FILTER, &mask, &pattern, NULL, &filter_id); log_result("StartMsgFilter", result);
                if (result) failed = 1;
                else { filtered = 1; printf("filter=%" PRIu32 "\n", filter_id); }
                if (tx && filtered) {
                    uint32_t count = 1; timestamp(); printf("WriteMsgs count=1 timeout=1000 data=%s\n", tx);
                    result = write_messages(channel, &transmit, &count, 1000); log_result("WriteMsgs", result);
                    printf("written=%" PRIu32 "\n", count); if (result) failed = 1;
                }
                PASSTHRU_MSG received[16] = {0}; uint32_t count = 16;
                timestamp(); printf("ReadMsgs count=16 timeout=1000\n");
                result = read_messages(channel, received, &count, 1000); log_result("ReadMsgs", result);
                printf("received=%" PRIu32 "\n", count);
                if (count > 16) { failed = 1; count = 16; }
                for (uint32_t i = 0; i < count; ++i) {
                    printf("message protocol=%" PRIu32 " rx=%" PRIu32 " tx=%" PRIu32 " timestamp=%" PRIu32 " size=%" PRIu32 " extra=%" PRIu32 " data=",
                           received[i].ProtocolID, received[i].RxStatus, received[i].TxFlags, received[i].Timestamp,
                           received[i].DataSize, received[i].ExtraDataIndex);
                    uint32_t size = received[i].DataSize;
                    if (size > sizeof(received[i].Data)) { failed = 1; size = sizeof(received[i].Data); }
                    for (uint32_t j = 0; j < size; ++j) printf("%02x", received[i].Data[j]);
                    printf("\n");
                }
            }
        }
        if (filtered) { result = stop_filter(channel, filter_id); log_result("StopMsgFilter", result); if (result) failed = 1; }
        if (connected) { result = disconnect(channel); log_result("Disconnect", result); if (result) failed = 1; }
        result = close_device(device); log_result("Close", result); if (result) failed = 1;
        if (failed) break;
    }
    FreeLibrary(library); return failed ? 1 : 0;
}
