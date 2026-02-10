#include <oskal/cr_debug.h>
#include <oskal/cr_memory.h>
#include <oskal/cr_status.h>
#include <oskal/cr_types.h>

CR_STATUS MapDeviceIORegion(IN UINTN BaseAddress, IN UINTN Size);
CR_STATUS UnMapMemRegion(IN UINTN BaseAddress, IN UINTN Size);
CR_STATUS MapMemRegion(
    IN UINTN BaseAddress, IN UINTN Size, IN UINT64 Attributes,
    IN UINT64 CacheAttr, IN UINT64 MemoryType);