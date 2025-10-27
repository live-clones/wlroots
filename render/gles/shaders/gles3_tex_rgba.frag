#version 300 es

precision highp float;

in vec2 v_texcoord;
uniform sampler2D tex;
uniform float alpha;

out vec4 frag_color;

void main() {
	frag_color = texture(tex, v_texcoord) * alpha;
}
