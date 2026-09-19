/* Bridges the adapter's raw CAN channel to a Linux CAN interface (normally a vcan), so SocketCAN software --
 * can-utils, python-can, udsoncan, the kernel's CAN_ISOTP sockets, SavvyCAN -- can use the adapter unchanged:
 *   mongoose-socketcan [--device NAME] [--bitrate N] [--29bit] [--transmit] [--stats S] IFNAME
 * Every frame the adapter receives is written to IFNAME. Frames other programs write to IFNAME go to the vehicle
 * only with --transmit; without it the bridge is listen-only and counts them as refused. It uses the J2534 API
 * alone, so it gets the library's checks (bit-rate list, identifier widths) for free. See docs/VOLVO.md. */
#include "mongoose/j2534.h"
#include <errno.h>
#include <linux/can.h>
#include <linux/can/raw.h>
#include <net/if.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

enum { batch = 32, read_wait_ms = 100, poll_wait_ms = 200 };

struct options {
    const char *device, *interface;
    uint32_t bitrate, connect_flags;
    int transmit;
    unsigned stats_seconds;
};
struct bridge {
    uint32_t channel;
    int socket;
    int transmit;
    unsigned stats_seconds;
    atomic_int failed;
    atomic_ulong to_bus, to_host, refused, overflows, host_drops;
};

static volatile sig_atomic_t stopping;
static void on_signal(int signal_number) { (void)signal_number; stopping = 1; }

static void report(const char *operation, int32_t status) {
    char error[80] = {0};
    PassThruGetLastError(error);
    fprintf(stderr, "mongoose-socketcan: %s failed: status %d: %s\n", operation, status, error);
}
static int fatal(int32_t status) {
    return status == ERR_DEVICE_NOT_CONNECTED || status == ERR_INVALID_CHANNEL_ID || status == ERR_INVALID_DEVICE_ID;
}
static void usage(void) {
    fprintf(stderr,
            "Usage: mongoose-socketcan [--device NAME] [--bitrate N] [--29bit] [--transmit] [--stats SECONDS] IFNAME\n"
            "  IFNAME     an existing, up CAN interface, normally a vcan:\n"
            "               ip link add dev mongoose0 type vcan && ip link set mongoose0 up\n"
            "  --device   J2534 open name: serial:S, tty:/dev/ttyACMn, ... (default: the one connected adapter)\n"
            "  --bitrate  the vehicle bus rate (default 500000); a wrong rate on a live bus is not harmless\n"
            "  --29bit    open the channel for 29-bit identifiers (transmit accepts both widths either way)\n"
            "  --transmit send frames written to IFNAME onto the vehicle bus (default: listen only)\n"
            "  --stats    print frame counts to stderr every SECONDS\n");
}
static int parse_number(const char *text, unsigned long maximum, unsigned long *value) {
    char *end = NULL;
    errno = 0;
    const unsigned long parsed = strtoul(text, &end, 10);
    if (errno || end == text || *end || text[0] == '-' || parsed > maximum) return 0;
    *value = parsed;
    return 1;
}
static int parse(int argc, char **argv, struct options *options) {
    *options = (struct options){NULL, NULL, 500000, 0, 0, 0};
    for (int i = 1; i < argc; ++i) {
        unsigned long value = 0;
        if (!strcmp(argv[i], "--device") && i + 1 < argc) options->device = argv[++i];
        else if (!strcmp(argv[i], "--bitrate") && i + 1 < argc && parse_number(argv[++i], 1000000, &value) && value)
            options->bitrate = (uint32_t)value;
        else if (!strcmp(argv[i], "--29bit")) options->connect_flags = CAN_29BIT_ID;
        else if (!strcmp(argv[i], "--transmit")) options->transmit = 1;
        else if (!strcmp(argv[i], "--stats") && i + 1 < argc && parse_number(argv[++i], 86400, &value))
            options->stats_seconds = (unsigned)value;
        else if (argv[i][0] != '-' && !options->interface) options->interface = argv[i];
        else return 0;
    }
    return options->interface != NULL;
}
static int open_interface(const char *name) {
    const unsigned index = if_nametoindex(name);
    if (!index) { fprintf(stderr, "mongoose-socketcan: no interface '%s' (create it first, see --help)\n", name); return -1; }
    const int fd = socket(PF_CAN, SOCK_RAW, CAN_RAW);
    if (fd < 0) { perror("mongoose-socketcan: CAN socket"); return -1; }
    struct sockaddr_can address = {0};
    address.can_family = AF_CAN;
    address.can_ifindex = (int)index;
    if (bind(fd, (struct sockaddr *)&address, sizeof address) < 0) {
        fprintf(stderr, "mongoose-socketcan: cannot bind to '%s': %s (is it a CAN interface?)\n", name, strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

/* J2534 raw CAN: four big-endian ID bytes, then up to eight data bytes; RxStatus 0x100 marks a 29-bit ID. */
static int to_socketcan(const PASSTHRU_MSG *message, struct can_frame *frame) {
    if (message->DataSize < 4 || message->DataSize > 12 || (message->RxStatus & TX_MSG_TYPE)) return 0;
    memset(frame, 0, sizeof *frame);
    const uint32_t id = (uint32_t)message->Data[0] << 24 | (uint32_t)message->Data[1] << 16 |
                        (uint32_t)message->Data[2] << 8 | message->Data[3];
    frame->can_id = message->RxStatus & CAN_29BIT_ID ? (id & CAN_EFF_MASK) | CAN_EFF_FLAG : id & CAN_SFF_MASK;
    frame->len = (uint8_t)(message->DataSize - 4);
    memcpy(frame->data, &message->Data[4], frame->len);
    return 1;
}
static int from_socketcan(const struct can_frame *frame, PASSTHRU_MSG *message) {
    if (frame->can_id & (CAN_RTR_FLAG | CAN_ERR_FLAG) || frame->len > 8) return 0;  /* J2534 has no remote frames */
    memset(message, 0, sizeof *message);
    const int extended = (frame->can_id & CAN_EFF_FLAG) != 0;
    const uint32_t id = frame->can_id & (extended ? CAN_EFF_MASK : CAN_SFF_MASK);
    message->ProtocolID = CAN;
    message->TxFlags = extended ? CAN_29BIT_ID : 0;
    message->Data[0] = (uint8_t)(id >> 24); message->Data[1] = (uint8_t)(id >> 16);
    message->Data[2] = (uint8_t)(id >> 8); message->Data[3] = (uint8_t)id;
    memcpy(&message->Data[4], frame->data, frame->len);
    message->DataSize = 4u + frame->len;
    return 1;
}

/* Adapter to interface. A timed read of 32 would sit out its whole timeout whenever fewer arrive, adding up to
 * that much latency to every ISO-TP exchange, so drain what is queued without waiting and only block for a single
 * frame when there is none. The library queues 4096 frames per channel, which covers a busy bus (about 2450
 * frames/s on the car this was developed on). */
static void *vehicle_to_host(void *argument) {
    struct bridge *bridge = argument;
    static PASSTHRU_MSG messages[batch];
    while (!stopping && !atomic_load(&bridge->failed)) {
        uint32_t count = batch;
        int32_t status = PassThruReadMsgs(bridge->channel, messages, &count, 0);
        if (status == ERR_BUFFER_EMPTY) {
            count = 1;
            status = PassThruReadMsgs(bridge->channel, messages, &count, read_wait_ms);
        }
        if (status == ERR_BUFFER_OVERFLOW) atomic_fetch_add(&bridge->overflows, 1);
        else if (status && status != ERR_TIMEOUT && status != ERR_BUFFER_EMPTY) {
            report("PassThruReadMsgs", status);
            if (fatal(status)) { atomic_store(&bridge->failed, 1); break; }
        }
        for (uint32_t i = 0; i < count; ++i) {
            struct can_frame frame;
            if (!to_socketcan(&messages[i], &frame)) continue;
            if (write(bridge->socket, &frame, sizeof frame) == (ssize_t)sizeof frame) atomic_fetch_add(&bridge->to_host, 1);
            else atomic_fetch_add(&bridge->host_drops, 1);
        }
    }
    return NULL;
}

/* Interface to adapter: every frame waiting on the socket goes to the adapter in one queued WriteMsgs. A timeout
 * of 0 returns once the adapter has accepted the frames, which keeps a flow-control frame's latency to one USB
 * round trip; ISO-TP allows the ECU's partner a second or so. */
static void host_to_vehicle(struct bridge *bridge) {
    static PASSTHRU_MSG messages[batch];
    struct pollfd waiting = {bridge->socket, POLLIN, 0};
    while (!stopping && !atomic_load(&bridge->failed)) {
        const int ready = poll(&waiting, 1, poll_wait_ms);
        if (ready < 0 && errno != EINTR) { perror("mongoose-socketcan: poll"); atomic_store(&bridge->failed, 1); break; }
        if (ready <= 0) continue;
        uint32_t count = 0;
        struct can_frame frame;
        while (count < batch && recv(bridge->socket, &frame, sizeof frame, MSG_DONTWAIT) == (ssize_t)sizeof frame) {
            if (bridge->transmit && from_socketcan(&frame, &messages[count])) ++count;
            else atomic_fetch_add(&bridge->refused, 1);
        }
        if (!count) continue;
        uint32_t sent = count;
        const int32_t status = PassThruWriteMsgs(bridge->channel, messages, &sent, 0);
        atomic_fetch_add(&bridge->to_bus, sent);
        if (sent < count) atomic_fetch_add(&bridge->refused, count - sent);
        if (status) {
            report("PassThruWriteMsgs", status);
            if (fatal(status)) atomic_store(&bridge->failed, 1);
        }
    }
}

static void *print_stats(void *argument) {
    struct bridge *bridge = argument;
    const unsigned seconds = bridge->stats_seconds;
    const time_t started = time(NULL);
    while (!stopping && !atomic_load(&bridge->failed)) {
        for (unsigned waited = 0; waited < seconds * 10 && !stopping && !atomic_load(&bridge->failed); ++waited)
            nanosleep(&(struct timespec){0, 100000000}, NULL);
        if (stopping || atomic_load(&bridge->failed)) break;
        fprintf(stderr, "mongoose-socketcan: %lds: from vehicle %lu, to vehicle %lu, refused %lu, "
                        "adapter overflows %lu, interface drops %lu\n",
                (long)(time(NULL) - started), atomic_load(&bridge->to_host), atomic_load(&bridge->to_bus),
                atomic_load(&bridge->refused), atomic_load(&bridge->overflows), atomic_load(&bridge->host_drops));
    }
    return NULL;
}

int main(int argc, char **argv) {
    struct options options;
    if (argc == 2 && (!strcmp(argv[1], "--help") || !strcmp(argv[1], "-h"))) { usage(); return 0; }
    if (!parse(argc, argv, &options)) { usage(); return 2; }
    static struct bridge state;
    struct bridge *bridge = &state;
    bridge->transmit = options.transmit;
    bridge->stats_seconds = options.stats_seconds;
    if ((bridge->socket = open_interface(options.interface)) < 0) return 1;

    struct sigaction action = {0};
    action.sa_handler = on_signal;  /* no SA_RESTART: poll and the sleeps return early */
    sigaction(SIGINT, &action, NULL);
    sigaction(SIGTERM, &action, NULL);

    int result = 1;
    uint32_t device = 0, filter = 0;
    int32_t status = PassThruOpen((void *)options.device, &device);
    if (status) { report("PassThruOpen", status); goto close_socket; }
    if ((status = PassThruConnect(device, CAN, options.connect_flags, options.bitrate, &bridge->channel))) {
        report("PassThruConnect", status); goto close_device;
    }
    /* A raw CAN channel delivers nothing without a filter; an all-zero mask passes every frame. */
    PASSTHRU_MSG mask = {0}, pattern = {0};
    mask.ProtocolID = pattern.ProtocolID = CAN;
    mask.TxFlags = pattern.TxFlags = options.connect_flags;
    mask.DataSize = pattern.DataSize = 4;
    if ((status = PassThruStartMsgFilter(bridge->channel, PASS_FILTER, &mask, &pattern, NULL, &filter))) {
        report("PassThruStartMsgFilter", status); goto disconnect;
    }
    fprintf(stderr, "mongoose-socketcan: bridging %s at %u bit/s, %s-bit channel, %s\n", options.interface,
            options.bitrate, options.connect_flags ? "29" : "11",
            options.transmit ? "TRANSMIT ENABLED: frames written to the interface go to the vehicle" : "listen only");

    pthread_t reader, reporter;
    if (pthread_create(&reader, NULL, vehicle_to_host, bridge)) { fprintf(stderr, "mongoose-socketcan: no thread\n"); goto stop_filter; }
    const int reporting = options.stats_seconds && !pthread_create(&reporter, NULL, print_stats, bridge);
    host_to_vehicle(bridge);
    pthread_join(reader, NULL);
    if (reporting) pthread_join(reporter, NULL);
    result = atomic_load(&bridge->failed) ? 1 : 0;
    fprintf(stderr, "mongoose-socketcan: stopped: from vehicle %lu, to vehicle %lu, refused %lu, adapter overflows %lu, "
                    "interface drops %lu\n",
            atomic_load(&bridge->to_host), atomic_load(&bridge->to_bus), atomic_load(&bridge->refused),
            atomic_load(&bridge->overflows), atomic_load(&bridge->host_drops));
stop_filter:
    PassThruStopMsgFilter(bridge->channel, filter);
disconnect:
    PassThruDisconnect(bridge->channel);
close_device:
    PassThruClose(device);
close_socket:
    close(bridge->socket);
    return result;
}
