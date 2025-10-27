#version 300 es

#extension GL_OES_EGL_image_external_essl3 : require

precision highp float;

in vec2 v_texcoord;
uniform samplerExternalOES texture0;
uniform float alpha;

out vec4 frag_color;

void main() {
	frag_color = texture(texture0, v_texcoord) * alpha;
}
