#include "aeon_sr/interop/interop_bridges.hpp"
#include "aeon_sr/core/retry_backoff.hpp"
#include "aeon_sr/jitter/spirv_compile.hpp"
#include "aeon_sr/depth/vulkan_depth_pass.hpp"

#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>

#include <atomic>
#include <cstring>
#include <string>
#include <type_traits>

namespace aeon_sr {
namespace {

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

std::wstring to_wide(const char *s)
{
	std::wstring w;
	for (const char *p = s; p != nullptr && *p != '\0'; ++p)
		w.push_back(static_cast<wchar_t>(static_cast<unsigned char>(*p)));
	return w;
}

std::wstring one_line(std::wstring text)
{
	for (wchar_t &c : text)
		if (c == L'\n' || c == L'\r' || c == L'\t')
			c = L' ';
	while (!text.empty() && (text.back() == L' ' || text.back() == L'.'))
		text.pop_back();
	return text;
}

std::wstring vk_text(VkResult r)
{
	switch (r) {
	case VK_SUCCESS: return L"VK_SUCCESS";
	case VK_ERROR_OUT_OF_HOST_MEMORY: return L"VK_ERROR_OUT_OF_HOST_MEMORY";
	case VK_ERROR_OUT_OF_DEVICE_MEMORY: return L"VK_ERROR_OUT_OF_DEVICE_MEMORY";
	case VK_ERROR_INITIALIZATION_FAILED: return L"VK_ERROR_INITIALIZATION_FAILED";
	case VK_ERROR_DEVICE_LOST: return L"VK_ERROR_DEVICE_LOST";
	case VK_ERROR_FEATURE_NOT_PRESENT: return L"VK_ERROR_FEATURE_NOT_PRESENT";
	case VK_ERROR_EXTENSION_NOT_PRESENT: return L"VK_ERROR_EXTENSION_NOT_PRESENT";
	case VK_ERROR_FORMAT_NOT_SUPPORTED: return L"VK_ERROR_FORMAT_NOT_SUPPORTED";
	case VK_ERROR_INVALID_EXTERNAL_HANDLE: return L"VK_ERROR_INVALID_EXTERNAL_HANDLE";
	case VK_ERROR_UNKNOWN: return L"VK_ERROR_UNKNOWN";
	default: return L"VkResult " + std::to_wstring(static_cast<int>(r));
	}
}

struct PlaneFormat {
	reshade::api::format api;
	DXGI_FORMAT dxgi;
	VkFormat vk;
};

constexpr PlaneFormat kPlaneFormats[] = {
	{ reshade::api::format::b8g8r8a8_unorm,      DXGI_FORMAT_B8G8R8A8_UNORM,      VK_FORMAT_B8G8R8A8_UNORM },
	{ reshade::api::format::b8g8r8a8_unorm_srgb, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, VK_FORMAT_B8G8R8A8_SRGB },
	{ reshade::api::format::r8g8b8a8_unorm,      DXGI_FORMAT_R8G8B8A8_UNORM,      VK_FORMAT_R8G8B8A8_UNORM },
	{ reshade::api::format::r8g8b8a8_unorm_srgb, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, VK_FORMAT_R8G8B8A8_SRGB },
	{ reshade::api::format::r10g10b10a2_unorm,   DXGI_FORMAT_R10G10B10A2_UNORM,   VK_FORMAT_A2B10G10R10_UNORM_PACK32 },
	{ reshade::api::format::r11g11b10_float,     DXGI_FORMAT_R11G11B10_FLOAT,     VK_FORMAT_B10G11R11_UFLOAT_PACK32 },
	{ reshade::api::format::r16g16b16a16_float,  DXGI_FORMAT_R16G16B16A16_FLOAT,  VK_FORMAT_R16G16B16A16_SFLOAT },
	{ reshade::api::format::r16g16b16a16_unorm,  DXGI_FORMAT_R16G16B16A16_UNORM,  VK_FORMAT_R16G16B16A16_UNORM },
	{ reshade::api::format::r32g32b32a32_float,  DXGI_FORMAT_R32G32B32A32_FLOAT,  VK_FORMAT_R32G32B32A32_SFLOAT },
	{ reshade::api::format::r16g16_float,        DXGI_FORMAT_R16G16_FLOAT,        VK_FORMAT_R16G16_SFLOAT },
	{ reshade::api::format::r16g16_unorm,        DXGI_FORMAT_R16G16_UNORM,        VK_FORMAT_R16G16_UNORM },
	{ reshade::api::format::r32g32_float,        DXGI_FORMAT_R32G32_FLOAT,        VK_FORMAT_R32G32_SFLOAT },
	{ reshade::api::format::r32_float,           DXGI_FORMAT_R32_FLOAT,           VK_FORMAT_R32_SFLOAT },
	{ reshade::api::format::r16_float,           DXGI_FORMAT_R16_FLOAT,           VK_FORMAT_R16_SFLOAT },
	{ reshade::api::format::r16_unorm,           DXGI_FORMAT_R16_UNORM,           VK_FORMAT_R16_UNORM },
	{ reshade::api::format::r8g8_unorm,          DXGI_FORMAT_R8G8_UNORM,          VK_FORMAT_R8G8_UNORM },
	{ reshade::api::format::r8_unorm,            DXGI_FORMAT_R8_UNORM,            VK_FORMAT_R8_UNORM },
	{ reshade::api::format::r8g8b8a8_typeless,   DXGI_FORMAT_R8G8B8A8_UNORM,      VK_FORMAT_R8G8B8A8_UNORM },
	{ reshade::api::format::b8g8r8a8_typeless,   DXGI_FORMAT_B8G8R8A8_UNORM,      VK_FORMAT_B8G8R8A8_UNORM },
	{ reshade::api::format::r10g10b10a2_typeless, DXGI_FORMAT_R10G10B10A2_UNORM,  VK_FORMAT_A2B10G10R10_UNORM_PACK32 },
};

const PlaneFormat *plane_format_of(reshade::api::format fmt) noexcept
{
	for (const PlaneFormat &f : kPlaneFormats) {
		if (f.api == fmt)
			return &f;
	}
	return nullptr;
}

std::atomic<const char *> g_depth_pass_source_override{ nullptr };
std::atomic<uint32_t> g_depth_pass_builds{ 0 };
std::atomic<VulkanProcResolver> g_proc_resolver_override{ nullptr };

constexpr PlaneFormat kDepthPassPlane = {
	reshade::api::format::r32_float, DXGI_FORMAT_R32_FLOAT, VK_FORMAT_R32_SFLOAT
};

struct DeviceProcAddr {
	PFN_vkGetDeviceProcAddr loader = nullptr;
	VulkanProcResolver test = nullptr;

	static DeviceProcAddr get(HMODULE module) noexcept
	{
		DeviceProcAddr p;
		p.test = g_proc_resolver_override.load(std::memory_order_relaxed);
		if (module != nullptr)
			p.loader = reinterpret_cast<PFN_vkGetDeviceProcAddr>(GetProcAddress(module, "vkGetDeviceProcAddr"));
		return p;
	}

	explicit operator bool() const noexcept { return test != nullptr || loader != nullptr; }

	void *operator()(VkDevice device, const char *name) const
	{
		return test != nullptr ? test(device, name) : reinterpret_cast<void *>(loader(device, name));
	}
};

struct VulkanApi {
	PFN_vkDeviceWaitIdle DeviceWaitIdle = nullptr;
	PFN_vkCreateImage CreateImage = nullptr;
	PFN_vkDestroyImage DestroyImage = nullptr;
	PFN_vkGetImageMemoryRequirements GetImageMemoryRequirements = nullptr;
	PFN_vkAllocateMemory AllocateMemory = nullptr;
	PFN_vkFreeMemory FreeMemory = nullptr;
	PFN_vkBindImageMemory BindImageMemory = nullptr;
	PFN_vkCmdPipelineBarrier CmdPipelineBarrier = nullptr;
	PFN_vkCmdCopyImage CmdCopyImage = nullptr;
	PFN_vkGetMemoryWin32HandlePropertiesKHR GetMemoryWin32HandleProperties = nullptr;

	bool loaded() const noexcept { return CmdCopyImage != nullptr; }

	static HMODULE loader() noexcept { return GetModuleHandleW(L"vulkan-1.dll"); }

	const char *load(VkDevice device)
	{
		*this = VulkanApi{};
		const DeviceProcAddr get_proc = DeviceProcAddr::get(loader());
		if (!get_proc)
			return "vkGetDeviceProcAddr";

		const char *missing = nullptr;
		const auto need = [&](auto &fn, const char *name) {
			fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(get_proc(device, name));
			if (fn == nullptr && missing == nullptr)
				missing = name;
		};
		need(DeviceWaitIdle, "vkDeviceWaitIdle");
		need(CreateImage, "vkCreateImage");
		need(DestroyImage, "vkDestroyImage");
		need(GetImageMemoryRequirements, "vkGetImageMemoryRequirements");
		need(AllocateMemory, "vkAllocateMemory");
		need(FreeMemory, "vkFreeMemory");
		need(BindImageMemory, "vkBindImageMemory");
		need(CmdPipelineBarrier, "vkCmdPipelineBarrier");
		need(CmdCopyImage, "vkCmdCopyImage");
		need(GetMemoryWin32HandleProperties, "vkGetMemoryWin32HandlePropertiesKHR");
		if (missing != nullptr)
			*this = VulkanApi{};
		return missing;
	}
};

struct VulkanSyncApi {
	PFN_vkCreateSemaphore CreateSemaphore = nullptr;
	PFN_vkDestroySemaphore DestroySemaphore = nullptr;
	PFN_vkImportSemaphoreWin32HandleKHR ImportSemaphoreWin32Handle = nullptr;

	bool loaded() const noexcept { return ImportSemaphoreWin32Handle != nullptr; }

	bool load(VkDevice device)
	{
		*this = VulkanSyncApi{};
		const DeviceProcAddr get_proc = DeviceProcAddr::get(VulkanApi::loader());
		if (!get_proc)
			return false;
		CreateSemaphore = reinterpret_cast<PFN_vkCreateSemaphore>(get_proc(device, "vkCreateSemaphore"));
		DestroySemaphore = reinterpret_cast<PFN_vkDestroySemaphore>(get_proc(device, "vkDestroySemaphore"));
		ImportSemaphoreWin32Handle = reinterpret_cast<PFN_vkImportSemaphoreWin32HandleKHR>(
			get_proc(device, "vkImportSemaphoreWin32HandleKHR"));
		if (CreateSemaphore == nullptr || DestroySemaphore == nullptr || ImportSemaphoreWin32Handle == nullptr) {
			*this = VulkanSyncApi{};
			return false;
		}
		return true;
	}
};

struct SharedTimeline {
	ID3D12Fence *fence12 = nullptr;
	VkSemaphore sem = VK_NULL_HANDLE;
	uint64_t value = 0;

	reshade::api::fence api() const noexcept { return reshade::api::fence{ vk_value(sem) }; }
};

constexpr const char *kSyncGpu = "shared fence";
constexpr const char *kSyncNoExtension = "CPU wait: no VK_KHR_external_semaphore_win32";
constexpr const char *kSyncNoTimeline = "CPU wait: no timeline semaphores";
constexpr const char *kSyncImportRefused = "CPU wait: semaphore import refused";
constexpr const char *kSyncCheckFailed = "CPU wait: semaphore check failed";
constexpr const char *kSyncSubmitFailed = "CPU wait: semaphore submit failed";

struct VulkanComputeApi {
	PFN_vkCreateShaderModule CreateShaderModule = nullptr;
	PFN_vkDestroyShaderModule DestroyShaderModule = nullptr;
	PFN_vkCreateDescriptorSetLayout CreateDescriptorSetLayout = nullptr;
	PFN_vkDestroyDescriptorSetLayout DestroyDescriptorSetLayout = nullptr;
	PFN_vkCreatePipelineLayout CreatePipelineLayout = nullptr;
	PFN_vkDestroyPipelineLayout DestroyPipelineLayout = nullptr;
	PFN_vkCreateComputePipelines CreateComputePipelines = nullptr;
	PFN_vkDestroyPipeline DestroyPipeline = nullptr;
	PFN_vkCreateDescriptorPool CreateDescriptorPool = nullptr;
	PFN_vkDestroyDescriptorPool DestroyDescriptorPool = nullptr;
	PFN_vkAllocateDescriptorSets AllocateDescriptorSets = nullptr;
	PFN_vkCreateSampler CreateSampler = nullptr;
	PFN_vkDestroySampler DestroySampler = nullptr;
	PFN_vkCreateImageView CreateImageView = nullptr;
	PFN_vkDestroyImageView DestroyImageView = nullptr;

	bool loaded() const noexcept { return CreateComputePipelines != nullptr; }

	const char *load(VkDevice device)
	{
		*this = VulkanComputeApi{};
		const DeviceProcAddr get_proc = DeviceProcAddr::get(VulkanApi::loader());
		if (!get_proc)
			return "vkGetDeviceProcAddr";

		const char *missing = nullptr;
		const auto need = [&](auto &fn, const char *name) {
			fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(get_proc(device, name));
			if (fn == nullptr && missing == nullptr)
				missing = name;
		};
		need(CreateShaderModule, "vkCreateShaderModule");
		need(DestroyShaderModule, "vkDestroyShaderModule");
		need(CreateDescriptorSetLayout, "vkCreateDescriptorSetLayout");
		need(DestroyDescriptorSetLayout, "vkDestroyDescriptorSetLayout");
		need(CreatePipelineLayout, "vkCreatePipelineLayout");
		need(DestroyPipelineLayout, "vkDestroyPipelineLayout");
		need(CreateComputePipelines, "vkCreateComputePipelines");
		need(DestroyPipeline, "vkDestroyPipeline");
		need(CreateDescriptorPool, "vkCreateDescriptorPool");
		need(DestroyDescriptorPool, "vkDestroyDescriptorPool");
		need(AllocateDescriptorSets, "vkAllocateDescriptorSets");
		need(CreateSampler, "vkCreateSampler");
		need(DestroySampler, "vkDestroySampler");
		need(CreateImageView, "vkCreateImageView");
		need(DestroyImageView, "vkDestroyImageView");
		if (missing != nullptr)
			*this = VulkanComputeApi{};
		return missing;
	}
};

struct VulkanPlane {
	SharedPlane shared{};
	VkImage image = VK_NULL_HANDLE;
	VkDeviceMemory memory = VK_NULL_HANDLE;
	VkFormat vk_format = VK_FORMAT_UNDEFINED;
	bool storage = false;
	VkImageView view = VK_NULL_HANDLE;
	bool adopted = false;
};

struct ImportError {
	const char *stage = nullptr;
	VkResult result = VK_SUCCESS;

	bool ok() const noexcept { return stage == nullptr; }
	std::wstring text() const
	{
		if (stage == nullptr)
			return std::wstring();
		return result == VK_SUCCESS ? to_wide(stage) : to_wide(stage) + L" " + vk_text(result);
	}
};

class BridgeVulkan final : public FrameBridge {
public:
	~BridgeVulkan() override { shutdown(); }

	BridgeKind kind() const noexcept override { return BridgeKind::Vulkan; }
	const char *name() const noexcept override { return "Vulkan through VK_KHR_external_memory_win32"; }

	const char *sync_name() const noexcept override { return sync_; }

	bool init(reshade::api::device *game, EngineDevice &engine) override
	{
		if (game == nullptr || game->get_api() != reshade::api::device_api::vulkan) {
			last_error = L"not a Vulkan device";
			return false;
		}
		if (!engine.ready()) {
			last_error = engine.last_error.empty()
				? std::wstring(L"the engine device is not ready") : engine.last_error;
			return false;
		}

		device_vk_ = reinterpret_cast<VkDevice>(game->get_native());
		if (device_vk_ == VK_NULL_HANDLE) {
			last_error = L"the Vulkan device could not be read back from ReShade";
			return false;
		}

		uint64_t luid_bits = 0;
		if (!game->get_property(reshade::api::device_properties::adapter_luid, &luid_bits)) {
			last_error = L"this Vulkan driver does not report a device LUID, so there is no way "
				L"to tell whether the add-on's Direct3D 12 device ended up on the same GPU as "
				L"the game. Sharing a texture between two GPUs cannot be done, so the frame "
				L"has to travel through system memory instead.";
			shutdown();
			return false;
		}
		LUID vk_luid{};
		memcpy(&vk_luid, &luid_bits, sizeof(vk_luid));
		if (!engine.adapter_matched() || memcmp(&vk_luid, &engine.luid(), sizeof(LUID)) != 0) {
			last_error = L"the game's Vulkan device and the add-on's Direct3D 12 engine ended up "
				L"on different GPUs, and a shared texture cannot cross two of them. Force the "
				L"game onto the same GPU in the graphics driver's control panel.";
			shutdown();
			return false;
		}

		if (VulkanApi::loader() == nullptr) {
			last_error = L"vulkan-1.dll is not loaded in this process, so none of the Vulkan "
				L"entry points the bridge needs could be resolved. The file the game is "
				L"running against is very likely not the system loader.";
			shutdown();
			return false;
		}
		if (const char *const missing = vk_.load(device_vk_)) {
			last_error = L"this Vulkan driver does not provide " + to_wide(missing) +
				L". The add-on shares the frame through VK_KHR_external_memory_win32, which "
				L"ReShade turns on for every Vulkan game it runs in, so a driver without it is "
				L"either very old or a software one. Until it is updated the frame has to "
				L"travel through system memory.";
			shutdown();
			return false;
		}

		engine_ = &engine;
		device_ = game;

		const ImportError probe = self_test();
		if (!probe.ok()) {
			last_error = L"this Vulkan driver refused to import a Direct3D 12 texture (" +
				probe.text() + L"). The add-on shares the frame that way on every graphics "
				L"API but Direct3D 12 itself, so on this driver the frame has to travel "
				L"through system memory instead.";
			shutdown();
			return false;
		}
		setup_gpu_sync();
		return true;
	}

	void shutdown() override
	{
		if (engine_ != nullptr)
			engine_->wait_cpu(engine_->last_submitted());
		if (device_destroying)
			device_vk_ = VK_NULL_HANDLE;
		if (device_vk_ != VK_NULL_HANDLE && vk_.DeviceWaitIdle != nullptr)
			vk_.DeviceWaitIdle(device_vk_);

		destroy_depth_pass();
		release_plane(color_);
		release_plane(depth_);
		release_plane(motion_);
		release_plane(scene_);
		release_timeline(to_engine_);
		release_timeline(to_game_);
		gpu_sync_ = false;
		sync_ = kSyncNoExtension;
		sync_queue_ = nullptr;
		refused_waits_ = 0;

		vk_ = VulkanApi{};
		vkc_ = VulkanComputeApi{};
		vks_ = VulkanSyncApi{};
		device_vk_ = VK_NULL_HANDLE;
		device_ = nullptr;
		engine_ = nullptr;
		queue_ = nullptr;
	}

	bool ready() const noexcept override
	{
		return engine_ != nullptr && device_ != nullptr && device_vk_ != VK_NULL_HANDLE && vk_.loaded();
	}

	BridgeStep begin(reshade::api::effect_runtime *runtime,
		reshade::api::command_list *cmd_list, const BridgeInputs &in, BridgeFrame &out) override
	{
		out = BridgeFrame{};
		if (!ready())
			return BridgeStep::Init;

		queue_ = runtime != nullptr ? runtime->get_command_queue() : nullptr;
		if (queue_ == nullptr) {
			last_error = L"ReShade did not name a command queue this frame.";
			return BridgeStep::Import;
		}
		if (command_buffer(cmd_list) == VK_NULL_HANDLE) {
			last_error = L"ReShade did not name a command buffer this frame.";
			return BridgeStep::Begin;
		}
		if (in.color.handle == 0) {
			last_error = L"ReShade did not name a colour buffer this frame.";
			return BridgeStep::Import;
		}

		const reshade::api::resource_desc cdesc = device_->get_resource_desc(in.color);
		const PlaneFormat *const cfmt = carriable(cdesc, Carried::Colour);
		if (cfmt == nullptr)
			return BridgeStep::Import;
		if (!ensure_plane(color_, cdesc.texture.width, cdesc.texture.height, *cfmt, true)) {
			last_error += L" Without the colour plane there is no frame to share.";
			return BridgeStep::Import;
		}
		if (!copy_in(cmd_list, in.color, color_, cdesc.texture.width, cdesc.texture.height,
				reshade::api::resource_usage::render_target)) {
			last_error = L"ReShade did not name a command buffer this frame.";
			return BridgeStep::Import;
		}

		bool have_depth = false;
		depth_note.clear();
		if (in.depth.handle != 0) {
			const reshade::api::resource_desc ddesc = device_->get_resource_desc(in.depth);
			const PlaneFormat *const dfmt = carriable(ddesc, Carried::Depth);
			if (dfmt != nullptr) {
				if (ensure_plane(depth_, ddesc.texture.width, ddesc.texture.height, *dfmt, false))
					have_depth = copy_in(cmd_list, in.depth, depth_,
						ddesc.texture.width, ddesc.texture.height, in.depth_state);
			} else {
				const std::wstring why_no_copy = last_error;
				last_error.clear();
				have_depth = read_depth(cmd_list, in, ddesc, why_no_copy);
			}
			if (!have_depth && depth_note.empty())
				depth_note = L"the shared plane for the game's depth buffer could not be "
					L"allocated on the engine device this frame.";
		}

		bool have_motion = false;
		if (in.motion_vectors.handle != 0) {
			const reshade::api::resource_desc mdesc = device_->get_resource_desc(in.motion_vectors);
			const PlaneFormat *const mfmt = carriable(mdesc, Carried::Motion);
			if (mfmt != nullptr &&
				ensure_plane(motion_, mdesc.texture.width, mdesc.texture.height, *mfmt, false)) {
				have_motion = copy_in(cmd_list, in.motion_vectors, motion_,
					mdesc.texture.width, mdesc.texture.height,
					reshade::api::resource_usage::shader_resource);
			}
		}

		bool have_scene = false;
		if (in.scene.handle != 0) {
			const reshade::api::resource_desc sdesc = device_->get_resource_desc(in.scene);
			const PlaneFormat *const sfmt = carriable(sdesc, Carried::Colour);
			if (sfmt != nullptr && sdesc.texture.width == cdesc.texture.width &&
				sdesc.texture.height == cdesc.texture.height &&
				ensure_plane(scene_, sdesc.texture.width, sdesc.texture.height, *sfmt, true))
				have_scene = copy_in(cmd_list, in.scene, scene_, sdesc.texture.width, sdesc.texture.height,
					in.scene_state);
		}

		const bool handed = on_sync_queue(cmd_list) && hand_to_engine();
		if (!handed)
			queue_->wait_idle();
		depth_pass_.settle(handed ? to_engine_.value : 0);

		out.cmd = engine_->begin_list();
		if (out.cmd == nullptr) {
			last_error = engine_->last_error;
			release_engine_waits();
			return BridgeStep::Begin;
		}
		out.device = engine_->device();
		out.queue = engine_->queue();
		out.color = color_.shared.engine;
		out.depth = have_depth ? depth_.shared.engine : nullptr;
		out.motion_vectors = have_motion ? motion_.shared.engine : nullptr;
		out.scene = have_scene ? scene_.shared.engine : nullptr;
		out.width = color_.shared.width;
		out.height = color_.shared.height;
		return BridgeStep::Ok;
	}

	BridgeStep end(reshade::api::effect_runtime *, reshade::api::command_list *cmd_list,
		reshade::api::resource game_color) override
	{
		if (!ready())
			return BridgeStep::Init;
		if (!engine_->submit_list()) {
			last_error = engine_->last_error;
			return BridgeStep::Finish;
		}
		const bool handed = on_sync_queue(cmd_list) && hand_to_game();
		if (!handed && !engine_->wait_cpu(engine_->last_submitted())) {
			last_error = engine_->last_error;
			return BridgeStep::Finish;
		}

		if (game_color.handle == 0 || color_.image == VK_NULL_HANDLE)
			return BridgeStep::Export;
		const VkCommandBuffer cb = command_buffer(cmd_list);
		if (cb == VK_NULL_HANDLE) {
			last_error = L"ReShade did not name a command buffer to put the frame back into.";
			return BridgeStep::Export;
		}

		const reshade::api::resource_desc desc = device_->get_resource_desc(game_color);
		if (desc.texture.width != color_.shared.width || desc.texture.height != color_.shared.height) {
			last_error = L"the game's colour buffer changed size while the frame was being "
				L"upscaled; this one is dropped and the next is built at the new size.";
			return BridgeStep::Export;
		}
		const PlaneFormat *const fmt = plane_format_of(desc.texture.format);
		if (fmt == nullptr || fmt->dxgi != color_.shared.format) {
			last_error = L"the game's colour buffer changed format while the frame was being "
				L"upscaled; this one is dropped and the next is built in the new format.";
			return BridgeStep::Export;
		}

		const VkImage dst = vk_handle<VkImage>(game_color.handle);
		cmd_list->barrier(game_color, reshade::api::resource_usage::render_target,
			reshade::api::resource_usage::copy_dest);
		plane_barrier(cb, color_, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
		const VkImageCopy region = whole_image(color_.shared.width, color_.shared.height);
		vk_.CmdCopyImage(cb, color_.image, VK_IMAGE_LAYOUT_GENERAL,
			dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
		plane_barrier(cb, color_, VK_ACCESS_TRANSFER_READ_BIT,
			VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
		cmd_list->barrier(game_color, reshade::api::resource_usage::copy_dest,
			reshade::api::resource_usage::render_target);
		return BridgeStep::Ok;
	}

	bool request_readable(SharedPlane &plane, uint32_t w, uint32_t h, DXGI_FORMAT fmt) override
	{
		if (!ready() || w == 0 || h == 0 || fmt == DXGI_FORMAT_UNKNOWN)
			return false;
		if (plane.matches(w, h, fmt))
			return true;
		engine_->wait_cpu(engine_->last_submitted());
		bridge_util::release_plane(plane);

		D3D12_HEAP_PROPERTIES heap{};
		heap.Type = D3D12_HEAP_TYPE_DEFAULT;
		D3D12_RESOURCE_DESC desc{};
		desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		desc.Width = w;
		desc.Height = h;
		desc.DepthOrArraySize = 1;
		desc.MipLevels = 1;
		desc.Format = fmt;
		desc.SampleDesc.Count = 1;
		desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
		if (FAILED(engine_->device()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
				SharedPlane::kState, nullptr, IID_PPV_ARGS(&plane.engine)))) {
			last_error = L"the engine device refused a texture.";
			return false;
		}
		plane.width = w;
		plane.height = h;
		plane.format = fmt;
		return true;
	}

private:
	void setup_gpu_sync()
	{
		gpu_sync_ = false;
		if (!vks_.load(device_vk_)) {
			sync_ = kSyncNoExtension;
			return;
		}
		reshade::api::fence probe{};
		if (!device_->create_fence(0, reshade::api::fence_flags::none, &probe)) {
			sync_ = kSyncNoTimeline;
			return;
		}
		device_->destroy_fence(probe);
		if (!make_timeline(to_engine_) || !make_timeline(to_game_)) {
			release_timeline(to_engine_);
			release_timeline(to_game_);
			sync_ = kSyncImportRefused;
			return;
		}
		bool seen = SUCCEEDED(to_engine_.fence12->Signal(1)) && SUCCEEDED(to_game_.fence12->Signal(1));
		for (int i = 0; seen && i < 50; ++i) {
			if (device_->get_completed_fence_value(to_engine_.api()) >= 1 &&
				device_->get_completed_fence_value(to_game_.api()) >= 1)
				break;
			if (i == 49)
				seen = false;
			Sleep(1);
		}
		if (!seen) {
			release_timeline(to_engine_);
			release_timeline(to_game_);
			sync_ = kSyncCheckFailed;
			return;
		}
		to_engine_.value = 1;
		to_game_.value = 1;
		gpu_sync_ = true;
		sync_ = kSyncGpu;
	}

	bool make_timeline(SharedTimeline &t)
	{
		ID3D12Fence *fence = nullptr;
		if (FAILED(engine_->device()->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&fence))) ||
			fence == nullptr)
			return false;
		HANDLE handle = nullptr;
		if (FAILED(engine_->device()->CreateSharedHandle(fence, nullptr, GENERIC_ALL, nullptr, &handle)) ||
			handle == nullptr) {
			fence->Release();
			return false;
		}
		VkSemaphoreTypeCreateInfo type{};
		type.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
		type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
		VkSemaphoreCreateInfo sci{};
		sci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
		sci.pNext = &type;
		VkSemaphore sem = VK_NULL_HANDLE;
		if (vks_.CreateSemaphore(device_vk_, &sci, nullptr, &sem) != VK_SUCCESS) {
			CloseHandle(handle);
			fence->Release();
			return false;
		}
		VkImportSemaphoreWin32HandleInfoKHR imp{};
		imp.sType = VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_WIN32_HANDLE_INFO_KHR;
		imp.semaphore = sem;
		imp.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT;
		imp.handle = handle;
		const VkResult r = vks_.ImportSemaphoreWin32Handle(device_vk_, &imp);
		CloseHandle(handle);
		if (r != VK_SUCCESS) {
			vks_.DestroySemaphore(device_vk_, sem, nullptr);
			fence->Release();
			return false;
		}
		t.fence12 = fence;
		t.sem = sem;
		t.value = 0;
		return true;
	}

	void release_timeline(SharedTimeline &t)
	{
		if (t.sem != VK_NULL_HANDLE && device_vk_ != VK_NULL_HANDLE && vks_.DestroySemaphore != nullptr)
			vks_.DestroySemaphore(device_vk_, t.sem, nullptr);
		t.sem = VK_NULL_HANDLE;
		if (t.fence12 != nullptr)
			t.fence12->Release();
		t.fence12 = nullptr;
		t.value = 0;
	}

	void lose_gpu_sync()
	{
		gpu_sync_ = false;
		sync_ = kSyncSubmitFailed;
	}

	bool on_sync_queue(reshade::api::command_list *cmd_list)
	{
		if (!gpu_sync_ || queue_ == nullptr || cmd_list != queue_->get_immediate_command_list())
			return false;
		if (sync_queue_ == nullptr)
			sync_queue_ = queue_;
		return sync_queue_ == queue_;
	}

	void release_engine_waits()
	{
		if (!gpu_sync_ || to_engine_.fence12 == nullptr)
			return;
		to_engine_.fence12->Signal(to_engine_.value);
		lose_gpu_sync();
	}

	bool hand_to_engine()
	{
		const uint64_t v = to_engine_.value + 1;
		if (!queue_->signal(to_engine_.api(), v)) {
			lose_gpu_sync();
			return false;
		}
		to_engine_.value = v;
		if (SUCCEEDED(engine_->queue()->Wait(to_engine_.fence12, v))) {
			refused_waits_ = 0;
			return true;
		}
		if (++refused_waits_ >= 3)
			lose_gpu_sync();
		return false;
	}

	bool hand_to_game()
	{
		const uint64_t v = to_game_.value + 1;
		if (FAILED(engine_->queue()->Signal(to_game_.fence12, v)))
			return false;
		to_game_.value = v;
		if (!queue_->wait(to_game_.api(), v)) {
			lose_gpu_sync();
			return false;
		}
		return true;
	}

	static VkCommandBuffer command_buffer(reshade::api::command_list *cmd_list) noexcept
	{
		return cmd_list != nullptr
			? reinterpret_cast<VkCommandBuffer>(static_cast<uintptr_t>(cmd_list->get_native()))
			: VK_NULL_HANDLE;
	}

	static VkImageCopy whole_image(uint32_t w, uint32_t h) noexcept
	{
		VkImageCopy r{};
		r.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		r.srcSubresource.layerCount = 1;
		r.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		r.dstSubresource.layerCount = 1;
		r.extent.width = w;
		r.extent.height = h;
		r.extent.depth = 1;
		return r;
	}

	enum class Carried { Colour, Depth, Motion };

	const PlaneFormat *carriable(const reshade::api::resource_desc &desc, Carried plane)
	{
		const bool colour = plane == Carried::Colour;
		const wchar_t *const what = colour ? L"the game's colour buffer"
			: plane == Carried::Depth ? L"the depth buffer ReShade handed over" : L"the motion vectors handed over";
		if (desc.texture.width == 0 || desc.texture.height == 0) {
			last_error = std::wstring(what) + L" has no size this frame.";
			return nullptr;
		}
		if (desc.texture.samples > 1) {
			last_error = std::wstring(what) + L" is multisampled, and a "
				L"multisampled surface cannot be copied into a shared texture.";
			if (colour)
				last_error += L" ReShade resolves the frame itself when its own effects are on, so "
					L"turning one on gives the add-on something it can carry.";
			return nullptr;
		}
		if ((desc.usage & reshade::api::resource_usage::copy_source) == 0) {
			last_error = std::wstring(what) + L" was created without transfer "
				L"source usage, so Vulkan will not let anything copy out of it. Nothing on this "
				L"side can add that flag after the fact.";
			return nullptr;
		}
		if (colour && (desc.usage & reshade::api::resource_usage::copy_dest) == 0) {
			last_error = L"the game's colour buffer was created without transfer destination "
				L"usage, so the upscaled frame has no way back into it. The add-on asks for it "
				L"when the swap chain is created, so this one was created before the add-on "
				L"loaded; restarting the game fixes it.";
			return nullptr;
		}
		const PlaneFormat *const fmt = plane_format_of(desc.texture.format);
		if (fmt == nullptr) {
			last_error = std::wstring(what) + L" is in a format (ReShade "
				L"number " + std::to_wstring(static_cast<uint32_t>(desc.texture.format)) +
				L") that Direct3D 12 and Vulkan do not name the same way, so the add-on will "
				L"not share it rather than hand the engine pixels it has read wrongly.";
			return nullptr;
		}
		return fmt;
	}

	bool ensure_plane(VulkanPlane &plane, uint32_t w, uint32_t h, const PlaneFormat &fmt,
		bool render_target, bool storage = false)
	{
		if (plane.shared.matches(w, h, fmt.dxgi) && plane.image != VK_NULL_HANDLE &&
			plane.storage == storage)
			return true;

		engine_->wait_cpu(engine_->last_submitted());
		if (queue_ != nullptr)
			queue_->wait_idle();
		release_plane(plane);

		ID3D12Device *const dev12 = engine_->device();
		D3D12_HEAP_PROPERTIES heap{};
		heap.Type = D3D12_HEAP_TYPE_DEFAULT;
		D3D12_RESOURCE_DESC desc{};
		desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		desc.Width = w;
		desc.Height = h;
		desc.DepthOrArraySize = 1;
		desc.MipLevels = 1;
		desc.Format = fmt.dxgi;
		desc.SampleDesc.Count = 1;
		desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
		desc.Flags = render_target ? D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET
			: D3D12_RESOURCE_FLAG_NONE;
		if (storage)
			desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

		plane.vk_format = fmt.vk;
		plane.storage = storage;

		if (FAILED(dev12->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_SHARED, &desc,
				SharedPlane::kState, nullptr, IID_PPV_ARGS(&plane.shared.engine)))) {
			last_error = L"the engine device refused a shareable texture.";
			release_plane(plane);
			return false;
		}

		const ImportError err = import_plane(plane, desc);
		if (!err.ok()) {
			last_error = L"this Vulkan driver refused to import the engine's texture: " + err.text() + L".";
			release_plane(plane);
			return false;
		}

		if (storage) {
			VkImageViewCreateInfo vci{};
			vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
			vci.image = plane.image;
			vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
			vci.format = plane.vk_format;
			vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			vci.subresourceRange.levelCount = 1;
			vci.subresourceRange.layerCount = 1;
			VkImageView view = VK_NULL_HANDLE;
			const VkResult vr = vkc_.loaded()
				? vkc_.CreateImageView(device_vk_, &vci, nullptr, &view)
				: VK_ERROR_INITIALIZATION_FAILED;
			if (vr != VK_SUCCESS) {
				last_error = L"this Vulkan driver would not make a storage view of the depth "
					L"plane (vkCreateImageView " + vk_text(vr) + L").";
				release_plane(plane);
				return false;
			}
			plane.view = view;
		}

		plane.shared.width = w;
		plane.shared.height = h;
		plane.shared.format = fmt.dxgi;
		plane.adopted = false;
		return true;
	}

	ImportError import_plane(VulkanPlane &plane, const D3D12_RESOURCE_DESC &desc)
	{
		ImportError err;
		ID3D12Device *const dev12 = engine_->device();

		HANDLE handle = nullptr;
		if (FAILED(dev12->CreateSharedHandle(plane.shared.engine, nullptr, GENERIC_ALL, nullptr,
				&handle)) || handle == nullptr) {
			err.stage = "the engine device would not export a shared handle for the texture";
			return err;
		}

		VkExternalMemoryImageCreateInfo ext{};
		ext.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
		ext.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT;

		VkImageCreateInfo ici{};
		ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
		ici.pNext = &ext;
		ici.imageType = VK_IMAGE_TYPE_2D;
		ici.format = plane.vk_format;
		ici.extent.width = static_cast<uint32_t>(desc.Width);
		ici.extent.height = desc.Height;
		ici.extent.depth = 1;
		ici.mipLevels = 1;
		ici.arrayLayers = 1;
		ici.samples = VK_SAMPLE_COUNT_1_BIT;
		ici.tiling = VK_IMAGE_TILING_OPTIMAL;
		ici.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
			VK_IMAGE_USAGE_SAMPLED_BIT;
		if (plane.storage)
			ici.usage |= VK_IMAGE_USAGE_STORAGE_BIT;
		ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
		ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

		VkImage image = VK_NULL_HANDLE;
		err.result = vk_.CreateImage(device_vk_, &ici, nullptr, &image);
		if (err.result != VK_SUCCESS) {
			err.stage = "vkCreateImage";
			CloseHandle(handle);
			return err;
		}
		plane.image = image;

		VkMemoryRequirements reqs{};
		vk_.GetImageMemoryRequirements(device_vk_, plane.image, &reqs);

		const D3D12_RESOURCE_ALLOCATION_INFO info = dev12->GetResourceAllocationInfo(0, 1, &desc);
		if (info.SizeInBytes != UINT64_MAX && reqs.size > info.SizeInBytes) {
			err.stage = "the image Vulkan wants is larger than the memory Direct3D 12 allocated";
			CloseHandle(handle);
			return err;
		}

		uint32_t bits = reqs.memoryTypeBits;
		VkMemoryWin32HandlePropertiesKHR props{};
		props.sType = VK_STRUCTURE_TYPE_MEMORY_WIN32_HANDLE_PROPERTIES_KHR;
		if (vk_.GetMemoryWin32HandleProperties(device_vk_,
				VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT, handle, &props) == VK_SUCCESS &&
			props.memoryTypeBits != 0)
			bits &= props.memoryTypeBits;
		uint32_t index = 0;
		while (index < VK_MAX_MEMORY_TYPES && (bits & (1u << index)) == 0)
			++index;
		if (index >= VK_MAX_MEMORY_TYPES) {
			err.stage = "no memory type on this device accepts a Direct3D 12 texture";
			CloseHandle(handle);
			return err;
		}

		VkMemoryDedicatedAllocateInfo dedicated{};
		dedicated.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
		dedicated.image = plane.image;

		VkImportMemoryWin32HandleInfoKHR import{};
		import.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR;
		import.pNext = &dedicated;
		import.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT;
		import.handle = handle;

		VkMemoryAllocateInfo alloc{};
		alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
		alloc.pNext = &import;
		alloc.allocationSize = reqs.size;
		alloc.memoryTypeIndex = index;

		VkDeviceMemory memory = VK_NULL_HANDLE;
		err.result = vk_.AllocateMemory(device_vk_, &alloc, nullptr, &memory);
		CloseHandle(handle);
		if (err.result != VK_SUCCESS) {
			err.stage = "vkAllocateMemory";
			return err;
		}
		plane.memory = memory;
		err.result = vk_.BindImageMemory(device_vk_, plane.image, plane.memory, 0);
		if (err.result != VK_SUCCESS) {
			err.stage = "vkBindImageMemory";
			return err;
		}
		return err;
	}

	ImportError self_test()
	{
		VulkanPlane probe;
		probe.vk_format = VK_FORMAT_B8G8R8A8_UNORM;

		D3D12_HEAP_PROPERTIES heap{};
		heap.Type = D3D12_HEAP_TYPE_DEFAULT;
		D3D12_RESOURCE_DESC desc{};
		desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		desc.Width = 64;
		desc.Height = 64;
		desc.DepthOrArraySize = 1;
		desc.MipLevels = 1;
		desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
		desc.SampleDesc.Count = 1;
		desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
		desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

		ImportError err;
		if (FAILED(engine_->device()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_SHARED, &desc,
				SharedPlane::kState, nullptr, IID_PPV_ARGS(&probe.shared.engine)))) {
			err.stage = "the engine device refused a shareable texture";
			return err;
		}
		err = import_plane(probe, desc);
		release_plane(probe);
		return err;
	}

	bool read_depth(reshade::api::command_list *cmd_list, const BridgeInputs &in,
		const reshade::api::resource_desc &ddesc, const std::wstring &why_no_copy)
	{
		const uint32_t w = ddesc.texture.width;
		const uint32_t h = ddesc.texture.height;
		if (in.depth_view.handle == 0) {
			depth_note = why_no_copy + L" ReShade published no view of it for shaders either, so there is "
				L"nothing to read it through.";
			return false;
		}
		if (w == 0 || h == 0) {
			depth_note = L"the depth buffer ReShade handed over has no size this frame.";
			return false;
		}
		if (ddesc.texture.samples > 1) {
			depth_note = L"the depth buffer handed over is multisampled, and the pass that reads it "
				L"reads a single-sampled image.";
			return false;
		}
		if ((ddesc.usage & reshade::api::resource_usage::shader_resource) == 0) {
			depth_note = why_no_copy + L" It was not created for shaders to read either.";
			return false;
		}
		if (queue_ == nullptr || cmd_list != queue_->get_immediate_command_list()) {
			depth_note = L"this frame was drawn through another add-on's call to ReShade on a "
				L"command list of the game's own, and depth is only read on ReShade's own list.";
			return false;
		}
		const uint64_t now_ms = GetTickCount64();
		if (!ensure_depth_pass(now_ms)) {
			depth_note = depth_pass_.pipeline_retry.reason(0);
			return false;
		}
		const uint64_t size_key = (static_cast<uint64_t>(w) << 32) | h;
		if (!depth_pass_.plane_retry.may_try(now_ms, size_key)) {
			depth_note = depth_pass_.plane_retry.reason(size_key);
			return false;
		}
		if (!ensure_plane(depth_, w, h, kDepthPassPlane, false, true)) {
			depth_note = L"the plane the depth pass writes could not be made at " + std::to_wstring(w) +
				L"x" + std::to_wstring(h) + L", because " + last_error + L" Depth is left out and the "
				L"rest of the frame is still carried. It is tried again after a pause that grows while "
				L"it keeps failing.";
			depth_pass_.plane_retry.failed(now_ms, size_key, depth_note);
			last_error.clear();
			return false;
		}
		depth_pass_.plane_retry.succeeded(size_key);
		if (depth_.shared.width != w || depth_.shared.height != h) {
			depth_note = L"the depth plane does not match the depth buffer's size.";
			return false;
		}

		const uint32_t slot = depth_pass_.next;
		if (!slot_retired(slot)) {
			depth_note = L"the game's queue did not finish the previous depth read in time.";
			return false;
		}
		const VkDescriptorSet source_set = depth_pass_.source_set[slot];
		const VkDescriptorSet plane_set = depth_pass_.plane_set[slot];
		const reshade::api::sampler_with_resource_view source{
			reshade::api::sampler{ vk_value(depth_pass_.sampler) }, in.depth_view };
		const reshade::api::resource_view plane_view{ vk_value(depth_.view) };
		const reshade::api::descriptor_table_update updates[2] = {
			{ reshade::api::descriptor_table{ vk_value(source_set) }, 0, 0, 1,
				reshade::api::descriptor_type::sampler_with_resource_view, &source },
			{ reshade::api::descriptor_table{ vk_value(plane_set) }, 0, 0, 1,
				reshade::api::descriptor_type::texture_unordered_access_view, &plane_view },
		};
		device_->update_descriptor_tables(2, updates);

		const VkCommandBuffer cb = command_buffer(cmd_list);
		if (cb == VK_NULL_HANDLE) {
			depth_note = L"ReShade did not name a command buffer to read the depth buffer in.";
			return false;
		}

		cmd_list->barrier(in.depth, in.depth_state, reshade::api::resource_usage::shader_resource);
		plane_barrier(cb, depth_, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
			VK_ACCESS_SHADER_WRITE_BIT);
		cmd_list->bind_pipeline(reshade::api::pipeline_stage::all_compute,
			reshade::api::pipeline{ vk_value(depth_pass_.pipeline) });
		const reshade::api::descriptor_table tables[2] = {
			{ vk_value(source_set) },
			{ vk_value(plane_set) },
		};
		cmd_list->bind_descriptor_tables(reshade::api::shader_stage::all_compute,
			reshade::api::pipeline_layout{ vk_value(depth_pass_.pipeline_layout) },
			1, 2, tables);
		cmd_list->dispatch((w + kVulkanDepthPassGroup - 1) / kVulkanDepthPassGroup,
			(h + kVulkanDepthPassGroup - 1) / kVulkanDepthPassGroup, 1);
		plane_barrier(cb, depth_, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT);
		cmd_list->barrier(in.depth, reshade::api::resource_usage::shader_resource, in.depth_state);
		depth_pass_.pending = static_cast<int>(slot);
		depth_pass_.next = slot ^ 1u;
		return true;
	}

	bool slot_retired(uint32_t slot)
	{
		const uint64_t need = depth_pass_.retire_at[slot];
		if (need == 0 || to_engine_.sem == VK_NULL_HANDLE)
			return true;
		if (device_->get_completed_fence_value(to_engine_.api()) >= need ||
			device_->wait(to_engine_.api(), need, 2000000000ull)) {
			depth_pass_.retire_at[slot] = 0;
			return true;
		}
		if (queue_ != nullptr) {
			queue_->wait_idle();
			depth_pass_.retire_at[slot] = 0;
			return true;
		}
		return false;
	}

	bool ensure_depth_pass(uint64_t now_ms)
	{
		if (depth_pass_.pipeline != VK_NULL_HANDLE)
			return true;
		if (!depth_pass_.pipeline_retry.may_try(now_ms, 0))
			return false;
		g_depth_pass_builds.fetch_add(1, std::memory_order_relaxed);

		const auto fail = [this, now_ms](std::wstring why) {
			release_depth_pass_objects();
			depth_pass_.pipeline_retry.failed(now_ms, 0, std::move(why) + L" It is tried again after a "
				L"pause that grows while it keeps failing.");
			return false;
		};

		if (const char *const missing = vkc_.load(device_vk_))
			return fail(L"this Vulkan driver does not provide " + to_wide(missing) +
				L", which the pass that reads the depth buffer needs.");

		std::vector<uint8_t> code;
		std::string entry;
		std::wstring compile_error;
		const char *const override_fx = g_depth_pass_source_override.load(std::memory_order_relaxed);
		if (!spirv_compile(override_fx != nullptr ? override_fx : kVulkanDepthPassFx, kVulkanDepthPassEntry,
				code, &entry, &compile_error))
			return fail(L"ReShade's shader compiler refused the pass that reads the depth buffer (" +
				one_line(compile_error) + L").");
		const std::string group_suffix = "_" + std::to_string(kVulkanDepthPassGroup) + "_" +
			std::to_string(kVulkanDepthPassGroup) + "_1";
		if (entry.size() < group_suffix.size() ||
			entry.compare(entry.size() - group_suffix.size(), group_suffix.size(), group_suffix) != 0)
			return fail(L"the pass that reads the depth buffer was compiled with a thread group that "
				L"is not the one the bridge dispatches for, so it is not run.");
		const std::vector<SpirvBinding> expected = {
			{ 1, 0, SpirvDescriptorKind::CombinedImageSampler },
			{ 2, 0, SpirvDescriptorKind::StorageImage },
		};
		if (spirv_descriptor_bindings(code) != expected)
			return fail(L"the pass that reads the depth buffer compiled to a descriptor layout "
				L"this bridge does not build, so it is not run rather than run against the "
				L"wrong descriptors. The ReShade this add-on was built against places them "
				L"differently from the one it was written for.");

		std::vector<uint32_t> words(code.size() / 4);
		std::memcpy(words.data(), code.data(), words.size() * sizeof(uint32_t));
		VkShaderModuleCreateInfo smci{};
		smci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
		smci.codeSize = words.size() * sizeof(uint32_t);
		smci.pCode = words.data();
		VkShaderModule module = VK_NULL_HANDLE;
		VkResult r = vkc_.CreateShaderModule(device_vk_, &smci, nullptr, &module);
		if (r != VK_SUCCESS)
			return fail(L"vkCreateShaderModule refused the depth pass (" + vk_text(r) + L").");
		depth_pass_.module = module;

		VkDescriptorSetLayoutBinding source_binding{};
		source_binding.binding = 0;
		source_binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		source_binding.descriptorCount = 1;
		source_binding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
		VkDescriptorSetLayoutBinding plane_binding = source_binding;
		plane_binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
		const auto make_layout = [this](const VkDescriptorSetLayoutBinding *binding,
			VkDescriptorSetLayout &out) {
			VkDescriptorSetLayoutCreateInfo lci{};
			lci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
			lci.bindingCount = binding != nullptr ? 1u : 0u;
			lci.pBindings = binding;
			VkDescriptorSetLayout made = VK_NULL_HANDLE;
			const VkResult result = vkc_.CreateDescriptorSetLayout(device_vk_, &lci, nullptr, &made);
			if (result == VK_SUCCESS)
				out = made;
			return result;
		};
		if ((r = make_layout(nullptr, depth_pass_.empty_layout)) != VK_SUCCESS ||
			(r = make_layout(&source_binding, depth_pass_.source_layout)) != VK_SUCCESS ||
			(r = make_layout(&plane_binding, depth_pass_.plane_layout)) != VK_SUCCESS)
			return fail(L"vkCreateDescriptorSetLayout refused the depth pass (" + vk_text(r) + L").");

		const VkDescriptorSetLayout set_layouts[3] = {
			depth_pass_.empty_layout, depth_pass_.source_layout, depth_pass_.plane_layout };
		VkPipelineLayoutCreateInfo plci{};
		plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
		plci.setLayoutCount = 3;
		plci.pSetLayouts = set_layouts;
		VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
		if ((r = vkc_.CreatePipelineLayout(device_vk_, &plci, nullptr, &pipeline_layout)) != VK_SUCCESS)
			return fail(L"vkCreatePipelineLayout refused the depth pass (" + vk_text(r) + L").");
		depth_pass_.pipeline_layout = pipeline_layout;

		VkComputePipelineCreateInfo cpci{};
		cpci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
		cpci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
		cpci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
		cpci.stage.module = depth_pass_.module;
		cpci.stage.pName = entry.c_str();
		cpci.layout = depth_pass_.pipeline_layout;
		cpci.basePipelineIndex = -1;
		VkPipeline pipeline = VK_NULL_HANDLE;
		if ((r = vkc_.CreateComputePipelines(device_vk_, VK_NULL_HANDLE, 1, &cpci, nullptr,
				&pipeline)) != VK_SUCCESS)
			return fail(L"vkCreateComputePipelines refused the depth pass (" + vk_text(r) + L").");
		depth_pass_.pipeline = pipeline;

		const VkDescriptorPoolSize sizes[2] = {
			{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, DepthPassState::kSlots },
			{ VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, DepthPassState::kSlots },
		};
		VkDescriptorPoolCreateInfo dpci{};
		dpci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
		dpci.maxSets = 2 * DepthPassState::kSlots;
		dpci.poolSizeCount = 2;
		dpci.pPoolSizes = sizes;
		VkDescriptorPool pool = VK_NULL_HANDLE;
		if ((r = vkc_.CreateDescriptorPool(device_vk_, &dpci, nullptr, &pool)) != VK_SUCCESS)
			return fail(L"vkCreateDescriptorPool refused the depth pass (" + vk_text(r) + L").");
		depth_pass_.pool = pool;

		const VkDescriptorSetLayout alloc_layouts[2 * DepthPassState::kSlots] = {
			depth_pass_.source_layout, depth_pass_.plane_layout,
			depth_pass_.source_layout, depth_pass_.plane_layout };
		VkDescriptorSetAllocateInfo dsai{};
		dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
		dsai.descriptorPool = depth_pass_.pool;
		dsai.descriptorSetCount = 2 * DepthPassState::kSlots;
		dsai.pSetLayouts = alloc_layouts;
		VkDescriptorSet allocated[2 * DepthPassState::kSlots] = {};
		if ((r = vkc_.AllocateDescriptorSets(device_vk_, &dsai, allocated)) != VK_SUCCESS)
			return fail(L"vkAllocateDescriptorSets refused the depth pass (" + vk_text(r) + L").");
		for (uint32_t i = 0; i < DepthPassState::kSlots; ++i) {
			depth_pass_.source_set[i] = allocated[2 * i];
			depth_pass_.plane_set[i] = allocated[2 * i + 1];
		}

		VkSamplerCreateInfo sci{};
		sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
		sci.magFilter = VK_FILTER_NEAREST;
		sci.minFilter = VK_FILTER_NEAREST;
		sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
		sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		VkSampler sampler = VK_NULL_HANDLE;
		if ((r = vkc_.CreateSampler(device_vk_, &sci, nullptr, &sampler)) != VK_SUCCESS)
			return fail(L"vkCreateSampler refused the depth pass (" + vk_text(r) + L").");
		depth_pass_.sampler = sampler;

		depth_pass_.pipeline_retry.succeeded(0);
		return true;
	}

	void release_depth_pass_objects()
	{
		if (device_vk_ != VK_NULL_HANDLE && vkc_.loaded()) {
			if (depth_pass_.sampler != VK_NULL_HANDLE)
				vkc_.DestroySampler(device_vk_, depth_pass_.sampler, nullptr);
			if (depth_pass_.pool != VK_NULL_HANDLE)
				vkc_.DestroyDescriptorPool(device_vk_, depth_pass_.pool, nullptr);
			if (depth_pass_.pipeline != VK_NULL_HANDLE)
				vkc_.DestroyPipeline(device_vk_, depth_pass_.pipeline, nullptr);
			if (depth_pass_.pipeline_layout != VK_NULL_HANDLE)
				vkc_.DestroyPipelineLayout(device_vk_, depth_pass_.pipeline_layout, nullptr);
			if (depth_pass_.plane_layout != VK_NULL_HANDLE)
				vkc_.DestroyDescriptorSetLayout(device_vk_, depth_pass_.plane_layout, nullptr);
			if (depth_pass_.source_layout != VK_NULL_HANDLE)
				vkc_.DestroyDescriptorSetLayout(device_vk_, depth_pass_.source_layout, nullptr);
			if (depth_pass_.empty_layout != VK_NULL_HANDLE)
				vkc_.DestroyDescriptorSetLayout(device_vk_, depth_pass_.empty_layout, nullptr);
			if (depth_pass_.module != VK_NULL_HANDLE)
				vkc_.DestroyShaderModule(device_vk_, depth_pass_.module, nullptr);
		}
		RetryBackoff pipeline_retry = std::move(depth_pass_.pipeline_retry);
		RetryBackoff plane_retry = std::move(depth_pass_.plane_retry);
		depth_pass_ = DepthPassState{};
		depth_pass_.pipeline_retry = std::move(pipeline_retry);
		depth_pass_.plane_retry = std::move(plane_retry);
	}

	void destroy_depth_pass()
	{
		release_depth_pass_objects();
		depth_pass_ = DepthPassState{};
	}

	bool copy_in(reshade::api::command_list *cmd_list, reshade::api::resource src,
		VulkanPlane &plane, uint32_t w, uint32_t h, reshade::api::resource_usage held)
	{
		const VkCommandBuffer cb = command_buffer(cmd_list);
		if (cb == VK_NULL_HANDLE)
			return false;

		cmd_list->barrier(src, held, reshade::api::resource_usage::copy_source);
		plane_barrier(cb, plane, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
			VK_ACCESS_TRANSFER_WRITE_BIT);
		const VkImageCopy region = whole_image(w, h);
		vk_.CmdCopyImage(cb, vk_handle<VkImage>(src.handle),
			VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, plane.image, VK_IMAGE_LAYOUT_GENERAL,
			1, &region);
		plane_barrier(cb, plane, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT);
		cmd_list->barrier(src, reshade::api::resource_usage::copy_source, held);
		return true;
	}

	void plane_barrier(VkCommandBuffer cb, VulkanPlane &plane, VkAccessFlags src_access,
		VkAccessFlags dst_access)
	{
		VkImageMemoryBarrier b{};
		b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		b.srcAccessMask = plane.adopted ? src_access : 0;
		b.dstAccessMask = dst_access;
		b.oldLayout = plane.adopted ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED;
		b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
		b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		b.image = plane.image;
		b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		b.subresourceRange.levelCount = 1;
		b.subresourceRange.layerCount = 1;
		vk_.CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
			VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
		plane.adopted = true;
	}

	void release_plane(VulkanPlane &plane)
	{
		if (device_vk_ != VK_NULL_HANDLE) {
			if (plane.view != VK_NULL_HANDLE && vkc_.DestroyImageView != nullptr)
				vkc_.DestroyImageView(device_vk_, plane.view, nullptr);
			if (plane.image != VK_NULL_HANDLE && vk_.DestroyImage != nullptr)
				vk_.DestroyImage(device_vk_, plane.image, nullptr);
			if (plane.memory != VK_NULL_HANDLE && vk_.FreeMemory != nullptr)
				vk_.FreeMemory(device_vk_, plane.memory, nullptr);
		}
		plane.view = VK_NULL_HANDLE;
		plane.image = VK_NULL_HANDLE;
		plane.memory = VK_NULL_HANDLE;
		plane.vk_format = VK_FORMAT_UNDEFINED;
		plane.storage = false;
		plane.adopted = false;
		bridge_util::release_plane(plane.shared);
	}

	EngineDevice *engine_ = nullptr;
	reshade::api::device *device_ = nullptr;
	reshade::api::command_queue *queue_ = nullptr;
	VkDevice device_vk_ = VK_NULL_HANDLE;
	VulkanApi vk_{};
	VulkanComputeApi vkc_{};
	VulkanSyncApi vks_{};

	SharedTimeline to_engine_{};
	SharedTimeline to_game_{};
	bool gpu_sync_ = false;
	const char *sync_ = kSyncNoExtension;
	reshade::api::command_queue *sync_queue_ = nullptr;
	uint32_t refused_waits_ = 0;

	VulkanPlane color_{};
	VulkanPlane depth_{};
	VulkanPlane motion_{};
	VulkanPlane scene_{};

	struct DepthPassState {
		VkShaderModule module = VK_NULL_HANDLE;
		VkDescriptorSetLayout empty_layout = VK_NULL_HANDLE;
		VkDescriptorSetLayout source_layout = VK_NULL_HANDLE;
		VkDescriptorSetLayout plane_layout = VK_NULL_HANDLE;
		VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
		VkPipeline pipeline = VK_NULL_HANDLE;
		VkDescriptorPool pool = VK_NULL_HANDLE;
		static constexpr uint32_t kSlots = 2;
		VkDescriptorSet source_set[kSlots] = {};
		VkDescriptorSet plane_set[kSlots] = {};
		uint64_t retire_at[kSlots] = {};
		uint32_t next = 0;
		int pending = -1;
		VkSampler sampler = VK_NULL_HANDLE;

		void settle(uint64_t value) noexcept
		{
			if (pending >= 0)
				retire_at[pending] = value;
			pending = -1;
		}
		RetryBackoff pipeline_retry;
		RetryBackoff plane_retry;
	};
	DepthPassState depth_pass_{};
};

}

FrameBridge *make_bridge_vulkan() { return new BridgeVulkan(); }

void vulkan_depth_pass_source_for_tests(const char *fx) noexcept
{
	g_depth_pass_source_override.store(fx, std::memory_order_relaxed);
}

uint32_t vulkan_depth_pass_builds_for_tests() noexcept
{
	return g_depth_pass_builds.load(std::memory_order_relaxed);
}

void vulkan_proc_resolver_for_tests(VulkanProcResolver resolver) noexcept
{
	g_proc_resolver_override.store(resolver, std::memory_order_relaxed);
}

DXGI_FORMAT vulkan_plane_format(reshade::api::format fmt) noexcept
{
	const PlaneFormat *const f = plane_format_of(fmt);
	return f != nullptr ? f->dxgi : DXGI_FORMAT_UNKNOWN;
}

}
