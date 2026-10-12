#include "aeon_sr/jitter/shader_position.hpp"

#define SPV_ENABLE_UTILITY_CODE
#include <spirv.hpp>

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace aeon_sr {
namespace {

namespace spirv_pos {

using namespace spv;

constexpr uint32_t kNone = 0xFFFFFFFFu;
constexpr uint32_t kMaxBound = 1u << 20;
constexpr uint32_t kMaxPath = 8u;
constexpr uint32_t kMaxDepth = 16u;
constexpr uint32_t kMaxContexts = 1024u;
constexpr uint64_t kMaxSteps = 4000000u;
constexpr uint32_t kAll = 0xFFFFu;

constexpr uint8_t kFromUniform = 1u, kFromInput = 2u, kFromOther = 4u;

struct Instr {
	uint32_t at = 0;
	uint16_t op = 0;
	uint16_t wc = 0;
};

struct Block {
	uint32_t label = 0;
	uint32_t last = kNone;
	uint32_t func = 0;
	std::vector<uint32_t> preds;
	std::vector<uint32_t> mem;
};

struct Func {
	uint32_t id = 0;
	uint32_t first_block = 0, end_block = 0;
	std::vector<uint32_t> params;
	std::vector<uint32_t> calls_in;
	std::vector<uint32_t> returns;
};

struct EntryPoint {
	uint32_t func = 0;
	size_t name = 0, name_end = 0;
};

struct Loc {
	uint32_t root = 0;
	uint32_t depth = 0;
	uint32_t path[kMaxPath]{};
	bool truncated = false;
	uint32_t comps = 0;
};

struct Item {
	uint32_t id = 0;
	uint32_t ctx = 0;
	uint32_t lanes = 0;
};

struct Weight {
	bool matrix = false;
	uint32_t arith = 0;
	uint32_t terms = 0;
};

bool is_terminator(uint32_t op) noexcept
{
	switch (op) {
	case OpBranch:
	case OpBranchConditional:
	case OpSwitch:
	case OpKill:
	case OpReturn:
	case OpReturnValue:
	case OpUnreachable:
	case OpTerminateInvocation:
		return true;
	default:
		return false;
	}
}

bool is_atomic_write(uint32_t op) noexcept
{
	return (op >= OpAtomicStore && op <= OpAtomicXor) || op == OpAtomicFAddEXT;
}

bool is_constant(uint32_t op) noexcept
{
	switch (op) {
	case OpUndef:
	case OpConstantTrue:
	case OpConstantFalse:
	case OpConstant:
	case OpConstantComposite:
	case OpConstantNull:
	case OpSpecConstantTrue:
	case OpSpecConstantFalse:
	case OpSpecConstant:
	case OpSpecConstantComposite:
	case OpSpecConstantOp:
		return true;
	default:
		return false;
	}
}

bool uniform_storage(uint32_t sc) noexcept
{
	return sc == StorageClassUniformConstant || sc == StorageClassUniform || sc == StorageClassPushConstant ||
		sc == StorageClassStorageBuffer || sc == StorageClassShaderRecordBufferKHR;
}

bool glsl_per_component(uint32_t inst) noexcept
{
	return (inst >= 1u && inst <= 32u) || (inst >= 37u && inst <= 50u) || inst == 53u || (inst >= 79u && inst <= 81u);
}

bool glsl_takes_pointer(uint32_t inst) noexcept
{
	return inst == 35u || inst == 51u || (inst >= 76u && inst <= 78u);
}

constexpr uint32_t kGlslFma = 50u;

class Reader {
public:
	Reader(const uint32_t *w, size_t n) : w_(w), n_(n) {}

	bool parse();
	JitterVertexClass classify(const char *entry);

private:
	uint32_t word(uint32_t index, uint32_t k) const noexcept
	{
		const Instr &in = instrs_[index];
		return k < in.wc ? w_[in.at + k] : 0u;
	}
	uint32_t def(uint32_t id) const noexcept { return id < bound_ ? def_[id] : kNone; }
	uint32_t op_of(uint32_t id) const noexcept
	{
		const uint32_t d = def(id);
		return d != kNone ? instrs_[d].op : 0u;
	}
	uint32_t type_of(uint32_t id) const noexcept;
	uint32_t vector_size(uint32_t type) const noexcept;
	uint32_t member_type(uint32_t type, uint32_t index) const noexcept;
	bool constant_index(uint32_t id, uint32_t *out) const noexcept;
	uint32_t lanes_of(uint32_t id) const noexcept
	{
		const uint32_t n = vector_size(type_of(id));
		return n == 0u ? 1u : n >= 16u ? kAll : (1u << n) - 1u;
	}
	bool literal_is(size_t from, size_t end, const char *s) const noexcept;

	bool resolve(uint32_t ptr, Loc &out, uint32_t guard = 0);
	void finish_loc(Loc &l) const noexcept;
	uint32_t storage_of(uint32_t root) const noexcept;
	bool step() noexcept
	{
		if (++steps_ > kMaxSteps)
			fail_ = true;
		return !fail_;
	}

	uint32_t push_ctx(uint32_t ctx, uint32_t call);
	uint32_t ctx_depth(uint32_t ctx) const noexcept
	{
		uint32_t d = 0;
		for (; ctx != 0u && d <= kMaxDepth; ctx = ctxs_[ctx].parent)
			++d;
		return d;
	}

	bool writes_global(uint32_t fi, uint32_t root, uint32_t depth);
	uint32_t written(uint32_t index, const Loc &loc, uint32_t lanes, uint32_t ctx, std::vector<Item> &out, uint32_t depth);
	uint32_t walk(uint32_t block, uint32_t end, const Loc &loc, uint32_t lanes, uint32_t ctx, std::vector<Item> &out,
		uint32_t depth);
	uint32_t walk_exit(uint32_t fi, const Loc &loc, uint32_t lanes, uint32_t ctx, std::vector<Item> &out, uint32_t depth);
	void before_start(uint32_t fi, const Loc &loc, uint32_t lanes, uint32_t ctx, std::vector<Item> &out, uint32_t depth);
	bool load_values(uint32_t index, uint32_t ctx, uint32_t lanes, std::vector<Item> &out);
	template <class Fn>
	void callers(uint32_t fi, uint32_t ctx, Fn &&fn);

	uint8_t source(uint32_t id, uint32_t ctx, uint32_t lanes, uint32_t depth);
	void weigh_product(uint32_t index, uint32_t ctx, uint32_t a, uint32_t la, uint32_t b, uint32_t lb, uint32_t addend,
		uint32_t laddend, std::unordered_set<uint32_t> &arith, std::unordered_set<uint32_t> &terms);
	template <class Fn>
	bool parameter_args(uint32_t param, uint32_t ctx, Fn &&fn);
	void trace(std::vector<Item> start, Weight &wt);

	const uint32_t *w_;
	size_t n_;
	uint32_t bound_ = 0;
	std::vector<Instr> instrs_;
	std::vector<uint32_t> def_;
	std::vector<uint32_t> block_of_;
	std::vector<Block> blocks_;
	std::vector<Func> funcs_;
	std::unordered_map<uint32_t, uint32_t> func_of_id_;
	std::unordered_map<uint32_t, std::pair<uint32_t, uint32_t>> param_of_;
	std::unordered_map<uint32_t, std::pair<bool, Loc>> resolved_;
	std::vector<EntryPoint> entries_;
	std::vector<uint32_t> pos_vars_;
	std::vector<std::pair<uint32_t, uint32_t>> pos_members_;
	std::vector<uint32_t> outputs_;
	uint32_t glsl_ = 0;

	struct Ctx {
		uint32_t parent = 0, call = 0;
	};
	std::vector<Ctx> ctxs_{ Ctx{} };
	std::unordered_map<uint64_t, uint32_t> ctx_index_;
	std::unordered_map<uint64_t, int8_t> writes_;
	std::unordered_map<uint64_t, uint8_t> sources_;
	struct Loaded {
		bool ok = false;
		std::vector<Item> items;
	};
	std::unordered_map<uint64_t, Loaded> loads_;
	uint64_t steps_ = 0;
	bool fail_ = false;
};

uint32_t Reader::type_of(uint32_t id) const noexcept
{
	const uint32_t d = def(id);
	if (d == kNone)
		return 0u;
	bool result = false, type = false;
	HasResultAndType(static_cast<Op>(instrs_[d].op), &result, &type);
	return type ? word(d, 1) : 0u;
}

uint32_t Reader::vector_size(uint32_t type) const noexcept
{
	const uint32_t d = def(type);
	if (d == kNone || instrs_[d].op != OpTypeVector)
		return 0u;
	const uint32_t n = word(d, 3);
	return n <= 16u ? n : 0u;
}

uint32_t Reader::member_type(uint32_t type, uint32_t index) const noexcept
{
	const uint32_t d = def(type);
	if (d == kNone)
		return 0u;
	switch (instrs_[d].op) {
	case OpTypeStruct:
		return index != kNone && index + 2u < instrs_[d].wc ? word(d, index + 2u) : 0u;
	case OpTypeArray:
	case OpTypeRuntimeArray:
	case OpTypeVector:
	case OpTypeMatrix:
		return word(d, 2);
	case OpTypePointer:
		return 0u;
	default:
		return 0u;
	}
}

bool Reader::constant_index(uint32_t id, uint32_t *out) const noexcept
{
	const uint32_t d = def(id);
	if (d == kNone || instrs_[d].op != OpConstant || instrs_[d].wc < 4u)
		return false;
	const uint32_t t = def(word(d, 1));
	if (t == kNone || instrs_[t].op != OpTypeInt)
		return false;
	if (word(t, 2) == 64u && (instrs_[d].wc < 5u || word(d, 4) != 0u))
		return false;
	*out = word(d, 3);
	return *out != kNone;
}

bool Reader::literal_is(size_t from, size_t end, const char *s) const noexcept
{
	const size_t len = std::strlen(s);
	for (size_t i = 0;; ++i) {
		const size_t wi = from + i / 4u;
		if (wi >= end)
			return false;
		const char c = static_cast<char>((w_[wi] >> (8u * (i % 4u))) & 0xFFu);
		if (i == len)
			return c == '\0';
		if (c != s[i])
			return false;
	}
}

bool Reader::parse()
{
	if (n_ < 5u || w_[0] != MagicNumber)
		return false;
	bound_ = w_[3];
	if (bound_ == 0u || bound_ > kMaxBound)
		return false;
	def_.assign(bound_, kNone);
	uint32_t cur_func = kNone, cur_block = kNone;
	for (size_t p = 5; p < n_;) {
		const uint32_t wc = w_[p] >> 16;
		const uint32_t op = w_[p] & 0xFFFFu;
		if (wc == 0u || wc > n_ - p)
			return false;
		const uint32_t index = static_cast<uint32_t>(instrs_.size());
		instrs_.push_back(Instr{ static_cast<uint32_t>(p), static_cast<uint16_t>(op), static_cast<uint16_t>(wc) });
		block_of_.push_back(kNone);
		bool has_result = false, has_type = false;
		HasResultAndType(static_cast<Op>(op), &has_result, &has_type);
		if (has_result) {
			const uint32_t k = has_type ? 2u : 1u;
			if (wc <= k)
				return false;
			const uint32_t id = w_[p + k];
			if (id == 0u || id >= bound_ || def_[id] != kNone)
				return false;
			def_[id] = index;
		}
		switch (op) {
		case OpEntryPoint:
			if (wc >= 4u && w_[p + 1] == ExecutionModelVertex)
				entries_.push_back(EntryPoint{ w_[p + 2], p + 3, p + wc });
			break;
		case OpExtInstImport:
			if (wc >= 3u && literal_is(p + 2, p + wc, "GLSL.std.450"))
				glsl_ = w_[p + 1];
			break;
		case OpDecorate:
			if (wc >= 4u && w_[p + 2] == DecorationBuiltIn && w_[p + 3] == BuiltInPosition)
				pos_vars_.push_back(w_[p + 1]);
			break;
		case OpMemberDecorate:
			if (wc >= 5u && w_[p + 3] == DecorationBuiltIn && w_[p + 4] == BuiltInPosition)
				pos_members_.emplace_back(w_[p + 1], w_[p + 2]);
			break;
		case OpVariable:
			if (wc >= 4u && w_[p + 3] == StorageClassOutput)
				outputs_.push_back(w_[p + 2]);
			break;
		default:
			break;
		}
		if (op == OpFunction) {
			if (cur_func != kNone || wc < 5u)
				return false;
			Func f;
			f.id = w_[p + 2];
			f.first_block = f.end_block = static_cast<uint32_t>(blocks_.size());
			cur_func = static_cast<uint32_t>(funcs_.size());
			func_of_id_[f.id] = cur_func;
			funcs_.push_back(std::move(f));
		} else if (op == OpFunctionParameter) {
			if (cur_func == kNone || cur_block != kNone || funcs_[cur_func].first_block != blocks_.size())
				return false;
			param_of_[w_[p + 2]] = { cur_func, static_cast<uint32_t>(funcs_[cur_func].params.size()) };
			funcs_[cur_func].params.push_back(w_[p + 2]);
		} else if (op == OpLabel) {
			if (cur_func == kNone || cur_block != kNone)
				return false;
			Block b;
			b.label = index;
			b.func = cur_func;
			cur_block = static_cast<uint32_t>(blocks_.size());
			blocks_.push_back(std::move(b));
			block_of_[index] = cur_block;
		} else if (op == OpFunctionEnd) {
			if (cur_func == kNone || cur_block != kNone)
				return false;
			funcs_[cur_func].end_block = static_cast<uint32_t>(blocks_.size());
			cur_func = kNone;
		} else if (cur_func != kNone) {
			if (cur_block == kNone) {
				if (op != OpLine && op != OpNoLine)
					return false;
			} else {
				block_of_[index] = cur_block;
				if (op == OpStore || op == OpFunctionCall || op == OpCopyMemory || op == OpCopyMemorySized ||
					is_atomic_write(op))
					blocks_[cur_block].mem.push_back(index);
				if (is_terminator(op)) {
					blocks_[cur_block].last = index;
					cur_block = kNone;
				}
			}
		}
		p += wc;
	}
	if (cur_func != kNone)
		return false;
	for (uint32_t b = 0; b < blocks_.size(); ++b) {
		const uint32_t t = blocks_[b].last;
		if (t == kNone)
			return false;
		const Instr &in = instrs_[t];
		std::vector<uint32_t> targets;
		switch (in.op) {
		case OpBranch:
			targets.push_back(word(t, 1));
			break;
		case OpBranchConditional:
			targets.push_back(word(t, 2));
			targets.push_back(word(t, 3));
			break;
		case OpSwitch: {
			const uint32_t sel_type = def(type_of(word(t, 1)));
			const uint32_t literal = sel_type != kNone && instrs_[sel_type].op == OpTypeInt && word(sel_type, 2) == 64u ? 2u : 1u;
			if (in.wc < 3u || (in.wc - 3u) % (literal + 1u) != 0u)
				return false;
			targets.push_back(word(t, 2));
			for (uint32_t k = 3u + literal; k < in.wc; k += literal + 1u)
				targets.push_back(word(t, k));
			break;
		}
		case OpReturn:
		case OpReturnValue:
			funcs_[blocks_[b].func].returns.push_back(b);
			break;
		default:
			break;
		}
		for (const uint32_t label : targets) {
			const uint32_t d = def(label);
			if (d == kNone || instrs_[d].op != OpLabel || block_of_[d] == kNone)
				return false;
			const uint32_t to = block_of_[d];
			if (blocks_[to].func != blocks_[b].func)
				return false;
			blocks_[to].preds.push_back(b);
		}
		for (const uint32_t m : blocks_[b].mem) {
			if (instrs_[m].op != OpFunctionCall)
				continue;
			const auto it = func_of_id_.find(word(m, 3));
			if (it == func_of_id_.end() || instrs_[m].wc < 4u)
				return false;
			funcs_[it->second].calls_in.push_back(m);
		}
	}
	for (const Func &f : funcs_) {
		if (f.first_block == f.end_block || !blocks_[f.first_block].preds.empty())
			return false;
	}
	return true;
}

bool Reader::resolve(uint32_t ptr, Loc &out, uint32_t guard)
{
	if (guard > 64u || !step())
		return false;
	if (guard == 0u) {
		const auto it = resolved_.find(ptr);
		if (it != resolved_.end()) {
			out = it->second.second;
			return it->second.first;
		}
		Loc l;
		const bool ok = resolve(ptr, l, 1u);
		resolved_.emplace(ptr, std::make_pair(ok, l));
		out = l;
		return ok;
	}
	const uint32_t d = def(ptr);
	if (d == kNone)
		return false;
	const Instr &in = instrs_[d];
	const auto append = [&](uint32_t from) {
		for (uint32_t k = from; k < in.wc; ++k) {
			uint32_t v = kNone;
			if (!constant_index(word(d, k), &v))
				v = kNone;
			if (out.depth < kMaxPath)
				out.path[out.depth++] = v;
			else
				out.truncated = true;
		}
	};
	switch (in.op) {
	case OpVariable:
	case OpFunctionParameter:
		out = Loc{};
		out.root = ptr;
		return true;
	case OpAccessChain:
	case OpInBoundsAccessChain:
		if (!resolve(word(d, 3), out, guard + 1u))
			return false;
		append(4u);
		return true;
	case OpPtrAccessChain:
	case OpInBoundsPtrAccessChain: {
		uint32_t element = kNone;
		if (!constant_index(word(d, 4), &element) || element != 0u || !resolve(word(d, 3), out, guard + 1u))
			return false;
		append(5u);
		return true;
	}
	case OpCopyObject:
		return resolve(word(d, 3), out, guard + 1u);
	default:
		return false;
	}
}

void Reader::finish_loc(Loc &l) const noexcept
{
	l.comps = 0;
	if (l.truncated)
		return;
	const uint32_t pt = def(type_of(l.root));
	if (pt == kNone || instrs_[pt].op != OpTypePointer)
		return;
	uint32_t t = word(pt, 3);
	for (uint32_t i = 0; i < l.depth && t != 0u; ++i)
		t = member_type(t, l.path[i]);
	l.comps = vector_size(t);
}

uint32_t Reader::storage_of(uint32_t root) const noexcept
{
	const uint32_t d = def(root);
	if (d == kNone || instrs_[d].op != OpVariable)
		return kNone;
	return word(d, 3);
}

uint32_t Reader::push_ctx(uint32_t ctx, uint32_t call)
{
	const uint64_t key = (static_cast<uint64_t>(ctx) << 32) | call;
	const auto it = ctx_index_.find(key);
	if (it != ctx_index_.end())
		return it->second;
	if (ctxs_.size() >= kMaxContexts || ctx_depth(ctx) >= kMaxDepth) {
		fail_ = true;
		return 0;
	}
	ctxs_.push_back(Ctx{ ctx, call });
	const uint32_t index = static_cast<uint32_t>(ctxs_.size() - 1u);
	ctx_index_.emplace(key, index);
	return index;
}

bool Reader::writes_global(uint32_t fi, uint32_t root, uint32_t depth)
{
	const uint64_t key = (static_cast<uint64_t>(fi) << 32) | root;
	const auto it = writes_.find(key);
	if (it != writes_.end())
		return it->second > 0;
	if (depth > kMaxDepth) {
		fail_ = true;
		return true;
	}
	writes_[key] = 0;
	bool any = false;
	const Func &f = funcs_[fi];
	for (uint32_t b = f.first_block; b < f.end_block && !any; ++b) {
		for (const uint32_t m : blocks_[b].mem) {
			const uint32_t op = instrs_[m].op;
			if (op == OpFunctionCall) {
				const auto callee = func_of_id_.find(word(m, 3));
				any = callee == func_of_id_.end() || writes_global(callee->second, root, depth + 1u);
			} else {
				Loc l;
				const uint32_t ptr = is_atomic_write(op) ? word(m, op == OpAtomicStore ? 1 : 3) : word(m, 1);
				any = !resolve(ptr, l) || l.root == root;
			}
			if (any)
				break;
		}
	}
	writes_[key] = any ? 1 : 0;
	return any;
}

namespace {

bool definite(const Loc &l, uint32_t upto) noexcept
{
	if (l.truncated && upto >= l.depth)
		return false;
	for (uint32_t i = 0; i < upto && i < l.depth; ++i)
		if (l.path[i] == kNone)
			return false;
	return true;
}

bool overlaps(const Loc &a, const Loc &b) noexcept
{
	if (a.root != b.root)
		return false;
	const uint32_t n = std::min(a.depth, b.depth);
	for (uint32_t i = 0; i < n; ++i)
		if (a.path[i] != kNone && b.path[i] != kNone && a.path[i] != b.path[i])
			return false;
	return true;
}

}

uint32_t Reader::written(uint32_t index, const Loc &loc, uint32_t lanes, uint32_t ctx, std::vector<Item> &out,
	uint32_t depth)
{
	const uint32_t op = instrs_[index].op;
	if (op == OpStore) {
		Loc s;
		if (!resolve(word(index, 1), s)) {
			fail_ = true;
			return 0;
		}
		if (!overlaps(s, loc))
			return lanes;
		const uint32_t value = word(index, 2);
		if (s.depth <= loc.depth) {
			uint32_t value_lanes = kAll;
			if (s.depth == loc.depth && !s.truncated && !loc.truncated) {
				value_lanes = lanes;
			} else if (loc.depth == s.depth + 1u && !loc.truncated && loc.path[s.depth] != kNone &&
				loc.path[s.depth] < 16u && vector_size(type_of(value)) != 0u) {
				value_lanes = 1u << loc.path[s.depth];
			}
			out.push_back(Item{ value, ctx, value_lanes });
			const bool covers = definite(s, s.depth) && definite(loc, s.depth) && !s.truncated;
			return covers ? 0u : lanes;
		}
		if (s.depth == loc.depth + 1u && loc.comps != 0u && !s.truncated && definite(s, s.depth) &&
			definite(loc, loc.depth)) {
			const uint32_t c = s.path[loc.depth];
			if (c >= 16u)
				return lanes;
			const uint32_t bit = 1u << c;
			if ((lanes & bit) == 0u)
				return lanes;
			out.push_back(Item{ value, ctx, 1u });
			return lanes & ~bit;
		}
		out.push_back(Item{ value, ctx, kAll });
		return lanes;
	}
	if (op == OpFunctionCall) {
		const auto it = func_of_id_.find(word(index, 3));
		if (it == func_of_id_.end()) {
			fail_ = true;
			return 0;
		}
		const uint32_t fi = it->second;
		const uint32_t sc = storage_of(loc.root);
		const bool global = sc != kNone && sc != StorageClassFunction;
		if (global && writes_global(fi, loc.root, depth)) {
			if (depth >= kMaxDepth) {
				fail_ = true;
				return 0;
			}
			const uint32_t inner = push_ctx(ctx, index);
			if (fail_)
				return 0;
			lanes = walk_exit(fi, loc, lanes, inner, out, depth + 1u);
			if (fail_ || lanes == 0u)
				return 0;
		}
		for (uint32_t k = 4; k < instrs_[index].wc; ++k) {
			const uint32_t arg = word(index, k);
			const uint32_t at = def(type_of(arg));
			if (at == kNone || instrs_[at].op != OpTypePointer)
				continue;
			Loc a;
			if (!resolve(arg, a)) {
				fail_ = true;
				return 0;
			}
			if (!overlaps(a, loc))
				continue;
			if (a.depth != 0u || a.truncated || k - 4u >= funcs_[fi].params.size() || depth >= kMaxDepth) {
				fail_ = true;
				return 0;
			}
			Loc inner_loc = loc;
			inner_loc.root = funcs_[fi].params[k - 4u];
			const uint32_t inner = push_ctx(ctx, index);
			if (fail_)
				return 0;
			lanes = walk_exit(fi, inner_loc, lanes, inner, out, depth + 1u);
			if (fail_ || lanes == 0u)
				return 0;
		}
		return lanes;
	}
	Loc t;
	const uint32_t ptr = is_atomic_write(op) ? word(index, op == OpAtomicStore ? 1 : 3) : word(index, 1);
	if (!resolve(ptr, t) || overlaps(t, loc)) {
		fail_ = true;
		return 0;
	}
	return lanes;
}

uint32_t Reader::walk(uint32_t block, uint32_t end, const Loc &loc, uint32_t lanes, uint32_t ctx,
	std::vector<Item> &out, uint32_t depth)
{
	struct Pending {
		uint32_t block, end, lanes;
	};
	std::vector<Pending> stack{ Pending{ block, end, lanes } };
	std::unordered_map<uint32_t, uint32_t> seen;
	uint32_t at_start = 0;
	while (!stack.empty() && !fail_) {
		const Pending p = stack.back();
		stack.pop_back();
		uint32_t want = p.lanes;
		const Block &b = blocks_[p.block];
		for (size_t k = b.mem.size(); k-- > 0 && want != 0u;) {
			const uint32_t m = b.mem[k];
			if (m >= p.end)
				continue;
			if (!step())
				return 0;
			want = written(m, loc, want, ctx, out, depth);
			if (fail_)
				return 0;
		}
		if (want == 0u)
			continue;
		if (p.block == funcs_[b.func].first_block) {
			at_start |= want;
			continue;
		}
		for (const uint32_t pred : b.preds) {
			uint32_t &done = seen[pred];
			const uint32_t fresh = want & ~done;
			if (fresh == 0u)
				continue;
			done |= fresh;
			stack.push_back(Pending{ pred, kNone, fresh });
		}
	}
	return at_start;
}

uint32_t Reader::walk_exit(uint32_t fi, const Loc &loc, uint32_t lanes, uint32_t ctx, std::vector<Item> &out,
	uint32_t depth)
{
	uint32_t through = 0;
	for (const uint32_t r : funcs_[fi].returns) {
		through |= walk(r, kNone, loc, lanes, ctx, out, depth);
		if (fail_)
			return 0;
	}
	return through;
}

template <class Fn>
void Reader::callers(uint32_t fi, uint32_t ctx, Fn &&fn)
{
	if (ctx != 0u) {
		const uint32_t call = ctxs_[ctx].call;
		const auto it = func_of_id_.find(word(call, 3));
		if (it != func_of_id_.end() && it->second == fi) {
			fn(call, ctxs_[ctx].parent);
			return;
		}
	}
	for (const uint32_t call : funcs_[fi].calls_in) {
		fn(call, 0u);
		if (fail_)
			return;
	}
}

template <class Fn>
bool Reader::parameter_args(uint32_t param, uint32_t ctx, Fn &&fn)
{
	const auto p = param_of_.find(param);
	if (p == param_of_.end())
		return false;
	const uint32_t j = p->second.second;
	bool ok = true;
	callers(p->second.first, ctx, [&](uint32_t call, uint32_t outer) {
		if (4u + j >= instrs_[call].wc)
			ok = false;
		else
			fn(word(call, 4u + j), outer);
	});
	return ok;
}

void Reader::before_start(uint32_t fi, const Loc &loc, uint32_t lanes, uint32_t ctx, std::vector<Item> &out,
	uint32_t depth)
{
	if (lanes == 0u || fail_)
		return;
	if (depth > kMaxDepth) {
		fail_ = true;
		return;
	}
	const uint32_t d = def(loc.root);
	if (d == kNone) {
		fail_ = true;
		return;
	}
	const auto initializer = [&](uint32_t var) {
		if (instrs_[var].wc >= 5u)
			out.push_back(Item{ word(var, 4), ctx, loc.depth == 0u ? lanes : kAll });
	};
	if (instrs_[d].op == OpVariable) {
		if (word(d, 3) == StorageClassFunction || funcs_[fi].calls_in.empty()) {
			initializer(d);
			return;
		}
		callers(fi, ctx, [&](uint32_t call, uint32_t outer) {
			const uint32_t caller = blocks_[block_of_[call]].func;
			const uint32_t rest = walk(block_of_[call], call, loc, lanes, outer, out, depth);
			before_start(caller, loc, rest, outer, out, depth + 1u);
		});
		return;
	}
	const auto p = param_of_.find(loc.root);
	if (p == param_of_.end() || p->second.first != fi) {
		fail_ = true;
		return;
	}
	const uint32_t j = p->second.second;
	callers(fi, ctx, [&](uint32_t call, uint32_t outer) {
		Loc a;
		if (4u + j >= instrs_[call].wc || !resolve(word(call, 4u + j), a)) {
			fail_ = true;
			return;
		}
		for (uint32_t i = 0; i < loc.depth; ++i) {
			if (a.depth < kMaxPath)
				a.path[a.depth++] = loc.path[i];
			else
				a.truncated = true;
		}
		a.truncated = a.truncated || loc.truncated;
		finish_loc(a);
		const uint32_t caller = blocks_[block_of_[call]].func;
		const uint32_t rest = walk(block_of_[call], call, a, lanes, outer, out, depth);
		before_start(caller, a, rest, outer, out, depth + 1u);
	});
}

bool Reader::load_values(uint32_t index, uint32_t ctx, uint32_t lanes, std::vector<Item> &out)
{
	const uint64_t key = (static_cast<uint64_t>(index) << 32) | (static_cast<uint64_t>(ctx) << 16) | lanes;
	const auto it = loads_.find(key);
	if (it != loads_.end()) {
		if (!it->second.ok)
			fail_ = true;
		out.insert(out.end(), it->second.items.begin(), it->second.items.end());
		return it->second.ok;
	}
	Loaded got;
	Loc l;
	if (resolve(word(index, 3), l)) {
		finish_loc(l);
		const uint32_t fi = blocks_[block_of_[index]].func;
		const uint32_t want = l.comps != 0u ? lanes : 1u;
		const uint32_t rest = walk(block_of_[index], index, l, want, ctx, got.items, 0u);
		if (!fail_)
			before_start(fi, l, rest, ctx, got.items, 0u);
		got.ok = !fail_;
	}
	if (!got.ok)
		fail_ = true;
	out.insert(out.end(), got.items.begin(), got.items.end());
	loads_.emplace(key, std::move(got));
	return !fail_;
}

uint8_t Reader::source(uint32_t id, uint32_t ctx, uint32_t lanes, uint32_t depth)
{
	if (depth > 24u || fail_ || !step())
		return kFromOther;
	lanes &= lanes_of(id);
	if (lanes == 0u)
		return 0u;
	const uint64_t key = (static_cast<uint64_t>(id) << 36) | (static_cast<uint64_t>(ctx) << 16) | lanes;
	const auto it = sources_.find(key);
	if (it != sources_.end())
		return it->second;
	sources_[key] = 0u;
	const uint32_t d = def(id);
	uint8_t from = kFromOther;
	if (d != kNone) {
		const Instr &in = instrs_[d];
		const auto of = [&](uint32_t x, uint32_t l) { return source(x, ctx, l, depth + 1u); };
		switch (in.op) {
		case OpLoad: {
			Loc l;
			if (!resolve(word(d, 3), l)) {
				from = kFromOther;
				break;
			}
			const uint32_t sc = storage_of(l.root);
			if (sc == StorageClassInput) {
				from = kFromInput;
			} else if (sc != kNone && uniform_storage(sc)) {
				from = kFromUniform;
			} else {
				std::vector<Item> items;
				const bool keep = fail_;
				const bool ok = load_values(d, ctx, lanes, items);
				fail_ = keep;
				from = 0u;
				if (!ok)
					from = kFromOther;
				for (const Item &i : items)
					from |= source(i.id, i.ctx, i.lanes, depth + 1u);
			}
			break;
		}
		case OpCompositeExtract: {
			const uint32_t c = word(d, 3);
			from = vector_size(type_of(c)) != 0u && in.wc == 5u && word(d, 4) < 16u ? of(c, 1u << word(d, 4)) : of(c, kAll);
			break;
		}
		case OpCompositeInsert: {
			const uint32_t i = in.wc == 6u ? word(d, 5) : kNone;
			if (vector_size(type_of(word(d, 4))) != 0u && i < 16u) {
				from = 0u;
				if ((lanes & (1u << i)) != 0u)
					from |= of(word(d, 3), 1u);
				if ((lanes & ~(1u << i)) != 0u)
					from |= of(word(d, 4), lanes & ~(1u << i));
			} else {
				from = static_cast<uint8_t>(of(word(d, 3), kAll) | of(word(d, 4), kAll));
			}
			break;
		}
		case OpVectorShuffle: {
			const uint32_t n1 = vector_size(type_of(word(d, 3)));
			uint32_t m1 = 0, m2 = 0;
			for (uint32_t r = 0; r + 5u < in.wc && r < 16u; ++r) {
				if ((lanes & (1u << r)) == 0u)
					continue;
				const uint32_t c = word(d, 5u + r);
				if (c == kNone)
					continue;
				if (c < n1)
					m1 |= 1u << c;
				else if (c - n1 < 16u)
					m2 |= 1u << (c - n1);
			}
			from = 0u;
			if (m1 != 0u)
				from |= of(word(d, 3), m1);
			if (m2 != 0u)
				from |= of(word(d, 4), m2);
			break;
		}
		case OpCompositeConstruct: {
			from = 0u;
			const bool vec = vector_size(word(d, 1)) != 0u;
			uint32_t pos = 0;
			for (uint32_t k = 3; k < in.wc; ++k) {
				const uint32_t c = word(d, k);
				const uint32_t cn = vector_size(type_of(c));
				const uint32_t width = cn != 0u ? cn : 1u;
				const uint32_t mask = vec ? (pos < 16u ? (lanes >> pos) & ((1u << width) - 1u) : 0u) : kAll;
				pos += width;
				if (mask != 0u)
					from |= of(c, cn != 0u ? mask : 1u);
			}
			break;
		}
		case OpCopyObject:
		case OpCopyLogical:
		case OpFConvert:
		case OpUConvert:
		case OpSConvert:
		case OpConvertSToF:
		case OpConvertUToF:
			from = of(word(d, 3), lanes);
			break;
		case OpBitcast:
			from = of(word(d, 3), vector_size(type_of(word(d, 3))) == vector_size(word(d, 1)) ? lanes : kAll);
			break;
		case OpTranspose:
			from = of(word(d, 3), kAll);
			break;
		case OpSelect:
			from = static_cast<uint8_t>(of(word(d, 4), lanes) | of(word(d, 5), lanes));
			break;
		case OpPhi:
			from = 0u;
			for (uint32_t k = 3; k + 1u < in.wc; k += 2u)
				from |= of(word(d, k), lanes);
			break;
		case OpFunctionParameter: {
			from = 0u;
			const bool known = parameter_args(id, ctx, [&](uint32_t arg, uint32_t outer) {
				from |= source(arg, outer, lanes, depth + 1u);
			});
			if (!known)
				from = kFromOther;
			break;
		}
		case OpFunctionCall: {
			const auto f = func_of_id_.find(word(d, 3));
			if (f == func_of_id_.end() || depth >= kMaxDepth) {
				from = kFromOther;
				break;
			}
			const uint32_t inner = push_ctx(ctx, d);
			if (fail_) {
				from = kFromOther;
				break;
			}
			from = 0u;
			for (const uint32_t r : funcs_[f->second].returns) {
				const uint32_t t = blocks_[r].last;
				from |= instrs_[t].op == OpReturnValue ? source(word(t, 1), inner, lanes, depth + 1u) : kFromOther;
			}
			break;
		}
		default:
			from = is_constant(in.op) ? 0u : kFromOther;
			break;
		}
	}
	sources_[key] = from;
	return from;
}

void Reader::weigh_product(uint32_t index, uint32_t ctx, uint32_t a, uint32_t la, uint32_t b, uint32_t lb,
	uint32_t addend, uint32_t laddend, std::unordered_set<uint32_t> &arith, std::unordered_set<uint32_t> &terms)
{
	const uint8_t sa = source(a, ctx, la, 0u);
	const uint8_t sb = source(b, ctx, lb, 0u);
	const uint8_t sc = addend != 0u ? source(addend, ctx, laddend, 0u) : 0u;
	if (((sa | sb | sc) & kFromUniform) != 0u)
		arith.insert(index);
	if ((sa == kFromUniform && sb == kFromInput) || (sa == kFromInput && sb == kFromUniform))
		terms.insert(index);
}

void Reader::trace(std::vector<Item> stack, Weight &wt)
{
	std::unordered_map<uint64_t, uint32_t> seen;
	std::unordered_set<uint32_t> arith, terms;
	while (!stack.empty() && !fail_) {
		Item it = stack.back();
		stack.pop_back();
		if (!step())
			return;
		const uint32_t d = def(it.id);
		if (d == kNone) {
			fail_ = true;
			return;
		}
		const uint32_t lanes = it.lanes & lanes_of(it.id);
		uint32_t &done = seen[(static_cast<uint64_t>(it.id) << 32) | it.ctx];
		const uint32_t fresh = lanes & ~done;
		if (fresh == 0u)
			continue;
		done |= fresh;
		const Instr &in = instrs_[d];
		const uint32_t ctx = it.ctx;
		const auto push = [&](uint32_t id, uint32_t l) { stack.push_back(Item{ id, ctx, l }); };
		const auto operands = [&](uint32_t from, uint32_t l) {
			for (uint32_t k = from; k < in.wc; ++k)
				push(word(d, k), l);
		};
		if (is_constant(in.op))
			continue;
		switch (in.op) {
		case OpLoad: {
			Loc l;
			if (!resolve(word(d, 3), l)) {
				fail_ = true;
				return;
			}
			const uint32_t sc = storage_of(l.root);
			if (sc == StorageClassInput || (sc != kNone && uniform_storage(sc)))
				break;
			if (!load_values(d, ctx, fresh, stack))
				return;
			break;
		}
		case OpFunctionParameter:
			if (!parameter_args(it.id, ctx, [&](uint32_t arg, uint32_t outer) { stack.push_back(Item{ arg, outer, fresh }); }))
				fail_ = true;
			break;
		case OpFunctionCall: {
			const auto f = func_of_id_.find(word(d, 3));
			if (f == func_of_id_.end()) {
				fail_ = true;
				return;
			}
			const uint32_t inner = push_ctx(ctx, d);
			if (fail_)
				return;
			bool any = false;
			for (const uint32_t r : funcs_[f->second].returns) {
				const uint32_t t = blocks_[r].last;
				if (instrs_[t].op != OpReturnValue)
					continue;
				any = true;
				stack.push_back(Item{ word(t, 1), inner, fresh });
			}
			if (!any)
				fail_ = true;
			break;
		}
		case OpCompositeExtract: {
			const uint32_t c = word(d, 3);
			if (vector_size(type_of(c)) != 0u && in.wc == 5u && word(d, 4) < 16u)
				push(c, 1u << word(d, 4));
			else
				push(c, kAll);
			break;
		}
		case OpCompositeInsert: {
			const uint32_t i = in.wc == 6u ? word(d, 5) : kNone;
			if (vector_size(type_of(word(d, 4))) != 0u && i < 16u) {
				if ((fresh & (1u << i)) != 0u)
					push(word(d, 3), 1u);
				if ((fresh & ~(1u << i)) != 0u)
					push(word(d, 4), fresh & ~(1u << i));
			} else {
				push(word(d, 3), kAll);
				push(word(d, 4), kAll);
			}
			break;
		}
		case OpVectorShuffle: {
			const uint32_t n1 = vector_size(type_of(word(d, 3)));
			uint32_t m1 = 0, m2 = 0;
			for (uint32_t r = 0; r + 5u < in.wc && r < 16u; ++r) {
				if ((fresh & (1u << r)) == 0u)
					continue;
				const uint32_t c = word(d, 5u + r);
				if (c == kNone)
					continue;
				if (c < n1)
					m1 |= 1u << c;
				else if (c - n1 < 16u)
					m2 |= 1u << (c - n1);
			}
			if (m1 != 0u)
				push(word(d, 3), m1);
			if (m2 != 0u)
				push(word(d, 4), m2);
			break;
		}
		case OpCompositeConstruct: {
			const bool vec = vector_size(word(d, 1)) != 0u;
			uint32_t pos = 0;
			for (uint32_t k = 3; k < in.wc; ++k) {
				const uint32_t c = word(d, k);
				const uint32_t cn = vector_size(type_of(c));
				const uint32_t width = cn != 0u ? cn : 1u;
				const uint32_t mask = vec ? (pos < 16u ? (fresh >> pos) & ((1u << width) - 1u) : 0u) : kAll;
				pos += width;
				if (mask != 0u)
					push(c, cn != 0u ? mask : 1u);
			}
			break;
		}
		case OpCopyObject:
		case OpCopyLogical:
			push(word(d, 3), fresh);
			break;
		case OpBitcast:
			push(word(d, 3), vector_size(type_of(word(d, 3))) == vector_size(word(d, 1)) ? fresh : kAll);
			break;
		case OpTranspose:
		case OpMatrixTimesScalar:
		case OpOuterProduct:
		case OpAny:
		case OpAll:
		case OpIAddCarry:
		case OpISubBorrow:
		case OpUMulExtended:
		case OpSMulExtended:
			operands(3, kAll);
			break;
		case OpVectorTimesMatrix:
		case OpMatrixTimesVector:
		case OpMatrixTimesMatrix: {
			const uint8_t s = static_cast<uint8_t>(source(word(d, 3), ctx, kAll, 0u) | source(word(d, 4), ctx, kAll, 0u));
			if ((s & (kFromUniform | kFromInput)) != 0u)
				wt.matrix = true;
			operands(3, kAll);
			break;
		}
		case OpDot: {
			const uint8_t s = static_cast<uint8_t>(source(word(d, 3), ctx, kAll, 0u) | source(word(d, 4), ctx, kAll, 0u));
			if ((s & kFromUniform) != 0u)
				wt.matrix = true;
			operands(3, kAll);
			break;
		}
		case OpVectorTimesScalar:
			weigh_product(d, ctx, word(d, 3), fresh, word(d, 4), 1u, 0u, 0u, arith, terms);
			push(word(d, 3), fresh);
			push(word(d, 4), 1u);
			break;
		case OpFMul:
			weigh_product(d, ctx, word(d, 3), fresh, word(d, 4), fresh, 0u, 0u, arith, terms);
			operands(3, fresh);
			break;
		case OpFAdd:
		case OpFSub:
			if (((source(word(d, 3), ctx, fresh, 0u) | source(word(d, 4), ctx, fresh, 0u)) & kFromUniform) != 0u)
				arith.insert(d);
			operands(3, fresh);
			break;
		case OpSelect:
			push(word(d, 3), vector_size(type_of(word(d, 3))) != 0u ? fresh : 1u);
			push(word(d, 4), fresh);
			push(word(d, 5), fresh);
			break;
		case OpPhi:
			for (uint32_t k = 3; k + 1u < in.wc; k += 2u)
				push(word(d, k), fresh);
			break;
		case OpVectorExtractDynamic:
			push(word(d, 3), kAll);
			push(word(d, 4), 1u);
			break;
		case OpVectorInsertDynamic:
			push(word(d, 3), fresh);
			push(word(d, 4), 1u);
			push(word(d, 5), 1u);
			break;
		case OpBitFieldInsert:
			push(word(d, 3), fresh);
			push(word(d, 4), fresh);
			push(word(d, 5), kAll);
			push(word(d, 6), kAll);
			break;
		case OpBitFieldSExtract:
		case OpBitFieldUExtract:
			push(word(d, 3), fresh);
			push(word(d, 4), kAll);
			push(word(d, 5), kAll);
			break;
		case OpExtInst: {
			if (glsl_ == 0u || word(d, 3) != glsl_ || in.wc < 5u) {
				fail_ = true;
				return;
			}
			const uint32_t inst = word(d, 4);
			if (glsl_takes_pointer(inst)) {
				fail_ = true;
				return;
			}
			if (inst == kGlslFma && in.wc == 8u) {
				weigh_product(d, ctx, word(d, 5), fresh, word(d, 6), fresh, word(d, 7), fresh, arith, terms);
				operands(5, fresh);
			} else {
				operands(5, glsl_per_component(inst) ? fresh : kAll);
			}
			break;
		}
		default:
			if ((in.op >= OpConvertFToU && in.op <= OpQuantizeToF16) || in.op == OpSatConvertSToU ||
				in.op == OpSatConvertUToS || (in.op >= OpSNegate && in.op <= OpFMod) ||
				(in.op >= OpIsNan && in.op <= OpFUnordGreaterThanEqual) ||
				(in.op >= OpShiftRightLogical && in.op <= OpNot) || in.op == OpBitReverse || in.op == OpBitCount) {
				operands(3, fresh);
				break;
			}
			fail_ = true;
			return;
		}
	}
	wt.arith = std::max(wt.arith, static_cast<uint32_t>(arith.size()));
	wt.terms = std::max(wt.terms, static_cast<uint32_t>(terms.size()));
}

JitterVertexClass Reader::classify(const char *entry)
{
	std::vector<uint32_t> entry_funcs;
	const auto pick = [&](bool by_name) {
		for (const EntryPoint &e : entries_) {
			if (by_name && !literal_is(e.name, e.name_end, entry))
				continue;
			const auto it = func_of_id_.find(e.func);
			if (it != func_of_id_.end())
				entry_funcs.push_back(it->second);
		}
	};
	pick(entry != nullptr && entry[0] != '\0');
	if (entry_funcs.empty())
		return JitterVertexClass::Unknown;
	std::vector<Loc> positions;
	for (const uint32_t v : pos_vars_) {
		if (storage_of(v) != StorageClassOutput)
			continue;
		Loc l;
		l.root = v;
		finish_loc(l);
		positions.push_back(l);
	}
	for (const auto &m : pos_members_) {
		for (const uint32_t v : outputs_) {
			const uint32_t pt = def(type_of(v));
			if (pt == kNone || instrs_[pt].op != OpTypePointer || word(pt, 3) != m.first)
				continue;
			Loc l;
			l.root = v;
			l.depth = 1;
			l.path[0] = m.second;
			finish_loc(l);
			positions.push_back(l);
		}
	}
	if (positions.empty())
		return JitterVertexClass::Unknown;
	Weight total;
	bool any = false;
	for (uint32_t c = 0; c < 4u; ++c) {
		std::vector<Item> start;
		for (const uint32_t fi : entry_funcs) {
			for (const Loc &l : positions) {
				const uint32_t lane = l.comps != 0u ? 1u << c : 1u;
				if (l.comps != 0u ? c >= l.comps : c != 0u)
					continue;
				walk_exit(fi, l, lane, 0u, start, 0u);
				if (fail_)
					return JitterVertexClass::Unknown;
			}
		}
		if (start.empty())
			continue;
		any = true;
		Weight wt;
		trace(std::move(start), wt);
		if (fail_)
			return JitterVertexClass::Unknown;
		total.matrix = total.matrix || wt.matrix;
		total.arith = std::max(total.arith, wt.arith);
		total.terms = std::max(total.terms, wt.terms);
	}
	if (!any)
		return JitterVertexClass::Unknown;
	return total.matrix || total.arith >= 3u || total.terms >= 2u ? JitterVertexClass::Transformed
																	: JitterVertexClass::Screen;
}

}

namespace glsl_pos {

constexpr size_t kMaxSource = 1u << 20;
constexpr size_t kMaxTokens = 1u << 18;
constexpr uint32_t kMaxNesting = 64u;
constexpr uint32_t kMaxCalls = 8u;
constexpr uint32_t kMaxParseCalls = 16u;
constexpr uint32_t kMaxExpansion = 32u;
constexpr uint64_t kMaxSteps = 4000000u;
constexpr uint32_t kAllLanes = 0xFu;
constexpr uint8_t kFromUniform = 1u, kFromInput = 2u, kFromOther = 4u;

enum class Tok : uint8_t { Ident, Number, Punct };

struct Token {
	Tok kind = Tok::Punct;
	std::string text;
};

bool ident_start(char c) noexcept
{
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}
bool ident_char(char c) noexcept
{
	return ident_start(c) || (c >= '0' && c <= '9');
}
bool digit(char c) noexcept
{
	return c >= '0' && c <= '9';
}

bool tokenize(const std::string &text, std::vector<Token> &out)
{
	static const char *const kPuncts[] = { "<<=", ">>=", "++", "--", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=",
		"<<", ">>", "<=", ">=", "==", "!=", "&&", "||", "^^", "##" };
	size_t i = 0;
	while (i < text.size()) {
		const char c = text[i];
		if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v') {
			++i;
			continue;
		}
		if (out.size() >= kMaxTokens)
			return false;
		Token t;
		if (ident_start(c)) {
			size_t j = i + 1;
			while (j < text.size() && ident_char(text[j]))
				++j;
			t.kind = Tok::Ident;
			t.text.assign(text, i, j - i);
			i = j;
		} else if (digit(c) || (c == '.' && i + 1 < text.size() && digit(text[i + 1]))) {
			const bool hex = c == '0' && i + 1 < text.size() && (text[i + 1] == 'x' || text[i + 1] == 'X');
			size_t j = i + 1;
			while (j < text.size()) {
				const char d = text[j];
				if (ident_char(d) || d == '.' ||
					(!hex && (d == '+' || d == '-') && (text[j - 1] == 'e' || text[j - 1] == 'E'))) {
					++j;
					continue;
				}
				break;
			}
			t.kind = Tok::Number;
			t.text.assign(text, i, j - i);
			i = j;
		} else {
			size_t len = 1;
			for (const char *p : kPuncts) {
				const size_t n = std::strlen(p);
				if (text.compare(i, n, p) == 0) {
					len = n;
					break;
				}
			}
			if (len == 1 && std::strchr("(){}[];,.+-*/%<>=!~&|^?:#", c) == nullptr)
				return false;
			t.kind = Tok::Punct;
			t.text.assign(text, i, len);
			i += len;
		}
		out.push_back(std::move(t));
	}
	return true;
}

struct Macro {
	bool function_like = false;
	std::vector<std::string> params;
	std::vector<Token> body;
};

class Preprocessor {
public:
	bool run(const char *src, size_t len, std::vector<Token> &out);

private:
	bool active() const noexcept { return conds_.empty() || conds_.back().active; }
	bool directive(const std::string &text);
	bool defined(const std::string &name) const;
	bool eval(const std::vector<Token> &t, long long &v);
	bool eval_expr(const std::vector<Token> &t, size_t &i, int min_prec, long long &v, uint32_t depth);
	bool eval_primary(const std::vector<Token> &t, size_t &i, long long &v, uint32_t depth);
	bool expand(const std::vector<Token> &in, std::vector<Token> &out, std::vector<std::string> &hidden, uint32_t depth);

	struct Cond {
		bool parent = true, active = true, taken = true, else_seen = false;
	};
	std::vector<Cond> conds_;
	std::unordered_map<std::string, Macro> macros_;
	long long version_ = 110;
	std::string profile_;
};

bool Preprocessor::run(const char *src, size_t len, std::vector<Token> &out)
{
	if (len > kMaxSource)
		return false;
	std::string text;
	text.reserve(len);
	for (size_t i = 0; i < len && src[i] != '\0';) {
		const char c = src[i];
		if (c == '\\' && i + 1 < len && src[i + 1] == '\n') {
			i += 2;
		} else if (c == '\\' && i + 2 < len && src[i + 1] == '\r' && src[i + 2] == '\n') {
			i += 3;
		} else if (c == '/' && i + 1 < len && src[i + 1] == '/') {
			while (i < len && src[i] != '\n' && src[i] != '\0')
				++i;
			text += ' ';
		} else if (c == '/' && i + 1 < len && src[i + 1] == '*') {
			i += 2;
			text += ' ';
			for (;; ++i) {
				if (i + 1 >= len || src[i] == '\0')
					return false;
				if (src[i] == '*' && src[i + 1] == '/')
					break;
				if (src[i] == '\n')
					text += '\n';
			}
			i += 2;
		} else {
			text += c;
			++i;
		}
	}
	std::string code;
	code.reserve(text.size());
	for (size_t at = 0; at <= text.size();) {
		size_t end = text.find('\n', at);
		if (end == std::string::npos)
			end = text.size();
		const std::string line = text.substr(at, end - at);
		const size_t k = line.find_first_not_of(" \t\r\f\v");
		if (k != std::string::npos && line[k] == '#') {
			if (!directive(line.substr(k + 1)))
				return false;
		} else if (active()) {
			code += line;
		}
		code += '\n';
		at = end + 1;
	}
	if (!conds_.empty())
		return false;
	std::vector<Token> raw;
	if (!tokenize(code, raw))
		return false;
	std::vector<std::string> hidden;
	return expand(raw, out, hidden, 0);
}

bool Preprocessor::defined(const std::string &name) const
{
	if (macros_.count(name) != 0)
		return true;
	if (name == "__VERSION__" || name == "__LINE__" || name == "__FILE__")
		return true;
	if (name == "GL_core_profile")
		return profile_ == "core" || (profile_.empty() && version_ >= 150);
	if (name == "GL_compatibility_profile")
		return profile_ == "compatibility";
	if (name == "GL_es_profile" || name == "GL_ES")
		return profile_ == "es";
	return name.compare(0, 3, "GL_") == 0;
}

bool Preprocessor::directive(const std::string &text)
{
	std::vector<Token> t;
	if (!tokenize(text, t))
		return false;
	if (t.empty())
		return true;
	if (t[0].kind != Tok::Ident)
		return false;
	const std::string &name = t[0].text;
	if (name == "if" || name == "ifdef" || name == "ifndef") {
		const bool parent = active();
		bool v = false;
		if (parent) {
			if (name == "if") {
				long long value = 0;
				if (!eval(std::vector<Token>(t.begin() + 1, t.end()), value))
					return false;
				v = value != 0;
			} else {
				if (t.size() < 2u || t[1].kind != Tok::Ident)
					return false;
				v = defined(t[1].text) == (name == "ifdef");
			}
		}
		if (conds_.size() >= kMaxNesting)
			return false;
		conds_.push_back(Cond{ parent, parent && v, parent && v, false });
		return true;
	}
	if (name == "elif") {
		if (conds_.empty() || conds_.back().else_seen)
			return false;
		Cond &c = conds_.back();
		if (!c.parent || c.taken) {
			c.active = false;
			return true;
		}
		long long value = 0;
		if (!eval(std::vector<Token>(t.begin() + 1, t.end()), value))
			return false;
		c.active = c.taken = value != 0;
		return true;
	}
	if (name == "else") {
		if (conds_.empty() || conds_.back().else_seen)
			return false;
		Cond &c = conds_.back();
		c.else_seen = true;
		c.active = c.parent && !c.taken;
		c.taken = true;
		return true;
	}
	if (name == "endif") {
		if (conds_.empty())
			return false;
		conds_.pop_back();
		return true;
	}
	if (!active())
		return true;
	if (name == "define") {
		const size_t n = text.find("define");
		size_t i = n + 6;
		while (i < text.size() && (text[i] == ' ' || text[i] == '\t'))
			++i;
		const size_t name_at = i;
		while (i < text.size() && ident_char(text[i]))
			++i;
		if (i == name_at || !ident_start(text[name_at]))
			return false;
		const std::string macro_name = text.substr(name_at, i - name_at);
		Macro m;
		if (i < text.size() && text[i] == '(') {
			m.function_like = true;
			const size_t close = text.find(')', i);
			if (close == std::string::npos)
				return false;
			std::vector<Token> params;
			if (!tokenize(text.substr(i + 1, close - i - 1), params))
				return false;
			for (size_t k = 0; k < params.size(); ++k) {
				if (k % 2u == 0u ? params[k].kind != Tok::Ident : params[k].text != ",")
					return false;
				if (k % 2u == 0u)
					m.params.push_back(params[k].text);
			}
			i = close + 1;
		}
		if (!tokenize(text.substr(i), m.body))
			return false;
		for (const Token &b : m.body)
			if (b.text == "#" || b.text == "##")
				return false;
		macros_[macro_name] = std::move(m);
		return true;
	}
	if (name == "undef") {
		if (t.size() < 2u || t[1].kind != Tok::Ident)
			return false;
		macros_.erase(t[1].text);
		return true;
	}
	if (name == "version") {
		if (t.size() < 2u || t[1].kind != Tok::Number)
			return false;
		version_ = std::atoll(t[1].text.c_str());
		if (t.size() >= 3u)
			profile_ = t[2].text;
		return true;
	}
	return name == "extension" || name == "pragma" || name == "line";
}

bool Preprocessor::eval(const std::vector<Token> &t, long long &v)
{
	size_t i = 0;
	return eval_expr(t, i, 0, v, 0) && i == t.size();
}

bool Preprocessor::eval_primary(const std::vector<Token> &t, size_t &i, long long &v, uint32_t depth)
{
	if (i >= t.size() || depth > kMaxNesting)
		return false;
	const Token &k = t[i++];
	if (k.kind == Tok::Number) {
		std::string s = k.text;
		while (!s.empty() && (s.back() == 'u' || s.back() == 'U'))
			s.pop_back();
		char *end = nullptr;
		v = std::strtoll(s.c_str(), &end, 0);
		return end != nullptr && *end == '\0';
	}
	if (k.text == "(") {
		if (!eval_expr(t, i, 0, v, depth + 1u) || i >= t.size() || t[i].text != ")")
			return false;
		++i;
		return true;
	}
	if (k.text == "-" || k.text == "+" || k.text == "!" || k.text == "~") {
		if (!eval_primary(t, i, v, depth + 1u))
			return false;
		v = k.text == "-" ? -v : k.text == "!" ? (v == 0 ? 1 : 0) : k.text == "~" ? ~v : v;
		return true;
	}
	if (k.kind != Tok::Ident)
		return false;
	if (k.text == "defined") {
		const bool paren = i < t.size() && t[i].text == "(";
		if (paren)
			++i;
		if (i >= t.size() || t[i].kind != Tok::Ident)
			return false;
		v = defined(t[i++].text) ? 1 : 0;
		if (paren) {
			if (i >= t.size() || t[i].text != ")")
				return false;
			++i;
		}
		return true;
	}
	const auto m = macros_.find(k.text);
	if (m != macros_.end()) {
		if (m->second.function_like || depth > 16u)
			return false;
		size_t j = 0;
		if (m->second.body.empty())
			return false;
		return eval_expr(m->second.body, j, 0, v, depth + 1u) && j == m->second.body.size();
	}
	if (k.text == "__VERSION__") {
		v = version_;
		return true;
	}
	v = defined(k.text) && k.text.compare(0, 3, "GL_") == 0 ? 1 : 0;
	return true;
}

bool Preprocessor::eval_expr(const std::vector<Token> &t, size_t &i, int min_prec, long long &v, uint32_t depth)
{
	if (depth > kMaxNesting || !eval_primary(t, i, v, depth))
		return false;
	for (;;) {
		if (i >= t.size())
			return true;
		const std::string &op = t[i].text;
		int prec = 0;
		if (op == "?")
			prec = 1;
		else if (op == "||")
			prec = 2;
		else if (op == "&&")
			prec = 3;
		else if (op == "|")
			prec = 4;
		else if (op == "^")
			prec = 5;
		else if (op == "&")
			prec = 6;
		else if (op == "==" || op == "!=")
			prec = 7;
		else if (op == "<" || op == ">" || op == "<=" || op == ">=")
			prec = 8;
		else if (op == "<<" || op == ">>")
			prec = 9;
		else if (op == "+" || op == "-")
			prec = 10;
		else if (op == "*" || op == "/" || op == "%")
			prec = 11;
		if (prec == 0 || prec < min_prec)
			return true;
		++i;
		if (op == "?") {
			long long a = 0, b = 0;
			if (!eval_expr(t, i, 0, a, depth + 1u) || i >= t.size() || t[i].text != ":")
				return false;
			++i;
			if (!eval_expr(t, i, 1, b, depth + 1u))
				return false;
			v = v != 0 ? a : b;
			continue;
		}
		long long r = 0;
		if (!eval_expr(t, i, prec + 1, r, depth + 1u))
			return false;
		if (op == "||")
			v = (v != 0 || r != 0) ? 1 : 0;
		else if (op == "&&")
			v = (v != 0 && r != 0) ? 1 : 0;
		else if (op == "|")
			v |= r;
		else if (op == "^")
			v ^= r;
		else if (op == "&")
			v &= r;
		else if (op == "==")
			v = v == r ? 1 : 0;
		else if (op == "!=")
			v = v != r ? 1 : 0;
		else if (op == "<")
			v = v < r ? 1 : 0;
		else if (op == ">")
			v = v > r ? 1 : 0;
		else if (op == "<=")
			v = v <= r ? 1 : 0;
		else if (op == ">=")
			v = v >= r ? 1 : 0;
		else if (op == "<<" || op == ">>") {
			if (r < 0 || r > 62)
				return false;
			v = op == "<<" ? static_cast<long long>(static_cast<unsigned long long>(v) << r) : v >> r;
		} else if (op == "+")
			v += r;
		else if (op == "-")
			v -= r;
		else if (op == "*")
			v *= r;
		else {
			if (r == 0 || (v == LLONG_MIN && r == -1))
				return false;
			v = op == "/" ? v / r : v % r;
		}
	}
}

bool Preprocessor::expand(const std::vector<Token> &in, std::vector<Token> &out, std::vector<std::string> &hidden,
	uint32_t depth)
{
	if (depth > kMaxExpansion)
		return false;
	for (size_t i = 0; i < in.size(); ++i) {
		if (out.size() >= kMaxTokens)
			return false;
		const Token &t = in[i];
		const auto m = t.kind == Tok::Ident ? macros_.find(t.text) : macros_.end();
		if (m == macros_.end() || std::find(hidden.begin(), hidden.end(), t.text) != hidden.end()) {
			out.push_back(t);
			continue;
		}
		const Macro &macro = m->second;
		std::vector<Token> body;
		if (!macro.function_like) {
			body = macro.body;
		} else {
			if (i + 1 >= in.size() || in[i + 1].text != "(") {
				out.push_back(t);
				continue;
			}
			std::vector<std::vector<Token>> args(1);
			int level = 0;
			size_t j = i + 2;
			for (;; ++j) {
				if (j >= in.size())
					return false;
				const std::string &s = in[j].text;
				if (level == 0 && s == ")")
					break;
				if (level == 0 && s == ",") {
					args.emplace_back();
					continue;
				}
				if (s == "(" || s == "[" || s == "{")
					++level;
				else if (s == ")" || s == "]" || s == "}")
					--level;
				args.back().push_back(in[j]);
			}
			if (macro.params.empty() && args.size() == 1u && args[0].empty())
				args.clear();
			if (args.size() != macro.params.size())
				return false;
			for (const Token &b : macro.body) {
				const auto p = b.kind == Tok::Ident ? std::find(macro.params.begin(), macro.params.end(), b.text)
													: macro.params.end();
				if (p == macro.params.end())
					body.push_back(b);
				else
					body.insert(body.end(), args[p - macro.params.begin()].begin(), args[p - macro.params.begin()].end());
			}
			i = j;
		}
		hidden.push_back(t.text);
		const bool ok = expand(body, out, hidden, depth + 1u);
		hidden.pop_back();
		if (!ok)
			return false;
	}
	return true;
}

enum class Store : uint8_t { Local, Uniform, Input, Output, Const, Global };

struct Var {
	Store store = Store::Global;
	std::string type;
	std::unordered_map<std::string, std::string> members;
};

struct Node {
	enum Kind : uint8_t { Ident, Number, Call, Member, Index, Unary, Binary, Ternary };
	Kind kind = Ident;
	std::string text;
	int a = -1, b = -1, c = -1;
	std::vector<int> args;
};

struct Assign {
	std::string target;
	uint32_t lanes = kAllLanes;
	uint8_t swizzle[4]{ 0, 1, 2, 3 };
	uint32_t swizzle_len = 0;
	bool kills = false;
	bool reads_self = false;
	int rhs = -1;
	int call = -1;
	int out_param = -1;
	std::string global;
};

struct Param {
	std::string name, type;
	bool out = false;
};

struct Function {
	std::string name, ret;
	std::vector<Param> params;
	size_t begin = 0, end = 0;
	bool parsed = false, ok = false, flow = false;
	std::vector<Assign> body;
	std::unordered_map<std::string, std::string> locals;
};

bool is_array_type(const std::string &t) noexcept
{
	return t.size() > 2u && t.compare(t.size() - 2u, 2u, "[]") == 0;
}

bool is_matrix_type(const std::string &t) noexcept
{
	return (t.compare(0, 3, "mat") == 0 || t.compare(0, 4, "dmat") == 0) && !is_array_type(t);
}

bool is_vector_constructor(const std::string &t) noexcept
{
	static const char *const kTypes[] = { "float", "int", "uint", "bool", "double", "vec2", "vec3", "vec4", "ivec2",
		"ivec3", "ivec4", "uvec2", "uvec3", "uvec4", "bvec2", "bvec3", "bvec4", "dvec2", "dvec3", "dvec4" };
	for (const char *k : kTypes)
		if (t == k)
			return true;
	return false;
}

bool is_type_name(const std::string &t) noexcept
{
	return t == "void" || is_vector_constructor(t) || is_matrix_type(t) || t.find("sampler") != std::string::npos ||
		t.find("image") != std::string::npos || t == "atomic_uint";
}

bool is_qualifier(const std::string &t) noexcept
{
	static const char *const kQualifiers[] = { "const", "uniform", "buffer", "in", "out", "inout", "attribute",
		"varying", "centroid", "flat", "smooth", "noperspective", "patch", "sample", "invariant", "precise", "highp",
		"mediump", "lowp", "coherent", "volatile", "restrict", "readonly", "writeonly", "shared" };
	for (const char *k : kQualifiers)
		if (t == k)
			return true;
	return false;
}

uint32_t swizzle_of(const std::string &s, uint8_t out[4]) noexcept
{
	static const char *const kSets[] = { "xyzw", "rgba", "stpq" };
	if (s.empty() || s.size() > 4u)
		return 0;
	for (const char *set : kSets) {
		bool all = true;
		for (size_t k = 0; k < s.size() && all; ++k) {
			const char *p = std::strchr(set, s[k]);
			if (p == nullptr || *p == '\0') {
				all = false;
			} else {
				out[k] = static_cast<uint8_t>(p - set);
			}
		}
		if (all)
			return static_cast<uint32_t>(s.size());
	}
	return 0;
}

bool builtin_input(const std::string &n) noexcept
{
	static const char *const kInputs[] = { "gl_Vertex", "gl_Normal", "gl_Color", "gl_SecondaryColor", "gl_FogCoord",
		"gl_VertexID", "gl_InstanceID", "gl_VertexIndex", "gl_InstanceIndex", "gl_DrawID", "gl_BaseVertex",
		"gl_BaseInstance" };
	for (const char *k : kInputs)
		if (n == k)
			return true;
	return n.compare(0, 16, "gl_MultiTexCoord") == 0;
}

bool builtin_matrix(const std::string &n) noexcept
{
	return n.compare(0, 3, "gl_") == 0 && n.find("Matrix") != std::string::npos;
}

bool builtin_uniform(const std::string &n) noexcept
{
	static const char *const kUniforms[] = { "gl_DepthRange", "gl_ClipPlane", "gl_Point", "gl_FrontMaterial",
		"gl_BackMaterial", "gl_LightSource", "gl_LightModel", "gl_FrontLightModelProduct", "gl_BackLightModelProduct",
		"gl_FrontLightProduct", "gl_BackLightProduct", "gl_TextureEnvColor", "gl_EyePlaneS", "gl_EyePlaneT",
		"gl_EyePlaneR", "gl_EyePlaneQ", "gl_ObjectPlaneS", "gl_ObjectPlaneT", "gl_ObjectPlaneR", "gl_ObjectPlaneQ",
		"gl_Fog" };
	for (const char *k : kUniforms)
		if (n == k)
			return true;
	return builtin_matrix(n);
}

bool builtin_output(const std::string &n) noexcept
{
	return n == "gl_Position" || n == "gl_PointSize" || n == "gl_ClipVertex" || n == "gl_ClipDistance" ||
		n == "gl_CullDistance" || n == "gl_FrontColor" || n == "gl_BackColor" || n == "gl_FrontSecondaryColor" ||
		n == "gl_BackSecondaryColor" || n == "gl_TexCoord" || n == "gl_FogFragCoord";
}

bool per_component(const std::string &n) noexcept
{
	static const char *const kNames[] = { "abs", "sign", "floor", "ceil", "fract", "mod", "min", "max", "clamp", "mix",
		"step", "smoothstep", "sqrt", "inversesqrt", "pow", "exp", "log", "exp2", "log2", "sin", "cos", "tan", "asin",
		"acos", "atan", "sinh", "cosh", "tanh", "radians", "degrees", "trunc", "round", "roundEven", "fma",
		"intBitsToFloat", "uintBitsToFloat", "floatBitsToInt", "floatBitsToUint" };
	for (const char *k : kNames)
		if (n == k)
			return true;
	return false;
}

bool whole_builtin(const std::string &n) noexcept
{
	static const char *const kNames[] = { "dot", "cross", "length", "distance", "normalize", "reflect", "refract",
		"faceforward", "transpose", "inverse", "determinant", "matrixCompMult", "outerProduct", "any", "all", "not",
		"equal", "notEqual", "lessThan", "lessThanEqual", "greaterThan", "greaterThanEqual", "packHalf2x16",
		"unpackHalf2x16", "packUnorm4x8", "unpackUnorm4x8" };
	for (const char *k : kNames)
		if (n == k)
			return true;
	return false;
}

class Program {
public:
	JitterVertexClass classify(const char *source, size_t length);

private:
	using Lanes = std::unordered_map<std::string, uint32_t>;

	bool step() noexcept
	{
		if (++steps_ > kMaxSteps)
			fail_ = true;
		return !fail_;
	}
	const std::string &tok(size_t i) const noexcept
	{
		static const std::string kEnd;
		return i < t_.size() ? t_[i].text : kEnd;
	}
	bool parse_globals();
	bool skip_layout(size_t &i);
	bool parse_members(size_t open, std::unordered_map<std::string, std::string> &out);
	size_t match(size_t i) const noexcept { return i < match_.size() ? match_[i] : std::string::npos; }
	bool parse_function(Function &f);
	bool parse_statement(Function &f, size_t &i, size_t end, uint32_t depth, bool top);
	bool parse_simple(Function &f, size_t begin, size_t end);
	bool parse_declaration(Function &f, size_t &i, size_t end);
	int parse_expr(size_t &i, size_t end, int min_prec, uint32_t depth);
	int parse_unary(size_t &i, size_t end, uint32_t depth);
	int node(Node n)
	{
		if (nodes_.size() >= kMaxTokens) {
			fail_ = true;
			return -1;
		}
		nodes_.push_back(std::move(n));
		return static_cast<int>(nodes_.size() - 1u);
	}
	bool target_of(int n, Assign &a) const;
	void call_effects(Function &f, int n, std::vector<Assign> &out, uint32_t depth = 0);
	Function *function(const std::string &name, size_t args);
	void globals_written(Function &f, std::unordered_set<std::string> &out, uint32_t depth);

	std::string type_of(const Function &f, int n, uint32_t depth = 0) const;
	bool matrix(const Function &f, int n, uint32_t depth = 0) const;
	bool constant(const Function &f, int n, uint32_t depth) const;
	uint8_t source(const Function &f, int n, uint32_t depth) const;
	const Var *global(const std::string &name) const
	{
		const auto it = globals_.find(name);
		return it != globals_.end() ? &it->second : nullptr;
	}
	bool is_local(const Function &f, const std::string &name) const
	{
		if (f.locals.count(name) != 0)
			return true;
		for (const Param &p : f.params)
			if (p.name == name)
				return true;
		return false;
	}

	bool run(Function &f, Lanes want, Lanes &at_start, uint32_t depth);
	void take(Function &f, const Assign &a, uint32_t lanes, Lanes &live, uint32_t depth);
	void reads(Function &f, int n, uint32_t lanes, Lanes &live, uint32_t depth);
	void weigh_call(Function &f, int n, uint32_t lanes, Lanes &live, const Lanes &want, uint32_t depth);

	std::vector<Token> t_;
	std::vector<size_t> match_;
	std::vector<Node> nodes_;
	std::unordered_map<std::string, Var> globals_;
	std::unordered_map<std::string, std::unordered_map<std::string, std::string>> structs_;
	std::vector<Function> functions_;
	bool matrix_ = false;
	std::unordered_set<int> arith_, terms_;
	bool position_ = false;
	uint32_t parsing_ = 0;
	uint64_t steps_ = 0;
	bool fail_ = false;
};

bool Program::skip_layout(size_t &i)
{
	if (tok(i) != "layout")
		return false;
	if (tok(i + 1) != "(" || match(i + 1) == std::string::npos) {
		fail_ = true;
		return false;
	}
	i = match(i + 1) + 1;
	return true;
}

bool Program::parse_members(size_t open, std::unordered_map<std::string, std::string> &out)
{
	const size_t close = match(open);
	if (close == std::string::npos)
		return false;
	for (size_t k = open + 1; k < close;) {
		while (k < close && (is_qualifier(tok(k)) || tok(k) == "layout")) {
			if (tok(k) == "layout") {
				if (!skip_layout(k))
					return false;
			} else {
				++k;
			}
		}
		if (k >= close || t_[k].kind != Tok::Ident)
			return false;
		std::string member_type = tok(k++);
		while (tok(k) == "[") {
			member_type += "[]";
			k = match(k) + 1;
		}
		for (;;) {
			if (k >= close || t_[k].kind != Tok::Ident)
				return false;
			const std::string name = tok(k++);
			std::string declared = member_type;
			while (tok(k) == "[") {
				declared += "[]";
				k = match(k) + 1;
			}
			out[name] = declared;
			if (tok(k) == ",") {
				++k;
				continue;
			}
			if (tok(k) != ";")
				return false;
			++k;
			break;
		}
	}
	return true;
}

bool Program::parse_globals()
{
	match_.assign(t_.size(), std::string::npos);
	std::vector<size_t> open;
	for (size_t i = 0; i < t_.size(); ++i) {
		const std::string &s = t_[i].text;
		if (t_[i].kind != Tok::Punct)
			continue;
		if (s == "(" || s == "[" || s == "{") {
			open.push_back(i);
		} else if (s == ")" || s == "]" || s == "}") {
			if (open.empty())
				return false;
			const std::string &o = t_[open.back()].text;
			if ((s == ")" && o != "(") || (s == "]" && o != "[") || (s == "}" && o != "{"))
				return false;
			match_[open.back()] = i;
			match_[i] = open.back();
			open.pop_back();
		}
	}
	if (!open.empty())
		return false;
	size_t i = 0;
	while (i < t_.size() && !fail_) {
		if (!step())
			return false;
		if (tok(i) == ";") {
			++i;
			continue;
		}
		if (tok(i) == "precision") {
			while (i < t_.size() && tok(i) != ";")
				++i;
			continue;
		}
		Store store = Store::Global;
		bool any_qualifier = false;
		for (;;) {
			if (skip_layout(i)) {
				any_qualifier = true;
				continue;
			}
			if (fail_)
				return false;
			const std::string &q = tok(i);
			if (!is_qualifier(q))
				break;
			any_qualifier = true;
			if (q == "uniform" || q == "buffer")
				store = Store::Uniform;
			else if (q == "in" || q == "attribute")
				store = Store::Input;
			else if (q == "out" || q == "varying")
				store = Store::Output;
			else if (q == "const" && store == Store::Global)
				store = Store::Const;
			++i;
		}
		if (i >= t_.size())
			return !any_qualifier;
		if (t_[i].kind != Tok::Ident)
			return false;
		std::string type = tok(i);
		if (type == "struct") {
			if (t_.size() <= i + 2u || t_[i + 1].kind != Tok::Ident || tok(i + 2) != "{")
				return false;
			if (!parse_members(i + 2, structs_[tok(i + 1)]))
				return false;
			type = tok(i + 1);
			i = match(i + 2) + 1;
		} else if (tok(i + 1) == "{") {
			const size_t close = match(i + 1);
			Var block;
			block.store = store;
			block.type = type;
			if (!parse_members(i + 1, block.members))
				return false;
			i = close + 1;
			if (t_.size() > i && t_[i].kind == Tok::Ident) {
				globals_[tok(i)] = block;
				++i;
				while (tok(i) == "[")
					i = match(i) + 1;
			} else {
				for (const auto &m : block.members) {
					Var v;
					v.store = store;
					v.type = m.second;
					globals_[m.first] = v;
				}
			}
			if (tok(i) != ";")
				return false;
			++i;
			continue;
		} else {
			++i;
		}
		bool type_array = false;
		while (tok(i) == "[") {
			type_array = true;
			i = match(i) + 1;
		}
		if (tok(i) == ";") {
			++i;
			continue;
		}
		if (i >= t_.size() || t_[i].kind != Tok::Ident)
			return false;
		const std::string name = tok(i++);
		if (tok(i) == "(") {
			Function f;
			f.name = name;
			f.ret = type;
			const size_t close = match(i);
			if (close == std::string::npos)
				return false;
			for (size_t k = i + 1; k < close;) {
				Param p;
				while (k < close && (is_qualifier(tok(k)) || tok(k) == "layout")) {
					if (tok(k) == "out" || tok(k) == "inout")
						p.out = true;
					++k;
				}
				if (k >= close)
					return false;
				p.type = tok(k++);
				if (p.type == "void" && k == close)
					break;
				if (k < close && t_[k].kind == Tok::Ident)
					p.name = tok(k++);
				while (tok(k) == "[") {
					p.type += "[]";
					k = match(k) + 1;
				}
				f.params.push_back(p);
				if (k < close && tok(k) != ",")
					return false;
				++k;
			}
			i = close + 1;
			if (tok(i) == "{") {
				f.begin = i + 1;
				f.end = match(i);
				i = f.end + 1;
				functions_.push_back(std::move(f));
			} else if (tok(i) == ";") {
				++i;
			} else {
				return false;
			}
			continue;
		}
		std::string current = name;
		for (;;) {
			Var v;
			v.store = store;
			v.type = type + (type_array ? "[]" : "");
			while (tok(i) == "[") {
				v.type += "[]";
				i = match(i) + 1;
			}
			if (tok(i) == "=") {
				for (++i; i < t_.size(); ++i) {
					const std::string &s = tok(i);
					if (s == "," || s == ";")
						break;
					if (s == "(" || s == "[" || s == "{")
						i = match(i);
				}
			}
			globals_[current] = v;
			if (tok(i) == ",") {
				++i;
				if (i >= t_.size() || t_[i].kind != Tok::Ident)
					return false;
				current = tok(i++);
				continue;
			}
			if (tok(i) != ";")
				return false;
			++i;
			break;
		}
	}
	return !fail_;
}

Function *Program::function(const std::string &name, size_t args)
{
	Function *found = nullptr;
	for (Function &f : functions_) {
		if (f.name != name || f.params.size() != args)
			continue;
		if (found != nullptr)
			return nullptr;
		found = &f;
	}
	return found;
}

int Program::parse_unary(size_t &i, size_t end, uint32_t depth)
{
	if (i >= end || depth > kMaxNesting || !step()) {
		fail_ = true;
		return -1;
	}
	const Token &t = t_[i];
	int n = -1;
	if (t.text == "-" || t.text == "+" || t.text == "!" || t.text == "~") {
		++i;
		Node u;
		u.kind = Node::Unary;
		u.text = t.text;
		u.a = parse_unary(i, end, depth + 1u);
		if (u.a < 0)
			return -1;
		return node(std::move(u));
	}
	if (t.text == "(") {
		const size_t close = match(i);
		if (close == std::string::npos || close >= end) {
			fail_ = true;
			return -1;
		}
		size_t k = i + 1;
		n = parse_expr(k, close, 0, depth + 1u);
		if (n < 0 || k != close) {
			fail_ = true;
			return -1;
		}
		i = close + 1;
	} else if (t.kind == Tok::Number) {
		Node num;
		num.kind = Node::Number;
		num.text = t.text;
		n = node(std::move(num));
		++i;
	} else if (t.kind == Tok::Ident) {
		++i;
		if (i < end && tok(i) == "(") {
			const size_t close = match(i);
			if (close == std::string::npos || close >= end) {
				fail_ = true;
				return -1;
			}
			Node call;
			call.kind = Node::Call;
			call.text = t.text;
			size_t k = i + 1;
			if (!(k == close || (tok(k) == "void" && k + 1 == close))) {
				for (;;) {
					const int arg = parse_expr(k, close, 0, depth + 1u);
					if (arg < 0)
						return -1;
					call.args.push_back(arg);
					if (k == close)
						break;
					if (tok(k) != ",") {
						fail_ = true;
						return -1;
					}
					++k;
				}
			}
			i = close + 1;
			n = node(std::move(call));
		} else {
			Node id;
			id.kind = Node::Ident;
			id.text = t.text;
			n = node(std::move(id));
		}
	} else {
		fail_ = true;
		return -1;
	}
	while (n >= 0 && i < end) {
		if (tok(i) == "." && i + 1 < end && t_[i + 1].kind == Tok::Ident) {
			Node m;
			m.kind = Node::Member;
			m.text = tok(i + 1);
			m.a = n;
			n = node(std::move(m));
			i += 2;
		} else if (tok(i) == "[") {
			const size_t close = match(i);
			if (close == std::string::npos || close >= end) {
				fail_ = true;
				return -1;
			}
			size_t k = i + 1;
			Node x;
			x.kind = Node::Index;
			x.a = n;
			x.b = parse_expr(k, close, 0, depth + 1u);
			if (x.b < 0 || k != close) {
				fail_ = true;
				return -1;
			}
			n = node(std::move(x));
			i = close + 1;
		} else if (tok(i) == "++" || tok(i) == "--") {
			fail_ = true;
			return -1;
		} else {
			break;
		}
	}
	return n;
}

int Program::parse_expr(size_t &i, size_t end, int min_prec, uint32_t depth)
{
	int lhs = parse_unary(i, end, depth);
	if (lhs < 0)
		return -1;
	for (;;) {
		if (i >= end)
			return lhs;
		const std::string &op = tok(i);
		int prec = 0;
		if (op == "?")
			prec = 1;
		else if (op == "||")
			prec = 2;
		else if (op == "^^")
			prec = 3;
		else if (op == "&&")
			prec = 4;
		else if (op == "|")
			prec = 5;
		else if (op == "^")
			prec = 6;
		else if (op == "&")
			prec = 7;
		else if (op == "==" || op == "!=")
			prec = 8;
		else if (op == "<" || op == ">" || op == "<=" || op == ">=")
			prec = 9;
		else if (op == "<<" || op == ">>")
			prec = 10;
		else if (op == "+" || op == "-")
			prec = 11;
		else if (op == "*" || op == "/" || op == "%")
			prec = 12;
		if (prec == 0 || prec < min_prec)
			return lhs;
		++i;
		if (op == "?") {
			Node t;
			t.kind = Node::Ternary;
			t.c = lhs;
			t.a = parse_expr(i, end, 0, depth + 1u);
			if (t.a < 0 || tok(i) != ":") {
				fail_ = true;
				return -1;
			}
			++i;
			t.b = parse_expr(i, end, 1, depth + 1u);
			if (t.b < 0)
				return -1;
			lhs = node(std::move(t));
			continue;
		}
		Node b;
		b.kind = Node::Binary;
		b.text = op;
		b.a = lhs;
		b.b = parse_expr(i, end, prec + 1, depth + 1u);
		if (b.b < 0)
			return -1;
		lhs = node(std::move(b));
	}
}

bool Program::target_of(int n, Assign &a) const
{
	if (n < 0)
		return false;
	const Node &x = nodes_[n];
	if (x.kind == Node::Ident) {
		a.target = x.text;
		a.lanes = kAllLanes;
		a.kills = true;
		return true;
	}
	if (x.kind == Node::Member && nodes_[x.a].kind == Node::Ident) {
		uint8_t s[4]{};
		const uint32_t len = swizzle_of(x.text, s);
		if (len != 0u) {
			a.target = nodes_[x.a].text;
			a.lanes = 0;
			for (uint32_t k = 0; k < len; ++k) {
				a.swizzle[k] = s[k];
				a.lanes |= 1u << s[k];
			}
			a.swizzle_len = len;
			a.kills = true;
			return true;
		}
	}
	int base = n;
	while (base >= 0 && (nodes_[base].kind == Node::Member || nodes_[base].kind == Node::Index))
		base = nodes_[base].a;
	if (base < 0 || nodes_[base].kind != Node::Ident)
		return false;
	a.target = nodes_[base].text;
	a.lanes = kAllLanes;
	a.kills = false;
	return true;
}

void Program::call_effects(Function &f, int n, std::vector<Assign> &out, uint32_t depth)
{
	if (n < 0 || fail_ || !step())
		return;
	if (depth > kMaxNesting) {
		fail_ = true;
		return;
	}
	const Node &x = nodes_[n];
	for (const int k : { x.a, x.b, x.c })
		call_effects(f, k, out, depth + 1u);
	for (const int k : x.args)
		call_effects(f, k, out, depth + 1u);
	if (x.kind != Node::Call)
		return;
	Function *callee = function(x.text, x.args.size());
	if (callee == nullptr)
		return;
	for (size_t j = 0; j < callee->params.size(); ++j) {
		if (!callee->params[j].out)
			continue;
		Assign a;
		if (!target_of(x.args[j], a)) {
			fail_ = true;
			return;
		}
		a.kills = false;
		a.call = n;
		a.out_param = static_cast<int>(j);
		out.push_back(a);
	}
	std::unordered_set<std::string> written;
	globals_written(*callee, written, 0);
	for (const std::string &g : written) {
		Assign a;
		a.target = g;
		a.kills = false;
		a.call = n;
		a.global = g;
		out.push_back(a);
	}
}

void Program::globals_written(Function &f, std::unordered_set<std::string> &out, uint32_t depth)
{
	if (depth > kMaxCalls || !parse_function(f))
		return;
	for (const Assign &a : f.body) {
		if (!is_local(f, a.target) && a.target != "$return")
			out.insert(a.target);
	}
}

bool Program::parse_declaration(Function &f, size_t &i, size_t end)
{
	while (i < end && is_qualifier(tok(i)))
		++i;
	if (i >= end || t_[i].kind != Tok::Ident)
		return false;
	std::string type = tok(i++);
	while (tok(i) == "[") {
		type += "[]";
		i = match(i) + 1;
	}
	for (;;) {
		if (i >= end || t_[i].kind != Tok::Ident)
			return false;
		const std::string name = tok(i++);
		std::string declared = type;
		while (i < end && tok(i) == "[") {
			declared += "[]";
			i = match(i) + 1;
		}
		f.locals[name] = declared;
		if (tok(i) == "=") {
			++i;
			size_t stop = i;
			while (stop < end && tok(stop) != "," && tok(stop) != ";") {
				if (tok(stop) == "(" || tok(stop) == "[" || tok(stop) == "{")
					stop = match(stop);
				++stop;
			}
			size_t k = i;
			Assign a;
			a.target = name;
			a.kills = true;
			if (tok(k) == "{") {
				a.rhs = -1;
				fail_ = true;
				return false;
			}
			a.rhs = parse_expr(k, stop, 0, 0);
			if (a.rhs < 0 || k != stop)
				return false;
			std::vector<Assign> effects;
			call_effects(f, a.rhs, effects);
			f.body.insert(f.body.end(), effects.begin(), effects.end());
			f.body.push_back(a);
			i = stop;
		}
		if (tok(i) == ",") {
			++i;
			continue;
		}
		if (tok(i) != ";")
			return false;
		++i;
		return true;
	}
}

bool Program::parse_simple(Function &f, size_t begin, size_t end)
{
	if (begin == end)
		return true;
	if (end - begin == 2u && ((tok(begin) == "++" || tok(begin) == "--") || (tok(begin + 1) == "++" || tok(begin + 1) == "--"))) {
		const size_t at = tok(begin) == "++" || tok(begin) == "--" ? begin + 1 : begin;
		if (t_[at].kind != Tok::Ident)
			return false;
		Assign a;
		a.target = tok(at);
		a.reads_self = true;
		f.body.push_back(a);
		return true;
	}
	size_t op = end;
	for (size_t k = begin; k < end; ++k) {
		const std::string &s = tok(k);
		if (s == "(" || s == "[" || s == "{") {
			k = match(k);
			continue;
		}
		if (s == "=" || s == "+=" || s == "-=" || s == "*=" || s == "/=" || s == "%=" || s == "&=" || s == "|=" ||
			s == "^=" || s == "<<=" || s == ">>=") {
			op = k;
			break;
		}
	}
	if (op == end) {
		size_t k = begin;
		const int n = parse_expr(k, end, 0, 0);
		if (n < 0 || k != end)
			return false;
		std::vector<Assign> effects;
		call_effects(f, n, effects);
		f.body.insert(f.body.end(), effects.begin(), effects.end());
		return !fail_;
	}
	size_t k = begin;
	const int lhs = parse_expr(k, op, 0, 0);
	if (lhs < 0 || k != op)
		return false;
	Assign a;
	if (!target_of(lhs, a))
		return false;
	k = op + 1;
	a.rhs = parse_expr(k, end, 0, 0);
	if (a.rhs < 0 || k != end)
		return false;
	if (tok(op) != "=") {
		a.kills = false;
		a.reads_self = true;
	}
	std::vector<Assign> effects;
	call_effects(f, a.rhs, effects);
	call_effects(f, lhs, effects);
	f.body.insert(f.body.end(), effects.begin(), effects.end());
	f.body.push_back(a);
	return !fail_;
}

bool Program::parse_statement(Function &f, size_t &i, size_t end, uint32_t depth, bool top)
{
	if (i >= end || depth > kMaxNesting || !step())
		return false;
	const std::string &s = tok(i);
	const auto paren = [&](size_t &k, bool record) -> bool {
		if (tok(k) != "(" || match(k) == std::string::npos || match(k) >= end)
			return false;
		const size_t close = match(k);
		if (record) {
			size_t e = k + 1;
			const int n = parse_expr(e, close, 0, depth + 1u);
			if (n < 0 || e != close)
				return false;
			std::vector<Assign> effects;
			call_effects(f, n, effects);
			f.body.insert(f.body.end(), effects.begin(), effects.end());
		}
		k = close + 1;
		return true;
	};
	if (s == "{") {
		const size_t close = match(i);
		if (close == std::string::npos || close >= end)
			return false;
		size_t k = i + 1;
		while (k < close)
			if (!parse_statement(f, k, close, depth + 1u, false))
				return false;
		i = close + 1;
		return true;
	}
	if (s == ";") {
		++i;
		return true;
	}
	if (s == "if") {
		f.flow = true;
		++i;
		if (!paren(i, true) || !parse_statement(f, i, end, depth + 1u, false))
			return false;
		if (tok(i) == "else") {
			++i;
			return parse_statement(f, i, end, depth + 1u, false);
		}
		return true;
	}
	if (s == "while") {
		f.flow = true;
		++i;
		return paren(i, true) && parse_statement(f, i, end, depth + 1u, false);
	}
	if (s == "do") {
		f.flow = true;
		++i;
		if (!parse_statement(f, i, end, depth + 1u, false) || tok(i) != "while")
			return false;
		++i;
		if (!paren(i, true) || tok(i) != ";")
			return false;
		++i;
		return true;
	}
	if (s == "for") {
		f.flow = true;
		++i;
		if (tok(i) != "(" || match(i) == std::string::npos)
			return false;
		const size_t close = match(i);
		size_t k = i + 1;
		size_t semi1 = k;
		while (semi1 < close && tok(semi1) != ";")
			semi1 = (tok(semi1) == "(" || tok(semi1) == "[") ? match(semi1) + 1 : semi1 + 1;
		if (semi1 >= close)
			return false;
		if (is_type_name(tok(k)) || is_qualifier(tok(k)) || structs_.count(tok(k)) != 0) {
			if (!parse_declaration(f, k, semi1 + 1) || k != semi1 + 1)
				return false;
		} else if (!parse_simple(f, k, semi1)) {
			return false;
		}
		size_t semi2 = semi1 + 1;
		while (semi2 < close && tok(semi2) != ";")
			semi2 = (tok(semi2) == "(" || tok(semi2) == "[") ? match(semi2) + 1 : semi2 + 1;
		if (semi2 >= close)
			return false;
		if (!parse_simple(f, semi2 + 1, close))
			return false;
		i = close + 1;
		return parse_statement(f, i, end, depth + 1u, false);
	}
	if (s == "switch") {
		f.flow = true;
		++i;
		if (!paren(i, true) || tok(i) != "{")
			return false;
		const size_t close = match(i);
		size_t k = i + 1;
		while (k < close) {
			if (tok(k) == "case") {
				while (k < close && tok(k) != ":")
					++k;
				++k;
			} else if (tok(k) == "default") {
				k += 2;
			} else if (!parse_statement(f, k, close, depth + 1u, false)) {
				return false;
			}
		}
		i = close + 1;
		return true;
	}
	if (s == "return") {
		++i;
		size_t stop = i;
		while (stop < end && tok(stop) != ";")
			stop = (tok(stop) == "(" || tok(stop) == "[") ? match(stop) + 1 : stop + 1;
		if (stop >= end)
			return false;
		if (stop > i) {
			Assign a;
			a.target = "$return";
			size_t k = i;
			a.rhs = parse_expr(k, stop, 0, 0);
			if (a.rhs < 0 || k != stop)
				return false;
			std::vector<Assign> effects;
			call_effects(f, a.rhs, effects);
			f.body.insert(f.body.end(), effects.begin(), effects.end());
			f.body.push_back(a);
		}
		i = stop + 1;
		if (!(top && i == end))
			f.flow = true;
		return true;
	}
	if (s == "break" || s == "continue" || s == "discard") {
		f.flow = true;
		++i;
		if (tok(i) != ";")
			return false;
		++i;
		return true;
	}
	if (s == "struct")
		return false;
	if ((is_type_name(s) || structs_.count(s) != 0 || is_qualifier(s)) &&
		(i + 1 < end && (t_[i + 1].kind == Tok::Ident || tok(i + 1) == "[")))
		return parse_declaration(f, i, end);
	size_t stop = i;
	while (stop < end && tok(stop) != ";")
		stop = (tok(stop) == "(" || tok(stop) == "[" || tok(stop) == "{") ? match(stop) + 1 : stop + 1;
	if (stop >= end)
		return false;
	if (!parse_simple(f, i, stop))
		return false;
	i = stop + 1;
	return true;
}

bool Program::parse_function(Function &f)
{
	if (f.parsed)
		return f.ok;
	if (parsing_ >= kMaxParseCalls) {
		fail_ = true;
		return false;
	}
	f.parsed = true;
	++parsing_;
	size_t i = f.begin;
	bool ok = true;
	while (ok && i < f.end && !fail_)
		ok = parse_statement(f, i, f.end, 0, true);
	--parsing_;
	f.ok = ok && !fail_;
	return f.ok;
}

std::string Program::type_of(const Function &f, int n, uint32_t depth) const
{
	if (n < 0 || depth > kMaxNesting)
		return std::string();
	const Node &x = nodes_[n];
	switch (x.kind) {
	case Node::Ident: {
		const auto l = f.locals.find(x.text);
		if (l != f.locals.end())
			return l->second;
		for (const Param &p : f.params)
			if (p.name == x.text)
				return p.type;
		if (const Var *v = global(x.text))
			return v->type;
		if (builtin_matrix(x.text))
			return x.text.find("Normal") != std::string::npos ? "mat3" : "mat4";
		return std::string();
	}
	case Node::Member: {
		if (nodes_[x.a].kind == Node::Ident && !is_local(f, nodes_[x.a].text)) {
			if (const Var *v = global(nodes_[x.a].text)) {
				const auto m = v->members.find(x.text);
				if (m != v->members.end())
					return m->second;
			}
		}
		const auto st = structs_.find(type_of(f, x.a, depth + 1u));
		if (st != structs_.end()) {
			const auto m = st->second.find(x.text);
			if (m != st->second.end())
				return m->second;
		}
		return std::string();
	}
	case Node::Index: {
		const std::string t = type_of(f, x.a, depth + 1u);
		return is_array_type(t) ? t.substr(0, t.size() - 2u) : std::string();
	}
	case Node::Call: {
		const Function *callee = nullptr;
		for (const Function &g : functions_)
			if (g.name == x.text && g.params.size() == x.args.size())
				callee = &g;
		return callee != nullptr ? callee->ret : x.text;
	}
	default:
		return std::string();
	}
}

bool Program::matrix(const Function &f, int n, uint32_t depth) const
{
	if (n < 0 || depth > kMaxNesting)
		return false;
	const Node &x = nodes_[n];
	if (x.kind == Node::Call && (x.text == "transpose" || x.text == "inverse" || x.text == "outerProduct" ||
		x.text == "matrixCompMult"))
		return true;
	if (x.kind == Node::Binary && x.text == "*")
		return matrix(f, x.a, depth + 1u) && matrix(f, x.b, depth + 1u);
	if (x.kind == Node::Unary)
		return matrix(f, x.a, depth + 1u);
	if (x.kind == Node::Ternary)
		return matrix(f, x.a, depth + 1u) || matrix(f, x.b, depth + 1u);
	return is_matrix_type(type_of(f, n));
}

bool Program::constant(const Function &f, int n, uint32_t depth) const
{
	if (n < 0 || depth > kMaxNesting)
		return false;
	const Node &x = nodes_[n];
	switch (x.kind) {
	case Node::Number:
		return true;
	case Node::Ident: {
		const Var *v = global(x.text);
		return !is_local(f, x.text) && v != nullptr && v->store == Store::Const;
	}
	case Node::Call:
		if (!is_vector_constructor(x.text) && !is_matrix_type(x.text))
			return false;
		for (const int a : x.args)
			if (!constant(f, a, depth + 1u))
				return false;
		return true;
	case Node::Unary:
		return constant(f, x.a, depth + 1u);
	case Node::Binary:
		return constant(f, x.a, depth + 1u) && constant(f, x.b, depth + 1u);
	default:
		return false;
	}
}

uint8_t Program::source(const Function &f, int n, uint32_t depth) const
{
	if (n < 0 || depth > 16u)
		return kFromOther;
	const Node &x = nodes_[n];
	switch (x.kind) {
	case Node::Number:
		return 0u;
	case Node::Ident: {
		if (is_local(f, x.text)) {
			const Assign *only = nullptr;
			for (const Assign &a : f.body) {
				if (a.target != x.text)
					continue;
				if (only != nullptr || !a.kills || a.swizzle_len != 0u || a.rhs < 0)
					return kFromOther;
				only = &a;
			}
			return only != nullptr ? source(f, only->rhs, depth + 1u) : kFromOther;
		}
		if (const Var *v = global(x.text)) {
			if (v->store == Store::Uniform)
				return kFromUniform;
			if (v->store == Store::Input)
				return kFromInput;
			if (v->store == Store::Const)
				return 0u;
			return kFromOther;
		}
		if (builtin_input(x.text))
			return kFromInput;
		if (builtin_uniform(x.text))
			return kFromUniform;
		return kFromOther;
	}
	case Node::Member:
	case Node::Index:
		return source(f, x.a, depth + 1u);
	case Node::Unary:
		return x.text == "-" || x.text == "+" ? source(f, x.a, depth + 1u) : kFromOther;
	case Node::Call: {
		if (!is_vector_constructor(x.text))
			return kFromOther;
		uint8_t s = 0u;
		for (const int a : x.args)
			s |= source(f, a, depth + 1u);
		return s;
	}
	default:
		return kFromOther;
	}
}

void Program::reads(Function &f, int n, uint32_t lanes, Lanes &live, uint32_t depth)
{
	if (n < 0 || fail_ || !step())
		return;
	if (depth > kMaxNesting) {
		fail_ = true;
		return;
	}
	if (lanes == 0u)
		return;
	const Node &x = nodes_[n];
	switch (x.kind) {
	case Node::Number:
		return;
	case Node::Ident: {
		if (is_local(f, x.text)) {
			live[x.text] |= lanes;
			return;
		}
		if (const Var *v = global(x.text)) {
			if (v->store == Store::Global || v->store == Store::Output)
				live[x.text] |= lanes;
			return;
		}
		if (builtin_input(x.text) || builtin_uniform(x.text))
			return;
		if (builtin_output(x.text)) {
			live[x.text] |= lanes;
			return;
		}
		fail_ = true;
		return;
	}
	case Node::Member: {
		uint8_t s[4]{};
		const uint32_t len = swizzle_of(x.text, s);
		const Node &base = nodes_[x.a];
		if (base.kind == Node::Ident) {
			const Var *v = is_local(f, base.text) ? nullptr : global(base.text);
			if (v != nullptr && !v->members.empty())
				return;
		}
		if (len == 0u) {
			reads(f, x.a, kAllLanes, live, depth + 1u);
			return;
		}
		uint32_t from = 0;
		for (uint32_t k = 0; k < 4u; ++k)
			if ((lanes & (1u << k)) != 0u)
				from |= 1u << s[len == 1u ? 0u : std::min(k, len - 1u)];
		reads(f, x.a, from, live, depth + 1u);
		return;
	}
	case Node::Index:
		reads(f, x.a, kAllLanes, live, depth + 1u);
		reads(f, x.b, kAllLanes, live, depth + 1u);
		return;
	case Node::Unary:
		reads(f, x.a, lanes, live, depth + 1u);
		return;
	case Node::Ternary:
		reads(f, x.c, kAllLanes, live, depth + 1u);
		reads(f, x.a, lanes, live, depth + 1u);
		reads(f, x.b, lanes, live, depth + 1u);
		return;
	case Node::Binary: {
		if (x.text == "*" && (matrix(f, x.a) || matrix(f, x.b))) {
			const int m = matrix(f, x.a) ? x.a : x.b;
			if (!constant(f, m, 0u))
				matrix_ = true;
			reads(f, x.a, kAllLanes, live, depth + 1u);
			reads(f, x.b, kAllLanes, live, depth + 1u);
			return;
		}
		if (x.text == "*" || x.text == "+" || x.text == "-") {
			const uint8_t sa = source(f, x.a, 0u), sb = source(f, x.b, 0u);
			if (((sa | sb) & kFromUniform) != 0u)
				arith_.insert(n);
			if (x.text == "*" && ((sa == kFromUniform && sb == kFromInput) || (sa == kFromInput && sb == kFromUniform)))
				terms_.insert(n);
		}
		const bool per_lane = x.text == "*" || x.text == "+" || x.text == "-" || x.text == "/" || x.text == "%" ||
			x.text == "&" || x.text == "|" || x.text == "^" || x.text == "<<" || x.text == ">>";
		reads(f, x.a, per_lane ? lanes : kAllLanes, live, depth + 1u);
		reads(f, x.b, per_lane ? lanes : kAllLanes, live, depth + 1u);
		return;
	}
	case Node::Call:
		weigh_call(f, n, lanes, live, Lanes{}, depth);
		return;
	}
}

void Program::weigh_call(Function &f, int n, uint32_t lanes, Lanes &live, const Lanes &want, uint32_t depth)
{
	const Node &x = nodes_[n];
	const std::string &name = x.text;
	if (name == "ftransform") {
		matrix_ = true;
		return;
	}
	if (is_vector_constructor(name) || is_matrix_type(name) || structs_.count(name) != 0) {
		for (const int a : x.args)
			reads(f, a, kAllLanes, live, depth + 1u);
		return;
	}
	if (name == "dot" && x.args.size() == 2u) {
		if (((source(f, x.args[0], 0u) | source(f, x.args[1], 0u)) & kFromUniform) != 0u)
			matrix_ = true;
	}
	if (name == "fma" && x.args.size() == 3u) {
		const uint8_t sa = source(f, x.args[0], 0u), sb = source(f, x.args[1], 0u), sc = source(f, x.args[2], 0u);
		if (((sa | sb | sc) & kFromUniform) != 0u)
			arith_.insert(n);
		if ((sa == kFromUniform && sb == kFromInput) || (sa == kFromInput && sb == kFromUniform))
			terms_.insert(n);
	}
	if (per_component(name) || whole_builtin(name)) {
		const bool each = per_component(name);
		for (const int a : x.args)
			reads(f, a, each ? lanes : kAllLanes, live, depth + 1u);
		return;
	}
	Function *callee = function(name, x.args.size());
	if (callee == nullptr || depth > kMaxNesting) {
		fail_ = true;
		return;
	}
	Lanes need = want;
	if (want.empty())
		need["$return"] = lanes;
	Lanes at_start;
	if (!run(*callee, need, at_start, depth + 1u))
		return;
	for (const auto &l : at_start) {
		if (l.second == 0u)
			continue;
		bool param = false;
		for (size_t j = 0; j < callee->params.size(); ++j) {
			if (callee->params[j].name != l.first)
				continue;
			param = true;
			reads(f, x.args[j], l.second, live, depth + 1u);
		}
		if (!param && !is_local(*callee, l.first))
			live[l.first] |= l.second;
	}
}

void Program::take(Function &f, const Assign &a, uint32_t lanes, Lanes &live, uint32_t depth)
{
	if (a.target == "gl_Position")
		position_ = true;
	if (a.reads_self)
		live[a.target] |= lanes;
	if (a.rhs >= 0) {
		uint32_t from = lanes;
		if (a.swizzle_len != 0u) {
			from = 0;
			for (uint32_t k = 0; k < a.swizzle_len; ++k)
				if ((lanes & (1u << a.swizzle[k])) != 0u)
					from |= 1u << k;
		}
		reads(f, a.rhs, from, live, depth);
		return;
	}
	if (a.call >= 0) {
		const Node &call = nodes_[a.call];
		Function *callee = function(call.text, call.args.size());
		if (callee == nullptr) {
			fail_ = true;
			return;
		}
		Lanes want;
		if (a.out_param >= 0)
			want[callee->params[static_cast<size_t>(a.out_param)].name] = kAllLanes;
		else
			want[a.global] = lanes;
		weigh_call(f, a.call, kAllLanes, live, want, depth);
	}
}

bool Program::run(Function &f, Lanes want, Lanes &at_start, uint32_t depth)
{
	if (depth > kMaxCalls || !parse_function(f)) {
		fail_ = true;
		return false;
	}
	Lanes live = std::move(want);
	if (!f.flow) {
		for (size_t k = f.body.size(); k-- > 0 && !fail_;) {
			const Assign &a = f.body[k];
			const auto it = live.find(a.target);
			if (it == live.end())
				continue;
			const uint32_t lanes = it->second & a.lanes;
			if (lanes == 0u)
				continue;
			if (a.kills)
				it->second &= ~a.lanes;
			take(f, a, lanes, live, depth);
		}
	} else {
		std::vector<uint32_t> taken(f.body.size(), 0u);
		for (bool grew = true; grew && !fail_;) {
			grew = false;
			for (size_t k = 0; k < f.body.size() && !fail_; ++k) {
				const Assign &a = f.body[k];
				const auto it = live.find(a.target);
				if (it == live.end())
					continue;
				const uint32_t fresh = it->second & a.lanes & ~taken[k];
				if (fresh == 0u)
					continue;
				taken[k] |= fresh;
				grew = true;
				take(f, a, fresh, live, depth);
			}
		}
	}
	at_start = std::move(live);
	return !fail_;
}

JitterVertexClass Program::classify(const char *source, size_t length)
{
	Preprocessor pp;
	if (source == nullptr || !pp.run(source, length, t_) || !parse_globals())
		return JitterVertexClass::Unknown;
	Function *main = function("main", 0);
	if (main == nullptr || main->ret != "void")
		return JitterVertexClass::Unknown;
	Lanes want;
	want["gl_Position"] = kAllLanes;
	Lanes at_start;
	if (!run(*main, want, at_start, 0) || fail_ || !position_)
		return JitterVertexClass::Unknown;
	return matrix_ || arith_.size() >= 3u || terms_.size() >= 2u ? JitterVertexClass::Transformed
																  : JitterVertexClass::Screen;
}

}

}

JitterVertexClass classify_spirv_vertex_shader(const void *code, size_t bytes, const char *entry_point)
{
	if (code == nullptr || bytes < 20u || (bytes & 3u) != 0u)
		return JitterVertexClass::Unknown;
	std::vector<uint32_t> copy;
	const uint32_t *words = static_cast<const uint32_t *>(code);
	if ((reinterpret_cast<uintptr_t>(code) & 3u) != 0u) {
		copy.resize(bytes / 4u);
		std::memcpy(copy.data(), code, bytes);
		words = copy.data();
	}
	spirv_pos::Reader r(words, bytes / 4u);
	if (!r.parse())
		return JitterVertexClass::Unknown;
	return r.classify(entry_point);
}

JitterVertexClass classify_glsl_vertex_shader(const char *source, size_t length)
{
	if (source == nullptr || length == 0u)
		return JitterVertexClass::Unknown;
	glsl_pos::Program program;
	return program.classify(source, length);
}

JitterVertexClass classify_gl_fixed_function(const float projection[16], const float modelview[16]) noexcept
{
	if (projection == nullptr || modelview == nullptr)
		return JitterVertexClass::Unknown;
	for (int i = 0; i < 16; ++i)
		if (!std::isfinite(projection[i]) || !std::isfinite(modelview[i]))
			return JitterVertexClass::Unknown;
	double m[16];
	for (int c = 0; c < 4; ++c) {
		for (int r = 0; r < 4; ++r) {
			double v = 0.0;
			for (int k = 0; k < 4; ++k)
				v += static_cast<double>(projection[k * 4 + r]) * static_cast<double>(modelview[c * 4 + k]);
			m[c * 4 + r] = v;
		}
	}
	if (projection[11] != 0.0f || m[3] != 0.0 || m[7] != 0.0 || m[11] != 0.0)
		return JitterVertexClass::Transformed;
	const double scale = std::max(std::fabs(m[0]), std::fabs(m[5]));
	const double eps = 1e-6 * (scale > 0.0 ? scale : 1.0);
	if (std::fabs(m[0]) > eps && std::fabs(m[5]) > eps && std::fabs(m[4]) <= eps && std::fabs(m[8]) <= eps &&
		std::fabs(m[1]) <= eps && std::fabs(m[9]) <= eps && m[15] != 0.0)
		return JitterVertexClass::Screen;
	return JitterVertexClass::Unknown;
}

bool gl_fixed_function_perspective(const float projection[16], const float modelview[16]) noexcept
{
	if (projection == nullptr || modelview == nullptr)
		return false;
	for (int i = 0; i < 16; ++i)
		if (!std::isfinite(projection[i]) || !std::isfinite(modelview[i]))
			return false;
	for (int c = 0; c < 3; ++c) {
		double w = 0.0;
		for (int k = 0; k < 4; ++k)
			w += static_cast<double>(projection[k * 4 + 3]) * static_cast<double>(modelview[c * 4 + k]);
		if (std::fabs(w) > 1e-6)
			return true;
	}
	return false;
}

}
