#ifdef GL_FRAGMENT_PRECISION_HIGH
precision highp float;
#else
precision mediump float;
#endif

varying vec2 v_texcoord;
uniform sampler2D tex;
uniform vec2 texel_step;

void main() {
	// 9-tap separable Gaussian, sigma=1.5
	vec4 c = vec4(0.0);
	c += texture2D(tex, v_texcoord - texel_step * 4.0) * 0.0076;
	c += texture2D(tex, v_texcoord - texel_step * 3.0) * 0.0361;
	c += texture2D(tex, v_texcoord - texel_step * 2.0) * 0.1096;
	c += texture2D(tex, v_texcoord - texel_step * 1.0) * 0.2134;
	c += texture2D(tex, v_texcoord) * 0.2666;
	c += texture2D(tex, v_texcoord + texel_step * 1.0) * 0.2134;
	c += texture2D(tex, v_texcoord + texel_step * 2.0) * 0.1096;
	c += texture2D(tex, v_texcoord + texel_step * 3.0) * 0.0361;
	c += texture2D(tex, v_texcoord + texel_step * 4.0) * 0.0076;
	gl_FragColor = c;
}
