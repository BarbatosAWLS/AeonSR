#include "aeon_sr/ngx/ngx_common.hpp"

#include "aeon_sr/core/diagnostics.hpp"
#include "aeon_sr/core/settings.hpp"

#include <Windows.h>
#include <ShlObj.h>

#include <cstdio>

namespace aeon_sr {

void NVSDK_CONV ngx_log_callback(const char *message, NVSDK_NGX_Logging_Level, NVSDK_NGX_Feature)
{
	if (message == nullptr)
		return;
	std::wstring wide;
	const int n = MultiByteToWideChar(CP_UTF8, 0, message, -1, nullptr, 0);
	if (n > 1) {
		wide.resize(static_cast<size_t>(n - 1));
		MultiByteToWideChar(CP_UTF8, 0, message, -1, wide.data(), n);
	}
	while (!wide.empty() && (wide.back() == L'\n' || wide.back() == L'\r'))
		wide.pop_back();
	diag_log(DiagLevel::Info, "NGX", wide);
}

const wchar_t *ngx_result_name(NVSDK_NGX_Result r)
{
	switch (r) {
	case NVSDK_NGX_Result_Success: return L"Success";
	case NVSDK_NGX_Result_FAIL_FeatureNotSupported: return L"FeatureNotSupported";
	case NVSDK_NGX_Result_FAIL_PlatformError: return L"PlatformError";
	case NVSDK_NGX_Result_FAIL_FeatureAlreadyExists: return L"FeatureAlreadyExists";
	case NVSDK_NGX_Result_FAIL_FeatureNotFound: return L"FeatureNotFound";
	case NVSDK_NGX_Result_FAIL_InvalidParameter: return L"InvalidParameter";
	case NVSDK_NGX_Result_FAIL_ScratchBufferTooSmall: return L"ScratchBufferTooSmall";
	case NVSDK_NGX_Result_FAIL_NotInitialized: return L"NotInitialized";
	case NVSDK_NGX_Result_FAIL_UnsupportedInputFormat: return L"UnsupportedInputFormat";
	case NVSDK_NGX_Result_FAIL_RWFlagMissing: return L"RWFlagMissing";
	case NVSDK_NGX_Result_FAIL_MissingInput: return L"MissingInput";
	case NVSDK_NGX_Result_FAIL_UnableToInitializeFeature: return L"UnableToInitializeFeature";
	case NVSDK_NGX_Result_FAIL_OutOfDate: return L"OutOfDate";
	case NVSDK_NGX_Result_FAIL_OutOfGPUMemory: return L"OutOfGPUMemory";
	case NVSDK_NGX_Result_FAIL_UnsupportedFormat: return L"UnsupportedFormat";
	case NVSDK_NGX_Result_FAIL_UnableToWriteToAppDataPath: return L"UnableToWriteToAppDataPath";
	case NVSDK_NGX_Result_FAIL_UnsupportedParameter: return L"UnsupportedParameter";
	case NVSDK_NGX_Result_FAIL_Denied: return L"Denied";
	case NVSDK_NGX_Result_FAIL_NotImplemented: return L"NotImplemented";
	default: return L"Unknown";
	}
}

std::wstring ngx_format_result(const wchar_t *what, NVSDK_NGX_Result r)
{
	wchar_t buf[256]{};
	_snwprintf_s(buf, _TRUNCATE, L"%s: %s (0x%08X)", what, ngx_result_name(r), static_cast<unsigned>(r));
	return buf;
}

bool file_exists_w(const std::wstring &path)
{
	const DWORD attrs = GetFileAttributesW(path.c_str());
	return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

std::wstring exe_directory_w()
{
	wchar_t path[MAX_PATH]{};
	const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
	if (n == 0 || n >= MAX_PATH)
		return {};
	std::wstring full(path, path + n);
	const auto slash = full.find_last_of(L"\\/");
	if (slash == std::wstring::npos)
		return {};
	return full.substr(0, slash);
}

std::wstring local_appdata_dir_w()
{
	wchar_t *path = nullptr;
	if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &path)) || path == nullptr)
		return {};
	std::wstring base(path);
	CoTaskMemFree(path);
	return join_path(base, L"AeonSR");
}

std::wstring ngx_dll_path(const std::wstring &dir)
{
	return join_path(dir, L"nvngx_dlss.dll");
}

const char kBlitHlsl[] = R"(
Texture2D srcTex : register(t0);
Texture2D origTex : register(t1);
Texture2D flowTex : register(t2);
Texture2D auxTex : register(t3);
SamplerState srcSamp : register(s0);
cbuffer BlitCB : register(b0) {
  float2 motionRange;
  float2 screenSize;
  float sharpness;
  float debugMode;
  float2 resample;
  float2 jitterUv;
  float2 depthRemap;
  float4 validUv;
};
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VSOut VSMain(uint id : SV_VertexID) {
  VSOut o;
  o.uv = float2((id << 1) & 2, id & 2);
  o.pos = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
  return o;
}
float3 PostRcas(float2 uv, float3 e) {
  float w = max(screenSize.x, 1.0);
  float h = max(screenSize.y, 1.0);
  float2 px = float2(1.0 / w, 1.0 / h);
  float3 b = srcTex.SampleLevel(srcSamp, uv + float2(0, -px.y), 0).rgb;
  float3 d = srcTex.SampleLevel(srcSamp, uv + float2(-px.x, 0), 0).rgb;
  float3 f = srcTex.SampleLevel(srcSamp, uv + float2( px.x, 0), 0).rgb;
  float3 g = srcTex.SampleLevel(srcSamp, uv + float2(0,  px.y), 0).rgb;

  float bL = b.b * 0.5 + (b.r * 0.5 + b.g);
  float dL = d.b * 0.5 + (d.r * 0.5 + d.g);
  float eL = e.b * 0.5 + (e.r * 0.5 + e.g);
  float fL = f.b * 0.5 + (f.r * 0.5 + f.g);
  float gL = g.b * 0.5 + (g.r * 0.5 + g.g);
  float spread = max(max(max(bL, dL), max(eL, fL)), gL) - min(min(min(bL, dL), min(eL, fL)), gL);
  float nz = saturate(abs(0.25 * (bL + dL + fL + gL) - eL) / max(spread, 1e-5));
  nz = 1.0 - 0.5 * nz;

  float3 mn4 = min(min(b, d), min(f, g));
  float3 mx4 = max(max(b, d), max(f, g));
  float3 hit_min = min(mn4, e) / max(4.0 * mx4, 1e-5);
  float3 hit_max = (1.0 - max(mx4, e)) / min(4.0 * mn4 - 4.0, -1e-5);
  float3 lobe3 = max(-hit_min, hit_max);
  const float kLimit = 0.25 - 1.0 / 16.0;
  float lobe = max(-kLimit, min(max(max(lobe3.r, lobe3.g), lobe3.b), 0.0));
  lobe *= saturate(sharpness * 1.25) * nz;
  return (lobe * (b + d + f + g) + e) / (4.0 * lobe + 1.0);
}
float4 SampleCatmullRom(float2 uv) {
  float2 size;
  srcTex.GetDimensions(size.x, size.y);
  float2 pos = uv * size;
  float2 p1 = floor(pos - 0.5) + 0.5;
  float2 f = pos - p1;
  float2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
  float2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
  float2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
  float2 w3 = f * f * (-0.5 + 0.5 * f);
  float2 w12 = w1 + w2;
  float2 t0 = (p1 - 1.0) / size;
  float2 t12 = (p1 + w2 / w12) / size;
  float2 t3 = (p1 + 2.0) / size;
  if (validUv.z > validUv.x) {
    t0 = clamp(t0, validUv.xy, validUv.zw);
    t12 = clamp(t12, validUv.xy, validUv.zw);
    t3 = clamp(t3, validUv.xy, validUv.zw);
  }
  float4 r = 0;
  r += srcTex.SampleLevel(srcSamp, float2(t0.x,  t0.y), 0)  * (w0.x  * w0.y);
  r += srcTex.SampleLevel(srcSamp, float2(t12.x, t0.y), 0)  * (w12.x * w0.y);
  r += srcTex.SampleLevel(srcSamp, float2(t3.x,  t0.y), 0)  * (w3.x  * w0.y);
  r += srcTex.SampleLevel(srcSamp, float2(t0.x,  t12.y), 0) * (w0.x  * w12.y);
  r += srcTex.SampleLevel(srcSamp, float2(t12.x, t12.y), 0) * (w12.x * w12.y);
  r += srcTex.SampleLevel(srcSamp, float2(t3.x,  t12.y), 0) * (w3.x  * w12.y);
  r += srcTex.SampleLevel(srcSamp, float2(t0.x,  t3.y), 0)  * (w0.x  * w3.y);
  r += srcTex.SampleLevel(srcSamp, float2(t12.x, t3.y), 0)  * (w12.x * w3.y);
  r += srcTex.SampleLevel(srcSamp, float2(t3.x,  t3.y), 0)  * (w3.x  * w3.y);
  return r;
}
float4 PSMain(VSOut i) : SV_Target {
  float2 uv = i.uv + jitterUv;
  if (validUv.z > validUv.x)
    uv = clamp(uv, validUv.xy, validUv.zw);
  float4 c = resample.x > 0.5 ? SampleCatmullRom(uv) : srcTex.SampleLevel(srcSamp, uv, 0);
  if (depthRemap.x > 0.0) {

    float d = 1.0 - depthRemap.x / max(c.r, depthRemap.x);
    return float4(d, d, d, 1);
  }
  if (sharpness > 1e-4)
    c.rgb = PostRcas(uv, c.rgb);
  return c;
}

float4 PSDebug(VSOut i) : SV_Target {
  float4 c = srcTex.SampleLevel(srcSamp, i.uv, 0);
  if (debugMode > 4.5) {

    float m = saturate(1.0 - saturate(c.r)) * saturate(max(sharpness, 0.0));
    return float4(m, m, m, 1);
  }
  if (debugMode > 2.5) {

    float w = max(screenSize.x, 1.0);
    float h = max(screenSize.y, 1.0);
    float2 px = float2(1.0 / w, 1.0 / h);
    float3 n = normalize(c.xyz + 1e-6);
    float spread = 0.0;
    spread = max(spread, 1.0 - dot(n, normalize(srcTex.SampleLevel(srcSamp, i.uv + float2( px.x, 0), 0).xyz + 1e-6)));
    spread = max(spread, 1.0 - dot(n, normalize(srcTex.SampleLevel(srcSamp, i.uv + float2(-px.x, 0), 0).xyz + 1e-6)));
    spread = max(spread, 1.0 - dot(n, normalize(srcTex.SampleLevel(srcSamp, i.uv + float2(0,  px.y), 0).xyz + 1e-6)));
    spread = max(spread, 1.0 - dot(n, normalize(srcTex.SampleLevel(srcSamp, i.uv + float2(0, -px.y), 0).xyz + 1e-6)));

    float valid = saturate(length(c.xyz) * 4.0);
    float m = saturate(spread * max(sharpness, 0.0)) * valid;
    if (debugMode > 3.5)
      m = 1.0 - m;
    return float4(m, m, m, 1);
  }
  if (debugMode < 1.5) {
    float2 v = c.rg;
    return float4(0.5 + 0.5 * v.x, 0.5 + 0.5 * v.y, saturate(length(v) * 0.5), 1);
  }
  return float4(c.rrr, 1);
}

float3 DbgFlowColor(float2 motion) {
  float angle = atan2(-motion.y, -motion.x) / 6.2831853 + 0.5;
  float raw = length(motion) * max(screenSize.x, 1.0) / 15.0;
  float compressed = raw / (1.0 + raw * 1.4);
  float magnitude = saturate(lerp(compressed, sqrt(compressed), saturate(raw * 3.0)));
  float3 hsv = float3(angle, 1, magnitude);
  float4 K = float4(1, 2.0 / 3.0, 1.0 / 3.0, 3);
  float3 p = abs(frac(hsv.xxx + K.xyz) * 6 - K.www);
  return hsv.z * lerp(K.xxx, clamp(p - K.xxx, 0, 1), hsv.y) + 0.1;
}
float DbgSegment(float2 p, float2 a, float2 b) {
  float2 pa = p - a, ba = b - a;
  float h = saturate(dot(pa, ba) / max(dot(ba, ba), 1e-6));
  return length(pa - ba * h);
}
float DbgTriangle(float2 p, float2 p0, float2 p1, float2 p2) {
  float2 e0 = p1 - p0, e1 = p2 - p1, e2 = p0 - p2;
  float2 v0 = p - p0, v1 = p - p1, v2 = p - p2;
  float2 q0 = v0 - e0 * saturate(dot(v0, e0) / dot(e0, e0));
  float2 q1 = v1 - e1 * saturate(dot(v1, e1) / dot(e1, e1));
  float2 q2 = v2 - e2 * saturate(dot(v2, e2) / dot(e2, e2));
  float s = sign(e0.x * e2.y - e0.y * e2.x);
  float2 d0 = float2(dot(q0, q0), s * (v0.x * e0.y - v0.y * e0.x));
  float2 d1 = float2(dot(q1, q1), s * (v1.x * e1.y - v1.y * e1.x));
  float2 d2 = float2(dot(q2, q2), s * (v2.x * e2.y - v2.y * e2.x));
  float2 d = min(min(d0, d1), d2);
  return -sqrt(d.x) * sign(d.y);
}
float4 PSDebugView(VSOut i) : SV_Target {
  float2 size = max(screenSize, 1.0);
  if (debugMode < 1.5) {
    return float4(DbgFlowColor(srcTex.SampleLevel(srcSamp, i.uv, 0).xy), 1);
  }
  if (debugMode < 2.5) {
    float cell = max(motionRange.x, 8.0);
    float head_len = 6.0, head_w = 3.0, thick = 1.2, outline_w = 1.0, aa = 1.4;
    float max_len = max(cell - 6.0, 8.0);
    float3 base = origTex.SampleLevel(srcSamp, i.uv, 0).rgb * 0.7;
    float2 px = i.uv * size;
    float2 cell_px = (floor(px / cell) + 0.5) * cell;
    float2 m = srcTex.SampleLevel(srcSamp, cell_px / size, 0).xy;
    float2 mpx = m * size;
    float mag = length(mpx);
    if (mag < 0.15)
      return float4(base, 1);
    float len = clamp(mag * 3.0, 8.0, max_len);
    float2 dir = -mpx / max(mag, 1e-6);
    float2 perp = float2(-dir.y, dir.x);
    float2 a = cell_px;
    float2 b = cell_px + dir * max(len - head_len * 0.65, 0.0);
    float2 tip = cell_px + dir * len;
    float d = min(DbgSegment(px, a, b) - thick,
      DbgTriangle(px, tip, b + perp * head_w, b - perp * head_w));
    float outline = saturate(0.5 - (d - outline_w) / aa);
    float fill = saturate(0.5 - d / aa);
    float t = saturate(dot(px - cell_px, dir) / max(len, 1e-4));
    float3 arrow = DbgFlowColor(m) * lerp(0.8, 1.2, t);
    float3 r = lerp(base, float3(0.02, 0.02, 0.02), outline);
    return float4(lerp(r, arrow, fill), 1);
  }
  if (debugMode < 3.5) {
    float c = saturate(srcTex.SampleLevel(srcSamp, i.uv, 0).x);
    float3 col = c < 0.5 ? lerp(float3(1, 0, 0), float3(1, 1, 0), c * 2.0)
                         : lerp(float3(1, 1, 0), float3(0, 1, 0), (c - 0.5) * 2.0);
    return float4(col, 1);
  }
  float z = saturate(srcTex.SampleLevel(srcSamp, i.uv, 0).x);
  if (depthRemap.y > 0.5)
    z = 1.0 - z;
  const float far = max(depthRemap.x, 1.0);
  z = z / (far - z * (far - 1.0));
  return float4(pow(saturate(z), 0.25).xxx, 1);
}
)"
R"(
cbuffer NeuralCB : register(b1) {
  float nrDetail;
  float nrColour;
  float nrPaperWhite;
  float nrGuard;
  float2 nrModelSize;
  float2 nrFrameSize;
  float nrHdr;
  float nrDebug;

  float2 nrJitter;

  float nrProxyIsFrame;

  float nrCarry;
  float nrAlpha;
  float nrSigmaR;
  float nrScaleComp;
  float2 nrMvScale;
  float nrHistory;
  float nrSmoothW;
  float nrCarryReject;
  float nrWarpX;
  float nrWarpY;
  float nrGuideMotion;
  float nrInputDetail;
  float _nrPad2;
  float _nrPad3;
};
float NrLuma(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }
float3 NrSrgbEncode(float3 c) { return c <= 0.0031308 ? c * 12.92 : 1.055 * pow(c, 1.0/2.4) - 0.055; }
float3 NrSrgbDecode(float3 c) { return c <= 0.04045 ? c / 12.92 : pow((c + 0.055)/1.055, 2.4); }

static const float kNrPqM1 = 0.1593017578125;
static const float kNrPqM2 = 78.84375;
static const float kNrPqC1 = 0.8359375;
static const float kNrPqC2 = 18.8515625;
static const float kNrPqC3 = 18.6875;
float3 NrPqDecode(float3 e) {
  float3 p = pow(max(e, 0.0), 1.0 / kNrPqM2);
  return pow(max(p - kNrPqC1, 0.0) / max(kNrPqC2 - kNrPqC3 * p, 1e-6), 1.0 / kNrPqM1);
}
float3 NrPqEncode(float3 y) {
  float3 p = pow(max(y, 0.0), kNrPqM1);
  return pow((kNrPqC1 + kNrPqC2 * p) / (1.0 + kNrPqC3 * p), kNrPqM2);
}

float3 NrEncode(float3 c) {
  c = max(c, 0.0);
  if (nrHdr > 1.5) c = NrPqDecode(c) * (10000.0 / 80.0);
  c /= nrPaperWhite;
  if (nrHdr > 0.5) c = NrSrgbEncode(saturate(c));
  return saturate(c);
}
float3 NrDecode(float3 c) {
  c = saturate(c);
  if (nrHdr > 0.5) c = NrSrgbDecode(c);
  c *= nrPaperWhite;
  if (nrHdr > 1.5) c = NrPqEncode(c * (80.0 / 10000.0));
  return c;
}

bool NrWarped() { return nrWarpX > 0.0; }
float2 NrWarpToFrame(float2 um) {
  if (!NrWarped())
    return um;
  float2 a = float2(nrWarpX, nrWarpY);
  float2 s = um - 0.5;
  return 0.5 + a * s + 4.0 * (1.0 - a) * s * s * s;
}
float2 NrWarpSlope(float2 um) {
  if (!NrWarped())
    return 1.0;
  float2 a = float2(nrWarpX, nrWarpY);
  float2 s = um - 0.5;
  return a + 12.0 * (1.0 - a) * s * s;
}
float NrAsinh(float x) { return sign(x) * log(abs(x) + sqrt(x * x + 1.0)); }
float NrWarpAxisToModel(float t, float a) {
  float b = 4.0 * (1.0 - a);
  if (b < 1e-6)
    return t / a;
  float p = a / b;
  float arg = (1.5 * t / (b * p)) * sqrt(3.0 / p);
  float z = NrAsinh(arg) / 3.0;
  return 2.0 * sqrt(p / 3.0) * 0.5 * (exp(z) - exp(-z));
}
float2 NrWarpToModel(float2 uf) {
  if (!NrWarped())
    return uf;
  return 0.5 + float2(NrWarpAxisToModel(uf.x - 0.5, nrWarpX), NrWarpAxisToModel(uf.y - 0.5, nrWarpY));
}
)"
R"(
float4 NrProxyBox(float2 uv, float2 stride) {
  float2 o = 0.25 * stride;
  float4 c = srcTex.SampleLevel(srcSamp, uv + float2(-o.x, -o.y), 0);
  c += srcTex.SampleLevel(srcSamp, uv + float2( o.x, -o.y), 0);
  c += srcTex.SampleLevel(srcSamp, uv + float2(-o.x,  o.y), 0);
  c += srcTex.SampleLevel(srcSamp, uv + float2( o.x,  o.y), 0);
  return 0.25 * c;
}

float4 PSNeuralProxy(VSOut i) : SV_Target {

  float2 uv = NrWarpToFrame(i.uv) + nrJitter;
  if (all(nrModelSize == nrFrameSize)) {
    float4 s = srcTex.SampleLevel(srcSamp, uv, 0);
    return float4(NrEncode(s.rgb), s.a);
  }

  float2 stride = (nrFrameSize / max(nrModelSize, 1.0)) * NrWarpSlope(i.uv) / max(nrFrameSize, 1.0);
  float4 c = NrProxyBox(uv, stride);
  if (nrInputDetail > 0.0) {
    float k = nrInputDetail;
    float3 w = float3(-k, 1.0 + 2.0 * k, -k);
    c *= w.y * w.y;
    [unroll] for (int y = -1; y <= 1; ++y) {
      [unroll] for (int x = -1; x <= 1; ++x) {
        if (x != 0 || y != 0)
          c += NrProxyBox(uv + float2(x, y) * stride, stride) * (w[x + 1] * w[y + 1]);
      }
    }
  }
  return float4(NrEncode(c.rgb), saturate(c.a));
}

float4 PSNeuralGuide(VSOut i) : SV_Target {
  float2 uf = NrWarpToFrame(i.uv);
  float4 s = srcTex.SampleLevel(srcSamp, uf + nrJitter, 0);
  if (nrGuideMotion > 0.5 && NrWarped())
    s.xy = (NrWarpToModel(uf + s.xy * nrMvScale) - i.uv) / nrMvScale;
  return s;
}

float4 NrHistory(float2 uv) {
  float2 size = max(nrModelSize, 1.0);
  float2 pos = uv * size;
  float2 c1 = floor(pos - 0.5) + 0.5;
  float2 f = pos - c1;
  float2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
  float2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
  float2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
  float2 w3 = f * f * (-0.5 + 0.5 * f);
  float2 w12 = w1 + w2;
  float2 t0 = (c1 - 1.0) / size;
  float2 t3 = (c1 + 2.0) / size;
  float2 t12 = (c1 + w2 / max(w12, 1e-6)) / size;

  float3 xw = float3(w0.x, w12.x, w3.x);
  float3 yw = float3(w0.y, w12.y, w3.y);
  float3 xs = float3(t0.x, t12.x, t3.x);
  float3 ys = float3(t0.y, t12.y, t3.y);
  float3 acc = 0.0;
  [unroll] for (int r = 0; r < 3; ++r) {
    [unroll] for (int c = 0; c < 3; ++c)
      acc += origTex.SampleLevel(srcSamp, float2(xs[c], ys[r]), 0).rgb * (xw[c] * yw[r]);
  }

  float a = origTex.SampleLevel(srcSamp, uv, 0).a;
  return float4(acc, a);
}
)"
R"(
float NrProxyContrast(float2 uv) {
  float2 texel = 1.0 / max(nrModelSize, 1.0);
  float lo = 1e9, hi = -1e9;
  [unroll] for (int dy = -1; dy <= 1; ++dy) {
    [unroll] for (int dx = -1; dx <= 1; ++dx) {
      float l = NrLuma(flowTex.SampleLevel(srcSamp, uv + float2(dx, dy) * texel, 0).rgb);
      lo = min(lo, l);
      hi = max(hi, l);
    }
  }
  return hi - lo;
}
static const float kNrContrastFloor = 0.05;
static const float kNrGuardFloor = 0.0039;

static const float kNrGainMax = 4.0;
)"
R"(
float4 PSNeuralDelta(VSOut i) : SV_Target {
  float3 N = srcTex.SampleLevel(srcSamp, i.uv, 0).rgb;
  float3 P = flowTex.SampleLevel(srcSamp, i.uv, 0).rgb;
  float3 cur_delta = N - P;
  float4 cur = float4(cur_delta / (NrProxyContrast(i.uv) + kNrContrastFloor), NrLuma(P));

  if (nrHistory > 0.5) {
    float2 v = auxTex.SampleLevel(srcSamp, i.uv, 0).xy;
    float2 prev_uv = i.uv + v * nrMvScale;
    float2 v_there = auxTex.SampleLevel(srcSamp, prev_uv, 0).xy;
    float2 texels = nrMvScale * max(nrModelSize, 1.0);
    float moved = length(v * texels);
    float mismatch = length((v - v_there) * texels);
    bool tracked = mismatch < nrCarryReject * (moved + 1.0);
    if (tracked && all(prev_uv >= 0.0) && all(prev_uv <= 1.0)) {
      float4 prev = NrHistory(prev_uv);
      float ref = prev.a;

      float lum = NrLuma(P);
      float disagree = abs(ref - lum) / (max(ref, lum) + 0.05);
      float trust = saturate(1.0 - disagree / 0.15);

      float keep = lerp(0.75, 1.0, trust);
      float3 carried = clamp(prev.rgb, -kNrGainMax, kNrGainMax);
      cur.rgb = lerp(carried * keep, cur.rgb, nrAlpha);
    }
  }
  return cur;
}
)"
R"(
)"
R"(
float3 NrDebugSigned(float3 delta) {
  return saturate(0.5 + 0.5 * tanh(16.0 * delta));
}

static const float kNrDeltaMax = 0.5;

float4 PSNeuralRebuild(VSOut i) : SV_Target {
  float4 state = srcTex.SampleLevel(srcSamp, i.uv, 0);
  float3 d = state.rgb * (NrProxyContrast(i.uv) + kNrContrastFloor);
  float guide = state.a;
  if (!all(isfinite(d)))
    return float4(0.0, 0.0, 0.0, guide);
  return float4(clamp(d, -kNrDeltaMax, kNrDeltaMax), guide);
}

static const float kNrSmoothDead = 0.0039216;

float4 PSNeuralSmooth(VSOut i) : SV_Target {
  float4 D = srcTex.SampleLevel(srcSamp, i.uv, 0);
  if (nrHistory < 0.5)
    return D;
  float2 v = auxTex.SampleLevel(srcSamp, i.uv, 0).xy;
  float2 prev_uv = i.uv + v * nrMvScale;
  if (any(prev_uv < 0.0) || any(prev_uv > 1.0))
    return D;
  float4 S = NrHistory(prev_uv);
  if (!all(isfinite(S.rgb)))
    return D;

  float3 d = D.rgb - S.rgb;
  float w = nrSmoothW;
  float unit = kNrSmoothDead / max(nrDetail * nrScaleComp, 1e-3);
  float3 next = S.rgb + d * w;
  return float4(round(next / unit) * unit, D.a);
}
)"
R"(
static const float kNrRangeRescue = 20.0;

float3 NrDeltaUpsample(float2 uv, float guide) {
  if (all(nrModelSize == nrFrameSize))
    return srcTex.SampleLevel(srcSamp, uv, 0).rgb;

  float2 mpix = 1.0 / max(nrModelSize, 1.0);
  float2 centre = NrWarpToModel(uv) * nrModelSize - 0.5;
  float2 base = floor(centre);
  float2 frac = centre - base;
  float3 acc = 0.0, tent_acc = 0.0;
  float wsum = 0.0;
  float inv2r2 = 0.5 / max(nrSigmaR * nrSigmaR, 1e-6);
  [unroll] for (int dy = 0; dy <= 1; ++dy) {
    [unroll] for (int dx = 0; dx <= 1; ++dx) {
      float2 q = base + float2(dx, dy);
      float2 t = float2(dx == 0 ? 1.0 - frac.x : frac.x,
                        dy == 0 ? 1.0 - frac.y : frac.y);
      float4 s = srcTex.SampleLevel(srcSamp, (q + 0.5) * mpix, 0);
      float dg = guide - s.a;
      float tw = t.x * t.y;
      float w = tw * exp(-dg * dg * inv2r2);
      acc += s.rgb * w;
      tent_acc += s.rgb * tw;
      wsum += w;
    }
  }
  return lerp(tent_acc, acc / max(wsum, 1e-6), saturate(wsum * kNrRangeRescue));
}
bool NrDeltaIsExact() { return all(nrModelSize == nrFrameSize) && !NrWarped(); }

void NrCarryEnding(float3 Op, float3 delta, out float3 base, out float3 result) {
  const float eps = 1e-6;
  float3 lin = Op + delta;
  float3 headroom = max(nrGuard, 1.0) * max(Op, kNrGuardFloor) - Op;
  float3 kc = float3(1e6, 1e6, 1e6);
  kc = delta > eps ? max(headroom, 0.0) / max(delta, eps) : kc;
  kc = delta < -eps ? max(Op, 0.0) / max(-delta, eps) : kc;
  float k = saturate(min(kc.r, min(kc.g, kc.b)));
  if (!NrDeltaIsExact() && k < 1.0)
    lin = Op + delta * k;
  lin = max(lin, 0.0);
  lin /= max(max(lin.r, max(lin.g, lin.b)), 1.0);
  base = NrDecode(Op);
  result = NrDecode(lin);
}

static const float kNrDetailExtraMax = 2.0;

float4 PSNeuralComposite(VSOut i) : SV_Target {
  if (nrCarry > 0.5) {
    const float eps = 1e-6;
    float4 orig = origTex.SampleLevel(srcSamp, i.uv, 0);
    float3 frame = orig.rgb;
    float3 Op = NrEncode(frame);
    float3 delta = NrDeltaUpsample(i.uv, NrLuma(Op));
    if (nrDebug > 2.5) return float4(NrDebugSigned(delta), orig.a);
    if (nrDebug > 1.5) return float4(NrDecode(saturate(Op + delta)), orig.a);
    if (nrDebug > 0.5) return float4(NrDecode(Op), orig.a);
    delta *= nrScaleComp;
    float3 base, result;
    NrCarryEnding(Op, delta * min(nrDetail, 1.0), base, result);
    float s = (NrLuma(result) + eps) / (NrLuma(base) + eps);
    float3 g = base > 0.0 ? (result + eps) / (base + eps) : s;
    float extra = 1.0;
    if (nrDetail > 1.0) {
      float3 baseX, resultX;
      NrCarryEnding(Op, delta * nrDetail, baseX, resultX);
      extra = clamp((NrLuma(resultX) + eps) / (NrLuma(result) + eps), 1.0 / kNrDetailExtraMax, kNrDetailExtraMax);
    }
    return float4(frame * lerp(s, g, saturate(nrColour)) * extra, orig.a);
  }

  const float eps = 1e-6;
  float4 orig = origTex.SampleLevel(srcSamp, i.uv, 0);
  float3 frame = orig.rgb;
  float3 N = srcTex.SampleLevel(srcSamp, i.uv, 0).rgb;
  float3 Op = NrEncode(frame);
  float3 P = nrProxyIsFrame > 0.5 ? Op : flowTex.SampleLevel(srcSamp, i.uv, 0).rgb;

  if (nrDebug > 2.5) return float4(NrDebugSigned(N - P), orig.a);
  if (nrDebug > 1.5) return float4(NrDecode(N), orig.a);
  if (nrDebug > 0.5) return float4(NrDecode(P), orig.a);

  float yO = NrLuma(Op);
  float yN = NrLuma(N);
  float yP = NrLuma(P);

  float ratio = (yN + eps) / (yP + eps);
  float gain = 1.0 + nrDetail * (ratio - 1.0);

  if (!NrDeltaIsExact())
    gain = min(gain, max(nrGuard, 1.0));
  gain = max(gain, 0.0);

  float3 lin = Op * gain;
  float mix = saturate(nrColour) * saturate(nrDetail);
  if (mix > 0.0) {

    float3 hueN = N / max(yN, eps);
    lin = lerp(lin, hueN * (yO * gain), mix);
  }

  lin /= max(max(lin.r, max(lin.g, lin.b)), 1.0);

  float3 base = NrDecode(Op);
  float3 result = NrDecode(lin);

  float s = (NrLuma(result) + eps) / (NrLuma(base) + eps);
  float3 g = base > 0.0 ? (result + eps) / (base + eps) : s;
  return float4(frame * lerp(s, g, mix), orig.a);
}
)";

}
