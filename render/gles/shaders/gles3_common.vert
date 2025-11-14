#version 300 es

uniform mat3 proj;
uniform mat3 tex_proj;
in vec2 pos;
out vec2 v_texcoord;

void main() {
	vec3 pos3 = vec3(pos, 1.0);
	gl_Position = vec4(pos3 * proj, 1.0);
	v_texcoord = (pos3 * tex_proj).xy;
}
