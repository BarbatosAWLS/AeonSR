#pragma once

#include <d3d12.h>

#include "xess.h"

#ifdef __cplusplus
extern "C" {
#endif

XESS_PACK_B()
typedef struct _xess_d3d12_execute_params_t {
	ID3D12Resource *pColorTexture;
	ID3D12Resource *pVelocityTexture;
	ID3D12Resource *pDepthTexture;
	ID3D12Resource *pExposureScaleTexture;
	ID3D12Resource *pResponsivePixelMaskTexture;
	ID3D12Resource *pOutputTexture;
	float jitterOffsetX;
	float jitterOffsetY;
	float exposureScale;
	uint32_t resetHistory;
	uint32_t inputWidth;
	uint32_t inputHeight;
	xess_coord_t inputColorBase;
	xess_coord_t inputMotionVectorBase;
	xess_coord_t inputDepthBase;
	xess_coord_t inputResponsiveMaskBase;
	xess_coord_t reserved0;
	xess_coord_t outputColorBase;
	ID3D12DescriptorHeap *pDescriptorHeap;
	uint32_t descriptorHeapOffset;
} xess_d3d12_execute_params_t;
XESS_PACK_E()

XESS_PACK_B()
typedef struct _xess_d3d12_init_params_t {
	xess_2d_t outputResolution;
	xess_quality_settings_t qualitySetting;
	uint32_t initFlags;
	uint32_t creationNodeMask;
	uint32_t visibleNodeMask;
	ID3D12Heap *pTempBufferHeap;
	uint64_t bufferHeapOffset;
	ID3D12Heap *pTempTextureHeap;
	uint64_t textureHeapOffset;
	ID3D12PipelineLibrary *pPipelineLibrary;
} xess_d3d12_init_params_t;
XESS_PACK_E()

XESS_API xess_result_t xessD3D12CreateContext(ID3D12Device *pDevice, xess_context_handle_t *phContext);
XESS_API xess_result_t xessD3D12BuildPipelines(xess_context_handle_t hContext,
	ID3D12PipelineLibrary *pPipelineLibrary, bool blocking, uint32_t initFlags);
XESS_API xess_result_t xessD3D12Init(xess_context_handle_t hContext, const xess_d3d12_init_params_t *pInitParams);
XESS_API xess_result_t xessD3D12Execute(xess_context_handle_t hContext,
	ID3D12GraphicsCommandList *pCommandList, const xess_d3d12_execute_params_t *pExecParams);

#ifdef __cplusplus
}
#endif
