#include "mongoose/j2534.h"
#include <stdio.h>
#include <string.h>
static void report(const char *operation, int32_t status) {
    char error[80] = {0};
    if (status) PassThruGetLastError(error);
    printf("%s status=%d %s\n", operation, status, error);
}
int main(int argc, char **argv) {
    uint32_t device = 0;
    if (argc > 2 || (argc == 2 && strncmp(argv[1], "serial:", 7))) {
        fprintf(stderr, "Usage: mongoose-client [serial:SERIAL]\n"); return 2;
    }
    int32_t status = PassThruOpen(argc == 2 ? argv[1] : NULL, &device);
    report("PassThruOpen", status);
    if (status) return 1;
    char firmware[80], driver[80], api[80];
    int32_t version = PassThruReadVersion(device, firmware, driver, api);
    report("PassThruReadVersion", version);
    if (!version) printf("firmware=%s driver=%s api=%s\n", firmware, driver, api);
    status = PassThruClose(device); report("PassThruClose", status);
    return status || version ? 1 : 0;
}
