// tsync2test: CoreGraphics' glyph masks in Safari's GPU process - a buffer of its own memory, a texture made over it
// first, the rows written by the CPU afterwards, then the texture drawn. Metal on Apple reads the written rows. When the
// plugin falls back to a copied texture (the linear view refused: tiled at offset 48, a row pitch NVIDIA's linear images
// cannot take), that copy holds the rows as they were at creation; tsync1 took the rows at the first submission as its
// baseline and never re-uploaded them, so the glyph stayed blank. Cases: tiled@48 BGRA, newTexture@48 BGRA, newTexture
// A8 with a 33-texel row in 64 bytes at offset 16. Uses the INSTALLED plugin only.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/message.h>
static NSString *src = @"#include <metal_stdlib>\n using namespace metal;\n struct VO { float4 p [[position]]; };\n"
"vertex VO vq(uint v [[vertex_id]]) { float2 q[6] = { {-1,-1},{1,-1},{-1,1},{-1,1},{1,-1},{1,1} }; VO o; o.p = float4(q[v], 0, 1); return o; }\n"
"fragment float4 f_read(VO i [[stage_in]], texture2d<float, access::read> t [[texture(0)]]) { return t.read(uint2(i.p.xy)); }\n";
typedef struct { const char *name; MTLPixelFormat fmt; int w, h, bpp, bpr, off, tiled; } Case;
static void fill(uint8_t *p, const Case *c, uint8_t v) {
  for (int y = 0; y < c->h; y++) for (int x = 0; x < c->w; x++) { uint8_t *q = p + c->off + y * c->bpr + x * c->bpp;
    if (c->bpp == 1) q[0] = v; else { q[0] = v; q[1] = (uint8_t)(v + 1); q[2] = (uint8_t)(v + 2); q[3] = 255; } } }
int main(int argc, char **argv) { @autoreleasepool {
  id<MTLDevice> d = MTLCreateSystemDefaultDevice(); if (!d) { printf("no device\n"); return 1; } printf("device %s\n", d.name.UTF8String);
  NSError *e = nil; id<MTLLibrary> l = [d newLibraryWithSource:src options:nil error:&e]; if (!l) { printf("lib: %s\n", e.description.UTF8String); return 1; }
  MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new]; pd.vertexFunction = [l newFunctionWithName:@"vq"];
  pd.fragmentFunction = [l newFunctionWithName:@"f_read"]; pd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
  id<MTLRenderPipelineState> ps = [d newRenderPipelineStateWithDescriptor:pd error:&e]; if (!ps) { printf("PSO: %s\n", e.description.UTF8String); return 1; }
  const Case cases[] = {
    { "tiled@48 BGRA 64x16   ", MTLPixelFormatBGRA8Unorm, 64, 16, 4, 256, 48, 1 },
    { "newTex@48 BGRA 64x16  ", MTLPixelFormatBGRA8Unorm, 64, 16, 4, 256, 48, 0 },
    { "newTex@16 A8 33x33/64 ", MTLPixelFormatA8Unorm,    33, 33, 1,  64, 16, 0 },
    // the sizes Safari's GPU process logged (tex1 tiled texture: offset 0)
    { "tiled@0 A8 33x33/64   ", MTLPixelFormatA8Unorm,    33, 33, 1,  64,  0, 1 },
    { "tiled@0 A8 59x77/64   ", MTLPixelFormatA8Unorm,    59, 77, 1,  64,  0, 1 },
    { "tiled@0 BGRA 316/1264 ", MTLPixelFormatBGRA8Unorm, 316, 316, 4, 1264, 0, 1 },
    { "tiled@0 BGRA 221/896  ", MTLPixelFormatBGRA8Unorm, 221, 221, 4, 896,  0, 1 },
  };
  SEL tsel = sel_registerName("newTiledTextureWithDescriptor:offset:bytesPerRow:");
  id<MTLCommandQueue> q = [d newCommandQueue]; int fails = 0;
  for (size_t ci = 0; ci < sizeof cases / sizeof cases[0]; ci++) {
    const Case *c = &cases[ci];
    id<MTLBuffer> buf = [d newBufferWithLength:(NSUInteger)(c->off + c->bpr * c->h + 4096) options:MTLResourceStorageModeShared];   // zero-filled, own memory
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:c->fmt width:c->w height:c->h mipmapped:NO];
    td.usage = MTLTextureUsageShaderRead; td.storageMode = MTLStorageModeShared;
    id<MTLTexture> t = c->tiled ? ([buf respondsToSelector:tsel] ? ((id (*)(id, SEL, id, NSUInteger, NSUInteger))objc_msgSend)(buf, tsel, td, c->off, c->bpr) : nil)
                                : [buf newTextureWithDescriptor:td offset:c->off bytesPerRow:c->bpr];
    if (!t) { printf("%s texture nil - SKIP\n", c->name); continue; }
    for (int round = 0; round < 3; round++) {
      const uint8_t want = round == 0 ? 0x40 : round == 1 ? 0x40 : 0x90;
      if (round != 1) fill((uint8_t *)buf.contents, c, want);   // the CPU writes after the texture exists (round 1: unchanged)
      MTLTextureDescriptor *od = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:c->w height:c->h mipmapped:NO];
      od.usage = MTLTextureUsageRenderTarget; od.storageMode = MTLStorageModeManaged; id<MTLTexture> o = [d newTextureWithDescriptor:od];
      MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor new]; rp.colorAttachments[0].texture = o;
      rp.colorAttachments[0].loadAction = MTLLoadActionClear; rp.colorAttachments[0].storeAction = MTLStoreActionStore;
      id<MTLCommandBuffer> cb = [q commandBuffer]; id<MTLRenderCommandEncoder> en = [cb renderCommandEncoderWithDescriptor:rp];
      [en setRenderPipelineState:ps]; [en setFragmentTexture:t atIndex:0]; [en drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6]; [en endEncoding];
      id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder]; [b synchronizeResource:o]; [b endEncoding]; [cb commit]; [cb waitUntilCompleted];
      uint8_t *px = malloc((size_t)c->w * c->h * 4); [o getBytes:px bytesPerRow:c->w * 4 fromRegion:MTLRegionMake2D(0, 0, c->w, c->h) mipmapLevel:0];
      // BGRA source -> RGBA target: blue byte (index 2) = want; A8 source -> alpha (index 3) = want
      const int ch = c->bpp == 1 ? 3 : 2; int bad = 0;
      for (int i = 0; i < c->w * c->h; i++) if (abs(px[i * 4 + ch] - want) > 1) bad++;
      printf("%s round %d: first texel %02x %02x %02x %02x (want %02x in byte %d) - %s (%d of %d texels wrong)\n", c->name, round,
             px[0], px[1], px[2], px[3], want, ch, bad ? "FAIL" : "PASS", bad, c->w * c->h);
      fails += bad != 0; free(px);
    }
  }
  printf("%s\n", fails ? "texture sync after creation: FAIL" : "texture sync after creation: PASS"); return fails ? 3 : 0;
} return 0; }
