#include "aeon_sr/jitter/vulkan_viewport_hook.hpp"

#include "aeon_sr/core/diagnostics.hpp"
#include "aeon_sr/jitter/scene_snapshot.hpp"

#include <Windows.h>

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <MinHook.h>

#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <type_traits>
#include <utility>

namespace aeon_sr {
namespace {

static_assert(sizeof(VkViewportFields) == sizeof(VkViewport), "VkViewportFields has to stay VkViewport");

using PfnSetViewportWithCount = void(VKAPI_PTR *)(VkCommandBuffer, uint32_t, const VkViewport *);
using PfnSetFlag = void(VKAPI_PTR *)(VkCommandBuffer, VkBool32);
using PfnSetCompare = void(VKAPI_PTR *)(VkCommandBuffer, VkCompareOp);

std::mutex g_lock;

bool g_minhook_up = false;

enum HookKind : uint32_t { kViewport, kDepthTest, kDepthWrite, kDepthOp, kHookKinds };

constexpr uint32_t kHookSlots = 16u;

struct HookSlot {
	std::atomic<void *> target{ nullptr };
	std::atomic<void *> original{ nullptr };
};
HookSlot g_slots[kHookKinds][kHookSlots];

std::atomic<uint32_t> g_enables_before_refusal{ 0 };
std::atomic<uint32_t> g_refused_enables{ 0 };

template <uint32_t Kind, uint32_t Slot, typename Pfn>
Pfn original_of() noexcept
{
	return reinterpret_cast<Pfn>(g_slots[Kind][Slot].original.load(std::memory_order_acquire));
}

template <uint32_t Slot>
void VKAPI_CALL set_viewport_with_count(VkCommandBuffer cmd, uint32_t count, const VkViewport *viewports)
{
	if (count != 0 && viewports != nullptr) {
		JitterViewport seen[kJitterViewportSlots];
		bool flipped[kJitterViewportSlots];
		const uint32_t n = count < kJitterViewportSlots ? count : kJitterViewportSlots;
		for (uint32_t i = 0; i < n; ++i) {
			VkViewportFields f;
			std::memcpy(&f, &viewports[i], sizeof(f));
			flipped[i] = f.height < 0.0f;
			seen[i] = jitter_viewport_from_vk(f);
		}
		vulkan_viewport_hook().note_viewports(cmd, 0u, n, seen, flipped);
	}
	if (const auto original = original_of<kViewport, Slot, PfnSetViewportWithCount>())
		original(cmd, count, viewports);
}

template <uint32_t Kind, uint32_t Slot>
void VKAPI_CALL set_depth_flag(VkCommandBuffer cmd, VkBool32 on)
{
	const int value = on != VK_FALSE ? 1 : 0;
	vulkan_viewport_hook().note_depth(cmd, Kind == kDepthTest ? value : -1, Kind == kDepthWrite ? value : -1, -1);
	if (const auto original = original_of<Kind, Slot, PfnSetFlag>())
		original(cmd, on);
}

template <uint32_t Slot>
void VKAPI_CALL set_depth_op(VkCommandBuffer cmd, VkCompareOp op)
{
	vulkan_viewport_hook().note_depth(cmd, -1, -1, static_cast<int>(op));
	if (const auto original = original_of<kDepthOp, Slot, PfnSetCompare>())
		original(cmd, op);
}

template <size_t... S>
std::array<void *, kHookSlots> viewport_detours(std::index_sequence<S...>) noexcept
{
	return { { reinterpret_cast<void *>(&set_viewport_with_count<static_cast<uint32_t>(S)>)... } };
}

template <uint32_t Kind, size_t... S>
std::array<void *, kHookSlots> flag_detours(std::index_sequence<S...>) noexcept
{
	return { { reinterpret_cast<void *>(&set_depth_flag<Kind, static_cast<uint32_t>(S)>)... } };
}

template <size_t... S>
std::array<void *, kHookSlots> op_detours(std::index_sequence<S...>) noexcept
{
	return { { reinterpret_cast<void *>(&set_depth_op<static_cast<uint32_t>(S)>)... } };
}

void *detour_of(uint32_t kind, uint32_t slot) noexcept
{
	static const std::array<void *, kHookSlots> detours[kHookKinds] = {
		viewport_detours(std::make_index_sequence<kHookSlots>()),
		flag_detours<kDepthTest>(std::make_index_sequence<kHookSlots>()),
		flag_detours<kDepthWrite>(std::make_index_sequence<kHookSlots>()),
		op_detours(std::make_index_sequence<kHookSlots>()),
	};
	return detours[kind][slot];
}

HookSlot *slot_of(uint32_t kind, void *target) noexcept
{
	for (HookSlot &s : g_slots[kind]) {
		if (s.target.load(std::memory_order_acquire) == target)
			return &s;
	}
	return nullptr;
}

MH_STATUS enable(void *target) noexcept
{
	if (g_refused_enables.load() != 0) {
		if (g_enables_before_refusal.load() != 0) {
			g_enables_before_refusal.fetch_sub(1u);
		} else {
			g_refused_enables.fetch_sub(1u);
			return MH_ERROR_MEMORY_PROTECT;
		}
	}
	const MH_STATUS st = MH_EnableHook(target);
	return st == MH_ERROR_ENABLED ? MH_OK : st;
}

void *hook_at(uint32_t kind, void *target, const char **why) noexcept
{
	*why = "the device has no such call";
	if (target == nullptr)
		return nullptr;
	HookSlot *slot = slot_of(kind, target);
	if (slot == nullptr) {
		for (uint32_t k = 0; k < kHookKinds; ++k) {
			if (k != kind && slot_of(k, target) != nullptr) {
				*why = "its address is another call's";
				return nullptr;
			}
		}
		uint32_t empty = 0;
		while (empty < kHookSlots && g_slots[kind][empty].target.load(std::memory_order_acquire) != nullptr)
			++empty;
		if (empty == kHookSlots) {
			*why = "it has been hooked at sixteen other addresses already";
			return nullptr;
		}
		void *made = nullptr;
		if (MH_CreateHook(target, detour_of(kind, empty), &made) != MH_OK || made == nullptr) {
			*why = "the hooking library would not hook it";
			return nullptr;
		}
		slot = &g_slots[kind][empty];
		slot->original.store(made, std::memory_order_release);
		slot->target.store(target, std::memory_order_release);
	}
	if (enable(target) != MH_OK) {
		*why = "the hooking library would not enable it";
		return nullptr;
	}
	return slot->original.load(std::memory_order_acquire);
}

}

void VulkanViewportHook::refuse_enables(uint32_t after, uint32_t n) noexcept
{
	g_enables_before_refusal.store(after);
	g_refused_enables.store(n);
}

JitterViewport jitter_viewport_from_vk(const VkViewportFields &v) noexcept
{
	JitterViewport out;
	out.x = v.x;
	out.y = v.y;
	out.width = v.width;
	out.height = v.height;
	out.min_depth = v.min_depth;
	out.max_depth = v.max_depth;
	if (out.height < 0.0f) {
		out.y += out.height;
		out.height = -out.height;
	}
	return out;
}

VkViewportFields vk_from_jitter_viewport(const JitterViewport &v, bool flipped) noexcept
{
	VkViewportFields out;
	out.x = v.x;
	out.y = flipped ? v.y + v.height : v.y;
	out.width = v.width;
	out.height = flipped ? -v.height : v.height;
	out.min_depth = v.min_depth;
	out.max_depth = v.max_depth;
	return out;
}

VulkanViewportHook::Slot *VulkanViewportHook::find(void *cmd) noexcept
{
	for (Slot &s : slots_) {
		if (s.cmd == cmd)
			return &s;
	}
	return nullptr;
}

const VulkanViewportHook::Slot *VulkanViewportHook::find(void *cmd) const noexcept
{
	for (const Slot &s : slots_) {
		if (s.cmd == cmd)
			return &s;
	}
	return nullptr;
}

VulkanViewportHook::Slot *VulkanViewportHook::claim(void *cmd) noexcept
{
	Slot *slot = find(cmd);
	if (slot == nullptr) {
		slot = &slots_[0];
		for (Slot &s : slots_) {
			if (s.cmd == nullptr) {
				slot = &s;
				break;
			}
			if (s.used < slot->used)
				slot = &s;
		}
		*slot = Slot{};
		slot->cmd = cmd;
	}
	slot->used = ++stamp_;
	return slot;
}

void VulkanViewportHook::note_viewports(void *cmd, uint32_t first, uint32_t count,
	const JitterViewport *vp, const bool *flipped) noexcept
{
	if (cmd == nullptr || vp == nullptr || first != 0u || count == 0u || count > kJitterViewportSlots)
		return;
	const std::lock_guard<std::mutex> lock(g_lock);
	saw_ = true;
	Slot *const slot = claim(cmd);
	slot->stamp = slot->used;
	slot->count = count;
	for (uint32_t i = 0; i < count; ++i) {
		slot->vp[i] = vp[i];
		slot->flipped[i] = flipped == nullptr || flipped[i];
	}
}

void VulkanViewportHook::forget_list(void *cmd) noexcept
{
	if (cmd == nullptr)
		return;
	const std::lock_guard<std::mutex> lock(g_lock);
	if (Slot *const slot = find(cmd))
		*slot = Slot{};
}

void VulkanViewportHook::note_depth(void *cmd, int enable, int write, int op) noexcept
{
	if (cmd == nullptr)
		return;
	const std::lock_guard<std::mutex> lock(g_lock);
	Slot *const slot = claim(cmd);
	if (enable >= 0)
		slot->depth_enable = static_cast<int8_t>(enable != 0 ? 1 : 0);
	if (write >= 0)
		slot->depth_write = static_cast<int8_t>(write != 0 ? 1 : 0);
	if (op >= 0 && op <= VK_COMPARE_OP_ALWAYS)
		slot->depth_op = static_cast<int8_t>(op);
}

bool VulkanViewportHook::depth_of(void *cmd, int *enable, int *write, int *op) const noexcept
{
	*enable = *write = *op = -1;
	if (cmd == nullptr)
		return false;
	const std::lock_guard<std::mutex> lock(g_lock);
	const Slot *const slot = find(cmd);
	if (slot == nullptr)
		return false;
	*enable = slot->depth_enable;
	*write = slot->depth_write;
	*op = slot->depth_op;
	return *enable >= 0 || *write >= 0 || *op >= 0;
}

uint64_t VulkanViewportHook::stamp_of(void *cmd) const noexcept
{
	if (cmd == nullptr)
		return 0u;
	const std::lock_guard<std::mutex> lock(g_lock);
	const Slot *const slot = find(cmd);
	return slot != nullptr ? slot->stamp : 0u;
}

uint32_t VulkanViewportHook::viewports_of(void *cmd, JitterViewport *out, uint32_t max) const noexcept
{
	if (cmd == nullptr || out == nullptr || max == 0u)
		return 0u;
	const std::lock_guard<std::mutex> lock(g_lock);
	const Slot *const slot = find(cmd);
	if (slot == nullptr)
		return 0u;
	const uint32_t n = slot->count < max ? slot->count : max;
	for (uint32_t i = 0; i < n; ++i)
		out[i] = slot->vp[i];
	return n;
}

bool VulkanViewportHook::set(void *cmd, uint32_t first, uint32_t count, const JitterViewport *vp) const noexcept
{
	if (!installed_ || cmd == nullptr || vp == nullptr || first != 0u ||
		count == 0u || count > kJitterViewportSlots)
		return false;
	PfnSetViewportWithCount call = nullptr;
	bool flipped[kJitterViewportSlots];
	{
		const std::lock_guard<std::mutex> lock(g_lock);
		call = reinterpret_cast<PfnSetViewportWithCount>(original_);
		const Slot *const slot = find(cmd);
		for (uint32_t i = 0; i < count; ++i)
			flipped[i] = slot == nullptr || i >= slot->count || slot->flipped[i];
	}
	if (call == nullptr)
		return false;
	VkViewport out[kJitterViewportSlots];
	for (uint32_t i = 0; i < count; ++i) {
		const VkViewportFields f = vk_from_jitter_viewport(vp[i], flipped[i]);
		std::memcpy(&out[i], &f, sizeof(f));
	}
	call(static_cast<VkCommandBuffer>(cmd), count, out);
	return true;
}

bool VulkanViewportHook::install(void *device, void *get_device_proc) noexcept
{
	if (installed_ && device == device_)
		return true;
	if (device == nullptr || get_device_proc == nullptr) {
		note_ = "no Vulkan device to hook";
		return false;
	}
	if (installed_)
		forget();
	if (note_[0] != '\0' && device == device_)
		return false;
	device_ = device;

	auto *const gdpa = reinterpret_cast<PFN_vkGetDeviceProcAddr>(get_device_proc);
	auto *const target = reinterpret_cast<void *>(
		gdpa(static_cast<VkDevice>(device), "vkCmdSetViewportWithCount"));
	auto *const target_ext = reinterpret_cast<void *>(
		gdpa(static_cast<VkDevice>(device), "vkCmdSetViewportWithCountEXT"));
	void *const use = target != nullptr ? target : target_ext;
	if (use == nullptr) {
		note_ = "this device has no vkCmdSetViewportWithCount to hook";
		return false;
	}

	if (!g_minhook_up) {
		const MH_STATUS st = MH_Initialize();
		if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED) {
			note_ = "the hooking library would not start";
			return false;
		}
		g_minhook_up = true;
	}

	const char *why = nullptr;
	void *const original = hook_at(kViewport, use, &why);
	if (original == nullptr) {
		note_ = "vkCmdSetViewportWithCount could not be hooked";
		diag_logf(DiagLevel::Warn, "jitter", L"vkCmdSetViewportWithCount could not be hooked: %hs", why);
		return false;
	}
	{
		const std::lock_guard<std::mutex> lock(g_lock);
		original_ = original;
	}
	installed_ = true;
	note_ = "";
	diag_info("jitter", L"hooked vkCmdSetViewportWithCount: this game sets its viewport where ReShade "
		L"does not report it, so the add-on reads it here instead");

	struct DepthCall {
		const char *name;
		uint32_t kind;
	};
	const DepthCall calls[] = {
		{ "vkCmdSetDepthTestEnable", kDepthTest },
		{ "vkCmdSetDepthTestEnableEXT", kDepthTest },
		{ "vkCmdSetDepthWriteEnable", kDepthWrite },
		{ "vkCmdSetDepthWriteEnableEXT", kDepthWrite },
		{ "vkCmdSetDepthCompareOp", kDepthOp },
		{ "vkCmdSetDepthCompareOpEXT", kDepthOp },
	};
	void *hooked[6] = {};
	uint32_t enabled = 0, test_calls = 0;
	for (uint32_t i = 0; i < 6u; ++i) {
		void *const call = reinterpret_cast<void *>(gdpa(static_cast<VkDevice>(device), calls[i].name));
		if (call == nullptr || call == use)
			continue;
		bool same = false;
		for (uint32_t k = 0; k < i; ++k)
			same = same || hooked[k] == call;
		if (same)
			continue;
		if (hook_at(calls[i].kind, call, &why) != nullptr) {
			hooked[i] = call;
			++enabled;
			test_calls += calls[i].kind == kDepthTest ? 1u : 0u;
		} else {
			diag_logf(DiagLevel::Info, "jitter", L"%hs could not be hooked: %hs", calls[i].name, why);
		}
	}
	depth_hooked_ = test_calls != 0u;
	if (depth_hooked_)
		diag_logf(DiagLevel::Info, "jitter", L"hooked %u of the calls that set the depth test dynamically: "
			L"ReShade does not report them, and they tell the interface and the quads over the scene apart",
			enabled);
	return true;
}

void VulkanViewportHook::forget() noexcept
{
	if (installed_) {
		MH_DisableHook(MH_ALL_HOOKS);
	}
	const std::lock_guard<std::mutex> lock(g_lock);
	installed_ = false;
	depth_hooked_ = false;
	saw_ = false;
	note_ = "";
	device_ = nullptr;
	original_ = nullptr;
	for (Slot &s : slots_)
		s = Slot{};
}

VulkanViewportHook &vulkan_viewport_hook() noexcept
{
	static VulkanViewportHook hook;
	return hook;
}

namespace {

using PfnBeginRendering = void(VKAPI_PTR *)(VkCommandBuffer, const VkRenderingInfo *);
using PfnEndRendering = void(VKAPI_PTR *)(VkCommandBuffer);

constexpr uint32_t kPassColours = 8u;
constexpr uint32_t kPassSlots = 32u;

struct PassRecord {
	VkCommandBuffer cb = VK_NULL_HANDLE;
	bool open = false;
	bool restartable = false;
	VkRenderingInfo info{};
	VkRenderingAttachmentInfo colour[kPassColours]{};
	VkRenderingAttachmentInfo depth{}, stencil{};
	VkRenderingAttachmentFlagsInfoKHR colour_flags[kPassColours]{}, depth_flags{}, stencil_flags{};
	bool has_depth = false, has_stencil = false;
	uint64_t age = 0;
	const char *why = "";
};

const char *attachment_why(const VkRenderingAttachmentInfo &a) noexcept
{
	if (a.imageView == VK_NULL_HANDLE)
		return nullptr;
	const auto *const first = static_cast<const VkBaseInStructure *>(a.pNext);
	const bool flags_only = first != nullptr && first->sType == VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_FLAGS_INFO_KHR &&
		first->pNext == nullptr;
	if (a.pNext != nullptr && !flags_only) {
		static char buf[96];
		const auto *const next = static_cast<const VkBaseInStructure *>(a.pNext);
		std::snprintf(buf, sizeof(buf), "an attachment carries an extension chain (sType %d%s)",
			static_cast<int>(next->sType), next->pNext != nullptr ? " and more" : "");
		return buf;
	}
	if (a.resolveMode != VK_RESOLVE_MODE_NONE)
		return "an attachment resolves";
	if (a.storeOp != VK_ATTACHMENT_STORE_OP_STORE)
		return a.storeOp == VK_ATTACHMENT_STORE_OP_NONE ? "an attachment is stored with STORE_OP_NONE"
														: "an attachment's store discards it";
	return nullptr;
}

std::mutex g_pass_lock;
PassRecord g_passes[kPassSlots];
uint64_t g_pass_age = 0;
PfnBeginRendering g_begin_original = nullptr;
PfnBeginRendering g_begin_entry = nullptr;
PfnEndRendering g_end_entry = nullptr;
PFN_vkCmdPipelineBarrier g_pipeline_barrier = nullptr;
PFN_vkCmdCopyImage g_copy_image = nullptr;
PFN_vkCmdResolveImage g_resolve_image = nullptr;
bool g_rendering_installed = false;
void *g_rendering_device = nullptr;
thread_local bool t_restarting = false;

template <typename H>
H vk_handle_of(uint64_t value) noexcept
{
	if constexpr (std::is_pointer_v<H>)
		return reinterpret_cast<H>(static_cast<uintptr_t>(value));
	else
		return static_cast<H>(value);
}

void keep_attachment(VkRenderingAttachmentInfo &into, VkRenderingAttachmentFlagsInfoKHR &flags,
	const VkRenderingAttachmentInfo &from) noexcept
{
	into = from;
	const auto *const first = static_cast<const VkBaseInStructure *>(from.pNext);
	if (first != nullptr && first->sType == VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_FLAGS_INFO_KHR &&
		first->pNext == nullptr) {
		flags = *static_cast<const VkRenderingAttachmentFlagsInfoKHR *>(from.pNext);
		into.pNext = &flags;
	} else {
		into.pNext = nullptr;
	}
}

PassRecord *pass_of(VkCommandBuffer cb) noexcept
{
	for (PassRecord &p : g_passes) {
		if (p.cb == cb)
			return &p;
	}
	return nullptr;
}

void VKAPI_CALL begin_rendering(VkCommandBuffer cb, const VkRenderingInfo *info)
{
	if (info != nullptr) {
		const std::lock_guard<std::mutex> lock(g_pass_lock);
		PassRecord *p = pass_of(cb);
		if (p == nullptr) {
			p = &g_passes[0];
			for (PassRecord &q : g_passes) {
				if (q.age < p->age)
					p = &q;
			}
		}
		*p = PassRecord{};
		p->cb = cb;
		p->open = true;
		p->age = ++g_pass_age;
		p->info = *info;
		const char *why = info->pNext != nullptr ? "the pass carries an extension chain"
			: info->flags != 0 ? "the pass has flags (suspending, resuming or secondary buffers)"
			: info->viewMask != 0 ? "the pass is multiview"
			: info->colorAttachmentCount == 0 || info->colorAttachmentCount > kPassColours ? "no colour attachment"
			: nullptr;
		const uint32_t n = info->colorAttachmentCount < kPassColours ? info->colorAttachmentCount : kPassColours;
		for (uint32_t i = 0; i < n; ++i) {
			if (why == nullptr)
				why = attachment_why(info->pColorAttachments[i]);
			keep_attachment(p->colour[i], p->colour_flags[i], info->pColorAttachments[i]);
		}
		if (info->pDepthAttachment != nullptr) {
			if (why == nullptr)
				why = attachment_why(*info->pDepthAttachment);
			keep_attachment(p->depth, p->depth_flags, *info->pDepthAttachment);
			p->has_depth = true;
		}
		if (info->pStencilAttachment != nullptr) {
			if (why == nullptr)
				why = attachment_why(*info->pStencilAttachment);
			keep_attachment(p->stencil, p->stencil_flags, *info->pStencilAttachment);
			p->has_stencil = true;
		}
		const bool ok = why == nullptr;
		p->why = ok ? "" : why;
		p->info.pColorAttachments = p->colour;
		p->info.pDepthAttachment = p->has_depth ? &p->depth : nullptr;
		p->info.pStencilAttachment = p->has_stencil ? &p->stencil : nullptr;
		p->restartable = ok;
	}
	if (g_begin_original != nullptr)
		g_begin_original(cb, info);
}

VkImageMemoryBarrier image_barrier(VkImage image, VkImageLayout from, VkImageLayout to, VkAccessFlags src,
	VkAccessFlags dst) noexcept
{
	VkImageMemoryBarrier b{};
	b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	b.srcAccessMask = src;
	b.dstAccessMask = dst;
	b.oldLayout = from;
	b.newLayout = to;
	b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	b.image = image;
	b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	b.subresourceRange.levelCount = 1;
	b.subresourceRange.layerCount = 1;
	return b;
}

}

bool vulkan_rendering_hook_install(void *device, void *get_device_proc) noexcept
{
	if (g_rendering_installed && device == g_rendering_device)
		return true;
	if (device == nullptr || get_device_proc == nullptr || g_rendering_installed)
		return false;
	auto *const gdpa = reinterpret_cast<PFN_vkGetDeviceProcAddr>(get_device_proc);
	const VkDevice dev = static_cast<VkDevice>(device);
	auto *begin = reinterpret_cast<PfnBeginRendering>(gdpa(dev, "vkCmdBeginRendering"));
	auto *end = reinterpret_cast<PfnEndRendering>(gdpa(dev, "vkCmdEndRendering"));
	if (begin == nullptr || end == nullptr) {
		begin = reinterpret_cast<PfnBeginRendering>(gdpa(dev, "vkCmdBeginRenderingKHR"));
		end = reinterpret_cast<PfnEndRendering>(gdpa(dev, "vkCmdEndRenderingKHR"));
	}
	auto *const barrier = reinterpret_cast<PFN_vkCmdPipelineBarrier>(gdpa(dev, "vkCmdPipelineBarrier"));
	auto *const copy = reinterpret_cast<PFN_vkCmdCopyImage>(gdpa(dev, "vkCmdCopyImage"));
	auto *const resolve = reinterpret_cast<PFN_vkCmdResolveImage>(gdpa(dev, "vkCmdResolveImage"));
	if (begin == nullptr || end == nullptr || barrier == nullptr || copy == nullptr || resolve == nullptr)
		return false;
	if (!g_minhook_up) {
		const MH_STATUS st = MH_Initialize();
		if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED)
			return false;
		g_minhook_up = true;
	}
	void *original = nullptr;
	MH_STATUS st = MH_CreateHook(reinterpret_cast<void *>(begin), reinterpret_cast<void *>(&begin_rendering), &original);
	if (st != MH_OK || original == nullptr)
		return false;
	{
		const std::lock_guard<std::mutex> lock(g_pass_lock);
		g_begin_original = reinterpret_cast<PfnBeginRendering>(original);
		g_begin_entry = begin;
		g_end_entry = end;
		g_pipeline_barrier = barrier;
		g_copy_image = copy;
		g_resolve_image = resolve;
	}
	if (MH_EnableHook(reinterpret_cast<void *>(begin)) != MH_OK) {
		MH_RemoveHook(reinterpret_cast<void *>(begin));
		return false;
	}
	g_rendering_installed = true;
	g_rendering_device = device;
	diag_info("interface", L"hooked vkCmdBeginRendering: the window can be copied before the interface is drawn "
		L"inside a render pass");
	return true;
}

bool vulkan_rendering_hook_installed() noexcept
{
	return g_rendering_installed;
}

void vulkan_rendering_ended(void *cmd) noexcept
{
	if (t_restarting)
		return;
	const std::lock_guard<std::mutex> lock(g_pass_lock);
	if (PassRecord *p = pass_of(static_cast<VkCommandBuffer>(cmd)))
		p->open = false;
}

bool vulkan_restarting_pass() noexcept
{
	return t_restarting;
}

bool vulkan_copy_in_pass(void *cmd, uint64_t window, uint64_t dst, uint32_t w, uint32_t h, bool resolve,
	const char **why) noexcept
{
	const auto fail = [why](const char *what) {
		if (why != nullptr)
			*why = what;
		return false;
	};
	if (!g_rendering_installed)
		return fail("vkCmdBeginRendering is not hooked");
	const VkCommandBuffer cb = static_cast<VkCommandBuffer>(cmd);
	PassRecord pass;
	{
		const std::lock_guard<std::mutex> lock(g_pass_lock);
		const PassRecord *const p = pass_of(cb);
		if (p == nullptr)
			return fail("the interface is drawn into a command buffer the add-on saw no render pass begin in "
				"(vkCmdBeginRendering was not called on it, or not through the hook)");
		if (!p->open)
			return fail("the interface is drawn after the render pass the add-on saw begin on its command buffer "
				"had ended (a pass begun some other way)");
		if (!p->restartable)
			return fail(p->why);
		pass = *p;
	}
	const VkImageLayout layout = pass.colour[0].imageLayout;
	if (layout == VK_IMAGE_LAYOUT_UNDEFINED)
		return fail("the window's layout in the pass is not known");
	const VkImage image = vk_handle_of<VkImage>(window);
	const VkImage copy_to = vk_handle_of<VkImage>(dst);

	t_restarting = true;
	g_end_entry(cb);
	VkImageMemoryBarrier in[2] = {
		image_barrier(image, layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT, VK_ACCESS_TRANSFER_READ_BIT),
		image_barrier(copy_to, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT),
	};
	g_pipeline_barrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
		nullptr, 2, in);
	VkImageSubresourceLayers first{};
	first.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	first.layerCount = 1;
	if (resolve) {
		VkImageResolve region{};
		region.srcSubresource = first;
		region.dstSubresource = first;
		region.extent = { w, h, 1 };
		g_resolve_image(cb, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, copy_to, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			1, &region);
	} else {
		VkImageCopy region{};
		region.srcSubresource = first;
		region.dstSubresource = first;
		region.extent = { w, h, 1 };
		g_copy_image(cb, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, copy_to, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
			&region);
	}
	VkImageMemoryBarrier out[2] = {
		image_barrier(image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, layout, VK_ACCESS_TRANSFER_READ_BIT,
			VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT),
		image_barrier(copy_to, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT),
	};
	g_pipeline_barrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0,
		nullptr, 2, out);
	for (uint32_t i = 0; i < pass.info.colorAttachmentCount; ++i) {
		pass.colour[i].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
		if (pass.colour[i].pNext != nullptr)
			pass.colour[i].pNext = &pass.colour_flags[i];
	}
	if (pass.depth.pNext != nullptr)
		pass.depth.pNext = &pass.depth_flags;
	if (pass.stencil.pNext != nullptr)
		pass.stencil.pNext = &pass.stencil_flags;
	pass.depth.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
	pass.stencil.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
	pass.info.pColorAttachments = pass.colour;
	pass.info.pDepthAttachment = pass.has_depth ? &pass.depth : nullptr;
	pass.info.pStencilAttachment = pass.has_stencil ? &pass.stencil : nullptr;
	g_begin_entry(cb, &pass.info);
	t_restarting = false;
	return true;
}

}
