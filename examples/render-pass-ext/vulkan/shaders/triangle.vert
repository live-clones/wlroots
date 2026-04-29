#version 450

// Keep push constants layout compatible with wlroots Vulkan helpers.
layout(push_constant, row_major) uniform UBO {
	mat4 proj;
	vec4 pos[3];
	vec4 color[3];
} data;

layout(location = 0) out vec3 v_color;

void main() {
	gl_Position = data.proj * vec4(data.pos[gl_VertexIndex].xy, 0.0, 1.0);
	v_color = data.color[gl_VertexIndex].rgb;
}
