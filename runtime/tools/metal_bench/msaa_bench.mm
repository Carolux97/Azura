// Phase 1a, second round: can 4x MSAA surfaces be kept in "sample space" (one texel per EDRAM
// sample, a 4x surface as a 1x texture twice as wide and tall), so that reinterpreting an EDRAM
// region between 1x and 4x needs no copy?
//   M1 cost of a typical pixel shader filling 1280x720 at 1x, 4x MSAA, and 2560x1440 1x
//      (sample space, every sample shaded)
//   M2 several passes per frame on the same targets: render 4x MSAA then read it as 1x (the
//      game's pattern) with a transfer, versus sample space with no transfer
// Build: clang++ -std=c++17 -O2 -fobjc-arc msaa_bench.mm -framework Metal -framework Foundation
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <algorithm>
#include <cstdio>
#include <functional>
#include <vector>

static NSString* const kSource = @R"MSL(
#include <metal_stdlib>
using namespace metal;
struct VOut { float4 pos [[position]]; float2 uv; };
vertex VOut vs_fullscreen(uint vid [[vertex_id]], constant float& depth [[buffer(0)]]) {
  float2 p = float2((vid << 1) & 2, vid & 2);
  VOut o; o.pos = float4(p * 2.0 - 1.0, depth, 1.0); o.uv = p; return o;
}
// A moderately heavy pixel shader: 4 texture samples and some ALU, like a lit surface.
fragment float4 fs_surface(VOut in [[stage_in]], texture2d<float> t [[texture(0)]],
                           sampler s [[sampler(0)]]) {
  float4 c = 0;
  for (int i = 0; i < 4; ++i) c += t.sample(s, in.uv * (1.0 + i * 0.37));
  float3 n = normalize(float3(c.xy * 2 - 1, 1));
  float l = max(dot(n, normalize(float3(0.3, 0.5, 0.8))), 0.0);
  return float4(c.rgb * (0.2 + l) * 0.25, 1);
}
// Each 1x texel takes one sample of the 4x surface: 2x2 texels per 4x pixel, as in the EDRAM.
fragment float4 fs_copy_4x_to_1x(VOut in [[stage_in]], texture2d_ms<float> src [[texture(0)]]) {
  uint2 p = uint2(in.pos.xy);
  return src.read(p >> 1, (p.y & 1) * 2 + (p.x & 1));
}
fragment float4 fs_read(VOut in [[stage_in]], texture2d<float> t [[texture(0)]]) {
  return t.read(uint2(in.pos.xy)) * 0.5;
}
)MSL";

struct B {
  id<MTLDevice> dev; id<MTLCommandQueue> q; id<MTLLibrary> lib;
  id<MTLTexture> tex(uint32_t w, uint32_t h, uint32_t samples, MTLPixelFormat f = MTLPixelFormatRGBA8Unorm, bool rt = true) {
    MTLTextureDescriptor* d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:f width:w height:h mipmapped:NO];
    d.textureType = samples > 1 ? MTLTextureType2DMultisample : MTLTextureType2D;
    d.sampleCount = samples; d.storageMode = MTLStorageModePrivate;
    d.usage = (rt ? MTLTextureUsageRenderTarget : 0) | MTLTextureUsageShaderRead;
    return [dev newTextureWithDescriptor:d];
  }
  id<MTLRenderPipelineState> pipe(const char* fs, MTLPixelFormat depth, uint32_t samples) {
    MTLRenderPipelineDescriptor* d = [MTLRenderPipelineDescriptor new];
    d.vertexFunction = [lib newFunctionWithName:@"vs_fullscreen"];
    d.fragmentFunction = [lib newFunctionWithName:@(fs)];
    d.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
    d.depthAttachmentPixelFormat = depth;
    d.rasterSampleCount = samples;
    NSError* e = nil;
    id<MTLRenderPipelineState> p = [dev newRenderPipelineStateWithDescriptor:d error:&e];
    if (!p) { fprintf(stderr, "%s\n", e.localizedDescription.UTF8String); exit(1); }
    return p;
  }
  double time_us(int reps, const std::function<void(id<MTLCommandBuffer>)>& enc) {
    std::vector<double> runs;
    for (int r = 0; r < 7; ++r) {
      id<MTLCommandBuffer> cb = [q commandBuffer];
      for (int i = 0; i < reps; ++i) enc(cb);
      [cb commit]; [cb waitUntilCompleted];
      if (r) runs.push_back((cb.GPUEndTime - cb.GPUStartTime) * 1e6 / reps);
    }
    std::sort(runs.begin(), runs.end()); return runs[runs.size() / 2];
  }
};

int main() {
  @autoreleasepool {
    B b; b.dev = MTLCreateSystemDefaultDevice(); b.q = [b.dev newCommandQueue];
    NSError* e = nil; b.lib = [b.dev newLibraryWithSource:kSource options:nil error:&e];
    if (!b.lib) { fprintf(stderr, "%s\n", e.localizedDescription.UTF8String); return 1; }
    id<MTLTexture> src = b.tex(512, 512, 1, MTLPixelFormatRGBA8Unorm, false);
    std::vector<uint8_t> noise(512 * 512 * 4); arc4random_buf(noise.data(), noise.size());
    // Private textures cannot be written from the CPU: stage through a shared one.
    MTLTextureDescriptor* sd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:512 height:512 mipmapped:NO];
    sd.storageMode = MTLStorageModeShared;
    id<MTLTexture> staging = [b.dev newTextureWithDescriptor:sd];
    [staging replaceRegion:MTLRegionMake2D(0, 0, 512, 512) mipmapLevel:0 withBytes:noise.data() bytesPerRow:2048];
    { id<MTLCommandBuffer> cb = [b.q commandBuffer]; id<MTLBlitCommandEncoder> bl = [cb blitCommandEncoder];
      [bl copyFromTexture:staging toTexture:src]; [bl endEncoding]; [cb commit]; [cb waitUntilCompleted]; }
    MTLSamplerDescriptor* smd = [MTLSamplerDescriptor new];
    smd.minFilter = smd.magFilter = MTLSamplerMinMagFilterLinear; smd.sAddressMode = smd.tAddressMode = MTLSamplerAddressModeRepeat;
    id<MTLSamplerState> smp = [b.dev newSamplerStateWithDescriptor:smd];
    MTLDepthStencilDescriptor* dsd = [MTLDepthStencilDescriptor new];
    dsd.depthCompareFunction = MTLCompareFunctionLessEqual; dsd.depthWriteEnabled = YES;
    id<MTLDepthStencilState> ds = [b.dev newDepthStencilStateWithDescriptor:dsd];

    struct Target { const char* name; uint32_t w, h, samples; };
    Target targets[] = {{"1280x720 1x            ", 1280, 720, 1},
                        {"1280x720 4x MSAA       ", 1280, 720, 4},
                        {"2560x1440 1x (sample space)", 2560, 1440, 1}};
    printf("M1: 4 full-screen layers of a lit, 4-sample pixel shader, depth tested\n");
    for (int round = 0; round < 3; ++round)
    for (auto& t : targets) {
      id<MTLTexture> c = b.tex(t.w, t.h, t.samples), d = b.tex(t.w, t.h, t.samples, MTLPixelFormatDepth32Float);
      id<MTLRenderPipelineState> p = b.pipe("fs_surface", MTLPixelFormatDepth32Float, t.samples);
      double us = b.time_us(20, [&](id<MTLCommandBuffer> cb) {
        MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = c; rp.colorAttachments[0].loadAction = MTLLoadActionClear; rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        rp.depthAttachment.texture = d; rp.depthAttachment.loadAction = MTLLoadActionClear; rp.depthAttachment.storeAction = MTLStoreActionStore;
        id<MTLRenderCommandEncoder> en = [cb renderCommandEncoderWithDescriptor:rp];
        [en setRenderPipelineState:p]; [en setDepthStencilState:ds];
        [en setFragmentTexture:src atIndex:0]; [en setFragmentSamplerState:smp atIndex:0];
        for (int l = 0; l < 4; ++l) { float z = 0.9f - l * 0.1f; [en setVertexBytes:&z length:4 atIndex:0]; [en drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3]; }
        [en endEncoding];
      });
      printf("  %-28s %8.1f us\n", t.name, us);
    }

    printf("\nM2: one 'frame' of the game's pattern, 20 rounds of: draw to a 4x surface, then read\n"
           "    the same EDRAM region as 1x\n");
    {
      id<MTLTexture> c4 = b.tex(1280, 720, 4), d4 = b.tex(1280, 720, 4, MTLPixelFormatDepth32Float);
      id<MTLTexture> c1 = b.tex(2560, 1440, 1), out = b.tex(2560, 1440, 1);
      id<MTLRenderPipelineState> draw4 = b.pipe("fs_surface", MTLPixelFormatDepth32Float, 4);
      id<MTLRenderPipelineState> copy = b.pipe("fs_copy_4x_to_1x", MTLPixelFormatInvalid, 1);
      id<MTLRenderPipelineState> read1 = b.pipe("fs_read", MTLPixelFormatInvalid, 1);
      float z = 0.5f;
      auto pass = [&](id<MTLCommandBuffer> cb, id<MTLTexture> color, id<MTLTexture> depth) {
        MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = color; rp.colorAttachments[0].loadAction = MTLLoadActionLoad; rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        if (depth) { rp.depthAttachment.texture = depth; rp.depthAttachment.loadAction = MTLLoadActionLoad; rp.depthAttachment.storeAction = MTLStoreActionStore; }
        id<MTLRenderCommandEncoder> en = [cb renderCommandEncoderWithDescriptor:rp];
        [en setVertexBytes:&z length:4 atIndex:0];
        return en;
      };
      double transfer = b.time_us(20, [&](id<MTLCommandBuffer> cb) {
        id<MTLRenderCommandEncoder> en = pass(cb, c4, d4);
        [en setRenderPipelineState:draw4]; [en setDepthStencilState:ds]; [en setFragmentTexture:src atIndex:0]; [en setFragmentSamplerState:smp atIndex:0];
        [en drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3]; [en endEncoding];
        en = pass(cb, c1, nil); [en setRenderPipelineState:copy]; [en setFragmentTexture:c4 atIndex:0];
        [en drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3]; [en endEncoding];
        en = pass(cb, out, nil); [en setRenderPipelineState:read1]; [en setFragmentTexture:c1 atIndex:0];
        [en drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3]; [en endEncoding];
      });
      id<MTLTexture> cs = b.tex(2560, 1440, 1), dss = b.tex(2560, 1440, 1, MTLPixelFormatDepth32Float), outs = b.tex(2560, 1440, 1);
      id<MTLRenderPipelineState> draws = b.pipe("fs_surface", MTLPixelFormatDepth32Float, 1);
      double sample_space = b.time_us(20, [&](id<MTLCommandBuffer> cb) {
        id<MTLRenderCommandEncoder> en = pass(cb, cs, dss);
        [en setRenderPipelineState:draws]; [en setDepthStencilState:ds]; [en setFragmentTexture:src atIndex:0]; [en setFragmentSamplerState:smp atIndex:0];
        [en drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3]; [en endEncoding];
        en = pass(cb, outs, nil); [en setRenderPipelineState:read1]; [en setFragmentTexture:cs atIndex:0];
        [en drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3]; [en endEncoding];
      });
      printf("  4x MSAA + transfer to 1x + read : %8.1f us per round\n", transfer);
      printf("  sample space, no transfer + read: %8.1f us per round\n", sample_space);
    }
  }
  return 0;
}
