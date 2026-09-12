#ifndef MONGOOSE_J2534_H
#define MONGOOSE_J2534_H
#include <stdint.h>
#if defined(_WIN32)
#define J2534_CALL __stdcall
#define J2534_API
#else
#define J2534_CALL
#define J2534_API __attribute__((visibility("default")))
#endif
#ifdef __cplusplus
extern "C" {
#endif
/* Native C ABI: SAE/Windows scalar fields remain 32 bits on LP64 hosts.
 * Pointer-bearing lists use native pointer width. Version buffers need 80
 * bytes each; GetLastError needs 80 bytes. No packed or unaligned fields. */
typedef struct {
    uint32_t ProtocolID, RxStatus, TxFlags, Timestamp, DataSize, ExtraDataIndex;
    uint8_t Data[4128];
} PASSTHRU_MSG;
typedef struct { uint32_t Parameter, Value; } SCONFIG;
typedef struct { uint32_t NumOfParams; SCONFIG *ConfigPtr; } SCONFIG_LIST;
typedef struct { uint32_t NumOfBytes; uint8_t *BytePtr; } SBYTE_ARRAY;
enum {
    STATUS_NOERROR = 0, ERR_NOT_SUPPORTED = 1, ERR_INVALID_CHANNEL_ID = 2,
    ERR_INVALID_PROTOCOL_ID = 3, ERR_NULL_PARAMETER = 4, ERR_INVALID_IOCTL_VALUE = 5,
    ERR_INVALID_FLAGS = 6, ERR_FAILED = 7, ERR_DEVICE_NOT_CONNECTED = 8,
    ERR_TIMEOUT = 9, ERR_INVALID_MSG = 10, ERR_INVALID_TIME_INTERVAL = 11,
    ERR_EXCEEDED_LIMIT = 12, ERR_INVALID_MSG_ID = 13, ERR_DEVICE_IN_USE = 14,
    ERR_INVALID_IOCTL_ID = 15, ERR_BUFFER_EMPTY = 16, ERR_BUFFER_FULL = 17,
    ERR_BUFFER_OVERFLOW = 18, ERR_PIN_INVALID = 19, ERR_CHANNEL_IN_USE = 20,
    ERR_MSG_PROTOCOL_ID = 21, ERR_INVALID_FILTER_ID = 22, ERR_NO_FLOW_CONTROL = 23,
    ERR_NOT_UNIQUE = 24, ERR_INVALID_BAUDRATE = 25, ERR_INVALID_DEVICE_ID = 26
};
enum { J1850VPW = 1, J1850PWM = 2, ISO9141 = 3, ISO14230 = 4, CAN = 5, ISO15765 = 6 };
enum { PASS_FILTER = 1, BLOCK_FILTER = 2, FLOW_CONTROL_FILTER = 3 };
enum { GET_CONFIG = 1, SET_CONFIG = 2, READ_VBATT = 3, FIVE_BAUD_INIT = 4,
       FAST_INIT = 5, CLEAR_TX_BUFFER = 7, CLEAR_RX_BUFFER = 8,
       CLEAR_PERIODIC_MSGS = 9, CLEAR_MSG_FILTERS = 10, READ_PROG_VOLTAGE = 14 };
enum { DATA_RATE = 1, LOOPBACK = 3, CAN_29BIT_ID = 0x100,
       ISO15765_FRAME_PAD = 0x40, ISO15765_ADDR_TYPE = 0x80 };
J2534_API int32_t J2534_CALL PassThruOpen(void *name, uint32_t *device);
J2534_API int32_t J2534_CALL PassThruClose(uint32_t device);
J2534_API int32_t J2534_CALL PassThruConnect(uint32_t device, uint32_t protocol, uint32_t flags, uint32_t baud, uint32_t *channel);
J2534_API int32_t J2534_CALL PassThruDisconnect(uint32_t channel);
J2534_API int32_t J2534_CALL PassThruReadMsgs(uint32_t channel, PASSTHRU_MSG *messages, uint32_t *count, uint32_t timeout);
J2534_API int32_t J2534_CALL PassThruWriteMsgs(uint32_t channel, PASSTHRU_MSG *messages, uint32_t *count, uint32_t timeout);
J2534_API int32_t J2534_CALL PassThruStartPeriodicMsg(uint32_t channel, PASSTHRU_MSG *message, uint32_t *id, uint32_t interval);
J2534_API int32_t J2534_CALL PassThruStopPeriodicMsg(uint32_t channel, uint32_t id);
J2534_API int32_t J2534_CALL PassThruStartMsgFilter(uint32_t channel, uint32_t type, PASSTHRU_MSG *mask, PASSTHRU_MSG *pattern, PASSTHRU_MSG *flow, uint32_t *id);
J2534_API int32_t J2534_CALL PassThruStopMsgFilter(uint32_t channel, uint32_t id);
J2534_API int32_t J2534_CALL PassThruSetProgrammingVoltage(uint32_t device, uint32_t pin, uint32_t voltage);
J2534_API int32_t J2534_CALL PassThruReadVersion(uint32_t device, char *firmware, char *dll, char *api);
J2534_API int32_t J2534_CALL PassThruGetLastError(char *description);
J2534_API int32_t J2534_CALL PassThruIoctl(uint32_t id, uint32_t ioctl_id, void *input, void *output);
#ifdef __cplusplus
}
#endif
#endif
