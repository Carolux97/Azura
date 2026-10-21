// Metal microbenchmarks for the EDRAM emulation design (card 29, phase 1a).
// Measures, on this Mac's GPU, the cost per render pass of the pieces a Xenos EDRAM emulation
// can be built from, at the sizes Gears of War 3 uses (1280x720, 1x and 4x MSAA):
//   T1 a render pass that loads and stores its attachments (what host render targets do now)
//   T2 a render target transfer between 4x and 1x MSAA images (the cost measured in the game)
//   T3 an EDRAM kept in a buffer, loaded into tile memory at the start of a pass and stored back
//      at its end, with memoryless attachments (the tile-memory idea)
//   T4 overdraw with blending: fixed function, programmable blending (framebuffer fetch) and
//      ordered read-modify-write of device memory (what the interlock path does)
// GPU time comes from MTLCommandBuffer GPUStartTime/GPUEndTime over many passes.
//
// Build: clang++ -std=c++17 -O2 -fobjc-arc edram_bench.mm -framework Metal -framework Foundation
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <algorithm>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

static const uint32_t kW = 1280, kH = 720;

static NSString* const kSource = @R"MSL(
#include <metal_stdlib>
using namespace metal;

struct VOut { float4 pos [[position]]; };

// Full-screen triangle, or a small quad-sized triangle at an offset for "game draws".
vertex VOut vs_fullscreen(uint vid [[vertex_id]]) {
  float2 p = float2((vid << 1) & 2, vid & 2);
  VOut o; o.pos = float4(p * 2.0 - 1.0, 0.5, 1.0); return o;
}
vertex VOut vs_small(uint vid [[vertex_id]], constant float2& off [[buffer(0)]]) {
  float2 p = float2((vid << 1) & 2, vid & 2) * 0.05 + off;
  VOut o; o.pos = float4(p, 0.5, 1.0); return o;
}

fragment float4 fs_solid() { return float4(0.3, 0.6, 0.9, 1.0); }

// Noise, so the GPU's lossless render target compression cannot shrink the data.
static float hash(uint2 p, uint s) {
  uint h = p.x * 73856093u ^ p.y * 19349663u ^ s * 83492791u;
  h ^= h >> 13; h *= 0x5bd1e995u; h ^= h >> 15;
  return float(h & 0xffffu) / 65535.0;
}
struct NoiseOut { float4 color [[color(0)]]; float d [[depth(any)]]; };
fragment NoiseOut fs_noise(VOut in [[stage_in]], uint s [[sample_id]]) {
  uint2 p = uint2(in.pos.xy);
  NoiseOut o;
  o.color = float4(hash(p, s), hash(p, s + 7), hash(p, s + 13), 1.0);
  o.d = hash(p, s + 29);
  return o;
}
struct NoiseColorOnly { float4 color [[color(0)]]; };
fragment NoiseColorOnly fs_noise_color(VOut in [[stage_in]], uint s [[sample_id]]) {
  uint2 p = uint2(in.pos.xy);
  NoiseColorOnly o; o.color = float4(hash(p, s), hash(p, s + 7), hash(p, s + 13), 1.0); return o;
}
struct NoiseDepthOnly { float d [[depth(any)]]; };
fragment NoiseDepthOnly fs_noise_depth(VOut in [[stage_in]], uint s [[sample_id]]) {
  NoiseDepthOnly o; o.d = hash(uint2(in.pos.xy), s + 29); return o;
}

// T2: transfers.
fragment float4 fs_color_4x_to_1x(VOut in [[stage_in]], texture2d_ms<float> src [[texture(0)]]) {
  return src.read(uint2(in.pos.xy), 0);
}
fragment float4 fs_color_1x_to_4x(VOut in [[stage_in]], uint s [[sample_id]],
                                  texture2d<float> src [[texture(0)]]) {
  return src.read(uint2(in.pos.xy)) + float4(s * 0.0);
}
struct DepthOut { float d [[depth(any)]]; };
fragment DepthOut fs_depth_4x_to_1x(VOut in [[stage_in]], depth2d_ms<float> src [[texture(0)]]) {
  DepthOut o; o.d = src.read(uint2(in.pos.xy), 0); return o;
}
fragment DepthOut fs_depth_1x_to_4x(VOut in [[stage_in]], uint s [[sample_id]],
                                    depth2d<float> src [[texture(0)]]) {
  DepthOut o; o.d = src.read(uint2(in.pos.xy)) + s * 0.0; return o;
}

// T3: EDRAM in a buffer, loaded into tile memory (color and depth as color) and stored back.
struct TileOut { float4 color [[color(0)]]; float depth [[color(1)]]; };
fragment TileOut fs_edram_load(VOut in [[stage_in]], uint s [[sample_id]],
                               device const uint2* edram [[buffer(0)]],
                               constant uint& samples [[buffer(1)]]) {
  uint2 p = uint2(in.pos.xy);
  uint2 v = edram[(p.y * 1280 + p.x) * samples + s];
  TileOut o; o.color = unpack_unorm4x8_to_float(v.x); o.depth = as_type<float>(v.y); return o;
}
struct TileIn { float4 color [[color(0)]]; float depth [[color(1)]]; };
fragment void fs_edram_store(VOut in [[stage_in]], uint s [[sample_id]], TileIn t,
                             device uint2* edram [[buffer(0)]],
                             constant uint& samples [[buffer(1)]]) {
  uint2 p = uint2(in.pos.xy);
  edram[(p.y * 1280 + p.x) * samples + s] = uint2(pack_float_to_unorm4x8(t.color),
                                                  as_type<uint>(t.depth));
}
// A game draw on the tile EDRAM: software depth test and blend against tile memory.
fragment TileOut fs_edram_draw(VOut in [[stage_in]], TileIn t) {
  TileOut o;
  float z = in.pos.z;
  bool pass = z <= t.depth;
  o.depth = pass ? z : t.depth;
  o.color = pass ? mix(t.color, float4(0.3, 0.6, 0.9, 1.0), 0.5) : t.color;
  return o;
}

// T4: overdraw with blending.
fragment float4 fs_blend_programmable(VOut in [[stage_in]], float4 dst [[color(0)]]) {
  return mix(dst, float4(0.3, 0.6, 0.9, 1.0), 0.5);
}
fragment void fs_blend_ordered_memory(VOut in [[stage_in]],
                                      device uint* edram [[buffer(0), raster_order_group(0)]]) {
  uint2 p = uint2(in.pos.xy);
  uint i = p.y * 1280 + p.x;
  float4 dst = unpack_unorm4x8_to_float(edram[i]);
  edram[i] = pack_float_to_unorm4x8(mix(dst, float4(0.3, 0.6, 0.9, 1.0), 0.5));
}
)MSL";

struct Bench {
  id<MTLDevice> device;
  id<MTLCommandQueue> queue;
  id<MTLLibrary> lib;

  id<MTLFunction> fn(const char* name) {
    id<MTLFunction> f = [lib newFunctionWithName:@(name)];
    if (!f) { fprintf(stderr, "missing function %s\n", name); exit(1); }
    return f;
  }

  id<MTLTexture> texture(MTLPixelFormat format, uint32_t samples, MTLStorageMode storage) {
    MTLTextureDescriptor* d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format
                                                                                 width:kW
                                                                                height:kH
                                                                             mipmapped:NO];
    d.textureType = samples > 1 ? MTLTextureType2DMultisample : MTLTextureType2D;
    d.sampleCount = samples;
    d.storageMode = storage;
    d.usage = MTLTextureUsageRenderTarget | (storage == MTLStorageModeMemoryless
                                                 ? 0
                                                 : MTLTextureUsageShaderRead);
    return [device newTextureWithDescriptor:d];
  }

  id<MTLRenderPipelineState> pipeline(const char* vs, const char* fs,
                                      std::vector<MTLPixelFormat> colors, MTLPixelFormat depth,
                                      uint32_t samples, bool blend = false,
                                      bool write_color = true) {
    MTLRenderPipelineDescriptor* d = [MTLRenderPipelineDescriptor new];
    d.vertexFunction = fn(vs);
    d.fragmentFunction = fn(fs);
    for (size_t i = 0; i < colors.size(); ++i) {
      d.colorAttachments[i].pixelFormat = colors[i];
      if (!write_color) d.colorAttachments[i].writeMask = MTLColorWriteMaskNone;
      if (blend) {
        d.colorAttachments[i].blendingEnabled = YES;
        d.colorAttachments[i].sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
        d.colorAttachments[i].destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
      }
    }
    d.depthAttachmentPixelFormat = depth;
    if (depth == MTLPixelFormatDepth32Float_Stencil8) d.stencilAttachmentPixelFormat = depth;
    d.rasterSampleCount = samples;
    NSError* err = nil;
    id<MTLRenderPipelineState> p = [device newRenderPipelineStateWithDescriptor:d error:&err];
    if (!p) { fprintf(stderr, "pipeline %s/%s: %s\n", vs, fs, err.localizedDescription.UTF8String); exit(1); }
    return p;
  }

  // Fills a color and/or depth render target with noise, once.
  void fill_noise(id<MTLTexture> color, id<MTLTexture> depth) {
    uint32_t samples = (uint32_t)(color ? color.sampleCount : depth.sampleCount);
    MTLPixelFormat cf = color ? color.pixelFormat : MTLPixelFormatInvalid;
    MTLPixelFormat df = depth ? depth.pixelFormat : MTLPixelFormatInvalid;
    const char* fs = color && depth ? "fs_noise" : (color ? "fs_noise_color" : "fs_noise_depth");
    std::vector<MTLPixelFormat> colors;
    if (color) colors.push_back(cf);
    id<MTLRenderPipelineState> p = pipeline("vs_fullscreen", fs, colors, df, samples);
    MTLDepthStencilDescriptor* dsd = [MTLDepthStencilDescriptor new];
    dsd.depthCompareFunction = MTLCompareFunctionAlways;
    dsd.depthWriteEnabled = YES;
    id<MTLDepthStencilState> ds = [device newDepthStencilStateWithDescriptor:dsd];
    id<MTLCommandBuffer> cb = [queue commandBuffer];
    MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
    if (color) {
      rp.colorAttachments[0].texture = color;
      rp.colorAttachments[0].loadAction = MTLLoadActionDontCare;
      rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    }
    if (depth) {
      rp.depthAttachment.texture = depth;
      rp.depthAttachment.loadAction = MTLLoadActionDontCare;
      rp.depthAttachment.storeAction = MTLStoreActionStore;
      if (df == MTLPixelFormatDepth32Float_Stencil8) {
        rp.stencilAttachment.texture = depth;
        rp.stencilAttachment.loadAction = MTLLoadActionClear;
        rp.stencilAttachment.storeAction = MTLStoreActionStore;
      }
    }
    id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:rp];
    [e setRenderPipelineState:p];
    if (depth) [e setDepthStencilState:ds];
    [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [e endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
  }

  // Runs `passes` passes per command buffer, 7 times, and returns the median GPU time per pass
  // in microseconds.
  double time_us(int passes, const std::function<void(id<MTLCommandBuffer>)>& encode_pass) {
    std::vector<double> runs;
    for (int r = 0; r < 7; ++r) {
      id<MTLCommandBuffer> cb = [queue commandBuffer];
      for (int i = 0; i < passes; ++i) encode_pass(cb);
      [cb commit];
      [cb waitUntilCompleted];
      if (cb.error) { fprintf(stderr, "GPU error: %s\n", cb.error.localizedDescription.UTF8String); exit(1); }
      if (r > 0) runs.push_back((cb.GPUEndTime - cb.GPUStartTime) * 1e6 / passes);
    }
    std::sort(runs.begin(), runs.end());
    return runs[runs.size() / 2];
  }
};

int main() {
  @autoreleasepool {
    Bench b;
    b.device = MTLCreateSystemDefaultDevice();
    b.queue = [b.device newCommandQueue];
    NSError* err = nil;
    b.lib = [b.device newLibraryWithSource:kSource options:nil error:&err];
    if (!b.lib) { fprintf(stderr, "MSL: %s\n", err.localizedDescription.UTF8String); return 1; }
    printf("GPU: %s, %ux%u\n\n", b.device.name.UTF8String, kW, kH);

    const MTLPixelFormat kColor = MTLPixelFormatRGBA8Unorm;
    const MTLPixelFormat kDepth = MTLPixelFormatDepth32Float_Stencil8;
    const int kPasses = 200;
    float offsets[4][2] = {{-0.5f, -0.5f}, {0.2f, -0.3f}, {-0.1f, 0.4f}, {0.5f, 0.1f}};

    // ---------- T1: pass that loads and stores its attachments, with 4 small draws ----------
    for (uint32_t samples : {1u, 4u}) {
      id<MTLTexture> color = b.texture(kColor, samples, MTLStorageModePrivate);
      id<MTLTexture> depth = b.texture(kDepth, samples, MTLStorageModePrivate);
      id<MTLRenderPipelineState> p = b.pipeline("vs_small", "fs_solid", {kColor}, kDepth, samples);
      b.fill_noise(color, depth);
      for (bool keep : {true, false}) {
        double us = b.time_us(kPasses, [&](id<MTLCommandBuffer> cb) {
          MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
          rp.colorAttachments[0].texture = color;
          rp.colorAttachments[0].loadAction = keep ? MTLLoadActionLoad : MTLLoadActionDontCare;
          rp.colorAttachments[0].storeAction = keep ? MTLStoreActionStore : MTLStoreActionDontCare;
          rp.depthAttachment.texture = depth;
          rp.depthAttachment.loadAction = rp.colorAttachments[0].loadAction;
          rp.depthAttachment.storeAction = rp.colorAttachments[0].storeAction;
          rp.stencilAttachment.texture = depth;
          rp.stencilAttachment.loadAction = rp.colorAttachments[0].loadAction;
          rp.stencilAttachment.storeAction = rp.colorAttachments[0].storeAction;
          id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:rp];
          [e setRenderPipelineState:p];
          for (auto& o : offsets) {
            [e setVertexBytes:o length:sizeof(o) atIndex:0];
            [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
          }
          [e endEncoding];
        });
        printf("T1 pass, %ux MSAA, color+depth %s, 4 small draws: %8.1f us\n", samples,
               keep ? "load+store       " : "dont care (no IO)", us);
      }
    }
    // T1b: is the per-pass cost fixed, or a wait because each pass depends on the previous one?
    {
      std::vector<id<MTLTexture>> colors, depths;
      for (int i = 0; i < 8; ++i) {
        colors.push_back(b.texture(kColor, 1, MTLStorageModePrivate));
        depths.push_back(b.texture(kDepth, 1, MTLStorageModePrivate));
        b.fill_noise(colors.back(), depths.back());
      }
      id<MTLRenderPipelineState> p = b.pipeline("vs_small", "fs_solid", {kColor}, kDepth, 1);
      for (int draws : {0, 4, 32}) {
        for (int targets : {1, 8}) {
          int next = 0;
          double us = b.time_us(kPasses, [&](id<MTLCommandBuffer> cb) {
            int i = next++ % targets;
            MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
            rp.colorAttachments[0].texture = colors[i];
            rp.colorAttachments[0].loadAction = MTLLoadActionLoad;
            rp.colorAttachments[0].storeAction = MTLStoreActionStore;
            rp.depthAttachment.texture = depths[i];
            rp.depthAttachment.loadAction = MTLLoadActionLoad;
            rp.depthAttachment.storeAction = MTLStoreActionStore;
            id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:rp];
            [e setRenderPipelineState:p];
            for (int d = 0; d < draws; ++d) {
              [e setVertexBytes:offsets[d % 4] length:sizeof(offsets[0]) atIndex:0];
              [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
            }
            [e endEncoding];
          });
          printf("T1b pass, 1x, load+store, %2d small draws, cycling %d target set(s): %8.1f us\n",
                 draws, targets, us);
        }
      }
    }
    printf("\n");

    // ---------- T2: render target transfers between 4x and 1x ----------
    {
      id<MTLTexture> c4 = b.texture(kColor, 4, MTLStorageModePrivate);
      id<MTLTexture> c1 = b.texture(kColor, 1, MTLStorageModePrivate);
      id<MTLTexture> d4 = b.texture(MTLPixelFormatDepth32Float, 4, MTLStorageModePrivate);
      id<MTLTexture> d1 = b.texture(MTLPixelFormatDepth32Float, 1, MTLStorageModePrivate);
      b.fill_noise(c4, nullptr);
      b.fill_noise(c1, nullptr);
      b.fill_noise(nullptr, d4);
      b.fill_noise(nullptr, d1);
      MTLDepthStencilDescriptor* dsd = [MTLDepthStencilDescriptor new];
      dsd.depthCompareFunction = MTLCompareFunctionAlways;
      dsd.depthWriteEnabled = YES;
      id<MTLDepthStencilState> ds = [b.device newDepthStencilStateWithDescriptor:dsd];
      struct Case { const char* name; const char* fs; id<MTLTexture> src; id<MTLTexture> dst; bool depth; };
      Case cases[] = {{"color 4x -> 1x", "fs_color_4x_to_1x", c4, c1, false},
                      {"color 1x -> 4x", "fs_color_1x_to_4x", c1, c4, false},
                      {"depth 4x -> 1x", "fs_depth_4x_to_1x", d4, d1, true},
                      {"depth 1x -> 4x", "fs_depth_1x_to_4x", d1, d4, true}};
      for (auto& c : cases) {
        uint32_t samples = (uint32_t)c.dst.sampleCount;
        id<MTLRenderPipelineState> p =
            c.depth ? b.pipeline("vs_fullscreen", c.fs, {}, MTLPixelFormatDepth32Float, samples)
                    : b.pipeline("vs_fullscreen", c.fs, {kColor}, MTLPixelFormatInvalid, samples);
        double us = b.time_us(kPasses, [&](id<MTLCommandBuffer> cb) {
          MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
          if (c.depth) {
            rp.depthAttachment.texture = c.dst;
            rp.depthAttachment.loadAction = MTLLoadActionDontCare;
            rp.depthAttachment.storeAction = MTLStoreActionStore;
          } else {
            rp.colorAttachments[0].texture = c.dst;
            rp.colorAttachments[0].loadAction = MTLLoadActionDontCare;
            rp.colorAttachments[0].storeAction = MTLStoreActionStore;
          }
          id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:rp];
          [e setRenderPipelineState:p];
          if (c.depth) [e setDepthStencilState:ds];
          [e setFragmentTexture:c.src atIndex:0];
          [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
          [e endEncoding];
        });
        printf("T2 transfer, full %ux%u, %s: %8.1f us\n", kW, kH, c.name, us);
      }
    }
    printf("\n");

    // ---------- T3: EDRAM in a buffer, through tile memory ----------
    for (uint32_t samples : {1u, 4u}) {
      id<MTLBuffer> edram = [b.device newBufferWithLength:(NSUInteger)kW * kH * samples * 8
                                                  options:MTLResourceStorageModeShared];
      arc4random_buf(edram.contents, edram.length);
      id<MTLTexture> color = b.texture(kColor, samples, MTLStorageModeMemoryless);
      id<MTLTexture> zcol = b.texture(MTLPixelFormatR32Float, samples, MTLStorageModeMemoryless);
      std::vector<MTLPixelFormat> formats = {kColor, MTLPixelFormatR32Float};
      id<MTLRenderPipelineState> load =
          b.pipeline("vs_fullscreen", "fs_edram_load", formats, MTLPixelFormatInvalid, samples);
      id<MTLRenderPipelineState> draw =
          b.pipeline("vs_small", "fs_edram_draw", formats, MTLPixelFormatInvalid, samples);
      id<MTLRenderPipelineState> store = b.pipeline("vs_fullscreen", "fs_edram_store", formats,
                                                    MTLPixelFormatInvalid, samples, false, false);
      for (bool with_draws : {false, true}) {
        double us = b.time_us(kPasses, [&](id<MTLCommandBuffer> cb) {
          MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
          for (int i = 0; i < 2; ++i) {
            rp.colorAttachments[i].texture = i ? zcol : color;
            rp.colorAttachments[i].loadAction = MTLLoadActionDontCare;
            rp.colorAttachments[i].storeAction = MTLStoreActionDontCare;
          }
          id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:rp];
          [e setFragmentBuffer:edram offset:0 atIndex:0];
          [e setFragmentBytes:&samples length:sizeof(samples) atIndex:1];
          [e setRenderPipelineState:load];
          [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
          if (with_draws) {
            [e setRenderPipelineState:draw];
            for (auto& o : offsets) {
              [e setVertexBytes:o length:sizeof(o) atIndex:0];
              [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
            }
          }
          [e setRenderPipelineState:store];
          [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
          [e endEncoding];
        });
        printf("T3 tile EDRAM pass, %ux MSAA, load+store from buffer%s: %8.1f us\n", samples,
               with_draws ? " + 4 small draws" : "                ", us);
      }
    }
    printf("\n");

    // ---------- T4: 8 full-screen layers of blended overdraw, 1x ----------
    {
      const int kLayers = 8;
      id<MTLTexture> color = b.texture(kColor, 1, MTLStorageModePrivate);
      b.fill_noise(color, nullptr);
      id<MTLTexture> scratch = b.texture(kColor, 1, MTLStorageModeMemoryless);
      id<MTLBuffer> edram = [b.device newBufferWithLength:(NSUInteger)kW * kH * 4
                                                  options:MTLResourceStorageModeShared];
      id<MTLRenderPipelineState> fixed =
          b.pipeline("vs_fullscreen", "fs_solid", {kColor}, MTLPixelFormatInvalid, 1, true);
      id<MTLRenderPipelineState> prog = b.pipeline("vs_fullscreen", "fs_blend_programmable",
                                                   {kColor}, MTLPixelFormatInvalid, 1);
      id<MTLRenderPipelineState> ordered =
          b.pipeline("vs_fullscreen", "fs_blend_ordered_memory", {kColor}, MTLPixelFormatInvalid,
                     1, false, false);
      struct Case { const char* name; id<MTLRenderPipelineState> p; bool memory; };
      Case cases[] = {{"fixed-function blending             ", fixed, false},
                      {"programmable blending (tile memory) ", prog, false},
                      {"ordered read-modify-write of memory ", ordered, true}};
      for (auto& c : cases) {
        double us = b.time_us(50, [&](id<MTLCommandBuffer> cb) {
          MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
          rp.colorAttachments[0].texture = c.memory ? scratch : color;
          rp.colorAttachments[0].loadAction = c.memory ? MTLLoadActionDontCare : MTLLoadActionLoad;
          rp.colorAttachments[0].storeAction =
              c.memory ? MTLStoreActionDontCare : MTLStoreActionStore;
          id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:rp];
          [e setRenderPipelineState:c.p];
          [e setFragmentBuffer:edram offset:0 atIndex:0];
          for (int l = 0; l < kLayers; ++l)
            [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
          [e endEncoding];
        });
        printf("T4 %d blended layers, %s: %8.1f us\n", kLayers, c.name, us);
      }
    }
  }
  return 0;
}
