static const char *kFlowHlsl = R"HLSL(
cbuffer Params : register(b0)
{
	uint2  dst;
	float2 inv_dst;
	float2 inv_src;
	float2 texel1;
	float2 texel2;
	uint   mip1;
	uint   mip2;
	uint   first;
	uint   has_depth;
	float2 inv_full;
	uint   src_mip;
	uint   filters;
	uint   model_iter;
	uint   model_flags;
	float  norm_x;
	float  px_per_unit;
	uint   terms_gx;
	uint   terms_gy;
	uint   partial_rows;
	float2 dejitter;
	uint   colour_space;
	uint   pad_colour;
	float2 prev_dejitter;
};

float2 on_grid(float2 uv) { return uv + dejitter; }

static const float kFlowMax = 0.25;
bool invalid_flow(float2 f) { return any(f != f) || any(abs(f) > kFlowMax); }

Texture2D<float4> Color    : register(t0);
Texture2D<float>  LumaCur  : register(t1);
Texture2D<float>  LumaPrev : register(t2);
Texture2D<float2> SrcFlow  : register(t3);
Texture2D<float2> PrevFlow : register(t4);
Texture2D<float2> GlobalF  : register(t5);
Texture2D<float>  Depth    : register(t6);
Texture2D<float>  Conf     : register(t7);
Texture2D<float4> StructQ  : register(t8);
Texture2D<float>  Theta    : register(t9);
Texture2D<float>  Aux      : register(t10);

RWTexture2D<float2> OutV  : register(u0);
RWTexture2D<float>  OutS  : register(u1);
RWTexture2D<float4> OutS4 : register(u2);

SamplerState LinearClamp : register(s0);
SamplerState PointClamp  : register(s1);

#ifndef AEON_FLOW_QUALITY
	#define AEON_FLOW_QUALITY 2
#endif

#if AEON_FLOW_QUALITY == 1
	#define SEARCH_ITER   3
	#define SEARCH_RADIUS 1
	#define REFINE_RADIUS 1
	#define ZAD_SAMPLES   5
	#define LUMA_SAMPLES  5
	#define MEDIAN_TAP    5
#else
	#define SEARCH_ITER   10
	#define SEARCH_RADIUS 3
	#define REFINE_RADIUS 2
	#define ZAD_SAMPLES   9
	#define LUMA_SAMPLES  13
	#define MEDIAN_TAP    9
#endif

#define EPSILON 1e-6

float2 uv_of(uint2 id) { return (float2(id) + 0.5) * inv_dst; }

float luminance(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }

float depth_at(float2 uv)
{
	return (has_depth != 0u) ? Depth.SampleLevel(LinearClamp, on_grid(uv), 0) : 0.5;
}

bool out_of_bounds(float2 uv) { return any(uv < 0.0) || any(uv > 1.0); }

float depth_tap(float2 uv) { return Depth.SampleLevel(LinearClamp, saturate(on_grid(uv)), 0); }

float2 depth_gradient(float2 uv, float centre, float2 duv)
{
	const float dxp = depth_tap(uv + float2(duv.x, 0.0)) - centre;
	const float dxm = centre - depth_tap(uv - float2(duv.x, 0.0));
	const float dyp = depth_tap(uv + float2(0.0, duv.y)) - centre;
	const float dym = centre - depth_tap(uv - float2(0.0, duv.y));
	return float2((abs(dxp) < abs(dxm)) ? dxp : dxm,
	              (abs(dyp) < abs(dym)) ? dyp : dym);
}

float depth_gate(float centre, float2 grad, float2 off, float neighbour)
{
	const float want = centre + dot(grad, off);
	const float tol = 0.02 * abs(want) + 0.25 * length(grad * off) + 1e-4;
	if (abs(neighbour - want) >= tol)
		return 0.0;
	return ((centre >= 0.999) != (neighbour >= 0.999)) ? 0.0 : 1.0;
}

float2 flow_at(float2 uv) { return SrcFlow.SampleLevel(PointClamp, uv, 0); }

float local_contrast(float2 uv, float2 texel, uint mip)
{
	float s[9];
	float mean = 0.0;
	int i = 0;
	[unroll]
	for (int dy = -1; dy <= 1; ++dy) {
		[unroll]
		for (int dx = -1; dx <= 1; ++dx) {
			s[i] = LumaCur.SampleLevel(LinearClamp, uv + float2(dx, dy) * texel, mip);
			mean += s[i];
			++i;
		}
	}
	mean *= (1.0 / 9.0);
	float mad = 0.0;
	[unroll]
	for (int j = 0; j < 9; ++j)
		mad += abs(s[j] - mean);
	mad *= (1.0 / 9.0);
	return mad / (mean + 0.02);
}

float zad(float2 pos_a, float2 pos_b, float2 texel, uint mip)
{
#if AEON_FLOW_QUALITY == 1
	static const int2 OFFS[5] = { int2(0,-2), int2(-2,0), int2(0,0), int2(2,0), int2(0,2) };
#else
	static const int2 OFFS[9] = { int2(0,-3), int2(0,-1), int2(-3,0), int2(-1,0), int2(0,0),
		int2(1,0), int2(3,0), int2(0,1), int2(0,3) };
#endif
	float a[ZAD_SAMPLES], b[ZAD_SAMPLES];
	float mean_a = 0.0, mean_b = 0.0;
	[unroll]
	for (int i = 0; i < ZAD_SAMPLES; ++i) {
		const float2 o = float2(OFFS[i]) * texel;
		a[i] = LumaCur.SampleLevel(LinearClamp, pos_a + o, mip);
		b[i] = LumaPrev.SampleLevel(LinearClamp, pos_b + o, mip);
		mean_a += a[i];
		mean_b += b[i];
	}
	mean_a /= (float)ZAD_SAMPLES;
	mean_b /= (float)ZAD_SAMPLES;
	float err = 0.0;
	[unroll]
	for (int j = 0; j < ZAD_SAMPLES; ++j)
		err += abs((a[j] - mean_a) - (b[j] - mean_b));
	return err / (float)ZAD_SAMPLES + EPSILON;
}

int ring8(int x) { return x & 7; }

float2 compute_flow(float2 uv)
{
	if (first != 0u)
		return float2(0.0, 0.0);

	static const int2 C8[8] = {
		int2(-1, 1), int2(0, 1), int2(1, 1), int2(1, 0),
		int2(1,-1), int2(0,-1), int2(-1,-1), int2(-1, 0)
	};
	static const int2 C8_IT[9] = {
		int2(6, 3), int2(0, 3), int2(0, 5), int2(2, 5),
		int2(2, 7), int2(4, 7), int2(4, 1), int2(6, 1),
		int2(0, 0)
	};

	float2 cand[12];
	cand[0]  = flow_at(uv);
	cand[1]  = flow_at(uv + float2(0.0, -inv_src.y));
	cand[2]  = flow_at(uv + float2(0.0,  inv_src.y));
	cand[3]  = flow_at(uv - float2(inv_src.x, 0.0));
	cand[4]  = flow_at(uv + float2(inv_src.x, 0.0));
	cand[5]  = flow_at(uv + float2(-inv_src.x, -inv_src.y));
	cand[6]  = flow_at(uv + float2( inv_src.x, -inv_src.y));
	cand[7]  = flow_at(uv + float2(-inv_src.x,  inv_src.y));
	cand[8]  = flow_at(uv + float2( inv_src.x,  inv_src.y));
	cand[9]  = GlobalF.SampleLevel(PointClamp, float2(0.5, 0.5), 0);
	cand[10] = float2(0.0, 0.0);
	cand[11] = PrevFlow.SampleLevel(PointClamp, uv, 0);

	float min_cost = 1e6;
	float2 prediction = cand[0];
	[loop]
	for (int i = 0; i < 12; ++i) {
		const float cost = zad(uv, uv + cand[i], texel1, mip1);
		if (cost < min_cost) {
			min_cost = cost;
			prediction = cand[i];
		}
	}

	float2 residual = float2(0.0, 0.0);
	float match_cost = zad(uv, uv + prediction, texel2, mip2);
	int match_i = 8;

	const int iters = (mip2 == 0u) ? SEARCH_ITER : min(SEARCH_ITER, 2);
	for (int s = 0; s < iters; ++s) {
		int i = C8_IT[match_i].x;
		const int end = C8_IT[match_i].y;
		const float2 centre = residual;

		float2 c = centre + float2(C8[i]) * texel2;
		float cost = zad(uv, uv + prediction + c, texel2, mip2);
		if (cost < match_cost) {
			residual = c;
			match_i = i;
			match_cost = cost;
		}
		i = ring8(i + 1);
		for (int k = 0; k < 8; ++k) {
			if (i == end)
				break;
			const float2 cc = centre + float2(C8[i]) * texel2;
			const float c_cost = zad(uv, uv + prediction + cc, texel2, mip2);
			if (c_cost < match_cost) {
				residual = cc;
				match_i = i;
				match_cost = c_cost;
			}
			i = ring8(i + 1);
		}
		if (all(centre == residual))
			break;
		if (match_cost < 0.01)
			break;
	}

	const float2 integer_match = prediction + residual;
	const float cl = zad(uv, uv + integer_match - float2(texel2.x, 0.0), texel2, mip2);
	const float cr = zad(uv, uv + integer_match + float2(texel2.x, 0.0), texel2, mip2);
	const float cd = zad(uv, uv + integer_match - float2(0.0, texel2.y), texel2, mip2);
	const float cu = zad(uv, uv + integer_match + float2(0.0, texel2.y), texel2, mip2);

	float2 sub;
	sub.x = (cl - cr) / (2.0 * (cl + cr - 2.0 * match_cost) + EPSILON);
	sub.y = (cd - cu) / (2.0 * (cd + cu - 2.0 * match_cost) + EPSILON);
	sub = clamp(sub, -0.5, 0.5);

	float2 refined = integer_match + sub * texel2;
	if (any(sub != 0.0)) {
		const float refined_cost = zad(uv, uv + refined, texel2, mip2);
		if (!(refined_cost < match_cost))
			refined = integer_match;
	}
	return refined;
}
)HLSL"
R"HLSL(
static const float kHdrPaperWhite = 2.5;
float3 pq_to_scrgb(float3 e)
{
	const float m1 = 0.1593017578125, m2 = 78.84375, c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
	const float3 p = pow(saturate(e), 1.0 / m2);
	return pow(max(p - c1, 0.0) / (c2 - c3 * p), 1.0 / m1) * (10000.0 / 80.0);
}
float frame_luma(float3 c)
{
	float l;
	if (colour_space == 2u)
		l = pow(max(dot(pq_to_scrgb(c), float3(0.2627, 0.6780, 0.0593)), 0.0) / kHdrPaperWhite, 1.0 / 2.2);
	else if (colour_space == 1u)
		l = pow(max(luminance(max(c, 0.0)), 0.0) / kHdrPaperWhite, 1.0 / 2.2);
	else
		l = luminance(max(c, 0.0));
	l = (l == l) ? l : 0.0;
	return l * rcp(1.0 + l);
}

[numthreads(8, 8, 1)]
void CSLuma(uint3 id : SV_DispatchThreadID)
{
	if (any(id.xy >= dst))
		return;
	const float2 uv = uv_of(id.xy);
#if AEON_FLOW_QUALITY == 1
	static const int2 OFFS[5] = { int2(0,0), int2(-1,0), int2(1,0), int2(0,-1), int2(0,1) };
	static const float W[5] = { 6, 4, 4, 4, 4 };
#else
	static const int2 OFFS[13] = { int2(0,-2), int2(-1,-1), int2(0,-1), int2(1,-1),
		int2(-2,0), int2(-1,0), int2(0,0), int2(1,0), int2(2,0),
		int2(-1,1), int2(0,1), int2(1,1), int2(0,2) };
	static const float W[13] = { 1, 3, 4, 3, 1, 4, 6, 4, 1, 3, 4, 3, 1 };
#endif
	float sum = 0.0, wsum = 0.0;
	[unroll]
	for (int i = 0; i < LUMA_SAMPLES; ++i) {
		const float3 c = Color.SampleLevel(LinearClamp, on_grid(uv + float2(OFFS[i]) * inv_full), 0).rgb;
		float l = frame_luma(c);
		sum += l * W[i];
		wsum += W[i];
	}
	const float centre = frame_luma(Color.SampleLevel(LinearClamp, on_grid(uv), 0).rgb);
	OutS[id.xy] = lerp(centre, sum / wsum, 0.9);
}

[numthreads(8, 8, 1)]
void CSDownsample(uint3 id : SV_DispatchThreadID)
{
	if (any(id.xy >= dst))
		return;
	OutS[id.xy] = LumaCur.SampleLevel(LinearClamp, uv_of(id.xy), src_mip);
}

[numthreads(8, 8, 1)]
void CSCoarseTop(uint3 id : SV_DispatchThreadID)
{
	if (any(id.xy >= dst))
		return;
	if (first != 0u) {
		OutV[id.xy] = float2(0.0, 0.0);
		return;
	}
	const float2 uv = uv_of(id.xy);
	const float2 global_p = GlobalF.SampleLevel(PointClamp, float2(0.5, 0.5), 0);
	const float2 local_p = PrevFlow.SampleLevel(PointClamp, uv, 0);
	const float2 static_p = float2(0.0, 0.0);

	const float gc = zad(uv, uv + global_p, texel1, mip1);
	const float lc = zad(uv, uv + local_p, texel1, mip1);
	const float sc = zad(uv, uv + static_p, texel1, mip1);

	float2 prediction = (sc < lc) ? ((sc < gc) ? static_p : global_p)
	                              : ((lc < gc) ? local_p : global_p);
	float2 best = prediction;
	float min_cost = zad(uv, uv + prediction, texel1, mip1);

	[loop]
	for (int y = -SEARCH_RADIUS; y <= SEARCH_RADIUS; ++y) {
		[loop]
		for (int x = -SEARCH_RADIUS; x <= SEARCH_RADIUS; ++x) {
			if (x == 0 && y == 0)
				continue;
			const float2 c = prediction + float2(x, y) * texel1;
			const float cost = zad(uv, uv + c, texel1, mip1);
			if (cost < min_cost) {
				min_cost = cost;
				best = c;
			}
		}
	}
	OutV[id.xy] = best;
}

[numthreads(8, 8, 1)]
void CSCoarse(uint3 id : SV_DispatchThreadID)
{
	if (any(id.xy >= dst))
		return;
	OutV[id.xy] = compute_flow(uv_of(id.xy));
}
)HLSL"
R"HLSL(
[numthreads(8, 8, 1)]
void CSMedian(uint3 id : SV_DispatchThreadID)
{
	if (any(id.xy >= dst))
		return;
	const float2 uv = uv_of(id.xy);
	float xs[MEDIAN_TAP], ys[MEDIAN_TAP];
#if MEDIAN_TAP == 5
	static const int2 OFFS[5] = { int2(0,0), int2(-1,0), int2(1,0), int2(0,-1), int2(0,1) };
	[unroll]
	for (int i = 0; i < 5; ++i) {
		const float2 v = flow_at(uv + float2(OFFS[i]) * inv_src);
		xs[i] = v.x;
		ys[i] = v.y;
	}
	const int SORT_LOOPS = 3, MAX_IDX = 4, MID = 2;
#else
	int idx = 0;
	[unroll]
	for (int dy = -1; dy <= 1; ++dy) {
		[unroll]
		for (int dx = -1; dx <= 1; ++dx) {
			const float2 v = flow_at(uv + float2(dx, dy) * inv_src);
			xs[idx] = v.x;
			ys[idx] = v.y;
			++idx;
		}
	}
	const int SORT_LOOPS = 5, MAX_IDX = 8, MID = 4;
#endif
	[loop]
	for (int k = 0; k < SORT_LOOPS; ++k) {
		[loop]
		for (int i2 = 0; i2 < MAX_IDX; ++i2) {
			if (i2 >= MAX_IDX - k)
				break;
			if (xs[i2] > xs[i2 + 1]) {
				const float t = xs[i2];
				xs[i2] = xs[i2 + 1];
				xs[i2 + 1] = t;
			}
			if (ys[i2] > ys[i2 + 1]) {
				const float t = ys[i2];
				ys[i2] = ys[i2 + 1];
				ys[i2 + 1] = t;
			}
		}
	}
	OutV[id.xy] = float2(xs[MID], ys[MID]);
}

[numthreads(8, 8, 1)]
void CSRefine(uint3 id : SV_DispatchThreadID)
{
	if (any(id.xy >= dst))
		return;
	const float2 uv = uv_of(id.xy);
	const float inv_spatial = -0.5 / (1.5 * 1.5);
	const float inv_luma = -0.5 / (0.1 * 0.1);
	const float inv_flow = -0.5 / (1.5 * 1.5);

	const float2 centre_flow = flow_at(uv);
	const float centre_luma = LumaCur.SampleLevel(LinearClamp, uv, mip1);
	const bool gate_on = (filters != 0u && has_depth != 0u);
	const float centre_depth = gate_on ? depth_tap(uv) : 0.0;
	const float2 dgrad = gate_on ? depth_gradient(uv, centre_depth, inv_src) : float2(0.0, 0.0);

	float2 sum = float2(0.0, 0.0);
	float wsum = 0.0;
	[loop]
	for (int y = -REFINE_RADIUS; y <= REFINE_RADIUS; ++y) {
		[loop]
		for (int x = -REFINE_RADIUS; x <= REFINE_RADIUS; ++x) {
			const float2 suv = uv + float2(x, y) * inv_src;
			const float2 nf = flow_at(suv);
			const float nl = LumaCur.SampleLevel(LinearClamp, suv, mip1);

			const float sw = exp(dot(float2(x, y), float2(x, y)) * inv_spatial);
			const float ld = centre_luma - nl;
			const float lw = exp(ld * ld * inv_luma);
			const float2 fd = (nf - centre_flow) / inv_full;
			const float fw = exp(dot(fd, fd) * inv_flow);

			const float gate = gate_on
				? depth_gate(centre_depth, dgrad, float2(x, y), depth_tap(suv)) : 1.0;
			float cw = 1.0;
			if (filters != 0u) {
				float nc = Conf.SampleLevel(LinearClamp, suv, 0);
				nc = (nc == nc) ? saturate(nc) : 0.0;
				cw = max(nc * nc, 0.01);
			}
			const float w = sw * lw * fw * gate * cw;
			sum += nf * w;
			wsum += w;
		}
	}
	OutV[id.xy] = (wsum > EPSILON) ? (sum / wsum) : centre_flow;
}

[numthreads(8, 8, 1)]
void CSGateProbe(uint3 id : SV_DispatchThreadID)
{
	if (any(id.xy >= dst))
		return;
	const float2 uv = uv_of(id.xy);
	const float centre = depth_tap(uv);
	const float2 grad = depth_gradient(uv, centre, inv_src);
	float kept = 0.0;
	float total = 0.0;
	[loop]
	for (int y = -REFINE_RADIUS; y <= REFINE_RADIUS; ++y) {
		[loop]
		for (int x = -REFINE_RADIUS; x <= REFINE_RADIUS; ++x) {
			kept += depth_gate(centre, grad, float2(x, y),
				depth_tap(uv + float2(x, y) * inv_src));
			total += 1.0;
		}
	}
	OutS[id.xy] = kept / total;
}

[numthreads(1, 1, 1)]
void CSGlobal(uint3 id : SV_DispatchThreadID)
{
	static const float2 GRID[32] = {
		float2(0.0625, 0.125), float2(0.1875, 0.125), float2(0.3125, 0.125), float2(0.4375, 0.125),
		float2(0.5625, 0.125), float2(0.6875, 0.125), float2(0.8125, 0.125), float2(0.9375, 0.125),
		float2(0.0625, 0.375), float2(0.1875, 0.375), float2(0.3125, 0.375), float2(0.4375, 0.375),
		float2(0.5625, 0.375), float2(0.6875, 0.375), float2(0.8125, 0.375), float2(0.9375, 0.375),
		float2(0.0625, 0.625), float2(0.1875, 0.625), float2(0.3125, 0.625), float2(0.4375, 0.625),
		float2(0.5625, 0.625), float2(0.6875, 0.625), float2(0.8125, 0.625), float2(0.9375, 0.625),
		float2(0.0625, 0.875), float2(0.1875, 0.875), float2(0.3125, 0.875), float2(0.4375, 0.875),
		float2(0.5625, 0.875), float2(0.6875, 0.875), float2(0.8125, 0.875), float2(0.9375, 0.875)
	};
	float xs[32], ys[32];
	int count = 0;
	[loop]
	for (int i = 0; i < 32; ++i) {
		if (depth_at(GRID[i]) < 0.999) {
			const float2 f = flow_at(GRID[i]);
			xs[count] = f.x;
			ys[count] = f.y;
			++count;
		}
	}
	if (count < 3) {
		OutV[uint2(0, 0)] = float2(0.0, 0.0);
		return;
	}
	const int mid = count / 2;
	[loop]
	for (int k = 0; k < 32; ++k) {
		if (k > mid)
			break;
		[loop]
		for (int j = 0; j < 32; ++j) {
			if (j >= count - 1 - k)
				break;
			if (xs[j] > xs[j + 1]) {
				const float t = xs[j];
				xs[j] = xs[j + 1];
				xs[j + 1] = t;
			}
			if (ys[j] > ys[j + 1]) {
				const float t = ys[j];
				ys[j] = ys[j + 1];
				ys[j + 1] = t;
			}
		}
	}
	OutV[uint2(0, 0)] = float2(xs[mid], ys[mid]);
}

[numthreads(8, 8, 1)]
void CSConfidence(uint3 id : SV_DispatchThreadID)
{
	if (any(id.xy >= dst))
		return;
	if (first != 0u) {
		OutS[id.xy] = 0.0;
		return;
	}
	const float2 uv = uv_of(id.xy);
	const float2 flow = flow_at(uv);
	if (invalid_flow(flow)) {
		OutS[id.xy] = 0.0;
		return;
	}
	const float2 prev_uv = uv + flow;
	if (out_of_bounds(prev_uv)) {
		OutS[id.xy] = 0.0;
		return;
	}
	const float cur_luma = LumaCur.SampleLevel(LinearClamp, uv, 2);
	const float prev_luma = LumaPrev.SampleLevel(LinearClamp, prev_uv, 2);
	const float luma_error = abs(cur_luma - prev_luma);
	const float gate = ((filters & 1u) != 0u)
		? 1.0 - smoothstep(0.10, 0.22, luma_error)
		: ((luma_error > 0.15) ? 0.0 : 1.0);
	if (gate <= 0.0) {
		OutS[id.xy] = 0.0;
		return;
	}
	const float subpixel = length(inv_full);
	const float magnitude = length(flow);
	float conf;
	if (magnitude <= subpixel) {
		conf = gate;
	} else {
		const float2 destination = flow_at(prev_uv);
		if (invalid_flow(destination)) {
			OutS[id.xy] = 0.0;
			return;
		}
		const float error = length(flow - destination);
		const float normalized = error / magnitude;
		const float penalty = magnitude / subpixel;

		const float length_conf = rcp(penalty * 0.05 + 1.0);
		const float consistency = rcp(normalized + 1.0);
		const float photometric = exp(-luma_error * 5.0) * gate;
		conf = consistency * length_conf * photometric;
	}
	conf = saturate(conf);
	if ((filters & 1u) != 0u) {
		const float structure = smoothstep(0.020, 0.080, local_contrast(uv, inv_dst, 2));
		const float claim = saturate(magnitude / (4.0 * subpixel));
		conf *= lerp(1.0, structure, claim);
	}
	if ((filters & 2u) == 0u) {
		OutS[id.xy] = conf;
		return;
	}
	float history = Conf.SampleLevel(LinearClamp, prev_uv, 0);
	if (history != history) {
		OutS[id.xy] = conf;
		return;
	}
	history = saturate(history);
	const float alpha = (conf < history - 0.08) ? 0.60 : 0.15;
	OutS[id.xy] = saturate(lerp(history, conf, alpha));
}

[numthreads(8, 8, 1)]
void CSExport(uint3 id : SV_DispatchThreadID)
{
	if (any(id.xy >= dst))
		return;
	const float2 uv = uv_of(id.xy);
	float2 flow = SrcFlow.SampleLevel(LinearClamp, uv, 0);
	float conf = Conf.SampleLevel(LinearClamp, uv, 0);
	if (invalid_flow(flow))
		flow = float2(0.0, 0.0);
	if (conf != conf)
		conf = 0.0;
	OutV[id.xy] = flow;
	OutS[id.xy] = saturate(conf);
}

[numthreads(8, 8, 1)]
void CSCopyFlow(uint3 id : SV_DispatchThreadID)
{
	if (any(id.xy >= dst))
		return;
	const float2 uv = uv_of(id.xy);
	OutV[id.xy] = flow_at(uv);
	OutS[id.xy] = Conf.SampleLevel(LinearClamp, uv, 0);
}
)HLSL"
R"HLSL(
static const float kLineTol = 1.0;
static const float kTexTol = 0.5;
static const float kLineNoise = 0.6;
static const float kTexNoise = 0.15;
static const int kSurfaceReach = 8;
static const float kSurfaceAgreeLo = 0.80;
static const float kSurfaceAgreeHi = 0.92;
static const float kArbFloorLo = 0.004;
static const float kArbFloorHi = 0.008;
static const float kArbRatioLo = 1.6;
static const float kArbRatioHi = 2.6;
static const float kArbLineLo = 0.55;
static const float kArbLineHi = 0.75;
static const float kArbStructLo = 0.60;
static const float kArbStructHi = 0.80;
static const float kArbNoise = 3e-4;
static const float kArbMaxPx = 3.0;
static const float kArbMaxPxHi = 4.5;
static const float kPhotoGrad = 0.002;
static const float kTextureShare = 0.008;
static const float kExtraPhotoTake = 0.5;
static const float kPhotoSigma = 4.0;
static const float kStructFloor = 2e-5;
static const uint kTerms = 76;

float2 norm_of(float2 uv) { return (uv - 0.5) * float2(norm_x, 2.0); }

float line_like(float4 st) { return smoothstep(0.35, 0.6, st.y); }

bool model_cold()
{
	const float carried = Theta.Load(int3(3, 1, 0));
	const float valid = Theta.Load(int3(0, 1, 0));
	const float inlier = Theta.Load(int3(1, 1, 0));
	const bool first_cold = ((model_flags & 1u) != 0u) || !(valid > 0.5) || !(inlier > 0.35);
	return (model_iter != 0u) ? (carried > 0.5) : first_cold;
}

float2 model_at(float th[11], float2 x, float rho)
{
	float den = th[6] * x.x + th[7] * x.y + 1.0 + th[10] * rho;
	if (!(den > 0.25))
		return x;
	return float2(th[0] * x.x + th[1] * x.y + th[2] + th[8] * rho,
	              th[3] * x.x + th[4] * x.y + th[5] + th[9] * rho) / den;
}

float model_support()
{
	const float valid = Theta.Load(int3(0, 1, 0));
	const float inlier = Theta.Load(int3(1, 1, 0));
	const float weight = Theta.Load(int3(2, 1, 0));
	const float textured = Theta.Load(int3(5, 1, 0)) / float(dst.x * dst.y);
	if (!(valid > 0.5) || !(inlier == inlier) || !(weight == weight) || !(textured == textured))
		return 0.0;
	return smoothstep(0.35, 0.55, inlier) * smoothstep(200.0, 400.0, weight) *
		smoothstep(kTextureShare, 2.0 * kTextureShare, textured);
}

float model_quality()
{
	const float at_rest = Theta.Load(int3(6, 1, 0));
	return model_support() * (1.0 - smoothstep(0.7, 0.85, (at_rest == at_rest) ? at_rest : 0.0));
}

float3 tensor_at(float2 uv, uint mip)
{
	const float2 step = inv_full * float(1u << mip);
	float L[81];
	[unroll]
	for (int y = 0; y < 9; ++y) {
		[unroll]
		for (int x = 0; x < 9; ++x)
			L[y * 9 + x] = LumaCur.SampleLevel(LinearClamp, uv + float2(x - 4, y - 4) * step, mip);
	}
	float sxx = 0.0, sxy = 0.0, syy = 0.0, wsum = 0.0;
	[unroll]
	for (int j = 1; j < 8; ++j) {
		[unroll]
		for (int i = 1; i < 8; ++i) {
			const float gx = 0.5 * (L[j * 9 + i + 1] - L[j * 9 + i - 1]);
			const float gy = 0.5 * (L[(j + 1) * 9 + i] - L[(j - 1) * 9 + i]);
			const float w = exp(-0.125 * float((i - 4) * (i - 4) + (j - 4) * (j - 4)));
			sxx += w * gx * gx;
			sxy += w * gx * gy;
			syy += w * gy * gy;
			wsum += w;
		}
	}
	return float3(sxx, sxy, syy) / wsum;
}

float surface_rho(float r[16])
{
	float mean = 0.0, near_rho = 0.0, bx = 0.0, by = 0.0;
	[unroll]
	for (int b = 0; b < 16; ++b) {
		const float2 o = float2(float(b & 3) - 1.5, float(b >> 2) - 1.5);
		mean += r[b];
		near_rho = max(near_rho, r[b]);
		bx += o.x * r[b];
		by += o.y * r[b];
	}
	mean *= 1.0 / 16.0;
	bx *= 1.0 / 20.0;
	by *= 1.0 / 20.0;
	float off = 0.0;
	[unroll]
	for (int b2 = 0; b2 < 16; ++b2) {
		const float2 o = float2(float(b2 & 3) - 1.5, float(b2 >> 2) - 1.5);
		off = max(off, abs(r[b2] - (mean + bx * o.x + by * o.y)));
	}
	return (off > 0.02 * mean + 1e-6) ? near_rho : mean;
}

[numthreads(8, 8, 1)]
void CSStructure(uint3 id : SV_DispatchThreadID)
{
	if (any(id.xy >= dst))
		return;
	const float2 uv = uv_of(id.xy);
	const float3 fine = tensor_at(uv, 0u);
	const float3 coarse = tensor_at(uv, 1u);
	const float tr = fine.x + fine.z;
	const float3 st = fine / max(tr, 1e-12) + coarse / max(coarse.x + coarse.z, 1e-12);
	const float sxx = st.x, sxy = st.y, syy = st.z;
	const float str = sxx + syy;
	const float disc = sqrt(max(0.25 * (sxx - syy) * (sxx - syy) + sxy * sxy, 0.0));
	const float aniso = (str > 1e-12) ? saturate(2.0 * disc / str) : 0.0;
	const float angle = (disc > 1e-6 * str) ? 0.5 * atan2(2.0 * sxy, sxx - syy) : 0.0;
	const float strength = tr / (tr + kStructFloor);

	float rho = 0.0;
	if (has_depth != 0u) {
		float r[16];
		[unroll]
		for (int b = 0; b < 16; ++b) {
			const float2 o = float2(float(b & 3) - 1.5, float(b >> 2) - 1.5);
			r[b] = 1.0 - Depth.SampleLevel(PointClamp, on_grid(uv + o * inv_full), 0);
		}
		rho = surface_rho(r);
	}
	OutS4[id.xy] = float4(angle, aniso, strength, rho);
}
)HLSL"
R"HLSL(
static const uint kTermParts = 4u;
static const uint kPartTerms = kTerms / kTermParts;
groupshared float gsTerms[64][kPartTerms | 1u];

void add_sample(inout float acc[kTerms], float w, float3 I, float2 x, float2 xm, float2 xp, float rho, float den)
{
	const float wd = w / (den * den);
	const float2 res = (xp - xm) * den;
	const float ix = I.x * xm.x + I.y * xm.y;
	const float iy = I.y * xm.x + I.z * xm.y;
	const float pw[6] = { wd * I.x, wd * I.z, wd * I.y, -wd * ix, -wd * iy, wd * (xm.x * ix + xm.y * iy) };
	const float mono[10] = { 1.0, x.x, x.y, rho, x.x * x.x, x.x * x.y, x.x * rho, x.y * x.y, x.y * rho, rho * rho };
	[unroll]
	for (int pp = 0; pp < 6; ++pp) {
		[unroll]
		for (int mm = 0; mm < 10; ++mm)
			acc[pp * 10 + mm] += pw[pp] * mono[mm];
	}
	const float rx = wd * (I.x * res.x + I.y * res.y);
	const float ry = wd * (I.y * res.x + I.z * res.y);
	const float rq = -(xm.x * rx + xm.y * ry);
	[unroll]
	for (int f = 0; f < 4; ++f) {
		acc[60 + f] += rx * mono[f];
		acc[64 + f] += ry * mono[f];
	}
	[unroll]
	for (int f2 = 1; f2 < 4; ++f2)
		acc[67 + f2] += rq * mono[f2];
}

void store_terms(float acc[kTerms], uint gi, uint row)
{
	const uint2 at = uint2((row / partial_rows) * kTerms, row % partial_rows);
	[unroll]
	for (uint part = 0u; part < kTermParts; ++part) {
		const uint base = part * kPartTerms;
		[unroll]
		for (uint a1 = 0u; a1 < kPartTerms; ++a1)
			gsTerms[gi][a1] = acc[base + a1];
		GroupMemoryBarrierWithGroupSync();
		[loop]
		for (uint span = 32u; span > 0u; span >>= 1u) {
			const uint items = span * kPartTerms;
			[loop]
			for (uint j = gi; j < items; j += 64u) {
				const uint i = j / kPartTerms;
				const uint e = j - i * kPartTerms;
				gsTerms[i][e] += gsTerms[i + span][e];
			}
			GroupMemoryBarrierWithGroupSync();
		}
		if (gi < kPartTerms)
			OutS[at + uint2(base + gi, 0)] = gsTerms[0][gi];
		GroupMemoryBarrierWithGroupSync();
	}
}

void load_fit(out float th[11], bool identity)
{
	[unroll]
	for (int k = 0; k < 11; ++k)
		th[k] = identity ? ((k == 0 || k == 4) ? 1.0 : 0.0) : Theta.Load(int3(k, 0, 0));
}

float model_den(float th[11], float2 x, float rho)
{
	const float dd = th[6] * x.x + th[7] * x.y + 1.0 + th[10] * rho;
	return (dd > 0.25) ? dd : 1.0;
}

[numthreads(8, 8, 1)]
void CSModelTerms(uint3 gid : SV_GroupID, uint3 tid : SV_GroupThreadID, uint gi : SV_GroupIndex)
{
	const bool cold = model_cold();
	float th[11];
	load_fit(th, cold && model_iter == 0u);
	float sigma;
	if (cold)
		sigma = (model_iter == 0u) ? 0.0 : ((model_iter == 1u) ? 4.0 : ((model_iter == 2u) ? 2.0 : 1.5));
	else
		sigma = (model_iter == 0u) ? 3.0 : ((model_iter == 1u) ? 2.0 : 1.5);

	float acc[kTerms];
	[unroll]
	for (int a0 = 0; a0 < (int)kTerms; ++a0)
		acc[a0] = 0.0;

	[loop]
	for (uint s = 0u; s < 4u; ++s) {
		const uint2 q = gid.xy * 16u + tid.xy * 2u + uint2(s & 1u, s >> 1u);
		if (any(q >= dst))
			continue;
		const float2 f = SrcFlow.Load(int3(q, 0));
		const float4 st = StructQ.Load(int3(q, 0));
		if (invalid_flow(f) || !(st.z > 1e-3) || any(st != st))
			continue;
		float c = Conf.Load(int3(q, 0));
		c = (c == c) ? saturate(c) : 0.0;
		const float2 n = float2(cos(st.x), sin(st.x));
		const float2 tg = float2(-n.y, n.x);
		const float l = line_like(st);
		const float2 x = norm_of((float2(q) + 0.5) * inv_dst);
		const float2 xp = x + f * float2(norm_x, 2.0);
		const float rho = (has_depth != 0u) ? st.w : 0.0;
		const float2 xm = model_at(th, x, rho);
		float wn = 1.0, wt = 1.0;
		if (sigma > 0.0) {
			const float2 r = (xm - xp) * px_per_unit;
			const float en = dot(r, n) / lerp(kTexTol, kLineTol, l);
			const float et = dot(r, tg) / kTexTol;
			if (model_iter == 3u) {
				const float tn = saturate(1.0 - en * en / (4.0 * sigma * sigma));
				const float tt = saturate(1.0 - et * et / (4.0 * sigma * sigma));
				wn = tn * tn;
				wt = tt * tt;
			} else {
				const float gn = 1.0 + en * en / (sigma * sigma);
				const float gt = 1.0 + et * et / (sigma * sigma);
				wn = 1.0 / (gn * gn);
				wt = 1.0 / (gt * gt);
			}
		}
		const float sn = lerp(kTexNoise, kLineNoise, l);
		const float an = st.z * wn / (sn * sn);
		const float at = st.z * (1.0 - l) * wt / (kTexNoise * kTexNoise);
		const float3 I = float3(an * n.x * n.x + at * tg.x * tg.x,
		                        an * n.x * n.y + at * tg.x * tg.y,
		                        an * n.y * n.y + at * tg.y * tg.y) * (px_per_unit * px_per_unit);
		add_sample(acc, 0.25 + 0.75 * c, I, x, xm, xp, rho, model_den(th, x, rho));
		const float base = st.z * (0.25 + 0.75 * c);
		acc[71] += base;
		acc[72] += base * wn * lerp(wt, 1.0, l);
		acc[73] += st.z * (1.0 - l) * wn * wt;
		if (st.z > 0.3) {
			acc[74] += 1.0;
			acc[75] += (all(abs(f * float2(norm_x, 2.0) * px_per_unit) < 0.01)) ? 1.0 : 0.0;
		}
	}
	store_terms(acc, gi, gid.y * terms_gx + gid.x);
}

[numthreads(8, 8, 1)]
void CSPhotoTerms(uint3 gid : SV_GroupID, uint3 tid : SV_GroupThreadID, uint gi : SV_GroupIndex)
{
	float th[11];
	load_fit(th, false);
	const float sigma = kPhotoSigma;
	const float2 texel = float(1u << src_mip) * inv_full;
	const float to_unit = px_per_unit / float(1u << src_mip);

	float acc[kTerms];
	[unroll]
	for (int a0 = 0; a0 < (int)kTerms; ++a0)
		acc[a0] = 0.0;

	[loop]
	for (uint s = 0u; s < 4u; ++s) {
		const uint2 q = gid.xy * 16u + tid.xy * 2u + uint2(s & 1u, s >> 1u);
		if (any(q >= dst))
			continue;
		const float2 uv = (float2(q) + 0.5) * inv_dst;
		const float rho = (has_depth != 0u) ? 1.0 - Depth.SampleLevel(PointClamp, on_grid(uv), 0) : 0.0;
		const float2 x = norm_of(uv);
		const float2 xm = model_at(th, x, rho);
		const float2 uvp = xm / float2(norm_x, 2.0) + 0.5;
		if (any(uvp < texel) || any(uvp > 1.0 - texel))
			continue;
		const float lc = LumaCur.SampleLevel(LinearClamp, uv, src_mip);
		const float lp = LumaPrev.SampleLevel(LinearClamp, uvp, src_mip);
		const float gxp = LumaPrev.SampleLevel(LinearClamp, uvp + float2(texel.x, 0.0), src_mip);
		const float gxm = LumaPrev.SampleLevel(LinearClamp, uvp - float2(texel.x, 0.0), src_mip);
		const float gyp = LumaPrev.SampleLevel(LinearClamp, uvp + float2(0.0, texel.y), src_mip);
		const float gym = LumaPrev.SampleLevel(LinearClamp, uvp - float2(0.0, texel.y), src_mip);
		const float2 g = 0.5 * float2(gxp - gxm, gyp - gym) * to_unit;
		const float gg = dot(g, g);
		if (!(gg > 1e-6))
			continue;
		const float r = lp - lc;
		const float2 xp = xm - g * (r / gg);
		const float e = (r / sqrt(gg)) * px_per_unit / sigma;
		const float tk = saturate(1.0 - e * e);
		const float wr = tk * tk;
		const float cap = 1.0 / (gg + kPhotoGrad * kPhotoGrad * px_per_unit * px_per_unit);
		add_sample(acc, wr, float3(g.x * g.x, g.x * g.y, g.y * g.y) * cap, x, xm, xp, rho, model_den(th, x, rho));
		acc[71] += 1.0;
		acc[72] += wr;
	}
	store_terms(acc, gi, gid.y * terms_gx + gid.x);
}
)HLSL"
R"HLSL(
groupshared float gsRed[256];

[numthreads(256, 1, 1)]
void CSModelReduce(uint3 gid : SV_GroupID, uint3 tid : SV_GroupThreadID)
{
	const uint e = gid.x;
	const uint rows = terms_gx * terms_gy;
	float sum = 0.0;
	[loop]
	for (uint r = tid.x; r < rows; r += 256u)
		sum += Aux.Load(int3((r / partial_rows) * kTerms + e, r % partial_rows, 0));
	gsRed[tid.x] = sum;
	GroupMemoryBarrierWithGroupSync();
	[loop]
	for (uint span = 128u; span > 0u; span >>= 1u) {
		if (tid.x < span)
			gsRed[tid.x] += gsRed[tid.x + span];
		GroupMemoryBarrierWithGroupSync();
	}
	if (tid.x == 0u)
		OutS[uint2(e, 0)] = gsRed[0];
}

static const uint kType[11] = { 0, 0, 0, 1, 1, 1, 2, 2, 0, 1, 2 };
static const uint kFactor[11] = { 1, 2, 0, 1, 2, 0, 1, 2, 3, 3, 3 };
static const uint kMono[16] = { 0, 1, 2, 3, 1, 4, 5, 6, 2, 5, 7, 8, 3, 6, 8, 9 };
static const uint kPair[9] = { 0, 2, 3, 2, 1, 4, 3, 4, 5 };
static const uint kRhs[3] = { 60, 64, 67 };

groupshared float gsSum[kTerms];
groupshared float gsD[11];
groupshared float gsP[11];
groupshared float gsA[11][12];
groupshared float gsPivot[11];

float system_entry(uint i, uint j)
{
	return gsSum[kPair[kType[i] * 3u + kType[j]] * 10u + kMono[kFactor[i] * 4u + kFactor[j]]];
}

float system_rhs(uint i)
{
	return gsSum[kRhs[kType[i]] + kFactor[i]];
}

[numthreads(144, 1, 1)]
void CSModelSolve(uint3 tid : SV_GroupThreadID)
{
	const uint t = tid.x;
	if (t < kTerms)
		gsSum[t] = Aux.Load(int3(t, 0, 0));
	GroupMemoryBarrierWithGroupSync();

	const bool cold = model_cold();
	if (t < 11u) {
		float dmax = 0.0;
		[loop]
		for (uint d = 0u; d < 11u; ++d)
			dmax = max(dmax, system_entry(d, d));
		gsD[t] = rsqrt(max(system_entry(t, t), dmax * 1e-10 + 1e-30));
		float cur = Theta.Load(int3(t, 0, 0));
		if ((cold && model_iter == 0u) || !(cur == cur) || !(abs(cur) < 1e4))
			cur = (t == 0u || t == 4u) ? 1.0 : 0.0;
		gsP[t] = cur;
	}
	GroupMemoryBarrierWithGroupSync();

	const float lambda = 1e-4;
	const float tie = 0.1;
	const uint row = t / 12u, col = t - row * 12u;
	const bool mine = t < 132u;
	if (mine) {
		float v;
		const uint partner = 4u - row;
		const bool scale_pair = row == 0u || row == 4u;
		const bool shear_pair = row == 1u || row == 3u;
		if (col < 11u) {
			v = system_entry(row, col) * gsD[row] * gsD[col];
			if (row == col)
				v += lambda;
			if (scale_pair && (col == 0u || col == 4u))
				v += (row == col) ? tie * gsD[row] / gsD[partner] : -tie;
			if (shear_pair && (col == 1u || col == 3u))
				v += (row == col) ? tie * gsD[row] / gsD[partner] : tie;
		} else {
			v = system_rhs(row) * gsD[row];
			const float scale_target = 0.5 * (gsP[2] * gsP[6] - gsP[5] * gsP[7]);
			const float shear_target = 0.5 * (gsP[2] * gsP[7] + gsP[5] * gsP[6]);
			if (scale_pair)
				v -= tie * (gsP[row] - gsP[partner] - ((row == 0u) ? scale_target : -scale_target)) / gsD[partner];
			if (shear_pair)
				v -= tie * (gsP[row] + gsP[partner] - shear_target) / gsD[partner];
		}
		gsA[row][col] = v;
	}
	GroupMemoryBarrierWithGroupSync();

	[loop]
	for (uint k = 0u; k < 11u; ++k) {
		float aik = 0.0, akk = 1.0, akj = 0.0;
		if (mine) {
			aik = gsA[row][k];
			akk = gsA[k][k];
			akj = gsA[k][col];
		}
		GroupMemoryBarrierWithGroupSync();
		if (mine) {
			const float piv = (abs(akk) > 1e-20) ? akk : 1e-20;
			if (row == k)
				gsA[row][col] = akj / piv;
			else
				gsA[row][col] -= aik / piv * akj;
			if (row == k && col == k)
				gsPivot[k] = akk;
		}
		GroupMemoryBarrierWithGroupSync();
	}

	if (t == 0u) {
		float th[11];
		bool ok = gsSum[71] > 0.0;
		[unroll]
		for (int i = 0; i < 11; ++i) {
			th[i] = gsP[i] + gsA[i][11] * gsD[i];
			ok = ok && (th[i] == th[i]) && gsPivot[i] > 1e-12;
		}
		ok = ok && abs(th[0] - 1.0) < 0.5 && abs(th[4] - 1.0) < 0.5 &&
			abs(th[1]) < 0.5 && abs(th[3]) < 0.5 && abs(th[6]) < 0.5 && abs(th[7]) < 0.5 &&
			abs(th[2]) < 0.3 * norm_x && abs(th[5]) < 0.6;
		const float take = (model_iter >= 6u) ? kExtraPhotoTake : 1.0;
		[unroll]
		for (int o = 0; o < 11; ++o)
			OutS[uint2(o, 0)] = ok ? lerp(gsP[o], th[o], take) : gsP[o];
		const bool photo = model_iter >= 4u;
		OutS[uint2(0, 1)] = ok ? 1.0 : 0.0;
		OutS[uint2(1, 1)] = photo ? Theta.Load(int3(1, 1, 0)) : gsSum[72] / max(gsSum[71], 1e-6);
		OutS[uint2(2, 1)] = photo ? Theta.Load(int3(2, 1, 0)) : gsSum[71];
		OutS[uint2(3, 1)] = cold ? 1.0 : 0.0;
		OutS[uint2(4, 1)] = photo ? gsSum[72] / max(gsSum[71], 1e-6) : 0.0;
		OutS[uint2(5, 1)] = photo ? Theta.Load(int3(5, 1, 0)) : gsSum[73];
		OutS[uint2(6, 1)] = photo ? Theta.Load(int3(6, 1, 0)) : gsSum[75] / max(gsSum[74], 1.0);
	}
}
)HLSL"
R"HLSL(
static const float kPublishSnapPx = 0.35;

static const float kCrossSwitchPx = 0.20;
static const float kCrossQuality = 0.5;
static const float kCrossDistrust = 2.0;
static const float kLandingDecay = 0.98;

float frame_apart(float a[11], float b[11])
{
	float s = 0.0;
	[loop]
	for (int j = 0; j < 9; ++j) {
		[loop]
		for (int i = 0; i < 16; ++i) {
			const float2 uv = (float2(i, j) + 0.5) / float2(16.0, 9.0);
			const float rho = (has_depth != 0u) ? 1.0 - Depth.SampleLevel(PointClamp, on_grid(uv), 0) : 0.0;
			const float2 x = norm_of(uv);
			s += length((model_at(a, x, rho) - model_at(b, x, rho)) * px_per_unit);
		}
	}
	return s / 144.0;
}

float2 frame_shift(float a[11], float b[11])
{
	float2 s = float2(0.0, 0.0);
	[loop]
	for (int j = 0; j < 9; ++j) {
		[loop]
		for (int i = 0; i < 16; ++i) {
			const float2 uv = (float2(i, j) + 0.5) / float2(16.0, 9.0);
			const float rho = (has_depth != 0u) ? 1.0 - Depth.SampleLevel(PointClamp, on_grid(uv), 0) : 0.0;
			const float2 x = norm_of(uv);
			s += (model_at(a, x, rho) - model_at(b, x, rho)) * px_per_unit;
		}
	}
	return s / 144.0;
}

float probe_apart(float a[11], float b[11])
{
	float d = 0.0;
	const float2 probe[5] = { float2(0.0, 0.0), float2(-0.35, -0.7), float2(0.35, -0.7), float2(-0.35, 0.7),
		float2(0.35, 0.7) };
	const float rhos[4] = { 0.0, 0.005, 0.01, 0.02 };
	[unroll]
	for (int p = 0; p < 5; ++p) {
		const float2 x = probe[p] * float2(norm_x, 1.0);
		[unroll]
		for (int q = 0; q < 4; ++q)
			d = max(d, length((model_at(a, x, rhos[q]) - model_at(b, x, rhos[q])) * px_per_unit));
	}
	return d;
}

[numthreads(1, 1, 1)]
void CSThetaPublish(uint3 id : SV_DispatchThreadID)
{
	float raw[11];
	[unroll]
	for (int k = 0; k < 11; ++k)
		raw[k] = Theta.Load(int3(k, 0, 0));
	const uint want = clamp(pad_colour, 1u, 4u);
	const float stored = OutS[uint2(0, 6)];
	uint n = (stored == stored && stored >= 0.0 && stored <= 4.0) ? (uint)stored : 0u;
	bool usable = Theta.Load(int3(0, 1, 0)) > 0.5;
	[unroll]
	for (int k1 = 0; k1 < 11; ++k1)
		usable = usable && (raw[k1] == raw[k1]);
	if ((model_flags & 1u) != 0u || !usable)
		n = 0u;
	if ((model_flags & 8u) != 0u) {
		OutS[uint2(14, 6)] = 0.0;
		OutS[uint2(15, 6)] = 0.0;
		OutS[uint2(14, 7)] = 0.0;
		OutS[uint2(15, 7)] = 0.0;
	}
	const float stored_distrust = OutS[uint2(12, 7)];
	float distrust = (stored_distrust == stored_distrust) ? clamp(stored_distrust, 0.0, 8.0) : 0.0;
	if (OutS[uint2(11, 7)] > 0.5 && (model_flags & 4u) == 0u) {
		if ((model_flags & 2u) != 0u && (model_flags & 1u) == 0u && usable) {
			float switched[11], before[11];
			[unroll]
			for (int k9 = 0; k9 < 11; ++k9) {
				switched[k9] = OutS[uint2(k9, 7)];
				before[k9] = OutS[uint2(3 + k9, 6)];
			}
			const bool against = frame_apart(raw, before) < frame_apart(raw, switched);
			distrust = against ? min(distrust + 1.0, 8.0) : max(distrust - 1.0, 0.0);
			OutS[uint2(13, 7)] = against ? 1.0 : -1.0;
		}
		OutS[uint2(11, 7)] = 0.0;
	}
	float apart = -1.0;
	bool keep = false;
	if ((model_flags & 4u) != 0u) {
		float pub[11];
		[unroll]
		for (int k8 = 0; k8 < 11; ++k8)
			pub[k8] = OutS[uint2(k8, 0)];
		apart = (n > 0u) ? frame_apart(raw, pub) : 0.0;
		const bool supported = model_support() > kCrossQuality;
		const bool asks = apart > kCrossSwitchPx && supported;
		if (n > 0u && supported) {
			const float2 moved = (dejitter - prev_dejitter) * float2(norm_x, 2.0) * px_per_unit;
			const float2 left = frame_shift(raw, pub);
			const float2 lm = left * moved;
			const float2 mm = moved * moved;
			const float2 tol = 0.1 * abs(moved);
			const bool take_x = lm.x >= -2.0 * mm.x - tol.x && lm.x <= 4.0 * mm.x + tol.x;
			const bool take_y = lm.y >= -2.0 * mm.y - tol.y && lm.y <= 4.0 * mm.y + tol.y;
			float4 sums = float4(OutS[uint2(14, 6)], OutS[uint2(15, 6)], OutS[uint2(14, 7)], OutS[uint2(15, 7)]);
			sums = (all(sums == sums) ? sums : float4(0.0, 0.0, 0.0, 0.0)) * kLandingDecay +
				float4(take_x ? lm.x : 0.0, take_x ? mm.x : 0.0, take_y ? lm.y : 0.0, take_y ? mm.y : 0.0);
			OutS[uint2(14, 6)] = sums.x;
			OutS[uint2(15, 6)] = sums.y;
			OutS[uint2(14, 7)] = sums.z;
			OutS[uint2(15, 7)] = sums.w;
		}
		if (asks) {
			[unroll]
			for (int k10 = 0; k10 < 11; ++k10) {
				OutS[uint2(k10, 7)] = raw[k10];
				OutS[uint2(3 + k10, 6)] = pub[k10];
			}
			OutS[uint2(11, 7)] = 1.0;
		}
		keep = !asks || distrust >= kCrossDistrust;
		n = 0u;
	}
	OutS[uint2(12, 7)] = distrust;
	OutS[uint2(1, 6)] = keep ? 1.0 : 0.0;
	OutS[uint2(2, 6)] = apart;
	if (keep)
		return;

	float hist[4][11];
	[unroll]
	for (int r = 0; r < 4; ++r) {
		[unroll]
		for (int k2 = 0; k2 < 11; ++k2)
			hist[r][k2] = OutS[uint2(k2, 2 + r)];
	}
	if (n > 0u) {
		float mean[11];
		[unroll]
		for (int k3 = 0; k3 < 11; ++k3) {
			float s = 0.0;
			[unroll]
			for (int r2 = 0; r2 < 4; ++r2)
				s += (uint(r2) < n) ? hist[r2][k3] : 0.0;
			mean[k3] = s / float(n);
		}
		if (!(probe_apart(raw, mean) <= kPublishSnapPx))
			n = 0u;
	}
	[unroll]
	for (int r3 = 3; r3 > 0; --r3) {
		[unroll]
		for (int k4 = 0; k4 < 11; ++k4)
			hist[r3][k4] = hist[r3 - 1][k4];
	}
	[unroll]
	for (int k5 = 0; k5 < 11; ++k5)
		hist[0][k5] = raw[k5];
	n = min(n + 1u, want);

	[unroll]
	for (int k6 = 0; k6 < 11; ++k6) {
		float s = 0.0;
		[unroll]
		for (int r4 = 0; r4 < 4; ++r4)
			s += (uint(r4) < n) ? hist[r4][k6] : 0.0;
		OutS[uint2(k6, 0)] = s / float(n);
	}
	[unroll]
	for (int c = 0; c < 16; ++c)
		OutS[uint2(c, 1)] = Theta.Load(int3(c, 1, 0));
	[unroll]
	for (int r5 = 0; r5 < 4; ++r5) {
		[unroll]
		for (int k7 = 0; k7 < 11; ++k7)
			OutS[uint2(k7, 2 + r5)] = hist[r5][k7];
	}
	OutS[uint2(0, 6)] = float(n);
}
float arbiter_veto(float2 uv, float4 sc, float2 f, float2 model_uv)
{
	if ((model_flags & 2u) == 0u)
		return 0.0;
	if (has_depth != 0u) {
		const float own_rho = 1.0 - Depth.SampleLevel(PointClamp, on_grid(uv), 0);
		float lo = own_rho, hi = own_rho;
		[unroll]
		for (int d = 0; d < 4; ++d) {
			const float2 o = float2((d & 1) ? ((d & 2) ? 3.0 : -3.0) : 0.0,
			                        (d & 1) ? 0.0 : ((d & 2) ? 3.0 : -3.0)) * inv_full;
			const float rn = 1.0 - Depth.SampleLevel(PointClamp, on_grid(uv + o), 0);
			lo = min(lo, rn);
			hi = max(hi, rn);
		}
		if (abs(sc.w - own_rho) > 0.02 * max(sc.w, own_rho) + 1e-6 ||
			hi - lo > 0.02 * max(hi, 1e-6))
			return 0.0;
	}
	const float apart = length((model_uv - f) * px_per_unit * float2(norm_x, 2.0));
	const float open = smoothstep(kArbStructLo, kArbStructHi, saturate(sc.z)) *
		(1.0 - smoothstep(kArbLineLo, kArbLineHi, saturate(sc.y))) *
		(1.0 - smoothstep(kArbMaxPx, kArbMaxPxHi, apart));
	if (!(open > 0.0))
		return 0.0;
	float em = 0.0, ep = 0.0;
	[unroll]
	for (int b = 0; b < 16; ++b) {
		const float2 o = (float2(float(b & 3), float(b >> 2)) - 1.5) * inv_full;
		const float lc = LumaCur.SampleLevel(LinearClamp, uv + o, 0);
		const float am = LumaPrev.SampleLevel(LinearClamp, uv + o + f, 0) - lc;
		const float ap = LumaPrev.SampleLevel(LinearClamp, uv + o + model_uv, 0) - lc;
		em += am * am;
		ep += ap * ap;
	}
	em = sqrt(em * (1.0 / 16.0));
	ep = sqrt(ep * (1.0 / 16.0));
	if (!(em == em) || !(ep == ep))
		return 0.0;
	if (!(em > 1e-6))
		return 0.0;
	return open * smoothstep(kArbRatioLo, kArbRatioHi, ep / max(em, kArbNoise)) *
		(1.0 - smoothstep(kArbFloorLo, kArbFloorHi, em));
}

float gate_alpha(float4 st, float2 f, float2 x, float th[11])
{
	const float rho = (has_depth != 0u) ? st.w : 0.0;
	const float2 r = (model_at(th, x, rho) - (x + f * float2(norm_x, 2.0))) * px_per_unit;
	const float2 n = float2(cos(st.x), sin(st.x));
	const float l = line_like(st);
	const float k = sqrt(saturate(st.z));
	const float an = 1.0 - smoothstep(1.0, 2.5, abs(dot(r, n)) * k / lerp(kTexTol, kLineTol, l));
	const float at = 1.0 - smoothstep(1.0, 2.5, abs(dot(r, float2(-n.y, n.x))) * k / kTexTol);
	return an * lerp(at, 1.0, l);
}
)HLSL"
R"HLSL(
[numthreads(8, 8, 1)]
void CSDecision(uint3 id : SV_DispatchThreadID)
{
	if (any(id.xy >= dst) || Theta.Load(int3(1, 6, 0)) > 0.5)
		return;
	const float quality = model_quality();
	if (!(quality > 0.0)) {
		OutS[id.xy] = 0.0;
		return;
	}
	float th[11];
	[unroll]
	for (int k = 0; k < 11; ++k)
		th[k] = Theta.Load(int3(k, 0, 0));
	const float4 sc = StructQ.Load(int3(id.xy, 0));
	const float rest_share = Theta.Load(int3(6, 1, 0));
	const float resting = smoothstep(0.5, 0.7, (rest_share == rest_share) ? rest_share : 0.0);
	float num = 0.0, wsum = 0.0, own = 1.0;
	[loop]
	for (int dy = -2; dy <= 2; ++dy) {
		[loop]
		for (int dx = -2; dx <= 2; ++dx) {
			const int2 q = int2(id.xy) + int2(dx, dy);
			if (any(q < 0) || any(q >= int2(dst)))
				continue;
			const float2 f = SrcFlow.Load(int3(q, 0));
			if (invalid_flow(f))
				continue;
			const float4 st = StructQ.Load(int3(q, 0));
			float a = gate_alpha(st, f, norm_of((float2(q) + 0.5) * inv_dst), th);
			if (all(abs(f * float2(norm_x, 2.0) * px_per_unit) < 0.01))
				a *= (st.z > 0.3 && line_like(st) < 0.5) ? 0.0 : 1.0 - resting;
			if (dx == 0 && dy == 0)
				own = a;
			float w = st.z * exp(-0.22 * float(dx * dx + dy * dy));
			if (has_depth != 0u)
				w *= (abs(st.w - sc.w) <= 0.1 * max(st.w, sc.w) + 1e-5) ? 1.0 : 0.05;
			num += w * a;
			wsum += w;
		}
	}
	const float prop = (wsum > 0.05) ? num / wsum : own;
	float alpha = saturate(lerp(prop, own, saturate(sc.z))) * quality;
	if (has_depth != 0u && alpha < 0.999) {
		float agree = 0.0, count = 0.0;
		[loop]
		for (int oy = -kSurfaceReach; oy <= kSurfaceReach; oy += 2) {
			[loop]
			for (int ox = -kSurfaceReach; ox <= kSurfaceReach; ox += 2) {
				const int2 q = int2(id.xy) + int2(ox, oy);
				if (any(q < 0) || any(q >= int2(dst)))
					continue;
				const float4 st = StructQ.Load(int3(q, 0));
				if (!(st.z > 0.3) || !(abs(st.w - sc.w) <= 0.02 * max(st.w, sc.w) + 1e-6))
					continue;
				const float2 f = SrcFlow.Load(int3(q, 0));
				if (invalid_flow(f))
					continue;
				agree += gate_alpha(st, f, norm_of((float2(q) + 0.5) * inv_dst), th);
				count += 1.0;
			}
		}
		if (count >= 20.0)
			alpha = max(alpha, smoothstep(kSurfaceAgreeLo, kSurfaceAgreeHi, agree / count) * quality);
	}
	if (alpha > 0.02) {
		const float2 uvc = uv_of(id.xy);
		const float2 fc = SrcFlow.Load(int3(id.xy, 0));
		if (!invalid_flow(fc)) {
			const float2 xc = norm_of(uvc);
			const float2 mv = (model_at(th, xc, (has_depth != 0u) ? sc.w : 0.0) - xc) /
				float2(norm_x, 2.0);
			if (!invalid_flow(mv))
				alpha *= 1.0 - arbiter_veto(uvc, sc, fc, mv);
		}
	}
	OutS[id.xy] = alpha;
}

float pixel_rho(float2 uv)
{
	float r[9];
	float mean = 0.0, near_rho = 0.0, bx = 0.0, by = 0.0;
	[unroll]
	for (int k = 0; k < 9; ++k) {
		const float2 o = float2(float(k % 3) - 1.0, float(k / 3) - 1.0);
		r[k] = 1.0 - Depth.SampleLevel(PointClamp, on_grid(uv + o * inv_full), 0);
		mean += r[k];
		near_rho = max(near_rho, r[k]);
		bx += o.x * r[k];
		by += o.y * r[k];
	}
	mean *= 1.0 / 9.0;
	bx *= 1.0 / 6.0;
	by *= 1.0 / 6.0;
	float off = 0.0;
	[unroll]
	for (int k2 = 0; k2 < 9; ++k2) {
		const float2 o = float2(float(k2 % 3) - 1.0, float(k2 / 3) - 1.0);
		off = max(off, abs(r[k2] - (mean + bx * o.x + by * o.y)));
	}
	return (off > 0.02 * mean + 1e-6) ? near_rho : r[4];
}

[numthreads(8, 8, 1)]
void CSFuse(uint3 id : SV_DispatchThreadID)
{
	if (any(id.xy >= dst) || Theta.Load(int3(1, 6, 0)) > 0.5)
		return;
	const float2 uv = uv_of(id.xy);
	float2 flow = SrcFlow.SampleLevel(LinearClamp, uv, 0);
	float conf = Conf.SampleLevel(LinearClamp, uv, 0);
	if (invalid_flow(flow))
		flow = float2(0.0, 0.0);
	if (conf != conf)
		conf = 0.0;
	const float2 matched = flow;
	float alpha = Aux.SampleLevel(LinearClamp, uv, 0);
	alpha = (alpha == alpha) ? saturate(alpha) : 0.0;
	if (alpha > 0.0) {
		float th[11];
		[unroll]
		for (int k = 0; k < 11; ++k)
			th[k] = Theta.Load(int3(k, 0, 0));
		const float rho = (has_depth != 0u) ? pixel_rho(uv) : 0.0;
		const float2 x = norm_of(uv);
		const float2 m = (model_at(th, x, rho) - x) / float2(norm_x, 2.0);
		if (!invalid_flow(m)) {
			const float4 st = StructQ.SampleLevel(PointClamp, uv, 0);
			const float2 n = float2(cos(st.x), sin(st.x));
			const float2 t = float2(-n.y, n.x);
			const float2 d = (flow - m) * float2(norm_x, 2.0);
			const float2 kept = n * dot(n, d) + t * (dot(t, d) * (1.0 - line_like(st) * alpha));
			flow = m + (1.0 - alpha) * kept / float2(norm_x, 2.0);
		}
	}
	if (invalid_flow(flow))
		flow = matched;
	OutV[id.xy] = flow;
	OutS[id.xy] = saturate(conf);
}
)HLSL";
