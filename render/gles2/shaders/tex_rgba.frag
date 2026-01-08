#ifdef GL_FRAGMENT_PRECISION_HIGH
precision highp float;
#else
precision mediump float;
#endif

varying vec2 v_texcoord;
uniform sampler2D tex;
uniform float alpha;
uniform vec4 dst_bounds; // x, y, x+width, y+height in output coordinates

float compute_coverage() {
	vec4 d = vec4(
		gl_FragCoord.x - dst_bounds.x,
		gl_FragCoord.y - dst_bounds.y,
		dst_bounds.z - gl_FragCoord.x,
		dst_bounds.w - gl_FragCoord.y
	);
	vec4 cov = clamp(d + 0.5, 0.0, 1.0);
	return min(min(cov.x, cov.y), min(cov.z, cov.w));
}

void main() {
	gl_FragColor = texture2D(tex, v_texcoord) * alpha * compute_coverage();
}
