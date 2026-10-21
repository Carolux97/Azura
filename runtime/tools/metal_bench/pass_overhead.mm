// Is the ~40 us per render pass a fixed cost or proportional to the attachment size?
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <algorithm>
#include <cstdio>
#include <vector>
int main() {
  @autoreleasepool {
    id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
    id<MTLCommandQueue> q = [dev newCommandQueue];
    for (int size : {64, 256, 640, 1280, 2560}) {
      MTLTextureDescriptor* d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:size height:size * 9 / 16 mipmapped:NO];
      d.usage = MTLTextureUsageRenderTarget; d.storageMode = MTLStorageModePrivate;
      id<MTLTexture> t = [dev newTextureWithDescriptor:d];
      for (bool load : {true, false}) {
        std::vector<double> runs;
        for (int r = 0; r < 6; ++r) {
          id<MTLCommandBuffer> cb = [q commandBuffer];
          for (int i = 0; i < 200; ++i) {
            MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
            rp.colorAttachments[0].texture = t;
            rp.colorAttachments[0].loadAction = load ? MTLLoadActionLoad : MTLLoadActionClear;
            rp.colorAttachments[0].storeAction = MTLStoreActionStore;
            id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:rp];
            [e endEncoding];
          }
          [cb commit]; [cb waitUntilCompleted];
          if (r) runs.push_back((cb.GPUEndTime - cb.GPUStartTime) * 1e6 / 200);
        }
        std::sort(runs.begin(), runs.end());
        printf("empty pass %4dx%-4d %-5s+store: %6.1f us\n", size, size * 9 / 16, load ? "load" : "clear", runs[runs.size() / 2]);
      }
    }
    std::vector<double> runs;
    id<MTLLibrary> lib = [dev newLibraryWithSource:@"kernel void k(device uint* b [[buffer(0)]], uint i [[thread_position_in_grid]]) { b[i] += 1; }" options:nil error:nil];
    id<MTLComputePipelineState> cp = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"k"] error:nil];
    id<MTLBuffer> buf = [dev newBufferWithLength:4096 options:MTLResourceStorageModePrivate];
    for (int r = 0; r < 6; ++r) {
      id<MTLCommandBuffer> cb = [q commandBuffer];
      for (int i = 0; i < 200; ++i) {
        id<MTLComputeCommandEncoder> e = [cb computeCommandEncoder];
        [e setComputePipelineState:cp]; [e setBuffer:buf offset:0 atIndex:0];
        [e dispatchThreads:MTLSizeMake(1024, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
        [e endEncoding];
      }
      [cb commit]; [cb waitUntilCompleted];
      if (r) runs.push_back((cb.GPUEndTime - cb.GPUStartTime) * 1e6 / 200);
    }
    std::sort(runs.begin(), runs.end());
    printf("tiny compute encoder: %6.1f us\n", runs[runs.size() / 2]);
  }
}
