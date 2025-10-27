#version 300 es

precision highp float;

in vec4 v_color;
in vec2 v_texcoord;
uniform vec4 color;

out vec4 frag_color;

void main() {
	frag_color = color;
}
