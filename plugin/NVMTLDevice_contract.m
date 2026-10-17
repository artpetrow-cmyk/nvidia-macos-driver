/*
 * NullMoth NVIDIA driver for macOS
 * Copyright (c) 2026 NullMoth Systems.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */

#import <Metal/Metal.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#import "NVMTLObjects.h"
#import <IOSurface/IOSurfaceRef.h>
@implementation NVMTLDevice (Contract)
- (NSUInteger)doubleFPConfig { return 0; }
- (NSUInteger)featureProfile { return 10002; }
- (NSUInteger)getTimestampFrequency { return 1000000000ULL; }
- (NSUInteger)halfFPConfig { return 0; }
- (BOOL)isDepth24Stencil8PixelFormatSupported { return YES; }
- (BOOL)metalAssertionsEnabled { return NO; }
- (NSUInteger)minimumTextureBufferAlignmentForPixelFormat:(NSUInteger)a0 { return 256; }
static void nvbt(const char *tag, int *count, int max);
- (id)newBufferWithBytesNoCopy:(void *)a0 length:(NSUInteger)a1 options:(NSUInteger)a2 deallocator:(id)a3
{
    const MTLStorageMode sm = (MTLStorageMode)((a2 >> MTLResourceStorageModeShift) & 0xF);
    if (!a1) { nvlog("newBufferWithBytesNoCopy: zero length -> nil (the deallocator is not called)"); return nil; }
    if (sm == MTLStorageModePrivate || sm == MTLStorageModeMemoryless) {
        nvlog("newBufferWithBytesNoCopy: storage mode %d -> nil (a real driver ABORTS: storageModePrivate incompatible with ...WithBytes variant of newBuffer)", (int)sm);
        return nil; }
    if (!a0) {
        NVMTLBuffer *z = (NVMTLBuffer *)[self newBufferWithLength:a1 options:a2];
        if (!z) { nvlog("newBufferWithBytesNoCopy: NULL pointer, %lu bytes: no buffer to stand in -> nil", (unsigned long)a1); return nil; }
        void *zc = [z contents];
        if (zc) { memset(zc, 0, a1); if (z->_shadow.map) [z didModifyRange:NSMakeRange(0, a1)]; }
        if (a3) { void (^user)(void *, NSUInteger) = [a3 copy]; const NSUInteger n = a1;
                  z->_hostDealloc = ^(void *p, NSUInteger l) { (void)p; (void)l; user(NULL, n); }; }
        { static int said; if (!said++) nvlog("newBufferWithBytesNoCopy: NULL pointer, %lu bytes -> a zero-filled buffer of our own; its deallocator gets (NULL, %lu)", (unsigned long)a1, (unsigned long)a1); }
        return z;
    }
    NVMTLBuffer *b = [NVMTLBuffer new];
    b->_storage = sm;
    b->_ropt = a2 & ((MTLResourceOptions)0xF | ((MTLResourceOptions)0x3 << MTLResourceHazardTrackingModeShift));
    b->_hostDealloc = a3 ? [a3 copy] : nil;
    // Managed on a discrete GPU means the GPU works on its own copy, synchronized by didModifyRange (CPU -> GPU) and
    // synchronizeResource (GPU -> CPU). Apple's OpenCL wraps every cl_mem this way; importing the pages zero-copy put
    // every kernel access across PCIe instead (a 5600x4200 byte-image rotate: 6.6 s per dispatch, Geekbench 6 OpenCL
    // Horizon Detection hit the 60 s GPU watchdog). The caller's pages become the managed shadow, so contents is still
    // the caller's pointer, and the GPU side lives in VRAM. Under 1 MiB the transfer outweighs the win (the same
    // threshold the Shared pair uses), so small buffers stay zero-copy.
    if (sm == MTLStorageModeManaged && a1 >= (1u << 20) && nvmtl_vk_buffer_import_host(a0, a1, &b->_shadow) == 0) {
        if (nvmtl_vk_buffer_create(a1, 0, &b->_b) == 0) {
            [b didModifyRange:NSMakeRange(0, a1)];
            { static int said; if (!said++) nvlog("newBufferWithBytesNoCopy: managed %lu bytes -> VRAM copy, caller's pages are the shadow", (unsigned long)a1); }
            return b;
        }
        nvlog("newBufferWithBytesNoCopy: managed %lu bytes: VRAM refused - the GPU reads the caller's pages directly", (unsigned long)a1);
        b->_b = b->_shadow; memset(&b->_shadow, 0, sizeof b->_shadow);
        return b;
    }
    if (nvmtl_vk_buffer_import_host(a0, a1, &b->_b) == 0) {
        static int said; if (!said++) nvlog("newBufferWithBytesNoCopy: IMPORTED the caller's pages (%lu bytes at %p) \u2014 zero copy", (unsigned long)a1, a0);
        return b;
    }
    if (nvmtl_vk_buffer_create(a1, 1, &b->_b) || !b->_b.map) {
        nvmtl_alloc_failed("newBufferWithBytesNoCopy");
        nvlog("newBufferWithBytesNoCopy: could neither import nor allocate %lu bytes -> nil", (unsigned long)a1);
        return nil;
    }
    memcpy(b->_b.map, a0, a1);
    b->_hostPtr = a0; b->_hostLen = (size_t)a1;
    // hpin1: the kernel refuses to wire these pages writable (os_lock_user_pages prepare -> KERN_PROTECTION_FAILURE): the
    // caller handed us a read-only mapping (WindowServer wraps r--/r-- shared memory this way). The buffer is copied in at
    // every submission; copying it BACK would write into read-only pages (a fault in WindowServer) or undo what the
    // pages' owner wrote meanwhile, so a read-only mapping is never synced out. The first 8 per process are logged.
    { BOOL ro = NO; int prot = -1, maxp = -1; kern_return_t kr = KERN_SUCCESS;
      mach_vm_address_t at = (mach_vm_address_t)(uintptr_t)a0; const mach_vm_address_t end = at + a1;
      while (at < end) {
          mach_vm_address_t ra = at; mach_vm_size_t rs = 0; vm_region_basic_info_data_64_t ri; mach_msg_type_number_t cnt = VM_REGION_BASIC_INFO_COUNT_64; mach_port_t obj = MACH_PORT_NULL;
          kr = mach_vm_region(mach_task_self(), &ra, &rs, VM_REGION_BASIC_INFO_64, (vm_region_info_t)&ri, &cnt, &obj);
          if (obj != MACH_PORT_NULL) mach_port_deallocate(mach_task_self(), obj);
          if (kr != KERN_SUCCESS || ra > at || !rs) { ro = YES; break; }
          if (prot < 0) { prot = ri.protection; maxp = ri.max_protection; }
          if (!(ri.protection & VM_PROT_WRITE)) { ro = YES; prot = ri.protection; maxp = ri.max_protection; break; }
          at = ra + rs;
      }
      b->_hostRO = ro;
      static _Atomic int said; if (said < 8) { said++;
        nvlog("hpin1 newBufferWithBytesNoCopy: import refused for %lu bytes at %p (storage %d) - prot %d/%d%s (kr %d)",
              (unsigned long)a1, a0, (int)sm, prot, maxp, ro ? ", READ-ONLY: copied in, never synced out" : "", kr); } }
    { static int said; if (!said++) nvlog("newBufferWithBytesNoCopy: COPIED %lu bytes at %p (import alignment %zu) \u2014 resynced at every submission",
                                          (unsigned long)a1, a0, nvmtl_vk_host_import_align()); }
    return b;
}
- (id)newBufferWithDescriptor:(id)a0 { nvlog("newBufferWithDescriptor: -> nil (not built yet)"); return nil; }
- (id)newBufferWithIOSurface:(IOSurfaceRef)a0 { nvlog("newBufferWithIOSurface: -> nil (not built yet)"); return nil; }
- (id)newIndirectCommandBufferWithDescriptor:(id)a0 maxCount:(NSUInteger)a1 options:(NSUInteger)a2 { nvlog("newIndirectCommandBufferWithDescriptor:maxCount:options: -> nil (not built yet)"); return nil; }
- (id)newIndirectComputeCommandEncoderWithBuffer:(id)a0 { nvlog("newIndirectComputeCommandEncoderWithBuffer: -> nil (not built yet)"); return nil; }
- (id)newIndirectRenderCommandEncoderWithBuffer:(id)a0 { nvlog("newIndirectRenderCommandEncoderWithBuffer: -> nil (not built yet)"); return nil; }
- (id)newMotionEstimationPipelineWithDescriptor:(id)a0 { nvlog("newMotionEstimationPipelineWithDescriptor: -> nil (not built yet)"); return nil; }
- (id)newVisibleFunctionTableWithDescriptor:(id)a0 { nvlog("newVisibleFunctionTableWithDescriptor: -> nil (not built yet)"); return nil; }
- (void)receive:(id)a0 { nvlog("receive: (ignored)"); }
- (void)reserveResourceIndicesForResourceType:(NSUInteger)a0 indices:(NSUInteger *)a1 indexCount:(NSUInteger)a2 { nvlog("reserveResourceIndicesForResourceType:indices:indexCount: (ignored)"); }
- (NSUInteger)resourcePatchingTypeForResourceType:(NSUInteger)a0 { return 0; }
- (void)setMetalAssertionsEnabled:(BOOL)a0 { nvlog("setMetalAssertionsEnabled: (ignored)"); }
- (NSUInteger)singleFPConfig { return 0; }
- (MTLSize)sparseTileSizeWithTextureType:(NSUInteger)a0 pixelFormat:(NSUInteger)a1 sampleCount:(NSUInteger)a2 { return MTLSizeMake(32, 32, 1); }
- (MTLSize)sparseTileSizeWithTextureType:(NSUInteger)a0 pixelFormat:(NSUInteger)a1 sampleCount:(NSUInteger)a2 sparsePageSize:(NSInteger)a3 { return MTLSizeMake(32, 32, 1); }
- (MTLSize)tileSizeWithTextureType:(NSUInteger)a0 pixelFormat:(NSUInteger)a1 sampleCount:(NSUInteger)a2 { return MTLSizeMake(32, 32, 1); }
@end
