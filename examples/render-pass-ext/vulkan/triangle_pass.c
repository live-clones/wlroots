#include "render/vulkan.h"
#include "util/matrix.h"

#include "triangle.vert.h"
#include "triangle.frag.h"
#include "triangle_pass.h"

#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <assert.h>
#include <math.h>

#include <wlr/render/vulkan.h>
#include <wlr/util/log.h>

struct custom_triangle_push_data {
	float mat4[4][4];
	float pos[3][4];
	float color[3][4];
};

static void custom_rect_union_add(struct rect_union *ru, pixman_box32_t box) {
	if (box.x1 >= box.x2 || box.y1 >= box.y2) {
		return;
	}

	ru->bounding_box.x1 = ru->bounding_box.x1 < box.x1 ? ru->bounding_box.x1 : box.x1;
	ru->bounding_box.y1 = ru->bounding_box.y1 < box.y1 ? ru->bounding_box.y1 : box.y1;
	ru->bounding_box.x2 = ru->bounding_box.x2 > box.x2 ? ru->bounding_box.x2 : box.x2;
	ru->bounding_box.y2 = ru->bounding_box.y2 > box.y2 ? ru->bounding_box.y2 : box.y2;

	if (!ru->alloc_failure) {
		pixman_box32_t *entry = wl_array_add(&ru->unsorted, sizeof(*entry));
		if (entry != NULL) {
			*entry = box;
		} else {
			ru->alloc_failure = true;
			wl_array_release(&ru->unsorted);
			wl_array_init(&ru->unsorted);
		}
	}
}

static void render_pass_mark_box_updated(struct wlr_vk_render_pass *pass,
		const struct wlr_box *box) {
	if (!pass->two_pass) {
		return;
	}

	pixman_box32_t pixman_box = {
		.x1 = box->x,
		.x2 = box->x + box->width,
		.y1 = box->y,
		.y2 = box->y + box->height,
	};
	custom_rect_union_add(&pass->updated_region, pixman_box);
}

static bool create_graphics_pipeline(struct vk_render_triangle_pass *pass,
		struct wlr_vk_render_pass *vk_pass,
		VkPipelineLayout *layout_out,
		VkPipeline *pipeline_out) {
	VkDevice dev = pass->renderer->dev->dev;

	VkPushConstantRange push_range = {
		.stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
		.offset = 0,
		.size = sizeof(struct custom_triangle_push_data),
	};
	VkPipelineLayoutCreateInfo layout_info = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		.pushConstantRangeCount = 1,
		.pPushConstantRanges = &push_range,
	};
	VkResult res = vkCreatePipelineLayout(dev, &layout_info, NULL, layout_out);
	if (res != VK_SUCCESS) {
		wlr_log(WLR_ERROR, "Failed to create custom Vulkan pipeline layout (VkResult=%d)", res);
		return false;
	}

	VkPipelineShaderStageCreateInfo shader_stages[2] = {
		{
			.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
			.stage = VK_SHADER_STAGE_VERTEX_BIT,
			.module = pass->vert_module,
			.pName = "main",
		},
		{
			.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
			.stage = VK_SHADER_STAGE_FRAGMENT_BIT,
			.module = pass->frag_module,
			.pName = "main",
		},
	};

	VkPipelineVertexInputStateCreateInfo vertex_input = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
	};
	VkPipelineInputAssemblyStateCreateInfo input_assembly = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
		.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
	};
	VkPipelineViewportStateCreateInfo viewport_state = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
		.viewportCount = 1,
		.scissorCount = 1,
	};
	VkPipelineRasterizationStateCreateInfo rasterization = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
		.polygonMode = VK_POLYGON_MODE_FILL,
		.cullMode = VK_CULL_MODE_NONE,
		.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
		.lineWidth = 1.0f,
	};
	VkPipelineMultisampleStateCreateInfo multisample = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
		.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
	};
	VkPipelineColorBlendAttachmentState blend_attachment = {
		.blendEnable = VK_FALSE,
		.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
			VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
	};
	VkPipelineColorBlendStateCreateInfo blend = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
		.attachmentCount = 1,
		.pAttachments = &blend_attachment,
	};
	VkDynamicState dyn_states[] = {
		VK_DYNAMIC_STATE_VIEWPORT,
		VK_DYNAMIC_STATE_SCISSOR,
	};
	VkPipelineDynamicStateCreateInfo dynamic = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
		.dynamicStateCount = sizeof(dyn_states) / sizeof(dyn_states[0]),
		.pDynamicStates = dyn_states,
	};

	VkGraphicsPipelineCreateInfo pipeline_info = {
		.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
		.stageCount = 2,
		.pStages = shader_stages,
		.pVertexInputState = &vertex_input,
		.pInputAssemblyState = &input_assembly,
		.pViewportState = &viewport_state,
		.pRasterizationState = &rasterization,
		.pMultisampleState = &multisample,
		.pColorBlendState = &blend,
		.pDynamicState = &dynamic,
		.layout = *layout_out,
		.renderPass = vk_pass->render_setup->render_pass,
		.subpass = 0,
	};

	res = vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &pipeline_info, NULL, pipeline_out);
	if (res != VK_SUCCESS) {
		wlr_log(WLR_ERROR, "Failed to create custom Vulkan graphics pipeline (VkResult=%d)", res);
		vkDestroyPipelineLayout(dev, *layout_out, NULL);
		*layout_out = VK_NULL_HANDLE;
		return false;
	}

	return true;
}

static void triangle_render(struct wlr_render_pass *render_pass,
		const struct custom_render_triangle_options *options) {
	struct wlr_vk_render_pass *vk_pass =
		wlr_vk_render_pass_from_render_pass(render_pass);
	struct render_triangle_pass *base =
		vk_pass->render_buffer->renderer->wlr_renderer.data;
	struct vk_render_triangle_pass *vk_triangle_pass =
		vk_render_triangle_pass_from_pass(base);

	VkCommandBuffer cb = vk_pass->command_buffer->vk;
 	VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
	VkPipeline pipeline = VK_NULL_HANDLE;

	struct wlr_box box;
	struct wlr_buffer *wlr_buffer = vk_pass->render_buffer->wlr_buffer;
	custom_render_triangle_options_get_box(options, wlr_buffer, &box);
	render_pass_mark_box_updated(vk_pass, &box);

	float proj[9], matrix[9];
	wlr_matrix_identity(proj);
	wlr_matrix_project_box(matrix, &box, WL_OUTPUT_TRANSFORM_NORMAL, proj);
	wlr_matrix_multiply(matrix, vk_pass->projection, matrix);

	if (!create_graphics_pipeline(vk_triangle_pass, vk_pass,
			&pipeline_layout, &pipeline)) {
		vk_pass->failed = true;
		return;
	}

	struct custom_triangle_push_data push_data = {
		.pos = {
			{ 0.5f, 0.1f, 0.0f, 0.0f },
			{ 0.1f, 0.9f, 0.0f, 0.0f },
			{ 0.9f, 0.9f, 0.0f, 0.0f },
		},
	};

	const float alpha = 1.0f;
	push_data.color[0][0] = wlr_color_to_linear_premult(1.0f, alpha);
	push_data.color[0][1] = wlr_color_to_linear_premult(0.1f, alpha);
	push_data.color[0][2] = wlr_color_to_linear_premult(0.1f, alpha);
	push_data.color[1][0] = wlr_color_to_linear_premult(0.1f, alpha);
	push_data.color[1][1] = wlr_color_to_linear_premult(1.0f, alpha);
	push_data.color[1][2] = wlr_color_to_linear_premult(0.1f, alpha);
	push_data.color[2][0] = wlr_color_to_linear_premult(0.1f, alpha);
	push_data.color[2][1] = wlr_color_to_linear_premult(0.2f, alpha);
	push_data.color[2][2] = wlr_color_to_linear_premult(1.0f, alpha);
	wlr_encode_proj_matrix(matrix, push_data.mat4);

	vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
	vkCmdPushConstants(cb, pipeline_layout,
		VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(push_data), &push_data);

	VkRect2D scissor = {
		.offset = { .x = 0, .y = 0 },
		.extent = { .width = wlr_buffer->width, .height = wlr_buffer->height },
	};
	vkCmdSetScissor(cb, 0, 1, &scissor);
	vkCmdDraw(cb, 3, 1, 0, 0);

	VkDevice dev = vk_triangle_pass->renderer->dev->dev;
	vkDestroyPipeline(dev, pipeline, NULL);
	vkDestroyPipelineLayout(dev, pipeline_layout, NULL);
}

static void triangle_destroy(struct render_triangle_pass *pass) {
	struct vk_render_triangle_pass *vk_triangle_pass =
		(struct vk_render_triangle_pass *)pass;

	if (vk_triangle_pass->vert_module != VK_NULL_HANDLE) {
		vkDestroyShaderModule(vk_triangle_pass->renderer->dev->dev,
			vk_triangle_pass->vert_module, NULL);
	}
	if (vk_triangle_pass->frag_module != VK_NULL_HANDLE) {
		vkDestroyShaderModule(vk_triangle_pass->renderer->dev->dev,
			vk_triangle_pass->frag_module, NULL);
	}

	free(vk_triangle_pass);
}

static const struct render_triangle_pass_impl render_triangle_pass_impl = {
	.destroy = triangle_destroy,
	.render = triangle_render,
};

struct render_triangle_pass *vk_render_triangle_pass_create(
		struct wlr_renderer *renderer) {
	struct vk_render_triangle_pass *pass = malloc(sizeof(*pass));
	if (pass == NULL) {
		wlr_log_errno(WLR_ERROR, "failed to allocate vk_render_triangle_pass");
		return NULL;
	}

	render_triangle_pass_init(&pass->base, &render_triangle_pass_impl);

	pass->renderer = wlr_vk_renderer_from_renderer(renderer);
	VkDevice dev = pass->renderer->dev->dev;

	if (!wlr_vk_create_shader_module(dev,
			custom_triangle_vert_data,
			sizeof(custom_triangle_vert_data),
			"custom triangle vertex",
			&pass->vert_module)) {
		render_triangle_pass_destroy(&pass->base);
		return NULL;
	}

	if (!wlr_vk_create_shader_module(dev,
			custom_triangle_frag_data,
			sizeof(custom_triangle_frag_data),
			"custom triangle fragment",
			&pass->frag_module)) {
		render_triangle_pass_destroy(&pass->base);
		return NULL;
	}

	return &pass->base;
}

bool render_triangle_pass_is_vk(const struct render_triangle_pass *triangle_pass) {
	return triangle_pass->impl == &render_triangle_pass_impl;
}

struct vk_render_triangle_pass *vk_render_triangle_pass_from_pass(
		struct render_triangle_pass *triangle_pass) {
	assert(triangle_pass->impl == &render_triangle_pass_impl);

	struct vk_render_triangle_pass *vk_pass =
		wl_container_of(triangle_pass, vk_pass, base);

	return vk_pass;
}
