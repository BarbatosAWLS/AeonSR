#pragma once

namespace aeon_sr::framegen_shaders {

inline constexpr char kInterpolatorHlsl[] =
R"HLSL(
cbuffer Parameters : register(b0)
{
	uint2  size;
	float2 invSize;
	float  t;
	uint   hasDepth;
	uint2  statSize;
	uint2  tileCount;
	uint   encodeSrgb;
	uint   isFloat;
};

Texture2D<float4> FrameA : register(t0);
Texture2D<float4> FrameB : register(t1);
Texture2D<float2> Motion : register(t2);
Texture2D<float2> GlobalFlow : register(t4);
Texture2D<float>  Depth : register(t5);
Texture2D<float2> TileStats : register(t6);
Texture2D<float>  SearchErrors : register(t7);
Texture2D<float4> Summary : register(t8);
Texture2D<float2> Refined : register(t9);
Texture2D<float4> Layers : register(t10);
Texture2D<float>  LayerDepth : register(t11);
SamplerState      LinearClamp : register(s0);

RWTexture2D<float4> Output : register(u0);
RWTexture2D<float2>       TileStatsOut : register(u1);
RWTexture2D<float>        SearchErrorsOut : register(u2);
RWTexture2D<float4>       SummaryOut : register(u3);
RWTexture2D<float2>       RefinedOut : register(u4);
RWTexture2D<float4>       LayersOut : register(u5);
RWTexture2D<float>        LayerDepthOut : register(u6);

static const int kStatStride = 8;
static const uint kTile = 8;
static const float kCutPixelError = 0.06;
static const float kCutFraction = 0.4;
static const int kSearchRadius = 6;
static const int kSearchSpan = 2 * kSearchRadius + 1;
static const int kSearchGrid = 32;
static const float kSearchCap = 0.1;
static const float kEndpointTolerance = 0.03;
static const float kGoodEnough = 0.003;
static const float kUnexplainedOnset = 0.05;
static const float kUnexplainedFull = 0.15;
static const float kSettled = 0.012;
static const uint kLocalCandidates = 4;
static const float kRefineLimit = 0.05;
static const float kRefineSpread = 0.5;
static const float kUnmatched = 0.06;
static const float kDominantPrior = 0.004;
static const float kExactFit = 0.002;
static const float kForegroundPreference = 0.005;
static const float kDepthSeparation = 0.02;
static const float kCoveredByMotion = 16.0;
static const float kTrajectoryReach = 32.0;
static const float kOcclusionOnset = 0.03;
static const float kLayerOnset = 0.01;
static const float kLayerFull = 0.03;
static const float kCoverTolerance = 0.03;
static const float kConsistencyWeight = 0.02;
static const float kDistinctColour = 0.08;
static const float kRelatedColour = 0.03;

int2 ClampPixel(int2 p)
{
	return clamp(p, int2(0, 0), int2(size) - 1);
}

float2 RawMotionAt(float2 position)
{
	return Motion.Load(int3(ClampPixel(int2(floor(position))), 0)) * float2(size);
}

float2 MotionAt(float2 position)
{
	return Refined.Load(int3(ClampPixel(int2(floor(position))), 0));
}

float DepthAt(float2 position)
{
	return Depth.Load(int3(ClampPixel(int2(floor(position))), 0));
}

float3 SampleA(float2 position)
{
	return FrameA.SampleLevel(LinearClamp, position * invSize, 0).rgb;
}

float3 SampleB(float2 position)
{
	return FrameB.SampleLevel(LinearClamp, position * invSize, 0).rgb;
}

float3 SharpSample(Texture2D<float4> frame, float2 position)
{
	float2 centre = floor(position - 0.5) + 0.5;
	float2 f = position - centre;
	float2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
	float2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
	float2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
	float2 w3 = f * f * (-0.5 + 0.5 * f);
	float2 w12 = w1 + w2;
	float2 offset12 = w2 / w12;
	float2 p0 = (centre - 1.0) * invSize;
	float2 p3 = (centre + 2.0) * invSize;
	float2 p12 = (centre + offset12) * invSize;
	float3 result = frame.SampleLevel(LinearClamp, float2(p12.x, p0.y), 0).rgb * (w12.x * w0.y);
	result += frame.SampleLevel(LinearClamp, float2(p0.x, p12.y), 0).rgb * (w0.x * w12.y);
	result += frame.SampleLevel(LinearClamp, float2(p12.x, p12.y), 0).rgb * (w12.x * w12.y);
	result += frame.SampleLevel(LinearClamp, float2(p3.x, p12.y), 0).rgb * (w3.x * w12.y);
	result += frame.SampleLevel(LinearClamp, float2(p12.x, p3.y), 0).rgb * (w12.x * w3.y);
	float normaliser = w12.x * w0.y + w0.x * w12.y + w12.x * w12.y + w3.x * w12.y + w12.x * w3.y;
	result /= normaliser;
	return isFloat != 0 ? max(result, 0.0) : saturate(result);
}

float4 Encoded(float3 c)
{
	[branch] if(encodeSrgb != 0)
	{
		c = saturate(c);
		c = c <= 0.0031308 ? c * 12.92 : 1.055 * pow(c, 1.0 / 2.4) - 0.055;
	}
	return float4(c, 1);
}

float ColourDistance(float3 a, float3 b)
{
	float3 d = abs(a - b);
	return (d.r + d.g + d.b) * (1.0 / 3.0);
}

float PatchError(float2 q, float2 v)
{
	float2 a = q + t * v;
	float2 b = q - (1.0 - t) * v;
	float centre = ColourDistance(SampleA(a), SampleB(b)) * 2.0;
	float topLeft = ColourDistance(SampleA(a + float2(-1, -1)), SampleB(b + float2(-1, -1)));
	float topRight = ColourDistance(SampleA(a + float2(1, -1)), SampleB(b + float2(1, -1)));
	float bottomLeft = ColourDistance(SampleA(a + float2(-1, 1)), SampleB(b + float2(-1, 1)));
	float bottomRight = ColourDistance(SampleA(a + float2(1, 1)), SampleB(b + float2(1, 1)));
	float halves = min(min(topLeft + bottomLeft, topRight + bottomRight), min(topLeft + topRight, bottomLeft + bottomRight));
	return (centre + halves) * 0.25;
}

groupshared float2 tileSums[64];

[numthreads(8, 8, 1)] void StatsMain(uint3 id : SV_DispatchThreadID, uint3 group : SV_GroupID, uint index : SV_GroupIndex)
{
	int2 pixel = int2(id.xy) * kStatStride + kStatStride / 2;
	float2 sums = float2(0, 0);
	if(all(pixel < int2(size)))
	{
		float2 q = float2(pixel) + 0.5;
		float3 b = FrameB.Load(int3(pixel, 0)).rgb;
		float2 g = GlobalFlow.Load(int3(0, 0, 0)) * float2(size);
		float e = min(min(ColourDistance(b, SampleA(q + RawMotionAt(q))), ColourDistance(b, SampleA(q + g))),
					  ColourDistance(b, FrameA.Load(int3(pixel, 0)).rgb));
		sums = float2(e > kCutPixelError ? 1.0 : 0.0, 1.0);
	}
	tileSums[index] = sums;
	GroupMemoryBarrierWithGroupSync();
	[unroll] for(uint stride = 32; stride > 0; stride >>= 1)
	{
		if(index < stride)
			tileSums[index] += tileSums[index + stride];
		GroupMemoryBarrierWithGroupSync();
	}
	if(index == 0)
		TileStatsOut[group.xy] = tileSums[0];
}

int2 SearchCentre(uint window)
{
	return window == 0 ? int2(0, 0) : int2(round(GlobalFlow.Load(int3(0, 0, 0)) * float2(size)));
}

groupshared float searchSums[128];

[numthreads(128, 1, 1)] void SearchMain(uint3 group : SV_GroupID, uint index : SV_GroupIndex)
{
	int2 v = SearchCentre(group.y / kSearchSpan) + int2(group.x, group.y % kSearchSpan) - kSearchRadius;
	float2 margin = float2(24, 24);
	float2 span = (float2(size) - 2.0 * margin) / kSearchGrid;
	float sum = 0.0;
	[unroll] for(uint k = 0; k < 8; ++k)
	{
		uint sampleIndex = index + 128 * k;
		int2 b = int2(margin + (float2(sampleIndex % kSearchGrid, sampleIndex / kSearchGrid) + 0.5) * span);
		float3 cb = FrameB.Load(int3(b, 0)).rgb;
		float3 ca = FrameA.Load(int3(ClampPixel(b + v), 0)).rgb;
		sum += min(ColourDistance(ca, cb), kSearchCap);
	}
	searchSums[index] = sum;
	GroupMemoryBarrierWithGroupSync();
	[unroll] for(uint stride = 64; stride > 0; stride >>= 1)
	{
		if(index < stride)
			searchSums[index] += searchSums[index + stride];
		GroupMemoryBarrierWithGroupSync();
	}
	if(index == 0)
		SearchErrorsOut[group.xy] = searchSums[0] / (8.0 * 128.0);
}

groupshared float2 reduceSums[256];
groupshared float2 reduceBest[256];

float SearchErrorAt(uint window, int2 cell)
{
	int2 inside = clamp(cell, 0, kSearchSpan - 1);
	return SearchErrors.Load(int3(inside.x, inside.y + window * kSearchSpan, 0));
}

[numthreads(256, 1, 1)] void ReduceMain(uint index : SV_GroupIndex)
{
	float2 sums = float2(0, 0);
	uint count = statSize.x * statSize.y;
	for(uint i = index; i < count; i += 256)
		sums += TileStats.Load(int3(i % statSize.x, i / statSize.x, 0));
	reduceSums[index] = sums;

	float2 best = float2(1e9, 0);
	for(uint c = index; c < 2 * kSearchSpan * kSearchSpan; c += 256)
	{
		int2 texel = int2(c % kSearchSpan, c / kSearchSpan);
		int2 v = SearchCentre(texel.y / kSearchSpan) + int2(texel.x, texel.y % kSearchSpan) - kSearchRadius;
		float e = SearchErrors.Load(int3(texel, 0)) + 1e-6 * dot(v, v);
		if(e < best.x)
			best = float2(e, c);
	}
	reduceBest[index] = best;
	GroupMemoryBarrierWithGroupSync();
	[unroll] for(uint stride = 128; stride > 0; stride >>= 1)
	{
		if(index < stride)
		{
			reduceSums[index] += reduceSums[index + stride];
			if(reduceBest[index + stride].x < reduceBest[index].x)
				reduceBest[index] = reduceBest[index + stride];
		}
		GroupMemoryBarrierWithGroupSync();
	}
	if(index == 0)
	{
		uint c = uint(reduceBest[0].y);
		uint window = (c / kSearchSpan) / kSearchSpan;
		int2 cell = int2(c % kSearchSpan, (c / kSearchSpan) % kSearchSpan);
		float centre = SearchErrorAt(window, cell);
		float2 minus = float2(SearchErrorAt(window, cell - int2(1, 0)), SearchErrorAt(window, cell - int2(0, 1)));
		float2 plus = float2(SearchErrorAt(window, cell + int2(1, 0)), SearchErrorAt(window, cell + int2(0, 1)));
		float2 steeper = max(minus, plus) - centre;
		float2 offset = centre > 1e-4 && steeper > 1e-6 ? clamp(0.5 * (minus - plus) / max(steeper, 1e-6), -0.5, 0.5) : 0.0;
		float2 found = float2(SearchCentre(window) + cell - kSearchRadius) + offset;
		SummaryOut[uint2(0, 0)] = float4(found, reduceSums[0].x / max(reduceSums[0].y, 1.0), centre);
	}
}

float MatchError(int2 pixel, float2 v)
{
	float2 a = float2(pixel) + 0.5 + v;
	float centre = ColourDistance(FrameB.Load(int3(pixel, 0)).rgb, SampleA(a)) * 2.0;
	float topLeft = ColourDistance(FrameB.Load(int3(ClampPixel(pixel + int2(-1, -1)), 0)).rgb, SampleA(a + float2(-1, -1)));
	float topRight = ColourDistance(FrameB.Load(int3(ClampPixel(pixel + int2(1, -1)), 0)).rgb, SampleA(a + float2(1, -1)));
	float bottomLeft = ColourDistance(FrameB.Load(int3(ClampPixel(pixel + int2(-1, 1)), 0)).rgb, SampleA(a + float2(-1, 1)));
	float bottomRight = ColourDistance(FrameB.Load(int3(ClampPixel(pixel + int2(1, 1)), 0)).rgb, SampleA(a + float2(1, 1)));
	float halves = min(min(topLeft + bottomLeft, topRight + bottomRight), min(topLeft + topRight, bottomLeft + bottomRight));
	return (centre + halves) * 0.25;
}

[numthreads(8, 8, 1)] void RefineMain(uint3 id : SV_DispatchThreadID)
{
	if(any(id.xy >= size))
		return;
	int2 pixel = int2(id.xy);
	float2 p = float2(pixel) + 0.5;
	float2 dominant = Summary.Load(int3(0, 0, 0)).xy;
	float2 camera = GlobalFlow.Load(int3(0, 0, 0)) * float2(size);
	static const float2 reach[12] = {float2(4, 0), float2(-4, 0), float2(0, 4), float2(0, -4),
									 float2(9, 0), float2(-9, 0), float2(0, 9), float2(0, -9),
									 float2(16, 0), float2(-16, 0), float2(0, 16), float2(0, -16)};
	float2 options[13];
	options[0] = RawMotionAt(p);
	float spread = 0.0;
	[unroll] for(uint k = 4; k < 8; ++k)
	{
		options[k + 1] = RawMotionAt(p + reach[k]);
		spread = max(spread, length(options[k + 1] - options[0]));
	}
	[branch] if(spread < kRefineSpread)
	{
		RefinedOut[pixel] = options[0];
		return;
	}
	[unroll] for(uint n = 0; n < 12; ++n)
	{
		if(n < 4 || n >= 8)
			options[n + 1] = RawMotionAt(p + reach[n]);
	}
	float bestError = 1e9;
	float2 best = options[0];
	float2 nearestCamera = options[0];
	float nearestCameraDistance = 1e9;
	[unroll] for(uint i = 0; i < 13; ++i)
	{
		float2 v = options[i];
		bool repeated = false;
		[unroll] for(uint j = 0; j < i; ++j)
			repeated = repeated || all(abs(options[j] - v) < 0.25);
		[branch] if(repeated)
			continue;
		float e = MatchError(pixel, v) + kDominantPrior * saturate(length(v - dominant) / 8.0);
		if(e < bestError)
		{
			bestError = e;
			best = v;
		}
		float d = length(v - camera);
		if(d < nearestCameraDistance)
		{
			nearestCameraDistance = d;
			nearestCamera = v;
		}
	}
	RefinedOut[pixel] = bestError > kUnmatched ? nearestCamera : best;
}
)HLSL"
R"HLSL([numthreads(8, 8, 1)] void LayersMain(uint3 id : SV_DispatchThreadID)
{
	if(any(id.xy >= tileCount))
		return;
	float2 centre = float2(id.xy * kTile) + kTile * 0.5;
	float2 camera = GlobalFlow.Load(int3(0, 0, 0)) * float2(size);
	float2 samples[36];
	float depths[36];
	[unroll] for(uint k = 0; k < 36; ++k)
	{
		float2 position = centre + float2(float(k % 6) * 10.0 - 25.0, float(k / 6) * 10.0 - 25.0);
		samples[k] = MotionAt(position);
		depths[k] = hasDepth != 0 ? DepthAt(position) : 0.0;
	}
	float nearest = 1.0;
	float farthest = 0.0;
	[unroll] for(uint d = 0; d < 36; ++d)
	{
		nearest = min(nearest, depths[d]);
		farthest = max(farthest, depths[d]);
	}
	float2 background = samples[0];
	float backgroundKey = -1e9;
	float backgroundDepth = farthest;
	[unroll] for(uint g = 0; g < 36; ++g)
	{
		bool eligible = hasDepth == 0 || depths[g] > farthest - kDepthSeparation;
		float key = -length(samples[g] - camera);
		if(eligible && key > backgroundKey)
		{
			backgroundKey = key;
			background = samples[g];
		}
	}
	float2 foreground = background;
	float foregroundKey = -1e9;
	[unroll] for(uint f = 0; f < 36; ++f)
	{
		bool eligible = hasDepth == 0 || depths[f] < nearest + kDepthSeparation;
		float key = length(samples[f] - background);
		if(eligible && key > foregroundKey)
		{
			foregroundKey = key;
			foreground = samples[f];
		}
	}
	if(hasDepth != 0 && farthest - nearest < kDepthSeparation)
		foreground = background;
	[branch] if(length(background - camera) > kCoveredByMotion)
	{
		if(length(foreground - camera) < length(background - camera))
			foreground = background;
		background = camera;
		backgroundDepth = 1.0;
	}
	foregroundKey = length(foreground - background);
	static const float2 directions[8] = {float2(1, 0), float2(-1, 0), float2(0, 1), float2(0, -1),
										 float2(0.7071, 0.7071), float2(-0.7071, 0.7071),
										 float2(0.7071, -0.7071), float2(-0.7071, -0.7071)};
	[unroll] for(uint r = 0; r < 24; ++r)
	{
		float2 position = centre + directions[r % 8] * (60.0 * float(r / 8 + 1));
		float2 travel = MotionAt(position);
		float along = saturate(dot(centre - position, travel) / max(dot(travel, travel), 1e-4));
		float miss = length(position + along * travel - centre);
		bool nearer = hasDepth == 0 || DepthAt(position) < backgroundDepth - kDepthSeparation;
		float key = length(travel - background);
		if(miss < kTrajectoryReach && nearer && key > foregroundKey)
		{
			foregroundKey = key;
			foreground = travel;
		}
	}
	LayersOut[id.xy] = float4(background, foreground);
	LayerDepthOut[id.xy] = backgroundDepth;
}

float EndpointError(float2 q, float2 v)
{
	float3 a = FrameA.Load(int3(ClampPixel(int2(q)), 0)).rgb;
	float3 b = FrameB.Load(int3(ClampPixel(int2(q)), 0)).rgb;
	return max(ColourDistance(b, SampleA(q + v)), ColourDistance(a, SampleB(q - v)));
}

bool PixelUnchanged(int2 p)
{
	return all(FrameA.Load(int3(p, 0)).rgb == FrameB.Load(int3(p, 0)).rgb);
}

[numthreads(8, 8, 1)] void InterpolateMain(uint3 id : SV_DispatchThreadID)
{
	if(any(id.xy >= size))
		return;
	int2 pixel = int2(id.xy);
	float2 q = float2(pixel) + 0.5;
	float4 summary = Summary.Load(int3(0, 0, 0));
	if(summary.z > kCutFraction)
	{
		Output[pixel] = Encoded(t < 0.5 ? FrameA.Load(int3(pixel, 0)).rgb : FrameB.Load(int3(pixel, 0)).rgb);
		return;
	}

	float2 here = MotionAt(q);
	if(PixelUnchanged(pixel))
	{
		float3 colour = FrameA.Load(int3(pixel, 0)).rgb;
		uint same = 0;
		uint distinct = 0;
		uint related = 0;
		[unroll] for(int y = -2; y <= 2; ++y)
		{
			[unroll] for(int x = -2; x <= 2; ++x)
			{
				int2 n = ClampPixel(pixel + int2(x, y));
				float3 a = FrameA.Load(int3(n, 0)).rgb;
				float3 b = FrameB.Load(int3(n, 0)).rgb;
				bool unchanged = all(a == b);
				if(abs(x) <= 1 && abs(y) <= 1)
					same += unchanged ? 1u : 0u;
				float closest = min(ColourDistance(a, colour), ColourDistance(b, colour));
				distinct += !unchanged && closest > kDistinctColour ? 1u : 0u;
				related += !unchanged && closest < kRelatedColour ? 1u : 0u;
			}
		}
		bool dominantExplains = EndpointError(q, summary.xy) < kEndpointTolerance;
		bool exactTranslation = summary.w < kExactFit && dominantExplains;
		bool staticField = same == 9 && (dot(here, here) < 0.0625 || (distinct + related == 0 && !exactTranslation));
		bool staticOverlay = same >= 4 && distinct > related && !dominantExplains;
		if(staticField || staticOverlay)
		{
			Output[pixel] = Encoded(colour);
			return;
		}
	}

	float4 layers = Layers.Load(int3(pixel / kTile, 0));
	float2 background = layers.xy;
	float2 foreground = layers.zw;
	float backgroundDepth = LayerDepth.Load(int3(pixel / kTile, 0));
	bool layered = length(foreground - background) > 1.0;
	float2 cameraMotion = GlobalFlow.Load(int3(0, 0, 0)) * float2(size);

	float2 candidates[8];
	candidates[0] = summary.xy;
	candidates[1] = MotionAt(q - (1.0 - t) * here);
	candidates[2] = here;
	candidates[3] = MotionAt(q - (1.0 - t) * candidates[1]);
	candidates[4] = foreground;
	candidates[5] = background;
	candidates[6] = cameraMotion;
	candidates[7] = float2(0, 0);

	float bestCost = 1e9;
	float2 best = here;
	float bestPhotometric = 1e9;
	[unroll] for(uint i = 0; i < 8; ++i)
	{
		float2 v = candidates[i];
		bool repeated = false;
		[unroll] for(uint j = 0; j < i; ++j)
			repeated = repeated || all(abs(candidates[j] - v) < 0.05);
		bool settled = bestCost < kGoodEnough || (i >= kLocalCandidates && bestCost < kSettled);
		[branch] if(repeated || settled)
			continue;
		float mismatch = length(MotionAt(q - (1.0 - t) * v) - v);
		float consistencyCost = kConsistencyWeight * saturate((mismatch - 0.25) / 2.0);
		if(i == 0)
			consistencyCost *= saturate(summary.w / 0.01);
		float photometric;
		[branch] if(i == 0 && !layered)
		{
			photometric = ColourDistance(SampleA(q + t * v), SampleB(q - (1.0 - t) * v));
			if(photometric >= kGoodEnough)
				photometric = PatchError(q, v);
		}
		else
		{
			photometric = PatchError(q, v);
		}
		float both = photometric + consistencyCost + kDominantPrior * saturate(length(v - summary.xy) / 8.0);
		if(both < bestCost)
		{
			bestCost = both;
			bestPhotometric = photometric;
			best = v;
		}
	}

	[branch] if(layered && length(best - background) < length(best - foreground))
	{
		float mismatch = length(MotionAt(q - (1.0 - t) * foreground) - foreground);
		float foregroundPhotometric = PatchError(q, foreground);
		float foregroundCost = foregroundPhotometric + kConsistencyWeight * saturate((mismatch - 0.25) / 2.0);
		if(foregroundCost < max(bestCost, kGoodEnough) + kForegroundPreference)
		{
			bestCost = foregroundCost;
			bestPhotometric = foregroundPhotometric;
			best = foreground;
		}
	}

	[branch] if(bestCost > kGoodEnough && bestCost < kRefineLimit)
	{
		float2 centre = best;
		float centreError = bestPhotometric;
		float2 minus = float2(PatchError(q, centre - float2(0.5, 0)), PatchError(q, centre - float2(0, 0.5)));
		float2 plus = float2(PatchError(q, centre + float2(0.5, 0)), PatchError(q, centre + float2(0, 0.5)));
		float2 curvature = minus + plus - 2.0 * centreError;
		float2 step = curvature > 1e-5 ? clamp(0.25 * (minus - plus) / max(curvature, 1e-5), -0.5, 0.5) : 0.0;
		float2 refined = centre + step;
		float refinedError = PatchError(q, refined);
		if(refinedError < centreError)
		{
			bestCost += refinedError - centreError;
			best = refined;
		}
	}

	float3 warpedA = SharpSample(FrameA, q + t * best);
	float3 warpedB = SharpSample(FrameB, q - (1.0 - t) * best);
	float unexplained = smoothstep(kUnexplainedOnset, kUnexplainedFull, bestCost);
	float3 result = lerp(warpedA, warpedB, lerp(t, t < 0.5 ? 0.0 : 1.0, unexplained));
	[branch] if(bestCost > kOcclusionOnset)
	{
		float2 a = q + t * background;
		float2 b = q - (1.0 - t) * background;
		bool coverA = PixelUnchanged(ClampPixel(int2(floor(a))));
		bool coverB = PixelUnchanged(ClampPixel(int2(floor(b))));
		float foregroundError = 1.0;
		[branch] if(layered)
		{
			bool foregroundB;
			if(hasDepth != 0)
			{
				foregroundB = DepthAt(b) < backgroundDepth - kDepthSeparation;
			}
			else
			{
				float2 fieldB = MotionAt(b);
				float3 cb = SampleB(b);
				bool labelled = length(fieldB - foreground) < length(fieldB - background);
				bool follows = ColourDistance(cb, SampleA(b + foreground)) < kCoverTolerance &&
							   ColourDistance(cb, SampleA(b + background)) > 2.0 * kCoverTolerance;
				foregroundB = labelled || follows;
			}
			float2 origin = a - foreground;
			float2 fieldOrigin = MotionAt(origin);
			bool foregroundA = length(fieldOrigin - foreground) < length(fieldOrigin - background) &&
							   ColourDistance(SampleA(a), SampleB(origin)) < kCoverTolerance;
			coverA = coverA || foregroundA;
			coverB = coverB || foregroundB;
			foregroundError = PatchError(q, foreground);
		}
		float direct = PatchError(q, background);
		float occlusion = coverA != coverB ? smoothstep(kLayerOnset, kLayerFull, min(direct, foregroundError)) : 0.0;
		[branch] if(occlusion > 0.0)
		{
			float3 visible = coverA ? SharpSample(FrameB, b) : SharpSample(FrameA, a);
			result = lerp(result, visible, occlusion);
		}
	}

	Output[pixel] = Encoded(result);
}
)HLSL";

}
