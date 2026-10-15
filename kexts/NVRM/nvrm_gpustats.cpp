/*
 * gst1: GPU statistics for macOS monitors (Activity Monitor's GPU History, Stats, iStat Menus).
 * NVAccel asks for them through NVRM::callPlatformFunction("nvrmGpuStats") about once a second, and only while someone
 * reads the accelerator's PerformanceStatistics. Everything comes from RM controls that the kernel half of RM forwards to
 * GSP, the same ones nvidia-smi uses on Linux:
 *   - graphics engine utilisation: NV2080_CTRL_CMD_PERF_GET_GPUMON_PERFMON_UTIL_SAMPLES_V2 (newest sample of the ring)
 *   - GPU temperature: NV2080_CTRL_CMD_THERMAL_SYSTEM_EXECUTE_V2, GET_STATUS_SENSOR_READING on the sensor whose target is
 *     the GPU (found once)
 *   - video memory: NV2080_CTRL_CMD_FB_GET_INFO_V2, TOTAL_RAM_SIZE and HEAP_FREE
 * One kernel RM client (root, device, subdevice) is kept for the whole boot and rebuilt after a failure.
 */
#include "nv-xnu.h"
#include <IOKit/IOLib.h>
extern "C" {
#include "nvtypes.h"
#include "nvos.h"
#include "nvmisc.h"
#include "nv-kernel-rmapi-ops.h"
#include "class/cl0080.h"
#include "class/cl2080.h"
#include "ctrl/ctrl2080/ctrl2080perf.h"
#include "ctrl/ctrl2080/ctrl2080thermal.h"
#include "ctrl/ctrl2080/ctrl2080fb.h"
void NV_API_CALL rm_kernel_rmapi_op(nvidia_stack_t *sp, void *ops_cmd);
}
#include "nvrm_gpustats_abi.h"
#define GSTLOG(fmt, ...) kprintf("NVRM-gst1: " fmt "\n", ##__VA_ARGS__)

static inline void gstZero(void *p, unsigned long n)
{ volatile unsigned char *b = (volatile unsigned char *)p; while (n--) *b++ = 0; }

static NvV32 gstAlloc(NvU32 hRoot, NvU32 hParent, NvU32 hClass, void *pParams, NvU32 size, NvU32 *hOut)
{
    nvidia_kernel_rmapi_ops_t ops; gstZero(&ops, sizeof ops);
    ops.op = NV04_ALLOC;
    ops.params.alloc.hRoot = hRoot; ops.params.alloc.hObjectParent = hParent; ops.params.alloc.hObjectNew = 0;
    ops.params.alloc.hClass = (NvV32)hClass; ops.params.alloc.pAllocParms = (NvP64)(NvUPtr)pParams;
    ops.params.alloc.pRightsRequested = (NvP64)0; ops.params.alloc.paramsSize = size;
    rm_kernel_rmapi_op(NULL, &ops);
    if (hOut) *hOut = ops.params.alloc.hObjectNew;
    return ops.params.alloc.status;
}
static NvV32 gstControl(NvU32 hClient, NvU32 hObject, NvU32 cmd, void *pParams, NvU32 size)
{
    nvidia_kernel_rmapi_ops_t ops; gstZero(&ops, sizeof ops);
    ops.op = NV04_CONTROL;
    ops.params.control.hClient = hClient; ops.params.control.hObject = hObject; ops.params.control.cmd = cmd;
    ops.params.control.params = (NvP64)(NvUPtr)pParams; ops.params.control.paramsSize = size;
    rm_kernel_rmapi_op(NULL, &ops);
    return ops.params.control.status;
}
static void gstFreeClient(NvU32 hClient)
{
    nvidia_kernel_rmapi_ops_t ops; gstZero(&ops, sizeof ops);
    ops.op = NV01_FREE;
    ops.params.free.hRoot = hClient; ops.params.free.hObjectParent = hClient; ops.params.free.hObjectOld = hClient;
    rm_kernel_rmapi_op(NULL, &ops);
}

static IOLock *gGstLock;
static NvU32 gClient, gSub, gGpuId;
static int gSensor = -2;                       // -2 not looked for yet, -1 none found, >= 0 the GPU sensor
static NV2080_CTRL_PERF_GET_GPUMON_PERFMON_UTIL_SAMPLES_V2_PARAMS *gUtil;   // ~50 KB, allocated once
static unsigned gFails;

static bool gstOpen(NvU32 gpuId)
{
    if (gClient && gGpuId == gpuId) return true;
    if (gClient) { gstFreeClient(gClient); gClient = gSub = 0; }
    NvU32 hClient = 0, hDevice = 0, hSub = 0;
    if (gstAlloc(0, 0, 0x41u, NULL, 0, &hClient) || !hClient) return false;
    struct { NvU32 gpuIds[32]; NvU32 failedId; } at; gstZero(&at, sizeof at);
    at.gpuIds[0] = gpuId; at.gpuIds[1] = 0xffffffffu;
    NV0080_ALLOC_PARAMETERS dp; gstZero(&dp, sizeof dp); dp.deviceId = 0; dp.hClientShare = hClient;
    NV2080_ALLOC_PARAMETERS sp; gstZero(&sp, sizeof sp); sp.subDeviceId = 0;
    if (gstControl(hClient, hClient, 0x00000215u, &at, sizeof at) ||
        gstAlloc(hClient, hClient, 0x80u, &dp, sizeof dp, &hDevice) || !hDevice ||
        gstAlloc(hClient, hDevice, 0x2080u, &sp, sizeof sp, &hSub) || !hSub) {
        gstFreeClient(hClient); return false;
    }
    gClient = hClient; gSub = hSub; gGpuId = gpuId; gSensor = -2;
    return true;
}

static NvV32 gstThermal(NV2080_CTRL_THERMAL_SYSTEM_INSTRUCTION *ins, NvU32 n)
{
    NV2080_CTRL_THERMAL_SYSTEM_EXECUTE_V2_PARAMS *p =
        (NV2080_CTRL_THERMAL_SYSTEM_EXECUTE_V2_PARAMS *)IOMalloc(sizeof *p);
    if (!p) return NV_ERR_NO_MEMORY;
    gstZero(p, sizeof *p);
    p->clientAPIVersion = THERMAL_SYSTEM_API_VER; p->clientAPIRevision = THERMAL_SYSTEM_API_REV;
    p->clientInstructionSizeOf = sizeof(NV2080_CTRL_THERMAL_SYSTEM_INSTRUCTION);
    p->executeFlags = NV2080_CTRL_THERMAL_SYSTEM_EXECUTE_FLAGS_IGNORE_FAIL;
    p->instructionListSize = n;
    for (NvU32 i = 0; i < n; i++) p->instructionList[i] = ins[i];
    const NvV32 st = gstControl(gClient, gSub, NV2080_CTRL_CMD_THERMAL_SYSTEM_EXECUTE_V2, p, sizeof *p);
    for (NvU32 i = 0; i < n; i++) ins[i] = p->instructionList[i];
    IOFree(p, sizeof *p);
    return st;
}
static void gstFindSensor()
{
    gSensor = -1;
    NV2080_CTRL_THERMAL_SYSTEM_INSTRUCTION in[1]; gstZero(in, sizeof in);
    in[0].opcode = NV2080_CTRL_THERMAL_SYSTEM_GET_INFO_SENSORS_AVAILABLE_OPCODE;
    if (gstThermal(in, 1) || !in[0].executed || in[0].result) { GSTLOG("thermal: sensor count unavailable"); return; }
    const NvU32 count = in[0].operands.getInfoSensorsAvailable.availableSensors;
    for (NvU32 s = 0; s < count && s < 16; s++) {
        NV2080_CTRL_THERMAL_SYSTEM_INSTRUCTION t[1]; gstZero(t, sizeof t);
        t[0].opcode = NV2080_CTRL_THERMAL_SYSTEM_GET_INFO_SENSOR_TARGET_OPCODE;
        t[0].operands.getInfoSensorTarget.sensorIndex = s;
        if (gstThermal(t, 1) || !t[0].executed || t[0].result) continue;
        NV2080_CTRL_THERMAL_SYSTEM_INSTRUCTION y[1]; gstZero(y, sizeof y);
        y[0].opcode = NV2080_CTRL_THERMAL_SYSTEM_GET_INFO_TARGET_TYPE_OPCODE;
        y[0].operands.getInfoTargetType.targetIndex = t[0].operands.getInfoSensorTarget.targetIndex;
        if (gstThermal(y, 1) || !y[0].executed || y[0].result) continue;
        if (y[0].operands.getInfoTargetType.type == NV2080_CTRL_THERMAL_SYSTEM_TARGET_GPU) { gSensor = (int)s; break; }
    }
    GSTLOG("thermal: %u sensor(s), GPU sensor %d", (unsigned)count, gSensor);
}

extern "C" bool nvrm_gpu_stats(NvU32 gpuId, struct NvrmGpuStats *out)
{
    if (!out) return false;
    gstZero(out, sizeof *out);
    if (!gGstLock) { IOLock *l = IOLockAlloc(); if (!l) return false;
        if (!OSCompareAndSwapPtr(nullptr, l, &gGstLock)) IOLockFree(l); }
    IOLockLock(gGstLock);
    bool any = false;
    if (gstOpen(gpuId)) {
        // utilisation: the newest sample of GSP's ring (util in 0.01 %)
        if (!gUtil) gUtil = (NV2080_CTRL_PERF_GET_GPUMON_PERFMON_UTIL_SAMPLES_V2_PARAMS *)IOMalloc(sizeof *gUtil);
        if (gUtil) {
            gstZero(gUtil, sizeof *gUtil);
            gUtil->type = NV2080_CTRL_GPUMON_SAMPLE_TYPE_PERFMON_UTIL;
            gUtil->bufSize = NV2080_CTRL_PERF_GPUMON_PERFMON_UTIL_BUFFER_SIZE;
            const NvV32 st = gstControl(gClient, gSub, NV2080_CTRL_CMD_PERF_GET_GPUMON_PERFMON_UTIL_SAMPLES_V2, gUtil, sizeof *gUtil);
            if (st == 0) {
                NvU64 newest = 0; int idx = -1;
                for (int i = 0; i < NV2080_CTRL_PERF_GPUMON_SAMPLE_COUNT_PERFMON_UTIL; i++)
                    if (gUtil->samples[i].base.timeStamp > newest) { newest = gUtil->samples[i].base.timeStamp; idx = i; }
                if (idx >= 0) {
                    out->grUtil100 = gUtil->samples[idx].gr.util; out->fbUtil100 = gUtil->samples[idx].fb.util;
                    out->decUtil100 = gUtil->samples[idx].nvdec.util; out->encUtil100 = gUtil->samples[idx].nvenc.util;
                    out->valid |= NVRM_GST_UTIL; any = true;
                }
            } else if (gFails++ < 4) GSTLOG("utilisation samples -> status 0x%x", (unsigned)st);
        }
        // temperature
        if (gSensor == -2) gstFindSensor();
        if (gSensor >= 0) {
            NV2080_CTRL_THERMAL_SYSTEM_INSTRUCTION r[1]; gstZero(r, sizeof r);
            r[0].opcode = NV2080_CTRL_THERMAL_SYSTEM_GET_STATUS_SENSOR_READING_OPCODE;
            r[0].operands.getStatusSensorReading.sensorIndex = (NvU32)gSensor;
            if (gstThermal(r, 1) == 0 && r[0].executed && !r[0].result) {
                out->tempC = r[0].operands.getStatusSensorReading.value; out->valid |= NVRM_GST_TEMP; any = true;
            }
        }
        // video memory (KB)
        struct { NvU32 fbInfoListSize; struct { NvU32 index; NvU32 data; } fbInfoList[0x80]; } fb; gstZero(&fb, sizeof fb);
        fb.fbInfoListSize = 2;
        fb.fbInfoList[0].index = NV2080_CTRL_FB_INFO_INDEX_TOTAL_RAM_SIZE;
        fb.fbInfoList[1].index = NV2080_CTRL_FB_INFO_INDEX_HEAP_FREE;
        if (gstControl(gClient, gSub, NV2080_CTRL_CMD_FB_GET_INFO_V2, &fb, sizeof fb) == 0 && fb.fbInfoList[0].data) {
            out->vramTotalKB = fb.fbInfoList[0].data; out->vramFreeKB = fb.fbInfoList[1].data;
            out->valid |= NVRM_GST_VRAM; any = true;
        }
        if (!any) { gstFreeClient(gClient); gClient = gSub = 0; }   // rebuilt on the next call
    }
    IOLockUnlock(gGstLock);
    return any;
}
