/* SPDX-License-Identifier: MIT */
/* Stable decoder wrapper interface between the VC8000 core and open shim. */
#ifndef VC_DWL_ABI_H
#define VC_DWL_ABI_H

#include "vc_legacy_types.h"
#include "vc_dec_abi.h"

#define DWL_OK 0
#define DWL_ERROR (-1)
#define DWL_HW_WAIT_OK DWL_OK
#define DWL_HW_WAIT_ERROR DWL_ERROR
#define DWL_HW_WAIT_TIMEOUT 1

#define DWL_LOG_ERROR 0U
#define DWL_LOG_WARNING 1U
#define DWL_LOG_INFO 2U
#define DWL_LOG_DEBUG 3U

#define DWL_CLIENT_TYPE_H264_DEC 1U
#define DWL_CLIENT_TYPE_MPEG4_DEC 2U
#define DWL_CLIENT_TYPE_JPEG_DEC 3U
#define DWL_CLIENT_TYPE_PP 4U
#define DWL_CLIENT_TYPE_VC1_DEC 5U
#define DWL_CLIENT_TYPE_MPEG2_DEC 6U
#define DWL_CLIENT_TYPE_VP6_DEC 7U
#define DWL_CLIENT_TYPE_AVS_DEC 8U
#define DWL_CLIENT_TYPE_RV_DEC 9U
#define DWL_CLIENT_TYPE_VP8_DEC 10U

struct DWLLinearMem {
	u32 *virtualAddress;
	g1_addr_t busAddress;
	u32 size;
};
typedef struct DWLLinearMem DWLLinearMem_t;

struct DWLInitParam {
	u32 clientType;
};
typedef struct DWLInitParam DWLInitParam_t;

typedef struct DecHwConfig_ DWLHwConfig_t;

u32 DWLReadAsicID(void);
void DWLReadAsicConfig(DWLHwConfig_t *config);
u32 DWLReadAsicCoreCount(void);

const void *DWLInit(DWLInitParam_t *params);
i32 DWLRelease(const void *instance);
i32 DWLReserveHw(const void *instance, i32 *core_id);
i32 DWLReserveHwPipe(const void *instance, i32 *core_id);
void DWLReleaseHw(const void *instance, i32 core_id);

i32 DWLMallocRefFrm(const void *instance, u32 size, DWLLinearMem_t *memory);
void DWLFreeRefFrm(const void *instance, DWLLinearMem_t *memory);
i32 DWLMallocLinear(const void *instance, u32 size, DWLLinearMem_t *memory);
void DWLFreeLinear(const void *instance, DWLLinearMem_t *memory);
void DWLDCacheRangeFlush(const void *instance, DWLLinearMem_t *memory);
void DWLDCacheRangeRefresh(const void *instance, DWLLinearMem_t *memory);

void DWLWriteReg(const void *instance, i32 core_id, u32 offset, u32 value);
u32 DWLReadReg(const void *instance, i32 core_id, u32 offset);
void DWLWriteRegAll(const void *instance, const u32 *table, u32 size);
void DWLReadRegAll(const void *instance, u32 *table, u32 size);
void DWLEnableHW(const void *instance, i32 core_id, u32 offset, u32 value);
void DWLDisableHW(const void *instance, i32 core_id, u32 offset, u32 value);
i32 DWLWaitHwReady(const void *instance, i32 core_id, u32 timeout);

typedef void DWLIRQCallbackFn(void *arg, i32 core_id);
void DWLSetIRQCallback(const void *instance, i32 core_id,
		       DWLIRQCallbackFn *callback, void *arg);

void *DWLmalloc(u32 size);
void DWLfree(void *address);
void *DWLcalloc(u32 count, u32 size);
void *DWLmemcpy(void *destination, const void *source, u32 size);
void *DWLmemset(void *destination, i32 value, u32 size);
void DWLLog(u32 level, const char *format, ...);

#ifdef DWL_ENABLE_DEBUG_LOG
#define DWLDebugLog(...) DWLLog(DWL_LOG_DEBUG, __VA_ARGS__)
#else
#define DWLDebugLog(...) do { } while (0)
#endif

#endif /* VC_DWL_ABI_H */
