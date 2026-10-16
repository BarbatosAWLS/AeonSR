#include "aeon_sr/framegen/frame_presenter.hpp"
#include "aeon_sr/framegen/present_sync.hpp"

#include "aeon_sr/core/frame_inputs.hpp"

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <d3d9.h>
#include <dxgi.h>

#include <cstdio>
#include <cstring>
#include <type_traits>

namespace aeon_sr {

using namespace reshade::api;

namespace {

bool is_dxgi(device_api a) noexcept
{
	return a == device_api::d3d10 || a == device_api::d3d11 || a == device_api::d3d12;
}

std::string module_of(const void *p)
{
	HMODULE m = nullptr;
	if (p == nullptr || !GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
			GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, static_cast<LPCSTR>(p), &m))
		return "?";
	char path[MAX_PATH] = {};
	GetModuleFileNameA(m, path, MAX_PATH);
	const char *slash = std::strrchr(path, '\\');
	return slash != nullptr ? slash + 1 : path;
}

template <typename H>
H vk_handle(uint64_t value) noexcept
{
	if constexpr (std::is_pointer_v<H>)
		return reinterpret_cast<H>(static_cast<uintptr_t>(value));
	else
		return static_cast<H>(value);
}

template <typename H>
uint64_t vk_value(H handle) noexcept
{
	if constexpr (std::is_pointer_v<H>)
		return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(handle));
	else
		return static_cast<uint64_t>(handle);
}

std::wstring widen(const std::string &s)
{
	return std::wstring(s.begin(), s.end());
}

}

void FramePresenter::release() noexcept
{
	if (device_ != nullptr) {
		if (real_.handle != 0)
			device_->destroy_resource(real_);
		if (gen_.handle != 0)
			device_->destroy_resource(gen_);
		if (vk_.fence != 0 && vk_.destroy_fence != nullptr)
			reinterpret_cast<PFN_vkDestroyFence>(vk_.destroy_fence)(vk_handle<VkDevice>(vk_.device),
				vk_handle<VkFence>(vk_.fence), nullptr);
	}
	real_ = gen_ = { 0 };
	device_ = nullptr;
	w_ = h_ = 0;
	format_ = format::unknown;
	vk_ = Vk{};
	gl_resolved_ = false;
	gl_swap_ = nullptr;
	d3d9_break_scene_ = false;
	failures_ = 0;
}

void FramePresenter::forget() noexcept
{
	device_ = nullptr;
	release();
}

bool FramePresenter::ensure(effect_runtime *runtime, std::wstring *why)
{
	device *const dev = runtime != nullptr ? runtime->get_device() : nullptr;
	if (dev == nullptr)
		return false;
	const resource bb = current_back_buffer_of(runtime);
	if (bb.handle == 0) {
		if (why != nullptr)
			*why = L"frame generation: the swap chain has no back buffer";
		return false;
	}
	resource_desc desc = dev->get_resource_desc(bb);
	if (desc.texture.samples > 1) {
		if (why != nullptr)
			*why = L"frame generation: this game's back buffer is multisampled, which it cannot copy";
		return false;
	}
	if (ready() && device_ == dev && desc.texture.width == w_ && desc.texture.height == h_ &&
		desc.texture.format == format_)
		return true;
	if (device_ != dev)
		release();
	else {
		runtime->get_command_queue()->wait_idle();
		if (real_.handle != 0)
			dev->destroy_resource(real_);
		if (gen_.handle != 0)
			dev->destroy_resource(gen_);
		real_ = gen_ = { 0 };
	}
	device_ = dev;
	w_ = desc.texture.width;
	h_ = desc.texture.height;
	format_ = desc.texture.format;
	desc.type = resource_type::texture_2d;
	desc.texture.depth_or_layers = 1;
	desc.texture.levels = 1;
	desc.texture.samples = 1;
	desc.heap = memory_heap::default_;
	desc.usage = resource_usage::copy_source | resource_usage::copy_dest | resource_usage::render_target |
		resource_usage::shader_resource;
	desc.flags = resource_flags::none;
	if (!dev->create_resource(desc, nullptr, resource_usage::render_target, &real_) ||
		!dev->create_resource(desc, nullptr, resource_usage::render_target, &gen_)) {
		if (real_.handle != 0)
			dev->destroy_resource(real_);
		real_ = gen_ = { 0 };
		if (why != nullptr)
			*why = L"frame generation: the game's device refused a texture the size of its back buffer";
		return false;
	}
	return true;
}

void FramePresenter::copy_back_buffer(effect_runtime *runtime)
{
	command_list *const cmd = runtime->get_command_queue()->get_immediate_command_list();
	const resource bb = current_back_buffer_of(runtime);
	if (bb.handle == 0)
		return;
	cmd->barrier(bb, resource_usage::present, resource_usage::copy_source);
	cmd->barrier(real_, resource_usage::render_target, resource_usage::copy_dest);
	cmd->copy_texture_region(bb, 0, nullptr, real_, 0, nullptr);
	cmd->barrier(real_, resource_usage::copy_dest, resource_usage::render_target);
	cmd->barrier(bb, resource_usage::copy_source, resource_usage::present);
}

bool FramePresenter::present_generated(effect_runtime *runtime, std::wstring *why)
{
	if (!ready())
		return false;
	const bool ok = runtime->get_device()->get_api() == device_api::vulkan ? present_vulkan(runtime, why)
		: present_native(runtime, why);
	failures_ = ok ? 0u : failures_ + 1u;
	return ok;
}

bool FramePresenter::resolve_gl(std::wstring *why)
{
	gl_resolved_ = true;
	wchar_t sys[MAX_PATH] = {};
	GetSystemDirectoryW(sys, MAX_PATH);
	const std::wstring path = std::wstring(sys) + L"\\opengl32.dll";
	const HMODULE system_gl = GetModuleHandleW(path.c_str());
	const HMODULE proxy = GetModuleHandleW(L"opengl32.dll");
	if (system_gl == nullptr || system_gl == proxy) {
		if (why != nullptr)
			*why = system_gl == nullptr ? L"frame generation: System32's opengl32.dll is not loaded"
				: L"frame generation: ReShade is not the opengl32.dll this game loaded";
		return false;
	}
	gl_swap_ = reinterpret_cast<void *>(GetProcAddress(system_gl, "wglSwapBuffers"));
	route_ = "System32 wglSwapBuffers (" + module_of(gl_swap_) + ")";
	return gl_swap_ != nullptr;
}

bool FramePresenter::present_native(effect_runtime *runtime, std::wstring *why)
{
	const device_api api = runtime->get_device()->get_api();
	command_queue *const q = runtime->get_command_queue();
	command_list *const cmd = q->get_immediate_command_list();
	if (api == device_api::opengl && !gl_resolved_ && !resolve_gl(why))
		return false;
	if (api == device_api::opengl && gl_swap_ == nullptr)
		return false;

	const resource bb = current_back_buffer_of(runtime);
	if (bb.handle == 0) {
		if (why != nullptr)
			*why = L"frame generation: the swap chain named no current back buffer";
		return false;
	}
	cmd->barrier(bb, resource_usage::present, resource_usage::copy_dest);
	cmd->barrier(gen_, resource_usage::render_target, resource_usage::copy_source);
	cmd->copy_texture_region(gen_, 0, nullptr, bb, 0, nullptr);
	cmd->barrier(gen_, resource_usage::copy_source, resource_usage::render_target);
	cmd->barrier(bb, resource_usage::copy_dest, resource_usage::present);
	q->flush_immediate_command_list();

	bool ok = false;
	if (is_dxgi(api)) {
		auto *const sc = reinterpret_cast<IDXGISwapChain *>(static_cast<uintptr_t>(runtime->get_native()));
		if (route_.empty())
			route_ = "IDXGISwapChain::Present on the native swap chain";
		const present_sync::OwnPresent own;
		const HRESULT hr = sc->Present(sync_interval_, 0);
		last_code_ = hr;
		ok = SUCCEEDED(hr);
	} else if (api == device_api::d3d9) {
		auto *const sc = reinterpret_cast<IDirect3DSwapChain9 *>(static_cast<uintptr_t>(runtime->get_native()));
		auto *const dev = reinterpret_cast<IDirect3DDevice9 *>(static_cast<uintptr_t>(runtime->get_device()->get_native()));
		HRESULT hr = D3DERR_INVALIDCALL;
		if (!d3d9_break_scene_) {
			hr = sc->Present(nullptr, nullptr, nullptr, nullptr, 0);
			if (FAILED(hr))
				d3d9_break_scene_ = true;
			else if (route_.empty())
				route_ = "IDirect3DSwapChain9::Present";
		}
		if (d3d9_break_scene_) {
			dev->EndScene();
			hr = sc->Present(nullptr, nullptr, nullptr, nullptr, 0);
			dev->BeginScene();
			route_ = "IDirect3DSwapChain9::Present outside ReShade's scene";
		}
		last_code_ = hr;
		ok = SUCCEEDED(hr);
	} else if (api == device_api::opengl) {
		using SwapBuffers = BOOL(WINAPI *)(HDC);
		const BOOL r = reinterpret_cast<SwapBuffers>(gl_swap_)(reinterpret_cast<HDC>(static_cast<uintptr_t>(runtime->get_native())));
		last_code_ = r != FALSE ? 0 : static_cast<int64_t>(GetLastError());
		ok = r != FALSE;
	} else {
		if (why != nullptr)
			*why = L"frame generation: no extra present for this API";
		return false;
	}
	if (!ok && why != nullptr) {
		wchar_t b[96];
		std::swprintf(b, 96, L"frame generation: the extra present failed (0x%08llX)",
			static_cast<unsigned long long>(last_code_) & 0xFFFFFFFFull);
		*why = b;
	}

	const resource now = current_back_buffer_of(runtime);
	if (now.handle == 0)
		return false;
	cmd->barrier(now, resource_usage::present, resource_usage::copy_dest);
	cmd->barrier(real_, resource_usage::render_target, resource_usage::copy_source);
	cmd->copy_texture_region(real_, 0, nullptr, now, 0, nullptr);
	cmd->barrier(real_, resource_usage::copy_source, resource_usage::render_target);
	cmd->barrier(now, resource_usage::copy_dest, resource_usage::present);
	return ok;
}

bool FramePresenter::resolve_vulkan(effect_runtime *runtime)
{
	vk_.tried = true;
	const auto dev = vk_handle<VkDevice>(runtime->get_device()->get_native());
	vk_.device = vk_value(dev);
	const HMODULE reshade = reshade::internal::get_reshade_module_handle();
	const auto rs_gdpa = reshade != nullptr
		? reinterpret_cast<PFN_vkGetDeviceProcAddr>(GetProcAddress(reshade, "vkGetDeviceProcAddr")) : nullptr;
	if (rs_gdpa == nullptr) {
		vk_.why = L"ReShade exports no vkGetDeviceProcAddr";
		return false;
	}
	const auto acquire = reinterpret_cast<PFN_vkAcquireNextImageKHR>(rs_gdpa(dev, "vkAcquireNextImageKHR"));
	const auto wait = reinterpret_cast<PFN_vkWaitForFences>(rs_gdpa(dev, "vkWaitForFences"));
	const auto reset = reinterpret_cast<PFN_vkResetFences>(rs_gdpa(dev, "vkResetFences"));
	const auto destroy_fence = reinterpret_cast<PFN_vkDestroyFence>(rs_gdpa(dev, "vkDestroyFence"));
	const auto create_fence = reinterpret_cast<PFN_vkCreateFence>(rs_gdpa(dev, "vkCreateFence"));
	if (acquire == nullptr || wait == nullptr || reset == nullptr || create_fence == nullptr || destroy_fence == nullptr) {
		vk_.why = L"the next Vulkan layer gives no vkAcquireNextImageKHR or fence functions";
		return false;
	}
	HMODULE icd = nullptr;
	GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		reinterpret_cast<LPCWSTR>(acquire), &icd);
	const auto icd_gipa = icd != nullptr
		? reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(icd, "vk_icdGetInstanceProcAddr")) : nullptr;
	if (icd_gipa == nullptr) {
		vk_.why = L"the Vulkan call below ReShade lives in " + widen(module_of(reinterpret_cast<const void *>(acquire))) +
			L", which is no driver: a layer sits between ReShade and the driver";
		return false;
	}
	auto icd_gdpa = reinterpret_cast<PFN_vkGetDeviceProcAddr>(icd_gipa(VK_NULL_HANDLE, "vkGetDeviceProcAddr"));
	if (icd_gdpa == nullptr) {
		const auto create = reinterpret_cast<PFN_vkCreateInstance>(icd_gipa(VK_NULL_HANDLE, "vkCreateInstance"));
		VkApplicationInfo ai{ VK_STRUCTURE_TYPE_APPLICATION_INFO };
		ai.apiVersion = VK_API_VERSION_1_1;
		VkInstanceCreateInfo ci{ VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
		ci.pApplicationInfo = &ai;
		VkInstance inst = VK_NULL_HANDLE;
		if (create != nullptr && create(&ci, nullptr, &inst) == VK_SUCCESS) {
			icd_gdpa = reinterpret_cast<PFN_vkGetDeviceProcAddr>(icd_gipa(inst, "vkGetDeviceProcAddr"));
			if (const auto destroy = reinterpret_cast<PFN_vkDestroyInstance>(icd_gipa(inst, "vkDestroyInstance")))
				destroy(inst, nullptr);
		}
	}
	if (icd_gdpa == nullptr) {
		vk_.why = L"the Vulkan driver gives no vkGetDeviceProcAddr";
		return false;
	}
	if (reinterpret_cast<void *>(icd_gdpa(dev, "vkAcquireNextImageKHR")) != reinterpret_cast<void *>(acquire)) {
		vk_.why = L"a Vulkan layer sits between ReShade and the driver; its present cannot be named";
		return false;
	}
	const auto present = reinterpret_cast<PFN_vkQueuePresentKHR>(icd_gdpa(dev, "vkQueuePresentKHR"));
	if (present == nullptr) {
		vk_.why = L"the Vulkan driver gives no vkQueuePresentKHR";
		return false;
	}
	VkFenceCreateInfo fi{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
	VkFence fence = VK_NULL_HANDLE;
	if (create_fence(dev, &fi, nullptr, &fence) != VK_SUCCESS) {
		vk_.why = L"vkCreateFence failed";
		return false;
	}
	vk_.fence = vk_value(fence);
	vk_.acquire = reinterpret_cast<void *>(acquire);
	vk_.present = reinterpret_cast<void *>(present);
	vk_.wait = reinterpret_cast<void *>(wait);
	vk_.reset = reinterpret_cast<void *>(reset);
	vk_.destroy_fence = reinterpret_cast<void *>(destroy_fence);
	vk_.ok = true;
	route_ = "one more swap chain image, vkQueuePresentKHR of the driver (" +
		module_of(reinterpret_cast<const void *>(present)) + ")";
	return true;
}

bool FramePresenter::present_vulkan(effect_runtime *runtime, std::wstring *why)
{
	if (!vk_.tried)
		(void)resolve_vulkan(runtime);
	if (!vk_.ok) {
		if (why != nullptr)
			*why = L"frame generation: " + vk_.why;
		return false;
	}
	command_queue *const q = runtime->get_command_queue();
	command_list *const cmd = q->get_immediate_command_list();
	const auto dev = vk_handle<VkDevice>(vk_.device);
	const auto fence = vk_handle<VkFence>(vk_.fence);
	const auto sc = vk_handle<VkSwapchainKHR>(runtime->get_native());
	const auto queue = vk_handle<VkQueue>(q->get_native());
	uint32_t j = 0;
	VkResult r = reinterpret_cast<PFN_vkAcquireNextImageKHR>(vk_.acquire)(dev, sc, 4000000ull, VK_NULL_HANDLE, fence,
		&j);
	last_code_ = r;
	if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) {
		if (why != nullptr)
			*why = r == VK_TIMEOUT || r == VK_NOT_READY ? L"frame generation: the swap chain had no image to spare"
				: L"frame generation: vkAcquireNextImageKHR failed";
		return false;
	}
	if (reinterpret_cast<PFN_vkWaitForFences>(vk_.wait)(dev, 1, &fence, VK_TRUE, 1000000000ull) != VK_SUCCESS) {
		vk_.ok = false;
		vk_.why = L"the extra swap chain image never became ready";
		if (why != nullptr)
			*why = L"frame generation: " + vk_.why;
		return false;
	}
	reinterpret_cast<PFN_vkResetFences>(vk_.reset)(dev, 1, &fence);
	const resource img = runtime->get_back_buffer(j);
	cmd->barrier(img, resource_usage::undefined, resource_usage::copy_dest);
	cmd->barrier(gen_, resource_usage::render_target, resource_usage::copy_source);
	cmd->copy_texture_region(gen_, 0, nullptr, img, 0, nullptr);
	cmd->barrier(gen_, resource_usage::copy_source, resource_usage::render_target);
	cmd->barrier(img, resource_usage::copy_dest, resource_usage::present);
	q->wait_idle();
	VkResult per = VK_SUCCESS;
	VkPresentInfoKHR pi{ VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
	pi.swapchainCount = 1;
	pi.pSwapchains = &sc;
	pi.pImageIndices = &j;
	pi.pResults = &per;
	r = reinterpret_cast<PFN_vkQueuePresentKHR>(vk_.present)(queue, &pi);
	last_code_ = r;
	const bool ok = r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR;
	if (!ok && why != nullptr)
		*why = L"frame generation: vkQueuePresentKHR of the extra image failed";
	return ok;
}

}
