#include "aeon_sr/jitter/shader_offset.hpp"

#include <algorithm>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace aeon_sr {
namespace {
namespace dxil {

class Bits {
public:
	Bits(const uint8_t *p, size_t bytes) : p_(p), bits_(bytes * 8u) {}

	bool ok() const noexcept { return ok_; }
	bool at_end() const noexcept { return pos_ >= bits_; }
	size_t pos() const noexcept { return pos_; }
	size_t remaining() const noexcept { return bits_ - pos_; }
	void fail() noexcept
	{
		ok_ = false;
		pos_ = bits_;
	}

	uint64_t fixed(uint32_t n) noexcept
	{
		if (n == 0u)
			return 0u;
		if (n > 64u || !ok_ || bits_ - pos_ < n) {
			fail();
			return 0u;
		}
		uint64_t v = 0;
		for (uint32_t got = 0; got < n;) {
			const size_t byte = pos_ >> 3;
			const uint32_t bit = static_cast<uint32_t>(pos_ & 7u);
			const uint32_t take = std::min(8u - bit, n - got);
			const uint64_t piece = (static_cast<uint64_t>(p_[byte]) >> bit) & ((1ull << take) - 1ull);
			v |= piece << got;
			got += take;
			pos_ += take;
		}
		return v;
	}

	uint64_t vbr(uint32_t n) noexcept
	{
		if (n < 2u || n > 32u) {
			fail();
			return 0u;
		}
		const uint64_t hi = 1ull << (n - 1u);
		uint64_t v = 0;
		for (uint32_t shift = 0;; shift += n - 1u) {
			if (shift >= 64u) {
				fail();
				return 0u;
			}
			const uint64_t piece = fixed(n);
			if (!ok_)
				return 0u;
			v |= (piece & (hi - 1u)) << shift;
			if ((piece & hi) == 0u)
				return v;
		}
	}

	void align32() noexcept
	{
		const size_t to = (pos_ + 31u) & ~static_cast<size_t>(31u);
		if (to > bits_)
			fail();
		else
			pos_ = to;
	}

	bool seek(size_t to) noexcept
	{
		if (!ok_ || to > bits_ || to < pos_) {
			fail();
			return false;
		}
		pos_ = to;
		return true;
	}

	const uint8_t *bytes_at(size_t bytes) noexcept
	{
		if (!ok_ || (pos_ & 7u) != 0u || (bits_ - pos_) / 8u < bytes) {
			fail();
			return nullptr;
		}
		const uint8_t *p = p_ + pos_ / 8u;
		pos_ += bytes * 8u;
		return p;
	}

private:
	const uint8_t *p_;
	size_t bits_;
	size_t pos_ = 0;
	bool ok_ = true;
};

enum OpKind : uint8_t { kLiteral, kFixed, kVbr, kArray, kChar6, kBlob };

struct AbbrevOp {
	OpKind kind = kLiteral;
	uint64_t value = 0;
};
using Abbrev = std::vector<AbbrevOp>;

constexpr size_t kMaxAbbrevOps = 64u;
constexpr size_t kMaxAbbrevs = 4096u;
constexpr size_t kMaxRecordOps = 1u << 17;

struct Record {
	uint32_t code = 0;
	std::vector<uint64_t> ops;
};

enum class Item { Record, Block, Abbrev, End, Fail };

class Cursor {
public:
	Cursor(Bits &bits, uint32_t width, std::vector<Abbrev> abbrevs) : b_(bits), width_(width), abbrevs_(std::move(abbrevs)) {}

	Item next(Record &r)
	{
		if (!b_.ok() || b_.at_end())
			return Item::Fail;
		const uint64_t id = b_.fixed(width_);
		if (!b_.ok())
			return Item::Fail;
		switch (id) {
		case 0:
			b_.align32();
			return b_.ok() ? Item::End : Item::Fail;
		case 1: {
			sub_id_ = static_cast<uint32_t>(b_.vbr(8));
			sub_width_ = static_cast<uint32_t>(b_.vbr(4));
			b_.align32();
			const uint64_t words = b_.fixed(32);
			if (!b_.ok() || sub_width_ < 1u || sub_width_ > 32u || words > b_.remaining() / 32u)
				return Item::Fail;
			sub_end_ = b_.pos() + static_cast<size_t>(words) * 32u;
			return Item::Block;
		}
		case 2: {
			defined_.clear();
			const uint64_t n = b_.vbr(5);
			if (!b_.ok() || n == 0u || n > kMaxAbbrevOps)
				return Item::Fail;
			for (uint64_t i = 0; i < n; ++i) {
				AbbrevOp op;
				if (b_.fixed(1) != 0u) {
					op.kind = kLiteral;
					op.value = b_.vbr(8);
				} else {
					const uint64_t e = b_.fixed(3);
					if (e == 1u || e == 2u) {
						op.kind = e == 1u ? kFixed : kVbr;
						op.value = b_.vbr(5);
						if (op.value > 64u || (e == 2u && op.value != 0u && op.value < 2u))
							return Item::Fail;
					} else if (e == 3u) {
						op.kind = kArray;
					} else if (e == 4u) {
						op.kind = kChar6;
					} else if (e == 5u) {
						op.kind = kBlob;
					} else {
						return Item::Fail;
					}
				}
				if (!b_.ok())
					return Item::Fail;
				defined_.push_back(op);
			}
			if (abbrevs_.size() >= kMaxAbbrevs)
				return Item::Fail;
			abbrevs_.push_back(defined_);
			return Item::Abbrev;
		}
		case 3: {
			r.code = static_cast<uint32_t>(b_.vbr(6));
			const uint64_t n = b_.vbr(6);
			if (!b_.ok() || n > kMaxRecordOps)
				return Item::Fail;
			r.ops.clear();
			for (uint64_t i = 0; i < n && b_.ok(); ++i)
				r.ops.push_back(b_.vbr(6));
			return b_.ok() ? Item::Record : Item::Fail;
		}
		default:
			if (id - 4u >= abbrevs_.size())
				return Item::Fail;
			return read_abbreviated(abbrevs_[static_cast<size_t>(id - 4u)], r) ? Item::Record : Item::Fail;
		}
	}

	uint32_t sub_id() const noexcept { return sub_id_; }
	uint32_t sub_width() const noexcept { return sub_width_; }
	size_t sub_end() const noexcept { return sub_end_; }
	const Abbrev &defined() const noexcept { return defined_; }

	bool skip() { return b_.seek(sub_end_); }

private:
	bool scalar(const AbbrevOp &op, uint64_t &v)
	{
		switch (op.kind) {
		case kLiteral: v = op.value; return true;
		case kFixed: v = b_.fixed(static_cast<uint32_t>(op.value)); return b_.ok();
		case kVbr: v = op.value == 0u ? 0u : b_.vbr(static_cast<uint32_t>(op.value)); return b_.ok();
		case kChar6: {
			static const char kTable[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._";
			v = static_cast<uint8_t>(kTable[b_.fixed(6) & 63u]);
			return b_.ok();
		}
		default: return false;
		}
	}

	bool read_abbreviated(const Abbrev &a, Record &r)
	{
		r.ops.clear();
		bool have_code = false;
		const auto put = [&](uint64_t v) {
			if (!have_code) {
				r.code = static_cast<uint32_t>(v);
				have_code = true;
			} else {
				r.ops.push_back(v);
			}
		};
		for (size_t i = 0; i < a.size(); ++i) {
			const AbbrevOp &op = a[i];
			if (op.kind == kArray) {
				if (i + 2u != a.size())
					return false;
				const AbbrevOp &elt = a[i + 1u];
				const uint64_t n = b_.vbr(6);
				if (!b_.ok() || n > kMaxRecordOps || elt.kind == kArray || elt.kind == kBlob)
					return false;
				const bool empty = elt.kind == kLiteral || ((elt.kind == kFixed || elt.kind == kVbr) && elt.value == 0u);
				if (empty && n > 4096u)
					return false;
				for (uint64_t k = 0; k < n; ++k) {
					uint64_t v = 0;
					if (!scalar(elt, v))
						return false;
					put(v);
				}
				return have_code;
			}
			if (op.kind == kBlob) {
				const uint64_t n = b_.vbr(6);
				b_.align32();
				if (!b_.ok() || n > kMaxRecordOps)
					return false;
				const uint8_t *p = b_.bytes_at(static_cast<size_t>(n));
				if (p == nullptr)
					return false;
				for (uint64_t k = 0; k < n; ++k)
					put(p[k]);
				b_.align32();
				if (!b_.ok())
					return false;
				continue;
			}
			uint64_t v = 0;
			if (!scalar(op, v))
				return false;
			put(v);
		}
		return have_code;
	}

	Bits &b_;
	uint32_t width_;
	std::vector<Abbrev> abbrevs_;
	Abbrev defined_;
	uint32_t sub_id_ = 0, sub_width_ = 0;
	size_t sub_end_ = 0;
};

constexpr uint32_t kBlockInfo = 0, kModule = 8, kConstants = 11, kFunction = 12, kValueSymtab = 14, kMetadata = 15,
	kTypes = 17;

enum class TypeKind : uint8_t { Other, Void, Half, Float, Double, Int, Pointer, Function, Struct, Array, Vector, Label };

struct Type {
	TypeKind kind = TypeKind::Other;
	uint32_t width = 0;
	uint32_t elem = 0;
	std::vector<uint32_t> members;
};

enum class ValueKind : uint8_t { Other, Global, Function, Int, Float, Aggregate, Undef, Null, Arg, Inst };

enum class Op : uint8_t { Other, Binop, Cast, Call, ExtractValue, Phi, Select, Cmp, Load };

constexpr uint32_t kNone = ~0u;

struct Value {
	ValueKind kind = ValueKind::Other;
	Op op = Op::Other;
	uint8_t code = 0;
	bool fp = false;
	uint32_t type = kNone;
	int64_t i = 0;
	double f = 0.0;
	std::vector<uint32_t> ops;
};

struct MdNode {
	enum Kind : uint8_t { Other, String, Value, Node } kind = Other;
	std::string text;
	uint32_t value = kNone;
	std::vector<uint64_t> ops;
};

struct Body {
	uint32_t fn = kNone;
	uint32_t base = 0;
	std::vector<Value> values;
	std::vector<Value> calls;
};

struct Module {
	bool relative = false;
	std::vector<Type> types;
	std::vector<Value> values;
	std::vector<std::pair<uint32_t, bool>> functions;
	std::unordered_map<uint32_t, std::string> names;
	std::vector<MdNode> md;
	std::unordered_map<std::string, std::vector<uint64_t>> named;
	std::vector<Body> bodies;
	std::unordered_map<uint32_t, std::vector<Abbrev>> blockinfo;
};

constexpr size_t kMaxBodies = 16u;
constexpr size_t kMaxValues = 1u << 18;

int64_t signed_vbr(uint64_t v) noexcept
{
	if ((v & 1u) == 0u)
		return static_cast<int64_t>(v >> 1);
	if (v != 1u)
		return -static_cast<int64_t>(v >> 1);
	return INT64_MIN;
}

bool read_blockinfo(Bits &b, uint32_t width, Module &m)
{
	Cursor c(b, width, {});
	Record r;
	uint32_t target = kNone;
	for (;;) {
		switch (c.next(r)) {
		case Item::End:
			return true;
		case Item::Record:
			if (r.code == 1u && !r.ops.empty())
				target = static_cast<uint32_t>(r.ops[0]);
			break;
		case Item::Abbrev:
			if (target == kNone || m.blockinfo[target].size() >= kMaxAbbrevs)
				return false;
			m.blockinfo[target].push_back(c.defined());
			break;
		case Item::Block:
			if (!c.skip())
				return false;
			break;
		default:
			return false;
		}
	}
}

std::vector<Abbrev> abbrevs_for(const Module &m, uint32_t block)
{
	const auto it = m.blockinfo.find(block);
	return it != m.blockinfo.end() ? it->second : std::vector<Abbrev>{};
}

bool read_types(Bits &b, uint32_t width, Module &m)
{
	Cursor c(b, width, abbrevs_for(m, kTypes));
	Record r;
	for (;;) {
		switch (c.next(r)) {
		case Item::End:
			return true;
		case Item::Abbrev:
			break;
		case Item::Block:
			if (!c.skip())
				return false;
			break;
		case Item::Record: {
			if (r.code == 1u || r.code == 19u)
				break;
			if (m.types.size() >= kMaxValues)
				return false;
			Type t;
			const auto op = [&r](size_t k) { return k < r.ops.size() ? r.ops[k] : 0u; };
			switch (r.code) {
			case 2: t.kind = TypeKind::Void; break;
			case 3: t.kind = TypeKind::Float; break;
			case 4: t.kind = TypeKind::Double; break;
			case 5: t.kind = TypeKind::Label; break;
			case 7: t.kind = TypeKind::Int; t.width = static_cast<uint32_t>(op(0)); break;
			case 8: t.kind = TypeKind::Pointer; t.elem = static_cast<uint32_t>(op(0)); break;
			case 10: t.kind = TypeKind::Half; break;
			case 11: t.kind = TypeKind::Array; t.elem = static_cast<uint32_t>(op(1)); break;
			case 12: t.kind = TypeKind::Vector; t.elem = static_cast<uint32_t>(op(1)); break;
			case 18: case 20:
				t.kind = TypeKind::Struct;
				for (size_t k = 1; k < r.ops.size(); ++k)
					t.members.push_back(static_cast<uint32_t>(r.ops[k]));
				break;
			case 21:
				t.kind = TypeKind::Function;
				for (size_t k = 1; k < r.ops.size(); ++k)
					t.members.push_back(static_cast<uint32_t>(r.ops[k]));
				if (t.members.empty())
					return false;
				break;
			case 9:
				t.kind = TypeKind::Function;
				for (size_t k = 2; k < r.ops.size(); ++k)
					t.members.push_back(static_cast<uint32_t>(r.ops[k]));
				if (t.members.empty())
					return false;
				break;
			default:
				break;
			}
			m.types.push_back(std::move(t));
			break;
		}
		default:
			return false;
		}
	}
}

const Type *type_of(const Module &m, uint32_t id) noexcept
{
	return id < m.types.size() ? &m.types[id] : nullptr;
}

bool is_float(const Module &m, uint32_t type) noexcept
{
	const Type *t = type_of(m, type);
	if (t != nullptr && t->kind == TypeKind::Vector)
		t = type_of(m, t->elem);
	return t != nullptr && (t->kind == TypeKind::Float || t->kind == TypeKind::Half || t->kind == TypeKind::Double);
}

bool read_constants(Bits &b, uint32_t width, Module &m, std::vector<Value> &out)
{
	Cursor c(b, width, abbrevs_for(m, kConstants));
	Record r;
	uint32_t type = kNone;
	for (;;) {
		switch (c.next(r)) {
		case Item::End:
			return true;
		case Item::Abbrev:
			break;
		case Item::Block:
			if (!c.skip())
				return false;
			break;
		case Item::Record: {
			if (r.code == 1u) {
				if (r.ops.empty())
					return false;
				type = static_cast<uint32_t>(r.ops[0]);
				break;
			}
			if (out.size() >= kMaxValues)
				return false;
			Value v;
			v.type = type;
			switch (r.code) {
			case 2: v.kind = ValueKind::Null; break;
			case 3: v.kind = ValueKind::Undef; break;
			case 4:
				if (r.ops.empty())
					return false;
				v.kind = ValueKind::Int;
				v.i = signed_vbr(r.ops[0]);
				break;
			case 6: {
				if (r.ops.empty())
					return false;
				const Type *t = type_of(m, type);
				v.kind = ValueKind::Float;
				if (t != nullptr && t->kind == TypeKind::Float) {
					const uint32_t bits = static_cast<uint32_t>(r.ops[0]);
					float x;
					std::memcpy(&x, &bits, 4);
					v.f = x;
				} else if (t != nullptr && t->kind == TypeKind::Double) {
					const uint64_t bits = r.ops[0];
					std::memcpy(&v.f, &bits, 8);
				} else {
					v.kind = ValueKind::Other;
				}
				break;
			}
			case 7:
				v.kind = ValueKind::Aggregate;
				for (uint64_t x : r.ops)
					v.ops.push_back(static_cast<uint32_t>(x));
				break;
			default:
				v.kind = ValueKind::Other;
				break;
			}
			out.push_back(std::move(v));
			break;
		}
		default:
			return false;
		}
	}
}

bool read_metadata(Bits &b, uint32_t width, Module &m)
{
	Cursor c(b, width, abbrevs_for(m, kMetadata));
	Record r;
	std::string pending_name;
	bool named = false;
	for (;;) {
		switch (c.next(r)) {
		case Item::End:
			return true;
		case Item::Abbrev:
			break;
		case Item::Block:
			if (!c.skip())
				return false;
			break;
		case Item::Record: {
			if (r.code == 4u) {
				pending_name.clear();
				for (uint64_t ch : r.ops)
					pending_name.push_back(static_cast<char>(ch));
				named = true;
				break;
			}
			if (r.code == 10u) {
				if (!named)
					return false;
				m.named[pending_name] = r.ops;
				named = false;
				break;
			}
			if (r.code == 6u || r.code == 11u || r.code == 36u || r.code == 38u || r.code == 39u)
				break;
			if (r.code == 35u)
				return false;
			if (m.md.size() >= kMaxValues)
				return false;
			MdNode n;
			if (r.code == 1u) {
				n.kind = MdNode::String;
				for (uint64_t ch : r.ops)
					n.text.push_back(static_cast<char>(ch));
			} else if (r.code == 2u) {
				if (r.ops.size() < 2u)
					return false;
				n.kind = MdNode::Value;
				n.value = static_cast<uint32_t>(r.ops[1]);
			} else if (r.code == 3u || r.code == 5u) {
				n.kind = MdNode::Node;
				n.ops = r.ops;
			}
			m.md.push_back(std::move(n));
			break;
		}
		default:
			return false;
		}
	}
}

bool read_symtab(Bits &b, uint32_t width, Module &m)
{
	Cursor c(b, width, abbrevs_for(m, kValueSymtab));
	Record r;
	for (;;) {
		switch (c.next(r)) {
		case Item::End:
			return true;
		case Item::Abbrev:
			break;
		case Item::Block:
			if (!c.skip())
				return false;
			break;
		case Item::Record: {
			const size_t from = r.code == 1u ? 1u : r.code == 3u ? 2u : SIZE_MAX;
			if (from == SIZE_MAX || r.ops.size() < from)
				break;
			std::string name;
			for (size_t k = from; k < r.ops.size(); ++k)
				name.push_back(static_cast<char>(r.ops[k]));
			m.names[static_cast<uint32_t>(r.ops[0])] = std::move(name);
			break;
		}
		default:
			return false;
		}
	}
}

struct Reader {
	const Module &m;
	const Record &r;
	uint32_t next;
	size_t at = 0;
	bool ok = true;

	uint64_t take() noexcept
	{
		if (at >= r.ops.size()) {
			ok = false;
			return 0u;
		}
		return r.ops[at++];
	}
	uint32_t value() noexcept
	{
		const uint64_t v = take();
		return m.relative ? next - static_cast<uint32_t>(v) : static_cast<uint32_t>(v);
	}
	uint32_t value_and_type(uint32_t &type, const std::vector<Value> &local, uint32_t base) noexcept
	{
		const uint32_t v = value();
		if (!ok)
			return kNone;
		if (v >= next) {
			type = static_cast<uint32_t>(take());
		} else if (v < base) {
			type = v < m.values.size() ? m.values[v].type : kNone;
		} else {
			type = v - base < local.size() ? local[v - base].type : kNone;
		}
		return v;
	}
	bool left(size_t n) const noexcept { return r.ops.size() - at >= n; }
};

bool read_function(Bits &b, uint32_t width, Module &m, Body &body)
{
	Cursor c(b, width, abbrevs_for(m, kFunction));
	Record r;
	std::vector<Value> &vals = body.values;
	const uint32_t base = body.base;
	const auto next_id = [&]() { return base + static_cast<uint32_t>(vals.size()); };
	for (;;) {
		switch (c.next(r)) {
		case Item::End:
			return true;
		case Item::Abbrev:
			break;
		case Item::Block:
			if (c.sub_id() == kConstants) {
				if (!read_constants(b, c.sub_width(), m, vals) || !b.seek(c.sub_end()))
					return false;
			} else if (!c.skip()) {
				return false;
			}
			break;
		case Item::Record: {
			if (vals.size() >= kMaxValues)
				return false;
			Reader rd{ m, r, next_id() };
			Value v;
			v.kind = ValueKind::Inst;
			bool defines = true;
			switch (r.code) {
			case 1:
			case 33:
			case 35:
				defines = false;
				break;
			case 2: {
				uint32_t ty = kNone;
				const uint32_t a = rd.value_and_type(ty, vals, base);
				const uint32_t bb = rd.value();
				v.code = static_cast<uint8_t>(rd.take());
				v.op = Op::Binop;
				v.type = ty;
				v.fp = is_float(m, ty);
				v.ops = { a, bb };
				break;
			}
			case 3: {
				uint32_t ty = kNone;
				const uint32_t a = rd.value_and_type(ty, vals, base);
				v.type = static_cast<uint32_t>(rd.take());
				v.code = static_cast<uint8_t>(rd.take());
				v.op = Op::Cast;
				v.ops = { a };
				break;
			}
			case 26: {
				uint32_t ty = kNone;
				const uint32_t a = rd.value_and_type(ty, vals, base);
				if (!rd.left(1))
					return false;
				const uint64_t idx = rd.take();
				const Type *t = type_of(m, ty);
				uint32_t member = kNone;
				if (t != nullptr && t->kind == TypeKind::Struct && idx < t->members.size())
					member = t->members[static_cast<size_t>(idx)];
				else if (t != nullptr && (t->kind == TypeKind::Array || t->kind == TypeKind::Vector))
					member = t->elem;
				if (rd.left(1)) {
					v.ops = { a };
					break;
				}
				v.op = Op::ExtractValue;
				v.ops = { a, static_cast<uint32_t>(idx) };
				v.type = member;
				break;
			}
			case 27: {
				uint32_t ty = kNone;
				(void)rd.value_and_type(ty, vals, base);
				v.type = ty;
				break;
			}
			case 28: case 9: {
				uint32_t ty = kNone;
				const uint32_t a = rd.value_and_type(ty, vals, base);
				const uint32_t bb = rd.value();
				v.op = Op::Cmp;
				v.ops = { a, bb };
				break;
			}
			case 29: {
				uint32_t ty = kNone, cty = kNone;
				const uint32_t t = rd.value_and_type(ty, vals, base);
				const uint32_t f = rd.value();
				const uint32_t cnd = rd.value_and_type(cty, vals, base);
				v.op = Op::Select;
				v.type = ty;
				v.ops = { cnd, t, f };
				break;
			}
			case 16: {
				v.type = static_cast<uint32_t>(rd.take());
				v.op = Op::Phi;
				while (rd.ok && rd.left(2)) {
					const int64_t rel = signed_vbr(rd.take());
					(void)rd.take();
					const uint32_t id = m.relative ? static_cast<uint32_t>(static_cast<int64_t>(rd.next) - rel)
												   : static_cast<uint32_t>(rel);
					v.ops.push_back(id);
				}
				break;
			}
			case 20: {
				uint32_t ty = kNone;
				const uint32_t p = rd.value_and_type(ty, vals, base);
				if (rd.left(3)) {
					v.type = static_cast<uint32_t>(rd.take());
				} else {
					const Type *pt = type_of(m, ty);
					v.type = pt != nullptr && pt->kind == TypeKind::Pointer ? pt->elem : kNone;
				}
				v.op = Op::Load;
				v.ops = { p };
				break;
			}
			case 43: {
				(void)rd.take();
				(void)rd.take();
				while (rd.ok && rd.left(1)) {
					uint32_t ty = kNone;
					v.ops.push_back(rd.value_and_type(ty, vals, base));
				}
				break;
			}
			case 19:
			case 6:
			case 7:
			case 8:
			case 38:
			case 41:
			case 46:
			case 37:
				break;
			case 5: {
				v.type = static_cast<uint32_t>(rd.take());
				const uint32_t t = rd.value();
				const uint32_t f = rd.value();
				const uint32_t cnd = rd.value();
				v.op = Op::Select;
				v.ops = { cnd, t, f };
				break;
			}
			case 10:
			case 11:
			case 12:
			case 15:
			case 31:
			case 36:
			case 39:
			case 44:
			case 45:
			case 24:
			case 42:
				defines = false;
				break;
			case 34: {
				(void)rd.take();
				const uint64_t cc = rd.take();
				if (((cc >> 17) & 1u) != 0u)
					(void)rd.take();
				uint32_t fnty = kNone;
				if (((cc >> 15) & 1u) != 0u)
					fnty = static_cast<uint32_t>(rd.take());
				uint32_t callee_ty = kNone;
				const uint32_t callee = rd.value_and_type(callee_ty, vals, base);
				if (fnty == kNone) {
					const Type *pt = type_of(m, callee_ty);
					fnty = pt != nullptr && pt->kind == TypeKind::Pointer ? pt->elem : kNone;
				}
				const Type *ft = type_of(m, fnty);
				if (ft == nullptr || ft->kind != TypeKind::Function || ft->members.empty())
					return false;
				v.op = Op::Call;
				v.type = ft->members[0];
				v.ops.push_back(callee);
				for (size_t k = 1; k < ft->members.size() && rd.ok; ++k) {
					const Type *pt = type_of(m, ft->members[k]);
					v.ops.push_back(pt != nullptr && pt->kind == TypeKind::Label ? static_cast<uint32_t>(rd.take())
																			   : rd.value());
				}
				const Type *ret = type_of(m, v.type);
				defines = ret == nullptr || ret->kind != TypeKind::Void;
				break;
			}
			default:
				return false;
			}
			if (!rd.ok)
				return false;
			if (defines) {
				vals.push_back(std::move(v));
			} else if (v.op == Op::Call) {
				if (body.calls.size() >= kMaxValues)
					return false;
				body.calls.push_back(std::move(v));
			}
			break;
		}
		default:
			return false;
		}
	}
}

bool read_module(Bits &b, uint32_t width, Module &m)
{
	Cursor c(b, width, abbrevs_for(m, kModule));
	Record r;
	size_t body_index = 0;
	for (;;) {
		switch (c.next(r)) {
		case Item::End:
			return true;
		case Item::Abbrev:
			break;
		case Item::Block: {
			const uint32_t id = c.sub_id();
			bool ok = true;
			if (id == kBlockInfo) {
				ok = read_blockinfo(b, c.sub_width(), m);
			} else if (id == kTypes) {
				ok = read_types(b, c.sub_width(), m);
			} else if (id == kConstants) {
				ok = read_constants(b, c.sub_width(), m, m.values);
			} else if (id == kMetadata) {
				ok = read_metadata(b, c.sub_width(), m);
			} else if (id == kValueSymtab) {
				ok = read_symtab(b, c.sub_width(), m);
			} else if (id == kFunction) {
				while (body_index < m.functions.size() && !m.functions[body_index].second)
					++body_index;
				if (body_index >= m.functions.size() || m.bodies.size() >= kMaxBodies)
					return false;
				Body body;
				body.fn = m.functions[body_index++].first;
				body.base = static_cast<uint32_t>(m.values.size());
				const Value &fv = m.values[body.fn];
				const Type *ft = type_of(m, fv.type);
				if (ft != nullptr && ft->kind == TypeKind::Pointer)
					ft = type_of(m, ft->elem);
				if (ft == nullptr || ft->kind != TypeKind::Function)
					return false;
				for (size_t k = 1; k < ft->members.size(); ++k) {
					Value a;
					a.kind = ValueKind::Arg;
					a.type = ft->members[k];
					body.values.push_back(a);
				}
				ok = read_function(b, c.sub_width(), m, body);
				m.bodies.push_back(std::move(body));
			} else {
				ok = c.skip();
				break;
			}
			if (!ok || !b.seek(c.sub_end()))
				return false;
			break;
		}
		case Item::Record: {
			if (m.values.size() >= kMaxValues)
				return false;
			if (r.code == 1u) {
				m.relative = !r.ops.empty() && r.ops[0] >= 1u;
			} else if (r.code == 7u) {
				Value v;
				v.kind = ValueKind::Global;
				m.values.push_back(v);
			} else if (r.code == 8u) {
				if (r.ops.size() < 3u)
					return false;
				Value v;
				v.kind = ValueKind::Function;
				v.type = static_cast<uint32_t>(r.ops[0]);
				m.functions.emplace_back(static_cast<uint32_t>(m.values.size()), r.ops[2] == 0u);
				m.values.push_back(v);
			} else if (r.code == 9u || r.code == 14u) {
				m.values.push_back(Value{});
			}
			break;
		}
		default:
			return false;
		}
	}
}

constexpr uint32_t kDxbcMagic = 0x43425844u;
constexpr uint32_t kDxilPart = 0x4C495844u;
constexpr uint32_t kDxilMagic = 0x4C495844u;
constexpr uint32_t kVertexKind = 1u;

uint32_t read32(const uint8_t *p) noexcept
{
	uint32_t v;
	std::memcpy(&v, p, 4);
	return v;
}

bool read_container(const void *code, size_t bytes, Module &m)
{
	if (code == nullptr || bytes < 32u)
		return false;
	const auto *in = static_cast<const uint8_t *>(code);
	if (read32(in) != kDxbcMagic)
		return false;
	const uint32_t total = read32(in + 24);
	const uint32_t count = read32(in + 28);
	if (total < 32u || total > bytes || count == 0u || count > 64u || 32u + 4u * count > total)
		return false;
	for (uint32_t c = 0; c < count; ++c) {
		const uint32_t off = read32(in + 32u + 4u * c);
		if (off > total - 8u)
			return false;
		const uint32_t size = read32(in + off + 4u);
		if (size > total - off - 8u)
			return false;
		if (read32(in + off) != kDxilPart)
			continue;
		const uint8_t *part = in + off + 8u;
		if (size < 24u)
			return false;
		const uint32_t version = read32(part);
		if (((version >> 16) & 0xFFFFu) != kVertexKind || read32(part + 8u) != kDxilMagic)
			return false;
		const uint32_t bc_off = read32(part + 16u), bc_size = read32(part + 20u);
		if (bc_off > size - 8u || bc_size > size - 8u - bc_off || bc_size < 8u)
			return false;
		const uint8_t *bc = part + 8u + bc_off;
		if (bc[0] != 'B' || bc[1] != 'C' || bc[2] != 0xC0u || bc[3] != 0xDEu)
			return false;
		Bits b(bc + 4, bc_size - 4u);
		Record r;
		bool module = false;
		while (!b.at_end() && b.ok()) {
			Cursor top(b, 2u, {});
			const Item item = top.next(r);
			if (item != Item::Block)
				return module;
			const uint32_t id = top.sub_id();
			bool ok = true;
			if (id == kModule) {
				ok = read_module(b, top.sub_width(), m);
				module = ok;
			} else if (id == kBlockInfo) {
				ok = read_blockinfo(b, top.sub_width(), m);
			} else {
				ok = top.skip();
				continue;
			}
			if (!ok || !b.seek(top.sub_end()))
				return false;
			if (module)
				return true;
		}
		return module;
	}
	return false;
}

constexpr uint32_t kOpLoadInput = 4, kOpStoreOutput = 5, kOpFMad = 46, kOpFma = 47, kOpDot2 = 54, kOpDot3 = 55,
	kOpDot4 = 56, kOpCreateHandle = 57, kOpCBufferLoad = 58, kOpCBufferLoadLegacy = 59, kOpAnnotateHandle = 216,
	kOpCreateHandleFromBinding = 217;
constexpr int64_t kSemanticPosition = 3, kClassCBuffer = 2;

struct CBuffer {
	uint32_t space = 0;
	uint32_t lower = 0;
};

class Shader {
public:
	explicit Shader(const Module &m) : m_(m) {}

	bool load()
	{
		const auto ep = m_.named.find("dx.entryPoints");
		if (ep == m_.named.end() || ep->second.empty())
			return false;
		const MdNode *entry = md(static_cast<uint32_t>(ep->second[0]));
		if (entry == nullptr || entry->kind != MdNode::Node || entry->ops.size() < 4u)
			return false;
		const MdNode *fn = md_op(*entry, 0);
		if (fn == nullptr || fn->kind != MdNode::Value)
			return false;
		for (const Body &b : m_.bodies) {
			if (b.fn == fn->value)
				body_ = &b;
		}
		if (body_ == nullptr)
			return false;
		const MdNode *sigs = md_op(*entry, 2);
		const MdNode *outs = sigs != nullptr && sigs->kind == MdNode::Node ? md_op(*sigs, 1) : nullptr;
		if (outs == nullptr || outs->kind != MdNode::Node)
			return false;
		for (size_t k = 0; k < outs->ops.size(); ++k) {
			const MdNode *e = md_op(*outs, k);
			int64_t id = 0, kind = 0;
			if (e != nullptr && e->kind == MdNode::Node && md_int(md_op(*e, 0), id) && md_int(md_op(*e, 3), kind) &&
				kind == kSemanticPosition)
				pos_ = static_cast<int64_t>(id);
		}
		if (pos_ < 0)
			return false;
		const MdNode *res = md_op(*entry, 3);
		const MdNode *cbs = res != nullptr && res->kind == MdNode::Node ? md_op(*res, 2) : nullptr;
		if (cbs != nullptr && cbs->kind == MdNode::Node) {
			for (size_t k = 0; k < cbs->ops.size(); ++k) {
				const MdNode *e = md_op(*cbs, k);
				int64_t id = 0, space = 0, lower = 0;
				if (e != nullptr && e->kind == MdNode::Node && md_int(md_op(*e, 0), id) &&
					md_int(md_op(*e, 3), space) && md_int(md_op(*e, 4), lower) && id >= 0 && space >= 0 && lower >= 0)
					cbuffers_[static_cast<uint32_t>(id)] = CBuffer{ static_cast<uint32_t>(space), static_cast<uint32_t>(lower) };
			}
		}
		return true;
	}

	const Value *value(uint32_t id) const noexcept
	{
		if (id < m_.values.size())
			return &m_.values[id];
		if (body_ != nullptr && id >= body_->base && id - body_->base < body_->values.size())
			return &body_->values[id - body_->base];
		return nullptr;
	}

	uint32_t first() const noexcept { return body_->base; }
	uint32_t end() const noexcept { return body_->base + static_cast<uint32_t>(body_->values.size()); }
	const std::vector<Value> &calls() const noexcept { return body_->calls; }

	bool constant_int(uint32_t id, int64_t &out) const noexcept
	{
		const Value *v = value(id);
		if (v == nullptr)
			return false;
		if (v->kind == ValueKind::Null) {
			const Type *t = type_of(m_, v->type);
			if (t == nullptr || t->kind != TypeKind::Int)
				return false;
			out = 0;
			return true;
		}
		if (v->kind != ValueKind::Int)
			return false;
		out = v->i;
		return true;
	}

	int64_t dx_op(const Value &v) const
	{
		if (v.kind != ValueKind::Inst || v.op != Op::Call || v.ops.size() < 2u)
			return -1;
		const Value *callee = value(v.ops[0]);
		if (callee == nullptr || callee->kind != ValueKind::Function)
			return -1;
		const auto name = m_.names.find(v.ops[0]);
		if (!m_.names.empty() && (name == m_.names.end() || name->second.compare(0, 6, "dx.op.") != 0))
			return -1;
		int64_t op = 0;
		return constant_int(v.ops[1], op) ? op : -1;
	}

	int64_t position() const noexcept { return pos_; }

	bool binding(uint32_t handle, uint32_t &slot, uint32_t &space) const
	{
		for (int guard = 0; guard < 4; ++guard) {
			const Value *h = value(handle);
			if (h == nullptr)
				return false;
			const int64_t op = dx_op(*h);
			if (op == kOpAnnotateHandle && h->ops.size() >= 3u) {
				handle = h->ops[2];
				continue;
			}
			int64_t cls = 0, range = 0, index = 0;
			if (op == kOpCreateHandle && h->ops.size() >= 6u) {
				if (!constant_int(h->ops[2], cls) || cls != kClassCBuffer || !constant_int(h->ops[3], range) ||
					!constant_int(h->ops[4], index) || index < 0)
					return false;
				const auto it = cbuffers_.find(static_cast<uint32_t>(range));
				if (it == cbuffers_.end())
					return false;
				slot = static_cast<uint32_t>(index);
				space = it->second.space;
				return true;
			}
			if (op == kOpCreateHandleFromBinding && h->ops.size() >= 5u) {
				const Value *bind = value(h->ops[2]);
				if (bind == nullptr || bind->kind != ValueKind::Aggregate || bind->ops.size() < 4u)
					return false;
				int64_t sp = 0;
				if (!constant_int(bind->ops[2], sp) || !constant_int(bind->ops[3], cls) || cls != kClassCBuffer ||
					!constant_int(h->ops[3], index) || index < 0 || sp < 0)
					return false;
				slot = static_cast<uint32_t>(index);
				space = static_cast<uint32_t>(sp);
				return true;
			}
			return false;
		}
		return false;
	}

	bool game_constant(uint32_t id, bool &known, uint32_t &slot, uint32_t &space, uint32_t &reg, uint8_t &lane) const
	{
		known = false;
		const Value *v = value(id);
		if (v == nullptr || v->kind != ValueKind::Inst)
			return false;
		if (v->op == Op::ExtractValue && v->ops.size() == 2u) {
			const Value *load = value(v->ops[0]);
			if (load == nullptr || dx_op(*load) != kOpCBufferLoadLegacy || load->ops.size() < 4u)
				return false;
			const Type *t = type_of(m_, load->type);
			const Type *e = t != nullptr && t->kind == TypeKind::Struct && t->members.size() == 4u
				? type_of(m_, t->members[0]) : nullptr;
			int64_t r = 0;
			if (e != nullptr && e->kind == TypeKind::Float && v->ops[1] < 4u && constant_int(load->ops[3], r) &&
				r >= 0 && r < 4096 && binding(load->ops[2], slot, space)) {
				reg = static_cast<uint32_t>(r);
				lane = static_cast<uint8_t>(v->ops[1]);
				known = true;
			}
			return true;
		}
		if (dx_op(*v) == kOpCBufferLoad && v->ops.size() >= 4u) {
			int64_t off = 0;
			const Type *t = type_of(m_, v->type);
			if (t != nullptr && t->kind == TypeKind::Float && constant_int(v->ops[3], off) && off >= 0 &&
				off < 4096 * 16 && (off & 3) == 0 && binding(v->ops[2], slot, space)) {
				reg = static_cast<uint32_t>(off / 16);
				lane = static_cast<uint8_t>((off % 16) / 4);
				known = true;
			}
			return true;
		}
		return false;
	}

private:
	const MdNode *md(uint32_t id) const noexcept { return id < m_.md.size() ? &m_.md[id] : nullptr; }
	const MdNode *md_op(const MdNode &n, size_t k) const noexcept
	{
		if (k >= n.ops.size() || n.ops[k] == 0u)
			return nullptr;
		return md(static_cast<uint32_t>(n.ops[k] - 1u));
	}
	bool md_int(const MdNode *n, int64_t &out) const noexcept
	{
		return n != nullptr && n->kind == MdNode::Value && constant_int(n->value, out);
	}

	const Module &m_;
	const Body *body_ = nullptr;
	int64_t pos_ = -1;
	std::unordered_map<uint32_t, CBuffer> cbuffers_;
};

bool position_stores(const Shader &s, std::vector<uint32_t> stores[4])
{
	bool any = false;
	for (const Value &call : s.calls()) {
		const Value *v = &call;
		if (s.dx_op(*v) != kOpStoreOutput || v->ops.size() < 6u)
			continue;
		int64_t sig = 0, row = 0, col = 0;
		if (!s.constant_int(v->ops[2], sig) || sig != s.position())
			continue;
		if (!s.constant_int(v->ops[3], row) || !s.constant_int(v->ops[4], col) || row != 0 || col < 0 || col > 3)
			return false;
		stores[col].push_back(v->ops[5]);
		any = true;
	}
	return any;
}

template <class F>
void operands(const Shader &s, const Value &v, F &&f)
{
	if (v.kind != ValueKind::Inst)
		return;
	if (v.op == Op::Call) {
		const int64_t op = s.dx_op(v);
		if (op < 0 || op == kOpLoadInput || op == kOpCBufferLoad || op == kOpCBufferLoadLegacy)
			return;
		for (size_t k = 2; k < v.ops.size(); ++k)
			f(v.ops[k]);
		return;
	}
	if (v.op == Op::ExtractValue) {
		f(v.ops[0]);
		return;
	}
	for (uint32_t o : v.ops)
		f(o);
}

template <class F>
void products(const Shader &s, const Value &v, F &&f)
{
	if (v.kind != ValueKind::Inst)
		return;
	if (v.op == Op::Binop && v.fp && v.code == 2u && v.ops.size() == 2u) {
		f(v.ops[0], v.ops[1], -1);
		return;
	}
	const int64_t op = s.dx_op(v);
	if ((op == kOpFMad || op == kOpFma) && v.ops.size() >= 5u) {
		f(v.ops[2], v.ops[3], -1);
	} else if (op >= kOpDot2 && op <= kOpDot4) {
		const size_t n = static_cast<size_t>(op - kOpDot2 + 2);
		if (v.ops.size() >= 2u + 2u * n) {
			for (size_t k = 0; k < n; ++k)
				f(v.ops[2u + k], v.ops[2u + n + k], static_cast<int>(k));
		}
	}
}

struct Deps {
	std::vector<uint64_t> inputs;
	std::vector<uint8_t> constant;
};

Deps dependencies(const Shader &s)
{
	Deps d;
	const uint32_t first = s.first(), n = s.end() - first;
	d.inputs.assign(n, 0u);
	d.constant.assign(n, 0u);
	std::unordered_map<uint64_t, uint32_t> leaf;
	for (int pass = 0; pass < 8; ++pass) {
		bool changed = false;
		for (uint32_t k = 0; k < n; ++k) {
			const Value *v = s.value(first + k);
			uint64_t in = d.inputs[k];
			uint8_t cb = d.constant[k];
			const int64_t op = s.dx_op(*v);
			if (op == kOpLoadInput && v->ops.size() >= 5u) {
				int64_t sig = 0, row = 0, col = 0;
				uint64_t key = ~0ull;
				if (s.constant_int(v->ops[2], sig) && s.constant_int(v->ops[3], row) && s.constant_int(v->ops[4], col))
					key = (static_cast<uint64_t>(sig & 0xFFFF) << 32) | (static_cast<uint64_t>(row & 0xFFFF) << 8) |
						static_cast<uint64_t>(col & 0xFF);
				auto it = leaf.find(key);
				if (it == leaf.end())
					it = leaf.emplace(key, std::min<uint32_t>(static_cast<uint32_t>(leaf.size()), 63u)).first;
				in |= 1ull << it->second;
			} else if (op == kOpCBufferLoad || op == kOpCBufferLoadLegacy) {
				cb = 1u;
			} else {
				operands(s, *v, [&](uint32_t o) {
					if (o >= first && o - first < n) {
						in |= d.inputs[o - first];
						cb |= d.constant[o - first];
					}
				});
			}
			if (in != d.inputs[k] || cb != d.constant[k]) {
				d.inputs[k] = in;
				d.constant[k] = cb;
				changed = true;
			}
		}
		if (!changed)
			break;
	}
	return d;
}

uint32_t bits(uint64_t x) noexcept
{
	uint32_t n = 0;
	for (; x != 0u; x &= x - 1u)
		++n;
	return n;
}

JitterVertexClass classify(const Shader &s)
{
	std::vector<uint32_t> stores[4];
	if (!position_stores(s, stores))
		return JitterVertexClass::Unknown;
	const Deps d = dependencies(s);
	const uint32_t first = s.first(), n = s.end() - first;
	const auto local = [&](uint32_t id) { return id >= first && id - first < n; };
	const auto constant = [&](uint32_t id) { return local(id) && d.constant[id - first] != 0u && d.inputs[id - first] == 0u; };
	std::vector<uint8_t> cone(n, 0u);
	for (uint32_t c = 0; c < 4u; ++c) {
		std::fill(cone.begin(), cone.end(), static_cast<uint8_t>(0u));
		std::vector<uint32_t> work;
		for (uint32_t v : stores[c]) {
			if (local(v) && cone[v - first] == 0u) {
				cone[v - first] = 1u;
				work.push_back(v);
			}
		}
		while (!work.empty()) {
			const uint32_t id = work.back();
			work.pop_back();
			operands(s, *s.value(id), [&](uint32_t o) {
				if (local(o) && cone[o - first] == 0u) {
					cone[o - first] = 1u;
					work.push_back(o);
				}
			});
		}
		uint32_t terms = 0;
		uint64_t reach = 0;
		for (uint32_t k = 0; k < n; ++k) {
			if (cone[k] == 0u)
				continue;
			products(s, *s.value(first + k), [&](uint32_t a, uint32_t b, int) {
				const bool ka = constant(a), kb = constant(b);
				if (ka == kb)
					return;
				const uint32_t other = ka ? b : a;
				const uint64_t in = local(other) ? d.inputs[other - first] : 0u;
				if (in == 0u)
					return;
				++terms;
				reach |= in;
			});
		}
		if (terms >= 2u && bits(reach) >= 2u)
			return JitterVertexClass::Transformed;
	}
	return JitterVertexClass::Screen;
}

class WTracer {
public:
	WTracer(const Shader &s, DxbcWTerms &out) : s_(s), out_(out) {}

	bool run(uint32_t w)
	{
		std::vector<uint32_t> work{ w };
		int budget = 256;
		while (!work.empty()) {
			if (--budget < 0)
				return false;
			const uint32_t id = work.back();
			work.pop_back();
			const Value *v = s_.value(id);
			if (v == nullptr)
				return false;
			if (v->kind == ValueKind::Float || v->kind == ValueKind::Int || v->kind == ValueKind::Null)
				continue;
			if (v->kind != ValueKind::Inst)
				return false;
			bool known = false;
			uint32_t slot = 0, space = 0, reg = 0;
			uint8_t lane = 0;
			if (s_.game_constant(id, known, slot, space, reg, lane)) {
				if (!known)
					return false;
				addends_.push_back(Term{ slot, space, reg, lane });
				continue;
			}
			const int64_t op = s_.dx_op(*v);
			if (v->op == Op::Binop && v->fp && (v->code == 0u || v->code == 1u) && v->ops.size() == 2u) {
				work.push_back(v->ops[0]);
				work.push_back(v->ops[1]);
			} else if (v->op == Op::Binop && v->fp && v->code == 2u && v->ops.size() == 2u) {
				if (!product(v->ops[0], v->ops[1], -1))
					return false;
			} else if ((op == kOpFMad || op == kOpFma) && v->ops.size() >= 5u) {
				if (!product(v->ops[2], v->ops[3], -1))
					return false;
				work.push_back(v->ops[4]);
			} else if (op >= kOpDot2 && op <= kOpDot4) {
				const size_t n = static_cast<size_t>(op - kOpDot2 + 2);
				if (v->ops.size() < 2u + 2u * n)
					return false;
				for (size_t k = 0; k < n; ++k) {
					if (!product(v->ops[2u + k], v->ops[2u + n + k], static_cast<int>(k)))
						return false;
				}
			} else if (op == kOpLoadInput) {
				if (input_lane(id) != 3)
					return false;
			} else {
				return false;
			}
		}
		return name();
	}

private:
	struct Term {
		uint32_t slot = 0, space = 0, reg = 0;
		uint8_t lane = 0;
	};
	struct Product {
		Term k;
		uint32_t other = 0;
		int pair = -1;
		bool literal = false;
	};

	int input_lane(uint32_t id) const
	{
		const Value *v = s_.value(id);
		int64_t col = 0;
		if (v == nullptr || s_.dx_op(*v) != kOpLoadInput || v->ops.size() < 5u || !s_.constant_int(v->ops[4], col) ||
			col < 0 || col > 3)
			return -1;
		return static_cast<int>(col);
	}

	bool literal(uint32_t id) const
	{
		const Value *v = s_.value(id);
		return v != nullptr && (v->kind == ValueKind::Float || v->kind == ValueKind::Int || v->kind == ValueKind::Null);
	}

	bool product(uint32_t a, uint32_t b, int pair)
	{
		Term ta, tb;
		bool known_a = false, known_b = false;
		const bool ga = s_.game_constant(a, known_a, ta.slot, ta.space, ta.reg, ta.lane);
		const bool gb = s_.game_constant(b, known_b, tb.slot, tb.space, tb.reg, tb.lane);
		const bool ka = ga || literal(a), kb = gb || literal(b);
		if ((ga && !known_a) || (gb && !known_b))
			return false;
		if (ka && kb)
			return true;
		if (!ka && !kb)
			return false;
		Product p;
		p.k = ka ? ta : tb;
		p.literal = ka ? !ga : !gb;
		p.other = ka ? b : a;
		p.pair = pair;
		products_.push_back(p);
		return products_.size() <= 64u;
	}

	bool name()
	{
		std::vector<int> lanes(products_.size(), -1);
		std::vector<size_t> made;
		for (size_t i = 0; i < products_.size(); ++i) {
			const Product &p = products_[i];
			if (p.pair >= 0)
				lanes[i] = p.pair;
			else if (input_lane(p.other) >= 0)
				lanes[i] = input_lane(p.other);
			else if (!p.literal)
				made.push_back(i);
			else
				return false;
		}
		if (!made.empty()) {
			const Term &k0 = products_[made[0]].k;
			bool row = true, columns = true;
			uint32_t lo = k0.reg, hi = k0.reg;
			for (size_t a = 0; a < made.size(); ++a) {
				const Term &k = products_[made[a]].k;
				row = row && k.slot == k0.slot && k.space == k0.space && k.reg == k0.reg;
				columns = columns && k.slot == k0.slot && k.space == k0.space && k.lane == k0.lane;
				for (size_t b = 0; b < a; ++b) {
					const Term &o = products_[made[b]].k;
					row = row && o.lane != k.lane;
					columns = columns && o.reg != k.reg;
				}
				lo = std::min(lo, k.reg);
				hi = std::max(hi, k.reg);
			}
			columns = columns && hi - lo <= 3u;
			uint32_t base = lo;
			if (columns) {
				for (const Term &t : addends_) {
					if (t.slot == k0.slot && t.space == k0.space && t.lane == k0.lane && t.reg > hi && t.reg >= 3u &&
						t.reg - 3u <= lo)
						base = t.reg - 3u;
				}
			}
			for (size_t i : made) {
				const Term &k = products_[i].k;
				const int by_row = k.lane;
				const int by_column = static_cast<int>(k.reg - base);
				if (made.size() == 1u) {
					if ((by_row == 3) != (by_column == 3))
						return false;
					lanes[i] = by_row;
				} else if (row) {
					lanes[i] = by_row;
				} else if (columns) {
					lanes[i] = by_column;
				} else {
					return false;
				}
			}
		}
		for (size_t i = 0; i < products_.size(); ++i) {
			if (lanes[i] == 3)
				continue;
			if (products_[i].literal)
				return false;
			const Term &k = products_[i].k;
			bool seen = false;
			for (uint32_t t = 0; t < out_.count; ++t)
				seen = seen || (out_.slot[t] == k.slot && out_.space[t] == k.space && out_.reg[t] == k.reg &&
					out_.lane[t] == k.lane);
			if (seen)
				continue;
			if (out_.count == DxbcWTerms::kMax)
				return false;
			out_.slot[out_.count] = k.slot;
			out_.space[out_.count] = k.space;
			out_.reg[out_.count] = static_cast<uint16_t>(k.reg);
			out_.lane[out_.count] = k.lane;
			++out_.count;
		}
		return true;
	}

	const Shader &s_;
	DxbcWTerms &out_;
	std::vector<Term> addends_;
	std::vector<Product> products_;
};

}
}

JitterVertexClass classify_dxil_vertex_shader(const void *code, size_t bytes)
{
	try {
		dxil::Module m;
		if (!dxil::read_container(code, bytes, m))
			return JitterVertexClass::Unknown;
		dxil::Shader s(m);
		if (!s.load())
			return JitterVertexClass::Unknown;
		return dxil::classify(s);
	} catch (...) {
		return JitterVertexClass::Unknown;
	}
}

bool dxil_position_w_terms(const void *code, size_t bytes, DxbcWTerms *out)
{
	if (out == nullptr)
		return false;
	*out = DxbcWTerms{};
	try {
		dxil::Module m;
		if (!dxil::read_container(code, bytes, m))
			return false;
		dxil::Shader s(m);
		if (!s.load())
			return false;
		std::vector<uint32_t> stores[4];
		if (!dxil::position_stores(s, stores) || stores[3].size() != 1u)
			return false;
		dxil::WTracer t(s, *out);
		if (t.run(stores[3][0]))
			return true;
	} catch (...) {
	}
	*out = DxbcWTerms{};
	return false;
}

}
