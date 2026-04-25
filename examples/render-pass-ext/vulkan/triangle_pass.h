#ifndef CUSTOM_RENDER_PASS_VULKAN_PASS_H
#define CUSTOM_RENDER_PASS_VULKAN_PASS_H

#include "../triangle_pass.h"

#include <vulkan/vulkan.h>
#include <wlr/render/vulkan.h>

struct vk_render_triangle_pass {
	struct render_triangle_pass base;
	struct wlr_vk_renderer *renderer;
	VkShaderModule vert_module;
	VkShaderModule frag_module;
};

struct render_triangle_pass *vk_render_triangle_pass_create(
	struct wlr_renderer *renderer);
bool render_triangle_pass_is_vk(const struct render_triangle_pass *triangle_pass);
struct vk_render_triangle_pass *vk_render_triangle_pass_from_pass(
	struct render_triangle_pass *triangle_pass);

#endif
