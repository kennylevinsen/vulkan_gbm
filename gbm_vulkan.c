#include <stdlib.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <assert.h>
#include <sys/sysmacros.h>
#include <inttypes.h>
#include <xf86drm.h>
#include <errno.h>
#include <drm_fourcc.h>

#include "gbm_backend_abi.h"

#include <vulkan/vulkan.h>
#include <vulkan/vulkan_core.h>

#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))

static const struct gbm_core *core;

struct gbm_vulkan_device {
        struct gbm_device base;

        VkInstance instance;
        VkPhysicalDevice physical_device;
        VkDevice device;

        struct vulkan_format_props *format_props;
        uint32_t format_prop_count;

        struct {
                PFN_vkGetMemoryFdKHR vkGetMemoryFdKHR;
                PFN_vkGetImageDrmFormatModifierPropertiesEXT vkGetImageDrmFormatModifierPropertiesEXT;
        } api;
};

static inline struct gbm_vulkan_device *gbm_vulkan_device(struct gbm_device *gbm) {
        return (struct gbm_vulkan_device *) gbm;
}

struct vulkan_format {
	uint32_t drm;
	VkFormat vk;
	VkFormat vk_srgb; // sRGB version of the format, or 0 if nonexistent
};

struct vulkan_format_modifier_props {
	VkDrmFormatModifierPropertiesEXT props;
	VkExtent2D max_extent;
};

struct vulkan_format_props {
	struct vulkan_format format;

	uint32_t render_mod_count;
	struct vulkan_format_modifier_props *render_mods;
	uint32_t texture_mod_count;
	struct vulkan_format_modifier_props *texture_mods;
};

void vulkan_format_props_finish(struct vulkan_format_props *props) {
	free(props->render_mods);
	free(props->texture_mods);
}

static const struct vulkan_format formats[] = {
	// Vulkan non-packed 8-bits-per-channel formats have an inverted channel
	// order compared to the DRM formats, because DRM format channel order
	// is little-endian while Vulkan format channel order is in memory byte
	// order.
	{
		.drm = DRM_FORMAT_R8,
		.vk = VK_FORMAT_R8_UNORM,
		.vk_srgb = VK_FORMAT_R8_SRGB,
	},
	{
		.drm = DRM_FORMAT_GR88,
		.vk = VK_FORMAT_R8G8_UNORM,
		.vk_srgb = VK_FORMAT_R8G8_SRGB,
	},
	{
		.drm = DRM_FORMAT_RGB888,
		.vk = VK_FORMAT_B8G8R8_UNORM,
		.vk_srgb = VK_FORMAT_B8G8R8_SRGB,
	},
	{
		.drm = DRM_FORMAT_BGR888,
		.vk = VK_FORMAT_R8G8B8_UNORM,
		.vk_srgb = VK_FORMAT_R8G8B8_SRGB,
	},
	{
		.drm = DRM_FORMAT_XRGB8888,
		.vk = VK_FORMAT_B8G8R8A8_UNORM,
		.vk_srgb = VK_FORMAT_B8G8R8A8_SRGB,
	},
	{
		.drm = DRM_FORMAT_XBGR8888,
		.vk = VK_FORMAT_R8G8B8A8_UNORM,
		.vk_srgb = VK_FORMAT_R8G8B8A8_SRGB,
	},
	// The Vulkan _SRGB formats correspond to unpremultiplied alpha, but
	// the Wayland protocol specifies premultiplied alpha on electrical values
	{
		.drm = DRM_FORMAT_ARGB8888,
		.vk = VK_FORMAT_B8G8R8A8_UNORM,
	},
	{
		.drm = DRM_FORMAT_ABGR8888,
		.vk = VK_FORMAT_R8G8B8A8_UNORM,
	},
	// Vulkan packed formats have the same channel order as DRM formats on
	// little endian systems.
#if CPU_LITTLE_ENDIAN
	{
		.drm = DRM_FORMAT_RGBA4444,
		.vk = VK_FORMAT_R4G4B4A4_UNORM_PACK16,
	},
	{
		.drm = DRM_FORMAT_RGBX4444,
		.vk = VK_FORMAT_R4G4B4A4_UNORM_PACK16,
	},
	{
		.drm = DRM_FORMAT_BGRA4444,
		.vk = VK_FORMAT_B4G4R4A4_UNORM_PACK16,
	},
	{
		.drm = DRM_FORMAT_BGRX4444,
		.vk = VK_FORMAT_B4G4R4A4_UNORM_PACK16,
	},
	{
		.drm = DRM_FORMAT_RGB565,
		.vk = VK_FORMAT_R5G6B5_UNORM_PACK16,
	},
	{
		.drm = DRM_FORMAT_BGR565,
		.vk = VK_FORMAT_B5G6R5_UNORM_PACK16,
	},
	{
		.drm = DRM_FORMAT_RGBA5551,
		.vk = VK_FORMAT_R5G5B5A1_UNORM_PACK16,
	},
	{
		.drm = DRM_FORMAT_RGBX5551,
		.vk = VK_FORMAT_R5G5B5A1_UNORM_PACK16,
	},
	{
		.drm = DRM_FORMAT_BGRA5551,
		.vk = VK_FORMAT_B5G5R5A1_UNORM_PACK16,
	},
	{
		.drm = DRM_FORMAT_BGRX5551,
		.vk = VK_FORMAT_B5G5R5A1_UNORM_PACK16,
	},
	{
		.drm = DRM_FORMAT_ARGB1555,
		.vk = VK_FORMAT_A1R5G5B5_UNORM_PACK16,
	},
	{
		.drm = DRM_FORMAT_XRGB1555,
		.vk = VK_FORMAT_A1R5G5B5_UNORM_PACK16,
	},

	{
		.drm = DRM_FORMAT_ARGB2101010,
		.vk = VK_FORMAT_A2R10G10B10_UNORM_PACK32,
	},
	{
		.drm = DRM_FORMAT_XRGB2101010,
		.vk = VK_FORMAT_A2R10G10B10_UNORM_PACK32,
	},
	{
		.drm = DRM_FORMAT_ABGR2101010,
		.vk = VK_FORMAT_A2B10G10R10_UNORM_PACK32,
	},
	{
		.drm = DRM_FORMAT_XBGR2101010,
		.vk = VK_FORMAT_A2B10G10R10_UNORM_PACK32,
	},
#endif

	// Vulkan 16-bits-per-channel formats have an inverted channel order
	// compared to DRM formats, just like the 8-bits-per-channel ones.
	// On little endian systems the memory representation of each channel
	// matches the DRM formats'.
#if CPU_LITTLE_ENDIAN
	{
		.drm = DRM_FORMAT_ABGR16161616,
		.vk = VK_FORMAT_R16G16B16A16_UNORM,
	},
	{
		.drm = DRM_FORMAT_XBGR16161616,
		.vk = VK_FORMAT_R16G16B16A16_UNORM,
	},
	{
		.drm = DRM_FORMAT_ABGR16161616F,
		.vk = VK_FORMAT_R16G16B16A16_SFLOAT,
	},
	{
		.drm = DRM_FORMAT_XBGR16161616F,
		.vk = VK_FORMAT_R16G16B16A16_SFLOAT,
	},
#endif
};

const struct vulkan_format *vulkan_get_format_from_drm(uint32_t drm_format) {
	for (unsigned i = 0; i < sizeof(formats) / sizeof(formats[0]); ++i) {
		if (formats[i].drm == drm_format) {
			return &formats[i];
		}
	}
	return NULL;
}

struct vulkan_format_props *vulkan_format_props_from_drm(struct gbm_vulkan_device *dev, uint32_t drm_fmt) {
	for (size_t i = 0u; i < dev->format_prop_count; ++i) {
		if (dev->format_props[i].format.drm == drm_fmt) {
			return &dev->format_props[i];
		}
	}
	return NULL;
}

const struct vulkan_format_modifier_props *vulkan_format_props_find_modifier(
		const struct vulkan_format_props *props, uint64_t mod, bool render) {
	if (render) {
		for (uint32_t i = 0; i < props->render_mod_count; ++i) {
			if (props->render_mods[i].props.drmFormatModifier == mod) {
				return &props->render_mods[i];
			}
		}
	} else {
		for (uint32_t i = 0; i < props->texture_mod_count; ++i) {
			if (props->texture_mods[i].props.drmFormatModifier == mod) {
				return &props->texture_mods[i];
			}
		}

	}
	return NULL;
}

static int vulkan_find_mem_type(VkPhysicalDevice phdev,VkMemoryPropertyFlags flags,
		uint32_t req_bits) {
	VkPhysicalDeviceMemoryProperties props;
	vkGetPhysicalDeviceMemoryProperties(phdev, &props);

	for (unsigned i = 0u; i < props.memoryTypeCount; ++i) {
		if (req_bits & (1 << i)) {
			if ((props.memoryTypes[i].propertyFlags & flags) == flags) {
				return i;
			}
		}
	}

	return -1;
}

struct gbm_vulkan_bo {
        struct gbm_bo base;
        VkImage image;
        VkDeviceMemory memory;
        uint64_t modifier;
        size_t plane_cnt;
};

static inline struct gbm_vulkan_bo *gbm_vulkan_bo(struct gbm_bo *bo) {
        return (struct gbm_vulkan_bo *) bo;
}

static void gbm_vulkan_bo_destroy(struct gbm_bo *_bo) {
	struct gbm_vulkan_device *vulkan = gbm_vulkan_device(_bo->gbm);
	struct gbm_vulkan_bo *bo = gbm_vulkan_bo(_bo);

	if (bo->memory) {
		vkFreeMemory(vulkan->device, bo->memory, NULL);
	}
	if (bo->image) {
		vkDestroyImage(vulkan->device, bo->image, NULL);
	}
	free(bo);
}

static struct gbm_bo * gbm_vulkan_bo_create(struct gbm_device *gbm,
		uint32_t width, uint32_t height, uint32_t format, uint32_t usage,
		const uint64_t *modifiers, const unsigned int count) {
	struct gbm_vulkan_device *vulkan = gbm_vulkan_device(gbm);

	format = core->v0.format_canonicalize(format);

	if (usage & (GBM_BO_USE_PROTECTED | GBM_BO_USE_WRITE)) {
		// TODO: Dumb buffer fallback?
		return NULL;
	}

	struct gbm_vulkan_bo *bo = calloc(1, sizeof *bo);
	if (bo == NULL) {
		return NULL;
	}

	bo->base.gbm = gbm;
	bo->base.v0.width = width;
	bo->base.v0.height = height;
	bo->base.v0.format = format;

	const struct vulkan_format_props *format_props =
		vulkan_format_props_from_drm(vulkan, format);
	if (!format_props) {
		fprintf(stderr, "no matching drm format 0x%08x available\n", format);
		gbm_vulkan_bo_destroy(&bo->base);
		return NULL;
	}

	int filtered_mods_count = 0;
	uint64_t filtered_mods[count];
	for (unsigned int idx = 0; idx < count; idx++) {
		const struct vulkan_format_modifier_props *mod_props =
			vulkan_format_props_find_modifier(format_props, modifiers[idx], usage & GBM_BO_USE_RENDERING);
		if (mod_props == NULL) {
			continue;
		}

		// Why does vkImageCreateInfo not filter this when picking a modifier?!
		if (mod_props->max_extent.width < width ||
				mod_props->max_extent.height < height) {
			continue;
		}
		filtered_mods[filtered_mods_count++] = mod_props->props.drmFormatModifier;
	}


	VkImageDrmFormatModifierListCreateInfoEXT drm_format_mod = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT,
		.drmFormatModifierCount = filtered_mods_count,
		.pDrmFormatModifiers = filtered_mods,
	};
	VkExternalMemoryImageCreateInfo ext_mem = {
		.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
		.pNext = &drm_format_mod,
		.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
	};
	VkImageCreateInfo img_create = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.pNext = &ext_mem,
		.imageType = VK_IMAGE_TYPE_2D,
		.extent = { .width = width, .height = height, .depth = 1 },
		.mipLevels = 1,
		.arrayLayers = 1,
		.format = format_props->format.vk,
		.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
		.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
		.usage = VK_IMAGE_USAGE_SAMPLED_BIT,
		.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
		.samples = VK_SAMPLE_COUNT_1_BIT,
	};

	if (vkCreateImage(vulkan->device, &img_create, NULL, &bo->image) != VK_SUCCESS) {
		gbm_vulkan_bo_destroy(&bo->base);
		return NULL;
	}

	VkMemoryRequirements mem_reqs = {0};
	vkGetImageMemoryRequirements(vulkan->device, bo->image, &mem_reqs);

	int mem_type_index = vulkan_find_mem_type(vulkan->physical_device,
		VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, mem_reqs.memoryTypeBits);
	if (mem_type_index == -1) {
		gbm_vulkan_bo_destroy(&bo->base);
		return NULL;
	}

	VkExportMemoryAllocateInfo export_mem = {
		.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,
		.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
	};
	VkMemoryAllocateInfo mem_alloc = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.pNext = &export_mem,
		.allocationSize = mem_reqs.size,
		.memoryTypeIndex = mem_type_index,
	};

	if (vkAllocateMemory(vulkan->device, &mem_alloc, NULL, &bo->memory) != VK_SUCCESS) {
		gbm_vulkan_bo_destroy(&bo->base);
		return NULL;
	}

	if (vkBindImageMemory(vulkan->device, bo->image, bo->memory, 0) != VK_SUCCESS) {
		gbm_vulkan_bo_destroy(&bo->base);
		return NULL;
	}

	VkImageDrmFormatModifierPropertiesEXT img_mod_props = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_PROPERTIES_EXT,
	};
	if (vulkan->api.vkGetImageDrmFormatModifierPropertiesEXT(
				vulkan->device, bo->image, &img_mod_props) != VK_SUCCESS) {
		gbm_vulkan_bo_destroy(&bo->base);
		return NULL;
	}

	bo->modifier = img_mod_props.drmFormatModifier;
	const struct vulkan_format_modifier_props *mod_props =
		vulkan_format_props_find_modifier(format_props, img_mod_props.drmFormatModifier, usage & GBM_BO_USE_RENDERING);
	assert(mod_props != NULL);
	bo->plane_cnt = mod_props->props.drmFormatModifierPlaneCount;
	return &bo->base;
}

static int gbm_vulkan_bo_get_fd(struct gbm_bo *_bo) {
	struct gbm_vulkan_bo *bo = gbm_vulkan_bo(_bo);
	struct gbm_vulkan_device *dev = gbm_vulkan_device(_bo->gbm);
	int fd;

	if (bo->image == NULL) {
		return -1;
	}

	VkMemoryGetFdInfoKHR mem_get_fd = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR,
		.memory = bo->memory,
		.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
	};
	if (dev->api.vkGetMemoryFdKHR(dev->device, &mem_get_fd, &fd) != VK_SUCCESS) {
		return -1;
	}

	return fd;
}

static int gbm_vulkan_bo_get_plane_fd(struct gbm_bo *_bo, int plane) {
	struct gbm_vulkan_bo *bo = gbm_vulkan_bo(_bo);

	if ((size_t)plane >= bo->plane_cnt) {
		return -1;
	}

	// TODO: Cache and dup?
	return gbm_vulkan_bo_get_fd(_bo);
}

static uint64_t gbm_vulkan_bo_get_modifier(struct gbm_bo *_bo) {
	struct gbm_vulkan_bo *bo = gbm_vulkan_bo(_bo);
	if (!bo->image) {
		return DRM_FORMAT_MOD_LINEAR;
	}
	return bo->modifier;
}

static union gbm_bo_handle gbm_vulkan_bo_get_handle_for_plane(struct gbm_bo *_bo, int plane) {
	struct gbm_vulkan_bo *bo = gbm_vulkan_bo(_bo);

	if ((size_t)plane >= bo->plane_cnt) {
		return (union gbm_bo_handle){0};
	}

	// TODO: Anything better to return?
	union gbm_bo_handle ret;
	ret.ptr = _bo;
	return ret;
}

static uint32_t gbm_vulkan_bo_get_offset(struct gbm_bo *_bo, int plane) {
	struct gbm_vulkan_device *dev = gbm_vulkan_device(_bo->gbm);
	struct gbm_vulkan_bo *bo = gbm_vulkan_bo(_bo);

	if ((size_t)plane >= bo->plane_cnt) {
		return 0;
	}

	const VkImageAspectFlagBits plane_aspects[] = {
		VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT,
		VK_IMAGE_ASPECT_MEMORY_PLANE_1_BIT_EXT,
		VK_IMAGE_ASPECT_MEMORY_PLANE_2_BIT_EXT,
		VK_IMAGE_ASPECT_MEMORY_PLANE_3_BIT_EXT,
	};
	assert(bo->plane_cnt <=sizeof(plane_aspects) / sizeof(plane_aspects[0]));

	VkImageSubresource img_subres = {
		.aspectMask = plane_aspects[plane],
	};
	VkSubresourceLayout subres_layout = {0};
	vkGetImageSubresourceLayout(dev->device, bo->image, &img_subres, &subres_layout);
	return subres_layout.offset;
}

static uint32_t gbm_vulkan_bo_get_stride(struct gbm_bo *_bo, int plane) {
	struct gbm_vulkan_device *dev = gbm_vulkan_device(_bo->gbm);
	struct gbm_vulkan_bo *bo = gbm_vulkan_bo(_bo);

	if ((size_t)plane >= bo->plane_cnt) {
		return 0;
	}

	const VkImageAspectFlagBits plane_aspects[] = {
		VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT,
		VK_IMAGE_ASPECT_MEMORY_PLANE_1_BIT_EXT,
		VK_IMAGE_ASPECT_MEMORY_PLANE_2_BIT_EXT,
		VK_IMAGE_ASPECT_MEMORY_PLANE_3_BIT_EXT,
	};
	assert(bo->plane_cnt <=sizeof(plane_aspects) / sizeof(plane_aspects[0]));

	VkImageSubresource img_subres = {
		.aspectMask = plane_aspects[plane],
	};
	VkSubresourceLayout subres_layout = {0};
	vkGetImageSubresourceLayout(dev->device, bo->image, &img_subres, &subres_layout);
	return subres_layout.rowPitch;
}

static int gbm_vulkan_bo_get_planes(struct gbm_bo *_bo) {
	struct gbm_vulkan_bo *bo = gbm_vulkan_bo(_bo);
	return bo->plane_cnt;
}

static int gbm_vulkan_get_format_modifier_plane_count(struct gbm_device *gbm,
		uint32_t format, uint64_t modifier) {
	struct gbm_vulkan_device *dev = gbm_vulkan_device(gbm);
	struct vulkan_format_props *format_props = vulkan_format_props_from_drm(dev, format);

	const struct vulkan_format_modifier_props *mod_props =
		vulkan_format_props_find_modifier(format_props, modifier, false);
	if (mod_props == NULL) {
		return 0;
	}
	assert(mod_props != NULL);
	return mod_props->props.drmFormatModifierPlaneCount;
}

static int gbm_vulkan_is_format_supported(struct gbm_device *gbm, uint32_t format, uint32_t usage) {
	struct gbm_vulkan_device *dev = gbm_vulkan_device(gbm);
	if (usage & (GBM_BO_USE_WRITE | GBM_BO_USE_PROTECTED)) {
		// These usages are not implemented
		return 0;
	}
	struct vulkan_format_props *format_props = vulkan_format_props_from_drm(dev, format);
	if (format_props == NULL) {
		return 0;
	}
	return 1;
}

static int gbm_vulkan_bo_write(struct gbm_bo *_bo, const void *buf, size_t count) {
	(void)_bo, (void)buf, (void)count;
	errno = EINVAL;
	// NOT IMPLEMENTED
	return -1;
}

static struct gbm_bo *gbm_vulkan_bo_import(struct gbm_device *gbm, uint32_t type,
		void *buffer, uint32_t usage) {
	(void)gbm, (void)type, (void)buffer, (void)usage;
	// NOT IMPLEMENTED
	return NULL;
}

static void *gbm_vulkan_bo_map(struct gbm_bo *_bo, uint32_t x, uint32_t y,
		uint32_t width, uint32_t height, uint32_t flags, uint32_t *stride, void **map_data) {
	(void)_bo, (void)x, (void)y, (void)width, (void)height, (void)flags, (void)stride, (void)map_data;
	errno = EINVAL;
	// NOT IMPLEMENTEd
	return NULL;
}

static void gbm_vulkan_bo_unmap(struct gbm_bo *_bo, void *map_data) {
	(void)_bo, (void)map_data;
	// NOT IMPLEMENTED
}

static struct gbm_surface *gbm_vulkan_surface_create(struct gbm_device *gbm,
		uint32_t width, uint32_t height, uint32_t format, uint32_t flags,
		const uint64_t *modifiers, const unsigned count) {
	(void)gbm, (void)width, (void)height, (void)format, (void)flags, (void)modifiers, (void)count;
	// NOT IMPLEMENTED
	return NULL;
}

static void gbm_vulkan_surface_destroy(struct gbm_surface *_surf) {
	(void)_surf;
	// NOT IMPLEMENTED
}

static struct gbm_bo *gbm_vulkan_surface_lock_front_buffer(struct gbm_surface *_surf) {
	(void)_surf;
	// NOT IMPLEMENTED
	return NULL;
}

static void gbm_vulkan_surface_release_buffer(struct gbm_surface *_surf, struct gbm_bo *_bo) {
	(void)_surf, (void)_bo;
	// NOT IMPLEMENTED
}

static int gbm_vulkan_surface_has_free_buffers(struct gbm_surface *_surf) {
	(void)_surf;
	// NOT IMPLEMENTED
	return -1;
}

static void log_phdev(const VkPhysicalDeviceProperties *props) {
	uint32_t vv_major = VK_VERSION_MAJOR(props->apiVersion);
	uint32_t vv_minor = VK_VERSION_MINOR(props->apiVersion);
	uint32_t vv_patch = VK_VERSION_PATCH(props->apiVersion);

	uint32_t dv_major = VK_VERSION_MAJOR(props->driverVersion);
	uint32_t dv_minor = VK_VERSION_MINOR(props->driverVersion);
	uint32_t dv_patch = VK_VERSION_PATCH(props->driverVersion);

	const char *dev_type = "unknown";
	switch (props->deviceType) {
	case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
		dev_type = "integrated";
		break;
	case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
		dev_type = "discrete";
		break;
	case VK_PHYSICAL_DEVICE_TYPE_CPU:
		dev_type = "cpu";
		break;
	case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:
		dev_type = "vgpu";
		break;
	default:
		break;
	}

	fprintf(stderr, "Vulkan device: %s, type: %s, supported API version: %u.%u.%u, driver version: %u.%u.%u\n",
		props->deviceName, dev_type, vv_major, vv_minor, vv_patch, dv_major, dv_minor, dv_patch);
}

static bool check_extension(const VkExtensionProperties *avail, uint32_t avail_len, const char *name) {
	for (size_t i = 0; i < avail_len; i++) {
		if (strcmp(avail[i].extensionName, name) == 0) {
			return true;
		}
	}
	return false;
}

static int vulkan_select_queue_family(VkPhysicalDevice phdev) {
	uint32_t qfam_count;
	vkGetPhysicalDeviceQueueFamilyProperties(phdev, &qfam_count, NULL);
	assert(qfam_count > 0);
	VkQueueFamilyProperties queue_props[qfam_count];
	vkGetPhysicalDeviceQueueFamilyProperties(phdev, &qfam_count, queue_props);

	for (unsigned i = 0u; i < qfam_count; ++i) {
		if (queue_props[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
			return i;
		}
	}
	return -1;
}


static bool query_modifier_usage_support(VkPhysicalDevice phdev,
		VkFormat vk_format, VkFormat vk_format_variant, VkImageUsageFlags usage,
		const VkDrmFormatModifierPropertiesEXT *m,
		struct vulkan_format_modifier_props *out) {
	VkFormat view_formats[2] = {
		vk_format,
		vk_format_variant,
	};
	VkImageFormatListCreateInfoKHR listi = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO_KHR,
		.pViewFormats = view_formats,
		.viewFormatCount = vk_format_variant ? 2 : 1,
	};
	VkPhysicalDeviceImageDrmFormatModifierInfoEXT modi = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT,
		.drmFormatModifier = m->drmFormatModifier,
		.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
		.pNext = &listi,
	};
	VkPhysicalDeviceExternalImageFormatInfo efmti = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO,
		.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
		.pNext = &modi,
	};
	VkPhysicalDeviceImageFormatInfo2 fmti = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,
		.type = VK_IMAGE_TYPE_2D,
		.format = vk_format,
		.usage = usage,
		.flags = vk_format_variant ? VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT : 0,
		.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
		.pNext = &efmti,
	};

	VkExternalImageFormatProperties efmtp = {
		.sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES,
	};
	VkImageFormatProperties2 ifmtp = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2,
		.pNext = &efmtp,
	};
	const VkExternalMemoryProperties *emp = &efmtp.externalMemoryProperties;

	if (vkGetPhysicalDeviceImageFormatProperties2(phdev, &fmti, &ifmtp) != VK_SUCCESS) {
		return false;
	} else if (!(emp->externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT)) {
		return false;
	}

	VkExtent3D me = ifmtp.imageFormatProperties.maxExtent;
	*out = (struct vulkan_format_modifier_props){
		.props = *m,
		.max_extent.width = me.width,
		.max_extent.height = me.height,
	};
	return true;
}

static bool query_modifier_support(VkPhysicalDevice phdev,
		struct vulkan_format_props *props, size_t modifier_count) {
	VkDrmFormatModifierPropertiesListEXT modp = {
		.sType = VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT,
		.drmFormatModifierCount = modifier_count,
	};
	VkFormatProperties2 fmtp = {
		.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2,
		.pNext = &modp,
	};

	modp.pDrmFormatModifierProperties =
		calloc(modifier_count, sizeof(*modp.pDrmFormatModifierProperties));
	if (!modp.pDrmFormatModifierProperties) {
		return false;
	}

	vkGetPhysicalDeviceFormatProperties2(phdev, props->format.vk, &fmtp);

	props->render_mods =
		calloc(modp.drmFormatModifierCount, sizeof(*props->render_mods));
	props->texture_mods =
		calloc(modp.drmFormatModifierCount, sizeof(*props->texture_mods));
	if (!props->render_mods || !props->texture_mods) {
		free(modp.pDrmFormatModifierProperties);
		free(props->render_mods);
		free(props->texture_mods);
		props->render_mods = NULL;
		props->texture_mods = NULL;
		return false;
	}

	const VkImageUsageFlags vulkan_render_usage =
		VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
		VK_IMAGE_USAGE_SAMPLED_BIT |
		VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
	const VkImageUsageFlags vulkan_dma_tex_usage =
		VK_IMAGE_USAGE_SAMPLED_BIT |
		VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

	const VkFormatFeatureFlags render_features =
		VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
		VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT |
		VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
	const VkFormatFeatureFlags dma_tex_features =
		VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;

	bool found = false;
	for (uint32_t i = 0; i < modp.drmFormatModifierCount; ++i) {
		VkDrmFormatModifierPropertiesEXT m = modp.pDrmFormatModifierProperties[i];

		// check that specific modifier for render usage
		if ((m.drmFormatModifierTilingFeatures & render_features) == render_features) {
			struct vulkan_format_modifier_props p = {0};
			bool supported = false;
			if (query_modifier_usage_support(phdev, props->format.vk,
					props->format.vk_srgb, vulkan_render_usage, &m, &p)) {
				supported = true;
			}
			if (!supported && props->format.vk_srgb) {
				supported = query_modifier_usage_support(phdev, props->format.vk,
					0, vulkan_render_usage, &m, &p);
			}

			if (supported) {
				props->render_mods[props->render_mod_count++] = p;
				found = true;
			}
		}

		if ((m.drmFormatModifierTilingFeatures & dma_tex_features) == dma_tex_features) {
			struct vulkan_format_modifier_props p = {0};
			bool supported = false;
			if (query_modifier_usage_support(phdev, props->format.vk,
					props->format.vk_srgb, vulkan_dma_tex_usage, &m, &p)) {
				supported = true;
			}
			if (!supported && props->format.vk_srgb) {
				supported = query_modifier_usage_support(phdev, props->format.vk,
					0, vulkan_dma_tex_usage, &m, &p);
			}

			if (supported) {
				props->texture_mods[props->texture_mod_count++] = p;
				found = true;
			}
		}

		char *modifier_name = drmGetFormatModifierName(m.drmFormatModifier);
		fprintf(stderr, "    DMA-BUF modifier %s "
			"(0x%016"PRIX64", %"PRIu32" planes)\n",
			modifier_name ? modifier_name : "<unknown>", m.drmFormatModifier,
			m.drmFormatModifierPlaneCount);
		free(modifier_name);
	}

	free(modp.pDrmFormatModifierProperties);
	return found;
}

static void vulkan_format_props_query(struct gbm_vulkan_device *dev,
		VkPhysicalDevice phdev, const struct vulkan_format *format) {
	char *format_name = drmGetFormatName(format->drm);
	fprintf(stderr, "  %s (0x%08"PRIX32")\n",
		format_name ? format_name : "<unknown>", format->drm);
	free(format_name);

	VkDrmFormatModifierPropertiesListEXT modp = {
		.sType = VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT,
	};
	VkFormatProperties2 fmtp = {
		.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2,
		.pNext = &modp,
	};

	vkGetPhysicalDeviceFormatProperties2(phdev, format->vk, &fmtp);

	struct vulkan_format_props props = {0};
	props.format = *format;

	if (modp.drmFormatModifierCount > 0
			&& query_modifier_support(phdev, &props, modp.drmFormatModifierCount)) {
		dev->format_props[dev->format_prop_count] = props;
		++dev->format_prop_count;
	} else {
		vulkan_format_props_finish(&props);
	}
}

static void vulkan_destroy(struct gbm_device *gbm) {
	struct gbm_vulkan_device *vulkan = gbm_vulkan_device(gbm);
	if (vulkan == NULL) {
		return;
	}
	if (vulkan->device) {
		vkDestroyDevice(vulkan->device, NULL);
	}
	if (vulkan->instance) {
		vkDestroyInstance(vulkan->instance, NULL);
	}
	free(vulkan);
}

static void load_device_proc(struct gbm_vulkan_device *dev, const char *name, void *proc_ptr) {
	PFN_vkVoidFunction proc = vkGetDeviceProcAddr(dev->device, name);
	if (proc == NULL) {
		abort();
	}
	*(PFN_vkVoidFunction *)proc_ptr = proc;
}

static VkPhysicalDevice vulkan_select_physical_device(VkInstance instance, int fd) {
	struct stat drm_stat = { 0 };
	if (fstat(fd, &drm_stat) != 0) {
		fprintf(stderr, "Could not fstat DRM fd\n");
		return VK_NULL_HANDLE;
	}

	uint32_t num_phdevs;
	vkEnumeratePhysicalDevices(instance, &num_phdevs, NULL);
	if (num_phdevs == 0) {
		fprintf(stderr, "No physical Vulkan devices\n");
		return VK_NULL_HANDLE;
	}

	VkPhysicalDevice phdevs[1 + num_phdevs];
	vkEnumeratePhysicalDevices(instance, &num_phdevs, phdevs);
	int chosen = -1;
	for (uint32_t idx = 0; idx < num_phdevs; idx++) {
		VkPhysicalDevice phdev = phdevs[idx];

		uint32_t avail_extc = 0;
		if (vkEnumerateDeviceExtensionProperties(phdev, NULL, &avail_extc, NULL) != VK_SUCCESS || avail_extc == 0) {
			fprintf(stderr, "Could not enumerate device extensions\n");
			continue;
		}

		VkExtensionProperties avail_ext_props[avail_extc + 1];
		if (vkEnumerateDeviceExtensionProperties(phdev, NULL, &avail_extc, avail_ext_props) != VK_SUCCESS) {
			fprintf(stderr, "Could not enumerate device extensions\n");
			continue;
		}

		bool has_drm_props = check_extension(avail_ext_props, avail_extc,
		VK_EXT_PHYSICAL_DEVICE_DRM_EXTENSION_NAME);
		if (!has_drm_props) {
			fprintf(stderr, "Device does not support DRM extension\n");
			continue;
		}

		VkPhysicalDeviceDrmPropertiesEXT drm_props = {
			.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRM_PROPERTIES_EXT,
		};
		VkPhysicalDeviceProperties2 props = {
			.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
			.pNext = &drm_props,
		};
		vkGetPhysicalDeviceProperties2(phdev, &props);

		log_phdev(&props.properties);
		if (chosen != -1) {
			continue;
		}

		dev_t primary_devid = makedev(drm_props.primaryMajor, drm_props.primaryMinor);
		dev_t render_devid = makedev(drm_props.renderMajor, drm_props.renderMinor);
		if (primary_devid == drm_stat.st_rdev || render_devid == drm_stat.st_rdev) {
			chosen = idx;
		}
	}

	if (chosen != -1) {
		fprintf(stderr, "Selected device %d\n", chosen);
		return phdevs[chosen];
	}
	return VK_NULL_HANDLE;
}

static struct gbm_device *vulkan_device_create(int fd, uint32_t gbm_backend_version) {
	struct gbm_vulkan_device *vulkan = calloc(1, sizeof *vulkan);
	if (!vulkan) {
		return NULL;
	}

	vulkan->base.v0.fd = fd;
	vulkan->base.v0.backend_version = gbm_backend_version;
	vulkan->base.v0.name = "vulkan";

	vulkan->base.v0.destroy = vulkan_destroy;
	vulkan->base.v0.is_format_supported = gbm_vulkan_is_format_supported;
	vulkan->base.v0.get_format_modifier_plane_count =
		gbm_vulkan_get_format_modifier_plane_count;

	vulkan->base.v0.bo_create = gbm_vulkan_bo_create;
	vulkan->base.v0.bo_get_fd = gbm_vulkan_bo_get_fd;
	vulkan->base.v0.bo_get_planes = gbm_vulkan_bo_get_planes;
	vulkan->base.v0.bo_get_handle = gbm_vulkan_bo_get_handle_for_plane;
	vulkan->base.v0.bo_get_plane_fd = gbm_vulkan_bo_get_plane_fd;
	vulkan->base.v0.bo_get_stride = gbm_vulkan_bo_get_stride;
	vulkan->base.v0.bo_get_offset = gbm_vulkan_bo_get_offset;
	vulkan->base.v0.bo_get_modifier = gbm_vulkan_bo_get_modifier;
	vulkan->base.v0.bo_destroy = gbm_vulkan_bo_destroy;

	// The methods below are not implemented
	vulkan->base.v0.bo_import = gbm_vulkan_bo_import;
	vulkan->base.v0.bo_map = gbm_vulkan_bo_map;
	vulkan->base.v0.bo_unmap = gbm_vulkan_bo_unmap;
	vulkan->base.v0.bo_write = gbm_vulkan_bo_write;

	vulkan->base.v0.surface_create = gbm_vulkan_surface_create;
	vulkan->base.v0.surface_lock_front_buffer = gbm_vulkan_surface_lock_front_buffer;
	vulkan->base.v0.surface_release_buffer = gbm_vulkan_surface_release_buffer;
	vulkan->base.v0.surface_has_free_buffers = gbm_vulkan_surface_has_free_buffers;
	vulkan->base.v0.surface_destroy = gbm_vulkan_surface_destroy;

	VkApplicationInfo appInfo = {
		.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
		.pEngineName = "vulkan_gbm",
		.engineVersion = VK_MAKE_VERSION(0, 1, 0),
		.apiVersion = VK_API_VERSION_1_1,
	};

	VkInstanceCreateInfo createInfo = {
		.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
		.pApplicationInfo = &appInfo,
	};

	vulkan->instance = VK_NULL_HANDLE;
	if (vkCreateInstance(&createInfo, NULL, &vulkan->instance) != VK_SUCCESS) {
		fprintf(stderr, "Failed to create Vulkan instance\n");
		vulkan_destroy(&vulkan->base);
		return NULL;
	}

	vulkan->physical_device = vulkan_select_physical_device(vulkan->instance, fd);
	if (vulkan->physical_device == VK_NULL_HANDLE) {
		fprintf(stderr, "Could not find candidate device\n");
		vulkan_destroy(&vulkan->base);
		return NULL;
	}

	const char *extensions[4] = { 0 };
	size_t extensions_len = 0;
	extensions[extensions_len++] = VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME;
	extensions[extensions_len++] = VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME;
	extensions[extensions_len++] = VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME;

	const float prio = 1.f;
	int queue_family_idx = vulkan_select_queue_family(vulkan->physical_device);
	if (queue_family_idx == -1) {
		fprintf(stderr, "Could not pick queue family\n");
		vulkan_destroy(&vulkan->base);
		return NULL;
	}

	VkDeviceQueueCreateInfo qinfo = {
		.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
		.queueFamilyIndex = queue_family_idx,
		.queueCount = 1,
		.pQueuePriorities = &prio,
	};

	VkDeviceCreateInfo dev_info = {
		.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
		.queueCreateInfoCount = 1u,
		.pQueueCreateInfos = &qinfo,
		.enabledExtensionCount = extensions_len,
		.ppEnabledExtensionNames = extensions,
	};

	if (vkCreateDevice(vulkan->physical_device, &dev_info, NULL, &vulkan->device) != VK_SUCCESS) {
		fprintf(stderr, "Could not create device\n");
		vulkan_destroy(&vulkan->base);
		return NULL;
	}

	load_device_proc(vulkan, "vkGetMemoryFdKHR", &vulkan->api.vkGetMemoryFdKHR);
	load_device_proc(vulkan, "vkGetImageDrmFormatModifierPropertiesEXT",
		&vulkan->api.vkGetImageDrmFormatModifierPropertiesEXT);

	vulkan->format_props = calloc(ARRAY_SIZE(formats), sizeof(*vulkan->format_props));
	if (!vulkan->format_props) {
		vulkan_destroy(&vulkan->base);
		return NULL;
	}

	fprintf(stderr, "Supported Vulkan formats:\n");
	for (unsigned i = 0u; i < ARRAY_SIZE(formats); ++i) {
		vulkan_format_props_query(vulkan, vulkan->physical_device, &formats[i]);
	}
	return &vulkan->base;
}

struct gbm_backend gbm_vulkan_backend = {
	.v0.backend_version = GBM_BACKEND_ABI_VERSION,
	.v0.backend_name = "vulkan",
	.v0.create_device = vulkan_device_create,
};

struct gbm_backend *gbmint_get_backend(const struct gbm_core *gbm_core);

struct gbm_backend *gbmint_get_backend(const struct gbm_core *gbm_core) {
	core = gbm_core;
	return &gbm_vulkan_backend;
}
