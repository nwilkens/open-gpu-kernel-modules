
#include "g_mem_desc_nvoc.h"

NV_STATUS memdescUnmapIommuChecked(PMEMORY_DESCRIPTOR pMemDesc,
                                  NvU32 iovaspaceId);
NV_STATUS memdescUnmapIommuDupChecked(PMEMORY_DESCRIPTOR pMemDesc,
                                     NvU32 iovaspaceId);
#if defined(NV_SUNOS)
NV_STATUS memReleaseTrackedOsDescGuardsChecked(PMEMORY_DESCRIPTOR pMemDesc,
                                               NvU32 retainedMemoryRefs);
#endif

