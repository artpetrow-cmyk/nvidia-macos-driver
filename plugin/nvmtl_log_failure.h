#ifndef NVMTL_LOG_FAILURE_H
#define NVMTL_LOG_FAILURE_H
#include <string.h>
#include <ctype.h>

static inline int nvmtl_log_fault_word(const char *text, const char *word)
{
    const char *p = text;
    while ((p = strstr(p, word))) {
        if ((p == text || !isalnum((unsigned char)p[-1])) &&
            !isalnum((unsigned char)p[5])) return 1;
        p += 5;
    }
    return 0;
}

/* Keep device/feature failures in release syslog. In particular, Vulkan errors
 * such as "vkCreateDevice -> -8" do not contain the word FAIL. */
static inline int nvmtl_log_failure(const char *text)
{
    return text && (strstr(text, "FAIL") || strstr(text, "REFUS") ||
                    nvmtl_log_fault_word(text, "fault") ||
                    nvmtl_log_fault_word(text, "FAULT") || strstr(text, "-> -") ||
                    strstr(text, "missing") || strstr(text, "cannot") ||
                    strstr(text, "unsupported") || strstr(text, "refused") ||
                    /* a process denied a device: for WindowServer that is the "Failed to create MetalDevice" abort,
                     * and /tmp/nvmtl.log is gone after the reboot that rescues the machine */
                    strstr(text, "no device") ||
                    /* bring-up and census lines of the RTX 20 / macOS 26 work (rate-limited at their call sites) */
                    strstr(text, "fbf1") || strstr(text, "fbl1") || strstr(text, "ail1") ||
                    strstr(text, "lnk1") || strstr(text, "sync1") || strstr(text, "hpin1") || strstr(text, "fbc1") || strstr(text, "tsync1") ||
                    strstr(text, "tex1 ") || strstr(text, "mip1 ") || strstr(text, "mipq1") || strstr(text, "lin1") || strstr(text, "aniso1") ||
                    strstr(text, "dpark1") || strstr(text, "sin1 ") || strstr(text, "sin2 ") || strstr(text, "sin3") || strstr(text, "su1: ") ||
                    strstr(text, "vic1: ") || strstr(text, "knob1 ") || strstr(text, "knob2 ") || strstr(text, "ss1: ") || strstr(text, "dp1: ") || strstr(text, "ms1 ") ||
                    strstr(text, "sbx1 ") || strstr(text, "dsr1 ") || strstr(text, "hot1 ") || strstr(text, "hot2 ") || strstr(text, "hot3 ") || strstr(text, "bc1 ") || strstr(text, "tess1 ") || strstr(text, "tess2 ") || strstr(text, "kstage1 ") || strstr(text, "kstage2") || strstr(text, "kstage3") || strstr(text, "kstage4") || strstr(text, "kstage5") || strstr(text, "vis1 ") || strstr(text, "upl1 ") || strstr(text, "fz1 ") || strstr(text, "sbx2 ") || strstr(text, "vt1 ") || strstr(text, "smp1 ") || strstr(text, "sm1 ") || strstr(text, "vt3 ") || strstr(text, "lsh1 "));
}
#endif
