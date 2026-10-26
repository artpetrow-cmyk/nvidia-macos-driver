/* gst1: GPU statistics passed from NVRM to NVAccel through callPlatformFunction("nvrmGpuStats", p1 = struct NvrmGpuStats *) */
#pragma once
#include <stdint.h>
#define NVRM_GST_UTIL 0x1u
#define NVRM_GST_TEMP 0x2u
#define NVRM_GST_VRAM 0x4u
struct NvrmGpuStats {
    uint32_t valid;                 /* NVRM_GST_* bits for the fields that were read */
    uint32_t grUtil100;             /* graphics engine busy, 0.01 % units (10000 = 100 %) */
    uint32_t fbUtil100;             /* frame buffer bandwidth */
    uint32_t decUtil100, encUtil100;
    int32_t  tempC;                 /* GPU temperature, degrees C */
    uint32_t vramTotalKB, vramFreeKB;
};
