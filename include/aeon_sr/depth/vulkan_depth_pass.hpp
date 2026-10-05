#pragma once

#include <cstdint>

namespace aeon_sr {

inline constexpr const char *kVulkanDepthPassEntry = "CSDepthToPlane";
inline constexpr uint32_t kVulkanDepthPassGroup = 8;

inline constexpr const char *kVulkanDepthPassFx = R"(
texture2D AeonDepthSourceTex;
sampler2D AeonDepthSource
{
	Texture = AeonDepthSourceTex;
	MagFilter = POINT;
	MinFilter = POINT;
	MipFilter = POINT;
	AddressU = CLAMP;
	AddressV = CLAMP;
};

texture2D AeonDepthPlaneTex { Format = R32F; };
storage2D AeonDepthPlane { Texture = AeonDepthPlaneTex; };

void CSDepthToPlane(uint3 id : SV_DispatchThreadID)
{
	int2 size = tex2Dsize(AeonDepthSource, 0);
	if (id.x >= uint(size.x) || id.y >= uint(size.y))
		return;
	float depth = tex2Dfetch(AeonDepthSource, int2(id.xy)).x;
	tex2Dstore(AeonDepthPlane, int2(id.xy), float4(depth, 0.0, 0.0, 0.0));
}

technique AeonDepthToPlane
{
	pass
	{
		ComputeShader = CSDepthToPlane<8, 8>;
		DispatchSizeX = 1;
		DispatchSizeY = 1;
	}
}
)";

}
