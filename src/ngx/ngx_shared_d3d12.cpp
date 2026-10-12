#include "aeon_sr/ngx/ngx_shared_d3d12.hpp"
#include "aeon_sr/ngx/ngx_common.hpp"
#include "aeon_sr/ngx/ngx_session_ngx.hpp"

#include <mutex>
#include <vector>

namespace aeon_sr {
namespace {

struct SharedNgx12 {
	ID3D12Device *device = nullptr;
	const Ngx12Api *api = nullptr;
	int count = 0;
};
std::mutex g_shared_mutex;
std::vector<SharedNgx12> g_shared;

}

NVSDK_NGX_Result ngx12_acquire(const Ngx12Api &api, ID3D12Device *device, const wchar_t *data_dir,
	const NVSDK_NGX_FeatureCommonInfo *info, const wchar_t *tag)
{
	std::lock_guard lock(g_shared_mutex);
	for (SharedNgx12 &s : g_shared) {
		if (s.device == device && s.api == &api) {
			++s.count;
			wchar_t buf[96]{};
			_snwprintf_s(buf, _TRUNCATE, L"%sNGX D3D12 Init shared (%d sessions on this device)", tag, s.count);
			diag_info("dlss", buf);
			return NVSDK_NGX_Result_Success;
		}
	}
	note_own_ngx_start();
	NVSDK_NGX_Result r = api.init(ngx_dlss::kAppId, data_dir, device, info);
	diag_info("dlss", std::wstring(tag) + ngx_format_result(L"Init(AppId)", r));
	if (NVSDK_NGX_FAILED(r)) {
		r = api.init_project(ngx_dlss::kProjectId, NVSDK_NGX_ENGINE_TYPE_CUSTOM, ngx_dlss::kEngineVersion,
			data_dir, device, info);
		diag_info("dlss", std::wstring(tag) + ngx_format_result(L"Init_with_ProjectID", r));
	}
	if (!NVSDK_NGX_FAILED(r))
		g_shared.push_back(SharedNgx12{ device, &api, 1 });
	return r;
}

void ngx12_release(const Ngx12Api &api, ID3D12Device *device, const wchar_t *tag)
{
	std::lock_guard lock(g_shared_mutex);
	for (size_t i = 0; i < g_shared.size(); ++i) {
		if (g_shared[i].device != device || g_shared[i].api != &api)
			continue;
		if (--g_shared[i].count > 0) {
			wchar_t buf[96]{};
			_snwprintf_s(buf, _TRUNCATE, L"%sNGX D3D12 instance kept (%d sessions remain)", tag, g_shared[i].count);
			diag_info("dlss", buf);
			return;
		}
		g_shared.erase(g_shared.begin() + static_cast<ptrdiff_t>(i));
		break;
	}
	api.shutdown(device);
}

}
