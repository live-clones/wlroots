#version 450

layout(location = 0) out vec4 out_color;
layout(push_constant) uniform UBO {
	layout(offset = 80) vec4 color;
	vec4 dst_bounds; // x, y, x+width, y+height in output coordinates
} data;

float compute_coverage() {
	vec4 d = vec4(
		gl_FragCoord.x - data.dst_bounds.x,
		gl_FragCoord.y - data.dst_bounds.y,
		data.dst_bounds.z - gl_FragCoord.x,
		data.dst_bounds.w - gl_FragCoord.y
	);
	vec4 cov = clamp(d + 0.5, 0.0, 1.0);
	return min(min(cov.x, cov.y), min(cov.z, cov.w));
}

void main() {
	out_color = data.color * compute_coverage();
}
