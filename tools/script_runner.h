#ifndef MONGOOSE_SCRIPT_RUNNER_H
#define MONGOOSE_SCRIPT_RUNNER_H
#include <stdint.h>
/* Runs one step file from tools/scripts/ against the native library, the Linux twin of the
 * step interpreter in windows_reference.c: same syntax, same log lines, so a script run
 * on both stacks can be diffed. device_name is the "serial:..." selector, or NULL to let
 * PassThruOpen discover the adapter. With dry set, every step is parsed and validated but
 * no J2534 call is made, no delay is taken and the adapter is not needed.
 * Returns 0 on success, 1 if a step failed (or, in a dry run, did not parse), 2 if the
 * script could not be read. A malformed number or hex string exits with status 2. */
int script_run(const char *path, const char *device_name, uint32_t gap_ms, int dry);
#endif
