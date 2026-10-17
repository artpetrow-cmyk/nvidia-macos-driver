/*
 * NullMoth NVIDIA driver for macOS
 * Copyright (c) 2026 NullMoth Systems.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */

#include "nv-xnu.h"
#include <IOKit/IOLib.h>

extern "C" {
#include "nvtypes.h"
#include "nvos.h"
#include "nvmisc.h"
#include "nv-kernel-rmapi-ops.h"
#include "rs_access.h"
void NV_API_CALL rm_kernel_rmapi_op(nvidia_stack_t *sp, void *ops_cmd);
unsigned int nvrm_kapi_mem_handle(const void *mem);
int nvrm_kapi_dev_handles(const void *dev, unsigned int *c, unsigned int *d, unsigned int *s);
}
#define NVRM_SS_KERNEL_ABI 1
#include "nvrm_surfshare_abi.h"

static inline void ssZero(void *p, unsigned long n) { volatile unsigned char *b = (volatile unsigned char *)p; while (n--) *b++ = 0; }

extern "C" int nvrm_surfshare_rm(struct NVRMSurfShareRm *r)
{
    if (!r || r->version != NVRM_SS_RM_VERSION || r->op != NVRM_SS_RM_SHARE_KAPI) { if (r) r->status = NVRM_SS_BAD_VERSION; return 0; }
    unsigned int c = 0, d = 0, s = 0;
    if (!r->kapiDevice || !r->kapiMemory || !nvrm_kapi_dev_handles(r->kapiDevice, &c, &d, &s) || !c) { r->status = NVRM_SS_OFF; return 0; }
    unsigned int m = nvrm_kapi_mem_handle(r->kapiMemory);
    if (!m) { r->status = NVRM_SS_NO_VRAM; return 0; }
    nvidia_kernel_rmapi_ops_t ops; ssZero(&ops, sizeof ops);
    ops.op = NV04_SHARE;
    ops.params.share.hClient = c; ops.params.share.hObject = m;
    ops.params.share.sharePolicy.type = RS_SHARE_TYPE_CLIENT; ops.params.share.sharePolicy.target = r->target;
    ops.params.share.sharePolicy.action = 0;
    RS_ACCESS_MASK_ADD(&ops.params.share.sharePolicy.accessMask, RS_ACCESS_DUP_OBJECT);
    rm_kernel_rmapi_op(NULL, &ops);
    r->status = ops.params.share.status; r->hClient = c; r->hObject = m;
    return 0;
}

#include <sys/sysctl.h>
#include <libkern/OSAtomic.h>
extern "C" volatile unsigned long long nvrm_pageoff_last_runs, nvrm_pageoff_last_pages, nvrm_pageoff_coalesced, nvrm_pageoff_perpage;
SYSCTL_QUAD(_debug, OID_AUTO, nvrm_pageoff_runs, CTLFLAG_RD | CTLFLAG_LOCKED, (long long *)&nvrm_pageoff_last_runs, "runs in the last page-off");
SYSCTL_QUAD(_debug, OID_AUTO, nvrm_pageoff_pages, CTLFLAG_RD | CTLFLAG_LOCKED, (long long *)&nvrm_pageoff_last_pages, "pages in the last page-off");
SYSCTL_QUAD(_debug, OID_AUTO, nvrm_pageoff_coalesced, CTLFLAG_RD | CTLFLAG_LOCKED, (long long *)&nvrm_pageoff_coalesced, "page-offs copied per run");
SYSCTL_QUAD(_debug, OID_AUTO, nvrm_pageoff_perpage, CTLFLAG_RD | CTLFLAG_LOCKED, (long long *)&nvrm_pageoff_perpage, "page-offs copied per 4 KB page");
static volatile SInt32 gSsSysctl = 0;
extern "C" NV_STATUS nvrm_ce_copy_to_pages(void *nv, NvU32 hClient, NvU32 hMemory, const NvU64 *pages, NvU64 pageCount,
                                           NvU64 length, NvU64 *pUsec);
extern "C" int nvrm_surfshare_pageoff(struct NVRMSurfPageoff *p, void *nv)
{
    if (!p || p->version != NVRM_SS_RM_VERSION) { if (p) p->status = NVRM_SS_BAD_VERSION; return 0; }
    unsigned int c = 0, d = 0, s = 0;
    if (!p->kapiDevice || !p->kapiMemory || !nvrm_kapi_dev_handles(p->kapiDevice, &c, &d, &s) || !c) { p->status = NVRM_SS_OFF; return 0; }
    unsigned int m = nvrm_kapi_mem_handle(p->kapiMemory);
    if (!m) { p->status = NVRM_SS_NO_VRAM; return 0; }
    if (OSCompareAndSwap(0, 1, &gSsSysctl)) {
        sysctl_register_oid(&sysctl__debug_nvrm_pageoff_runs); sysctl_register_oid(&sysctl__debug_nvrm_pageoff_pages);
        sysctl_register_oid(&sysctl__debug_nvrm_pageoff_coalesced); sysctl_register_oid(&sysctl__debug_nvrm_pageoff_perpage);
    }
    NvU64 us = 0;
    p->status = nvrm_ce_copy_to_pages(nv, c, m, (const NvU64 *)p->pages, p->pageCount, p->length, &us);
    p->usec = us;
    return 0;
}

static volatile SInt64 gSsClearOk, gSsClearFail, gSsClearUsSum, gSsClearUsMax;
SYSCTL_QUAD(_debug, OID_AUTO, nvrm_ce_clear_ok, CTLFLAG_RD | CTLFLAG_LOCKED, (long long *)&gSsClearOk, "VRAM grants cleared on the copy engine");
SYSCTL_QUAD(_debug, OID_AUTO, nvrm_ce_clear_fail, CTLFLAG_RD | CTLFLAG_LOCKED, (long long *)&gSsClearFail, "copy-engine clears that failed (the caller clears by CPU)");
SYSCTL_QUAD(_debug, OID_AUTO, nvrm_ce_clear_us_sum, CTLFLAG_RD | CTLFLAG_LOCKED, (long long *)&gSsClearUsSum, "microseconds spent in copy-engine clears");
SYSCTL_QUAD(_debug, OID_AUTO, nvrm_ce_clear_us_max, CTLFLAG_RD | CTLFLAG_LOCKED, (long long *)&gSsClearUsMax, "the slowest copy-engine clear, us");
static volatile SInt32 gSsClearSysctl = 0;
extern "C" NV_STATUS nvrm_ce_memset(void *nv, NvU32 hClient, NvU32 hMemory, NvU64 length, NvU64 *pUsec);
extern "C" int nvrm_surfshare_clear(struct NVRMSurfClear *p, void *nv)
{
    if (!p || p->version != NVRM_SS_RM_VERSION) { if (p) p->status = NVRM_SS_BAD_VERSION; return 0; }
    unsigned int c = 0, d = 0, s = 0;
    if (!p->kapiDevice || !p->kapiMemory || !nvrm_kapi_dev_handles(p->kapiDevice, &c, &d, &s) || !c) { p->status = NVRM_SS_OFF; return 0; }
    unsigned int m = nvrm_kapi_mem_handle(p->kapiMemory);
    if (!m) { p->status = NVRM_SS_NO_VRAM; return 0; }
    if (OSCompareAndSwap(0, 1, &gSsClearSysctl)) {
        sysctl_register_oid(&sysctl__debug_nvrm_ce_clear_ok); sysctl_register_oid(&sysctl__debug_nvrm_ce_clear_fail);
        sysctl_register_oid(&sysctl__debug_nvrm_ce_clear_us_sum); sysctl_register_oid(&sysctl__debug_nvrm_ce_clear_us_max);
    }
    NvU64 us = 0;
    p->status = nvrm_ce_memset(nv, c, m, p->length, &us);
    p->usec = us;
    if (p->status == 0) {
        OSAddAtomic64(1, &gSsClearOk); OSAddAtomic64((SInt64)us, &gSsClearUsSum);
        if ((SInt64)us > gSsClearUsMax) gSsClearUsMax = (SInt64)us;
    } else if (OSAddAtomic64(1, &gSsClearFail) < 8) {
        kprintf("NVRM-xnu: nvSurfClear: %llu bytes on the copy engine FAILED (NV_STATUS 0x%x)\n", p->length, p->status);
        IOLog("NVRM-xnu: nvSurfClear: %llu bytes on the copy engine FAILED (NV_STATUS 0x%x)\n", p->length, p->status);
    }
    return 0;
}
