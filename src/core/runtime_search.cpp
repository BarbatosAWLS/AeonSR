#include "aeon_sr/core/runtime_search.hpp"

#include <cstring>
#include <utility>

namespace aeon_sr {
namespace {

void add_unique(std::vector<std::wstring> &out, std::wstring dir)
{
	while (dir.size() > 1u && (dir.back() == L'\\' || dir.back() == L'/'))
		dir.pop_back();
	if (dir.empty())
		return;
	for (const std::wstring &o : out) {
		if (_wcsicmp(o.c_str(), dir.c_str()) == 0)
			return;
	}
	out.push_back(std::move(dir));
}

}

std::wstring path_under(const std::wstring &dir, const wchar_t *file_name)
{
	const std::wstring name = file_name != nullptr ? std::wstring(file_name) : std::wstring();
	if (dir.empty())
		return name;
	std::wstring out = dir;
	if (out.back() != L'\\' && out.back() != L'/')
		out += L'\\';
	out += name;
	return out;
}

std::vector<std::wstring> runtime_search_dirs(const std::wstring &addon_dir,
	const std::wstring &exe_dir)
{
	std::vector<std::wstring> out;
	out.reserve(4u);
	for (const std::wstring *base : { &addon_dir, &exe_dir }) {
		if (base->empty())
			continue;
		add_unique(out, *base);
		add_unique(out, path_under(*base, L"runtime"));
	}
	return out;
}

std::vector<std::wstring> preferred_search_dirs(const std::wstring &addon_dir,
	const std::wstring &exe_dir, const wchar_t *preferred_subdir)
{
	const std::vector<std::wstring> stock = runtime_search_dirs(addon_dir, exe_dir);
	if (preferred_subdir == nullptr || *preferred_subdir == L'\0')
		return stock;
	std::vector<std::wstring> out;
	out.reserve(stock.size() * 2u);
	for (const std::wstring &d : stock)
		add_unique(out, path_under(d, preferred_subdir));
	for (const std::wstring &d : stock)
		add_unique(out, d);
	return out;
}

std::vector<std::wstring> fsr_search_dirs(const std::wstring &addon_dir,
	const std::wstring &exe_dir)
{
	return preferred_search_dirs(addon_dir, exe_dir, kFsrPreferredSubdir);
}

std::vector<std::wstring> runtime_candidates(const std::vector<std::wstring> &dirs,
	const wchar_t *file_name)
{
	std::vector<std::wstring> out;
	out.reserve(dirs.size());
	for (const std::wstring &d : dirs)
		out.push_back(path_under(d, file_name));
	return out;
}

}
