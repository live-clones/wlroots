#version 450

layout(set = 0, binding = 0) uniform sampler2D tex;

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 out_color;

// struct wlr_vk_frag_texture_pcr_data
layout(push_constant, row_major) uniform UBO {
	layout(offset = 80) mat4 matrix;
	float alpha;
	float luminance_multiplier;
	// Per-surface ICtCp tone mapping, luminances in cd/m². Tone mapping is
	// disabled when tm_content_max <= 0.
	float tm_content_ref;
	float tm_content_min;
	float tm_content_max;
	float tm_display_ref;
	float tm_display_min;
	float tm_display_max;
} data;

layout (constant_id = 0) const int TEXTURE_TRANSFORM = 0;

// Matches enum wlr_vk_texture_transform
#define TEXTURE_TRANSFORM_IDENTITY 0
#define TEXTURE_TRANSFORM_SRGB 1
#define TEXTURE_TRANSFORM_ST2084_PQ 2
#define TEXTURE_TRANSFORM_GAMMA22 3
#define TEXTURE_TRANSFORM_BT1886 4

float srgb_channel_to_linear(float x) {
	return mix(x / 12.92,
		pow((x + 0.055) / 1.055, 2.4),
		x > 0.04045);
}

vec3 srgb_color_to_linear(vec3 color) {
	return vec3(
		srgb_channel_to_linear(color.r),
		srgb_channel_to_linear(color.g),
		srgb_channel_to_linear(color.b)
	);
}

vec3 pq_color_to_linear(vec3 color) {
	float inv_m1 = 1 / 0.1593017578125;
	float inv_m2 = 1 / 78.84375;
	float c1 = 0.8359375;
	float c2 = 18.8515625;
	float c3 = 18.6875;
	vec3 num = max(pow(color, vec3(inv_m2)) - c1, 0);
	vec3 denom = c2 - c3 * pow(color, vec3(inv_m2));
	return pow(num / denom, vec3(inv_m1));
}

vec3 bt1886_color_to_linear(vec3 color) {
	float Lmin = 0.01;
	float Lmax = 100.0;
	float lb = pow(Lmin, 1.0 / 2.4);
	float lw = pow(Lmax, 1.0 / 2.4);
	float a  = pow(lw - lb, 2.4);
	float b  = lb / (lw - lb);
	vec3 L = a * pow(color + vec3(b), vec3(2.4));
	return (L - Lmin) / (Lmax - Lmin);
}

// PQ OETF (linear [0,1] where 1.0 == 10000 cd/m² -> PQ code), matching
// output.frag's linear_color_to_pq.
vec3 linear_to_pq(vec3 color) {
	float c1 = 0.8359375;
	float c2 = 18.8515625;
	float c3 = 18.6875;
	float m = 78.84375;
	float n = 0.1593017578125;
	vec3 pow_n = pow(clamp(color, vec3(0), vec3(1)), vec3(n));
	return pow((vec3(c1) + c2 * pow_n) / (vec3(1) + c3 * pow_n), vec3(m));
}

// Modified Reinhard intensity curve, matching KWin's colormanagement.glsl.
// Operates on luminance relative to the destination reference white; the white
// point `v` is solved so the input range maps exactly to the output range, and
// the curve is the identity when the two ranges match.
float tonemap_reinhard(float rel, float input_range, float output_range) {
	float v = (output_range * (1.0 + input_range) - input_range)
		/ (input_range * input_range);
	return rel * (1.0 + rel * v) / (1.0 + rel);
}

// KWin-style luminance-preserving tone mapping: convert to ICtCp (BT.2100),
// which cleanly separates intensity from chroma, tone-map only the intensity,
// and convert back. Hue and saturation are preserved by construction. The work
// is done in destination-referred absolute cd/m² (a blend value of 1.0 is the
// display reference white), so the curve uses a single reference like KWin.
vec3 tone_map(vec3 rgb, float cref, float cmin, float cmax,
		float dref, float dmin, float dmax) {
	// BT.2100 LMS from linear sRGB (LMS<-BT.2020 composed with BT.2020<-709).
	mat3 lms_from_srgb = transpose(mat3(
		vec3(0.295811, 0.623114, 0.081090),
		vec3(0.156248, 0.727323, 0.116427),
		vec3(0.035137, 0.156582, 0.808288)));
	// BT.2100 ICtCp from PQ-encoded L'M'S'.
	mat3 ictcp_from_lmspq = transpose(mat3(
		vec3(0.5, 0.5, 0.0),
		vec3(1.613769531, -3.323486328, 1.709716797),
		vec3(4.378173828, -4.245605469, -0.132568359)));

	// Black point compensation: recover content nits (blend 1.0 == content
	// reference) and map the content's [min, reference] range onto the
	// display's [min, reference] range, giving destination-referred nits.
	// Matches KWin's luminanceBefore/luminanceAfter (colorspace.cpp toOther).
	vec3 content_nits = rgb * cref;
	vec3 dst_nits = (content_nits - cmin) / (cref - cmin) * (dref - dmin) + dmin;
	vec3 lms = max(lms_from_srgb * dst_nits, vec3(0));
	vec3 ictcp = ictcp_from_lmspq * linear_to_pq(lms / 10000.0);

	float luminance = pq_color_to_linear(vec3(ictcp.x)).x * 10000.0;
	float rel = max(luminance / dref, 0.0);
	rel = tonemap_reinhard(rel, cmax / cref, dmax / dref);
	luminance = rel * dref;

	ictcp.x = linear_to_pq(vec3(luminance / 10000.0)).x;
	vec3 lms_pq = clamp(inverse(ictcp_from_lmspq) * ictcp, vec3(0), vec3(1));
	vec3 out_nits = inverse(lms_from_srgb) * (pq_color_to_linear(lms_pq) * 10000.0);
	// Clamp to the display's actual peak so out-of-gamut highlights collapse to
	// the correct white point (KWin colormanagement.glsl:134).
	out_nits = clamp(out_nits, vec3(0.0), vec3(dmax));
	return out_nits / dref;
}

void main() {
	vec4 in_color = textureLod(tex, uv, 0);

	// Convert from pre-multiplied alpha to straight alpha
	float alpha = in_color.a;
	vec3 rgb;
	if (alpha == 0) {
		rgb = vec3(0);
	} else {
		rgb = in_color.rgb / alpha;
	}

	if (TEXTURE_TRANSFORM != TEXTURE_TRANSFORM_IDENTITY) {
		rgb = max(rgb, vec3(0));
	}
	if (TEXTURE_TRANSFORM == TEXTURE_TRANSFORM_SRGB) {
		rgb = srgb_color_to_linear(rgb);
	} else if (TEXTURE_TRANSFORM == TEXTURE_TRANSFORM_ST2084_PQ) {
		rgb = pq_color_to_linear(rgb);
	} else if (TEXTURE_TRANSFORM == TEXTURE_TRANSFORM_GAMMA22) {
		rgb = pow(rgb, vec3(2.2));
	} else if (TEXTURE_TRANSFORM == TEXTURE_TRANSFORM_BT1886) {
		rgb = bt1886_color_to_linear(rgb);
	}

	rgb *= data.luminance_multiplier;

	rgb = mat3(data.matrix) * rgb;

	// Tone map the source content into the display's capability. Disabled
	// (tm_content_max <= 0) for bypass content (e.g. windows_scrgb) and when
	// the content cannot exceed the display.
	if (data.tm_content_max > 0.0) {
		rgb = tone_map(rgb, data.tm_content_ref, data.tm_content_min,
			data.tm_content_max, data.tm_display_ref, data.tm_display_min,
			data.tm_display_max);
	}

	// Back to pre-multiplied alpha
	out_color = vec4(rgb * alpha, alpha);

	out_color *= data.alpha;
}
