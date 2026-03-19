#version 450

layout(set = 0, binding = 0) uniform sampler2D tex;

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 out_color;

layout(push_constant) uniform PC {
	layout(offset = 80) vec2 texel_step;
} pc;

void main() {
	// 9-tap separable Gaussian, sigma=1.5
	vec4 c = vec4(0.0);
	c += texture(tex, uv - pc.texel_step * 4.0) * 0.0076;
	c += texture(tex, uv - pc.texel_step * 3.0) * 0.0361;
	c += texture(tex, uv - pc.texel_step * 2.0) * 0.1096;
	c += texture(tex, uv - pc.texel_step * 1.0) * 0.2134;
	c += texture(tex, uv) * 0.2666;
	c += texture(tex, uv + pc.texel_step * 1.0) * 0.2134;
	c += texture(tex, uv + pc.texel_step * 2.0) * 0.1096;
	c += texture(tex, uv + pc.texel_step * 3.0) * 0.0361;
	c += texture(tex, uv + pc.texel_step * 4.0) * 0.0076;
	out_color = c;
}
