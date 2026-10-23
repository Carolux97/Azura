# Debugging patches

Patches kept for investigations, not applied by `scripts/build.sh`. Apply them by hand on a local
checkout, build, and revert them before committing.

## clear-gpu-caches-keys

Keys that drop GPU caches while the game runs, so the next frame uploads their data again from
guest memory. They tell whether a rendering artifact comes from a stale host copy of guest data,
and which cache holds it: if the artifact goes away while a key is held, that cache is wrong; if it
stays with all of them, the wrong data is in guest memory itself.

| Key | Setting | Drops |
|---|---|---|
| F9 | `bind_clear_gpu_caches` | every cache |
| F10 | `bind_clear_gpu_textures` | host textures (converted again from the shared memory) |
| F11 | `bind_clear_gpu_shared_memory` | the GPU copy of guest memory, the textures over it and the vertex buffer state |
| F12 | `bind_clear_gpu_other` | render targets that own no EDRAM data, framebuffers, primitive and uniform buffers |

The keys exist only with `debug_clear_gpu_caches_key = true` in azura.toml, and every press is
logged ("Clearing GPU caches: ..."). On Metal the shared memory is guest memory itself, so F11 has
nothing to drop there.

- `clear-gpu-caches-keys-sdk.patch`: `IGraphicsSystem::ClearCaches` and `ClearCacheParts`, and the
  split by parts in the command processors (Vulkan and Metal). Apply it to the SDK source after
  the patches of `scripts/build.sh`: `git -C sdk/rexglue-0.10.0-src apply ../../patches/debug/clear-gpu-caches-keys-sdk.patch`.
- `clear-gpu-caches-keys-runtime.patch`: the setting and the keys in the runtime. Apply it from the
  repository root: `git apply patches/debug/clear-gpu-caches-keys-runtime.patch`.

Before committing other SDK work, revert the SDK patch (`git apply -R` with the same path), or
`scripts/regen_sdk_patches.sh` takes it into `rexglue-0.10.0-metal-backend.patch`. The Metal part
was never built.

Found the vertex buffer state kept across frames on Vulkan (black patches on faces in cutscenes):
only F11 removed them.
