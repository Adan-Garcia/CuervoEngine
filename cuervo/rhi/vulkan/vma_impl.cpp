// The single translation unit that compiles the Vulkan Memory Allocator.
// Function pointers come from volk at runtime (see vk_device.cpp), so VMA must
// not reference any Vulkan symbols statically.
#define VMA_IMPLEMENTATION
#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
#include <volk.h>
#include <vk_mem_alloc.h>
