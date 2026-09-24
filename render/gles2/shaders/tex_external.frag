#extension GL_OES_EGL_image_external : require

#ifdef GL_FRAGMENT_PRECISION_HIGH
precision highp float;
#else
precision mediump float;
#endif

varying vec2 v_texcoord;
uniform samplerExternalOES texture0;
uniform float alpha;
uniform mat4 color_matrix;

void main() {
	gl_FragColor = color_matrix * texture2D(texture0, v_texcoord) * alpha;
}
