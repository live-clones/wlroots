#ifdef GL_FRAGMENT_PRECISION_HIGH
precision highp float;
#else
precision mediump float;
#endif

varying vec3 v_color;

void main() {
	gl_FragColor = vec4(v_color, 1.0);
}
