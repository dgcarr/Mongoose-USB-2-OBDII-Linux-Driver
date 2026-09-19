/* End-to-end test of mongoose-socketcan built against tests/fake_j2534.c, over a real CAN interface:
 *   mongoose-socketcan-test BRIDGE [IFNAME]     (IFNAME defaults to $MONGOOSE_TEST_CANIF, then vcan0)
 * Exits 77 (CTest's skip) when the interface is missing, since creating a vcan needs root:
 *   modprobe vcan && ip link add dev vcan0 type vcan && ip link set vcan0 up */
#define _GNU_SOURCE
#include <errno.h>
#include <linux/can.h>
#include <linux/can/isotp.h>
#include <net/if.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)

static int open_interface(const char *name) {
    const unsigned index = if_nametoindex(name);
    if (!index) return -1;
    const int fd = socket(PF_CAN, SOCK_RAW, CAN_RAW);
    if (fd < 0) return -1;
    struct sockaddr_can address = {.can_family = AF_CAN, .can_ifindex = (int)index};
    if (bind(fd, (struct sockaddr *)&address, sizeof address) < 0) { close(fd); return -1; }
    return fd;
}
static int receive(int fd, struct can_frame *frame, int timeout_ms) {
    struct pollfd waiting = {fd, POLLIN, 0};
    if (poll(&waiting, 1, timeout_ms) != 1) return 0;
    return read(fd, frame, sizeof *frame) == (ssize_t)sizeof *frame;
}
static void send_frame(int fd, canid_t id, const uint8_t *data, uint8_t size) {
    struct can_frame frame = {.can_id = id, .len = size};
    memcpy(frame.data, data, size);
    CHECK(write(fd, &frame, sizeof frame) == (ssize_t)sizeof frame);
}
static pid_t start(const char *bridge, const char *interface, int transmit) {
    const pid_t child = fork();
    CHECK(child >= 0);
    if (!child) {
        if (transmit) execl(bridge, bridge, "--transmit", interface, (char *)NULL);
        else execl(bridge, bridge, interface, (char *)NULL);
        _exit(127);
    }
    return child;
}
static void stop(pid_t child) {
    int status = 0;
    CHECK(kill(child, SIGTERM) == 0);
    CHECK(waitpid(child, &status, 0) == child);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}
/* The three frames the fake delivers once its pass filter is set: 11-bit with eight bytes, 29-bit, and empty. */
static void expect_initial_frames(int fd) {
    struct can_frame frame;
    CHECK(receive(fd, &frame, 3000));
    CHECK(frame.can_id == 0x7e8 && frame.len == 8 && frame.data[1] == 0x41 && frame.data[2] == 0x0c);
    CHECK(receive(fd, &frame, 1000));
    CHECK(frame.can_id == (0x18daf110 | CAN_EFF_FLAG) && frame.len == 3 && frame.data[0] == 0xde);
    CHECK(receive(fd, &frame, 1000));
    CHECK(frame.can_id == 0x123 && frame.len == 0);
}
/* A UDS VIN read through the kernel's ISO-TP socket: the reply is multi-frame, so it completes only if the kernel's
 * flow-control frame travels out through the bridge and the consecutive frames travel back. Returns 0 when the
 * can-isotp module is not available, 1 when the read worked. */
static int read_vin_over_isotp(const char *interface) {
    const int fd = socket(PF_CAN, SOCK_DGRAM, CAN_ISOTP);
    if (fd < 0) return 0;
    struct can_isotp_options options = {0};
    options.flags = CAN_ISOTP_TX_PADDING;  /* the ECUs tested ignore unpadded requests */
    options.txpad_content = 0x55;
    CHECK(setsockopt(fd, SOL_CAN_ISOTP, CAN_ISOTP_OPTS, &options, sizeof options) == 0);
    struct sockaddr_can address = {.can_family = AF_CAN, .can_ifindex = (int)if_nametoindex(interface)};
    address.can_addr.tp.tx_id = 0x7e0;
    address.can_addr.tp.rx_id = 0x7e8;
    CHECK(bind(fd, (struct sockaddr *)&address, sizeof address) == 0);
    static const uint8_t request[3] = {0x22, 0xf1, 0x90};
    CHECK(write(fd, request, sizeof request) == (ssize_t)sizeof request);
    uint8_t reply[64];
    struct pollfd waiting = {fd, POLLIN, 0};
    CHECK(poll(&waiting, 1, 3000) == 1);
    CHECK(read(fd, reply, sizeof reply) == 20);
    CHECK(reply[0] == 0x62 && reply[1] == 0xf1 && reply[2] == 0x90 && memcmp(&reply[3], "YV1TESTVIN0000000", 17) == 0);
    close(fd);
    return 1;
}
static char *slurp(const char *path) {
    static char text[4096];
    FILE *file = fopen(path, "r");
    size_t size = file ? fread(text, 1, sizeof text - 1, file) : 0;
    if (file) fclose(file);
    text[size] = '\0';
    return text;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s BRIDGE [IFNAME]\n", argv[0]); return 2; }
    const char *interface = argc > 2 ? argv[2] : getenv("MONGOOSE_TEST_CANIF") ? getenv("MONGOOSE_TEST_CANIF") : "vcan0";
    const int fd = open_interface(interface);
    if (fd < 0) { printf("SKIP: no CAN interface '%s'\n", interface); return 77; }
    char log[] = "/tmp/mongoose-socketcan-test-XXXXXX";
    const int log_fd = mkstemp(log);
    CHECK(log_fd >= 0);
    close(log_fd);
    CHECK(setenv("FAKE_J2534_TX_LOG", log, 1) == 0);
    static const uint8_t request[8] = {0x02, 0x01, 0x00, 0x55, 0x55, 0x55, 0x55, 0x55}, extended[2] = {0x10, 0x20};
    struct can_frame frame;

    /* With --transmit, a request written to the interface reaches the "vehicle" and its reply comes back. */
    pid_t bridge = start(argv[1], interface, 1);
    expect_initial_frames(fd);
    send_frame(fd, 0x7df, request, 8);
    CHECK(receive(fd, &frame, 2000));
    CHECK(frame.can_id == 0x7e8 && frame.len == 8 && frame.data[0] == 0x06 && frame.data[1] == 0x41 && frame.data[2] == 0x00);
    send_frame(fd, 0x18db33f1 | CAN_EFF_FLAG, extended, 2);
    send_frame(fd, 0x7df | CAN_RTR_FLAG, NULL, 0);  /* J2534 cannot carry a remote frame: refused, not sent */
    usleep(300000);
    const int isotp = read_vin_over_isotp(interface);
    stop(bridge);
    const char *sent = slurp(log);
    static const char raw[] = "std 000007df 02 01 00 55 55 55 55 55\next 18db33f1 10 20\n";
    CHECK(strncmp(sent, raw, sizeof raw - 1) == 0);
    if (isotp) CHECK(strcmp(sent + sizeof raw - 1, "std 000007e0 03 22 f1 90 55 55 55 55\nstd 000007e0 30 00 00 55 55 55 55 55\n") == 0);
    else printf("note: no can-isotp module, ISO-TP part skipped\n");

    /* The raw socket saw the ISO-TP exchange too; start the next run from an empty queue. */
    while (receive(fd, &frame, 100)) {}
    /* Listen only (the default): the same request never reaches the vehicle, so nothing answers. */
    CHECK(truncate(log, 0) == 0);
    bridge = start(argv[1], interface, 0);
    expect_initial_frames(fd);
    send_frame(fd, 0x7df, request, 8);
    CHECK(!receive(fd, &frame, 500));
    stop(bridge);
    CHECK(slurp(log)[0] == '\0');

    unlink(log);
    close(fd);
    printf("PASS\n");
    return 0;
}
