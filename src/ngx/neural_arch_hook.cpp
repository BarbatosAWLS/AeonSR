#include "aeon_sr/ngx/neural_arch_hook.hpp"

#include <Windows.h>

#include <atomic>
#include <cstring>

namespace aeon_sr {
namespace {

using PfnGetProc = FARPROC(WINAPI *)(HMODULE, LPCSTR);
using PfnQuery = void *(*)(unsigned int);
using PfnArchInfo = int (*)(void *, void *);

constexpr unsigned int kGetArchInfoId = 0xD8265D24u;
constexpr int kNvApiError = -1;

std::atomic<PfnGetProc> g_get_proc{ nullptr };
std::atomic<PfnQuery> g_query{ nullptr };
std::atomic<PfnArchInfo> g_arch_info{ nullptr };
std::atomic<uint32_t> g_report{ 0u };

std::atomic<uint32_t> g_query_resolved{ 0u };
std::atomic<uint32_t> g_direct_resolved{ 0u };
std::atomic<uint32_t> g_arch_asked{ 0u };
std::atomic<uint32_t> g_arch_substituted{ 0u };
std::atomic<uint32_t> g_last_true{ 0u };

struct ArchInfo {
	unsigned int version;
	unsigned int architecture;
	unsigned int implementation;
	unsigned int revision;
};

int hooked_arch_info(void *gpu, void *info)
{
	const PfnArchInfo real = g_arch_info.load();
	if (real == nullptr)
		return kNvApiError;
	const int r = real(gpu, info);
	g_arch_asked.fetch_add(1u);
	if (r == 0 && info != nullptr) {
		auto *const a = static_cast<ArchInfo *>(info);
		g_last_true.store(a->architecture);
		const uint32_t report = g_report.load();
		if (report != 0u) {
			a->architecture = report;
			g_arch_substituted.fetch_add(1u);
		}
	}
	return r;
}

void *hooked_query(unsigned int id)
{
	const PfnQuery real = g_query.load();
	if (real == nullptr)
		return nullptr;
	void *const fn = real(id);
	if (id != kGetArchInfoId || fn == nullptr)
		return fn;
	g_arch_info.store(static_cast<PfnArchInfo>(fn));
	return reinterpret_cast<void *>(&hooked_arch_info);
}

FARPROC WINAPI hooked_get_proc(HMODULE module, LPCSTR name)
{
	const PfnGetProc real = g_get_proc.load();
	if (real == nullptr)
		return nullptr;
	const FARPROC fn = real(module, name);
	if (fn == nullptr || (reinterpret_cast<ULONG_PTR>(name) >> 16) == 0u)
		return fn;
	if (std::strcmp(name, "nvapi_QueryInterface") == 0) {
		g_query.store(reinterpret_cast<PfnQuery>(fn));
		g_query_resolved.fetch_add(1u);
		return reinterpret_cast<FARPROC>(&hooked_query);
	}
	if (std::strcmp(name, "nvapi_Direct_GetMethod") == 0)
		g_direct_resolved.fetch_add(1u);
	return fn;
}

void **get_proc_slot(HMODULE module) noexcept
{
	auto *const base = reinterpret_cast<unsigned char *>(module);
	const auto *const dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
	if (dos->e_magic != IMAGE_DOS_SIGNATURE)
		return nullptr;
	const auto *const nt = reinterpret_cast<const IMAGE_NT_HEADERS *>(base + dos->e_lfanew);
	if (nt->Signature != IMAGE_NT_SIGNATURE ||
		nt->OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_IMPORT)
		return nullptr;
	const IMAGE_DATA_DIRECTORY &dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
	if (dir.VirtualAddress == 0u || dir.Size == 0u)
		return nullptr;

	HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
	const PfnGetProc known = g_get_proc.load();
	const void *const real = known != nullptr ? reinterpret_cast<void *>(known)
		: kernel32 != nullptr ? reinterpret_cast<void *>(GetProcAddress(kernel32, "GetProcAddress"))
		: nullptr;

	for (auto *d = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR *>(base + dir.VirtualAddress);
			d->Name != 0u; ++d) {
		if (d->FirstThunk == 0u)
			continue;
		auto *const iat = reinterpret_cast<IMAGE_THUNK_DATA *>(base + d->FirstThunk);
		const auto *const names = d->OriginalFirstThunk != 0u
			? reinterpret_cast<const IMAGE_THUNK_DATA *>(base + d->OriginalFirstThunk) : nullptr;
		for (size_t i = 0; iat[i].u1.Function != 0u; ++i) {
			if (names != nullptr) {
				if (names[i].u1.AddressOfData == 0u || IMAGE_SNAP_BY_ORDINAL(names[i].u1.Ordinal))
					continue;
				const auto *const ibn = reinterpret_cast<const IMAGE_IMPORT_BY_NAME *>(
					base + names[i].u1.AddressOfData);
				if (std::strcmp(reinterpret_cast<const char *>(ibn->Name), "GetProcAddress") == 0)
					return reinterpret_cast<void **>(&iat[i].u1.Function);
			} else if (real != nullptr && reinterpret_cast<const void *>(iat[i].u1.Function) == real) {
				return reinterpret_cast<void **>(&iat[i].u1.Function);
			}
		}
	}
	return nullptr;
}

bool write_slot(void **slot, void *value) noexcept
{
	DWORD old = 0;
	if (!VirtualProtect(slot, sizeof(void *), PAGE_READWRITE, &old))
		return false;
	*slot = value;
	VirtualProtect(slot, sizeof(void *), old, &old);
	return true;
}

}

bool neural_arch_hook_install(void *module) noexcept
{
	if (module == nullptr)
		return false;
	void **const slot = get_proc_slot(static_cast<HMODULE>(module));
	if (slot == nullptr)
		return false;
	const auto hook = reinterpret_cast<void *>(&hooked_get_proc);
	if (*slot == hook)
		return true;
	PfnGetProc expected = nullptr;
	g_get_proc.compare_exchange_strong(expected, reinterpret_cast<PfnGetProc>(*slot));
	return write_slot(slot, hook);
}

bool neural_arch_hook_remove(void *module) noexcept
{
	g_query.store(nullptr);
	g_arch_info.store(nullptr);
	g_report.store(0u);
	if (module == nullptr)
		return false;
	void **const slot = get_proc_slot(static_cast<HMODULE>(module));
	if (slot == nullptr)
		return false;
	const PfnGetProc real = g_get_proc.load();
	if (*slot != reinterpret_cast<void *>(&hooked_get_proc) || real == nullptr)
		return true;
	return write_slot(slot, reinterpret_cast<void *>(real));
}

void neural_arch_hook_report(uint32_t architecture) noexcept
{
	g_report.store(architecture);
}

uint32_t neural_arch_hook_reporting() noexcept
{
	return g_report.load();
}

NeuralArchHookStats neural_arch_hook_stats() noexcept
{
	NeuralArchHookStats s;
	s.query_resolved = g_query_resolved.load();
	s.direct_resolved = g_direct_resolved.load();
	s.arch_asked = g_arch_asked.load();
	s.arch_substituted = g_arch_substituted.load();
	s.last_true_architecture = g_last_true.load();
	return s;
}

void neural_arch_hook_reset_stats() noexcept
{
	g_query_resolved.store(0u);
	g_direct_resolved.store(0u);
	g_arch_asked.store(0u);
	g_arch_substituted.store(0u);
	g_last_true.store(0u);
}

}
