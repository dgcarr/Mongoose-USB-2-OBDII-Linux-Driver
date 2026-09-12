#include "mongoose/j2534.h"
#include <stddef.h>
_Static_assert(sizeof(PASSTHRU_MSG) == 4152, "J2534 message size");
_Static_assert(offsetof(PASSTHRU_MSG, Data) == 24, "J2534 payload offset");
_Static_assert(offsetof(PASSTHRU_MSG, DataSize) == 16, "J2534 length offset");
_Static_assert(sizeof(SCONFIG) == 8, "configuration size");
_Static_assert(sizeof(((PASSTHRU_MSG *)0)->ProtocolID) == 4, "32-bit scalar");
#if UINTPTR_MAX == UINT64_MAX
_Static_assert(offsetof(SCONFIG_LIST, ConfigPtr) == 8, "native pointer alignment");
_Static_assert(sizeof(SCONFIG_LIST) == 16, "native list size");
#else
_Static_assert(sizeof(SCONFIG_LIST) == 8, "32-bit list size");
#endif
int main(void) { return 0; }
