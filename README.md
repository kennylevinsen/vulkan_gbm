# Vulkan GBM backend

Vulkan implementation of GBM, loadable with `GBM_BACKEND=vulkan` when installed in the GBM search path, or when the install location is explicitly set with `GBM_BACKENDS_PATH`.

The GBM backend bundled by Mesa uses the gallium drivers, meaning that combining Vulkan and GBM results in initializing both Vulkan and GL driver stacks within the same process. This driver instead uses Vulkan to back memory allocations, avoiding the dual driver situation.

## Caveats

- It does not implement `gbm_surface`, `gbm_bo_write`, protected BO's or BO mapping. It focuses on what display servers like those made with wlroots require.
- There is no Vulkan usage bit for scanout-compatible buffers, making `GBM_BO_SCANOUT` nothing but a wish. A mesa extension would allow us to propagate this information.
- GBM does not allow us to attach additional information, so we cannot share the same VkPhysicalDevice instance and thereby shader caches, etc. It is more efficient for e.g. display servers to do this internally, but this is not reasonable unless a scanout extension is formalized.
- This should not be needed once DMA-BUF heaps are adopted by GPU drivers.

## How to discuss

Go to [#kennylevinsen @ irc.libera.chat](ircs://irc.libera.chat/#kennylevinsen) to discuss.
