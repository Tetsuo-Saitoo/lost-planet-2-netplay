// SPA (XDBF) reader: see spa.h.
#include "spa.h"

#include <map>

namespace lp2net {
namespace {
constexpr uint32_t Tag(const char (&s)[5]) {
	return (uint32_t)(BYTE)s[0] << 24 | (uint32_t)(BYTE)s[1] << 16 | (uint32_t)(BYTE)s[2] << 8 | (BYTE)s[3];
}
constexpr uint16_t kNsMetadata = 1;
constexpr uint16_t kNsStrings = 3;
constexpr uint64_t kLocaleEnglish = 1;

// Bounds-checked big-endian reader over one section.
struct Reader {
	const BYTE* p;
	size_t size, pos = 0;
	bool ok = true;
	bool Need(size_t n) {
		if (!ok || pos + n > size) {
			ok = false;
		}
		return ok;
	}
	uint64_t Be(size_t n) {
		uint64_t v = 0;
		if (Need(n)) {
			for (size_t i = 0; i < n; ++i) {
				v = v << 8 | p[pos + i];
			}
			pos += n;
		}
		return v;
	}
	uint8_t U8() { return (uint8_t)Be(1); }
	uint16_t U16() { return (uint16_t)Be(2); }
	uint32_t U32() { return (uint32_t)Be(4); }
	void Skip(size_t n) {
		if (Need(n)) {
			pos += n;
		}
	}
};

struct Section {
	const BYTE* p = nullptr;
	size_t size = 0;
};

// Section header shared by every SPA section: magic, version, size.
bool Header(Reader& r, uint32_t magic) {
	return r.U32() == magic && (r.U32(), r.U32(), r.ok);
}

std::map<uint16_t, std::string> ParseStrings(Section s) {
	std::map<uint16_t, std::string> out;
	Reader r{s.p, s.size};
	if (!Header(r, Tag("XSTR"))) {
		return out;
	}
	uint16_t count = r.U16();
	for (uint16_t i = 0; i < count && r.ok; ++i) {
		uint16_t id = r.U16();
		uint16_t len = r.U16();
		if (r.Need(len)) {
			out[id].assign((const char*)r.p + r.pos, len);
			r.pos += len;
		}
	}
	return out;
}
}  // namespace

const SpaField* SpaView::Rating() const {
	for (const SpaField& f : fields) {
		if (f.attributeId == kColumnRating) {
			return &f;
		}
	}
	return nullptr;
}

const SpaField* SpaView::ByColumn(uint16_t column) const {
	for (const SpaField& f : fields) {
		if (f.attributeId == column) {
			return &f;
		}
	}
	for (const SpaField& f : fields) {
		if ((uint16_t)f.propertyId == column) {
			return &f;
		}
	}
	return nullptr;
}

const SpaField* SpaView::ByProperty(uint32_t propertyId) const {
	for (const SpaField& f : fields) {
		if (f.propertyId == propertyId) {
			return &f;
		}
	}
	return nullptr;
}

const SpaAchievement* Spa::Achievement(uint32_t id) const {
	for (const SpaAchievement& a : achievements) {
		if (a.id == id) {
			return &a;
		}
	}
	return nullptr;
}

const SpaView* Spa::View(uint32_t id) const {
	for (const SpaView& v : views) {
		if (v.id == id) {
			return &v;
		}
	}
	return nullptr;
}

bool ParseSpa(const BYTE* data, size_t size, Spa* out, std::string* error) {
	*out = Spa{};
	auto fail = [&](const char* why) {
		*error = why;
		return false;
	};
	Reader h{data, size};
	if (h.U32() != Tag("XDBF")) {
		return fail("not an XDBF file");
	}
	h.U32();  // version
	uint32_t tableSize = h.U32(), entryCount = h.U32(), freeSize = h.U32();
	h.U32();  // free entries in use
	size_t base = 24 + (size_t)tableSize * 18 + (size_t)freeSize * 8;
	if (!h.ok || entryCount > tableSize || base > size) {
		return fail("bad XDBF header");
	}
	std::map<std::pair<uint16_t, uint64_t>, Section> sections;
	for (uint32_t i = 0; i < entryCount; ++i) {
		uint16_t ns = h.U16();
		uint64_t id = h.Be(8);
		uint32_t offset = h.U32(), len = h.U32();
		if (!h.ok || base + offset + len > size) {
			return fail("bad XDBF entry");
		}
		sections[{ns, id}] = Section{data + base + offset, len};
	}
	auto strings = ParseStrings(sections[{kNsStrings, kLocaleEnglish}]);
	auto str = [&](uint16_t id) {
		auto it = strings.find(id);
		return it != strings.end() ? it->second : std::string();
	};

	// XACH: 36-byte records
	Section ach = sections[{kNsMetadata, Tag("XACH")}];
	Reader a{ach.p, ach.size};
	if (!Header(a, Tag("XACH"))) {
		return fail("no XACH section");
	}
	uint16_t achCount = a.U16();
	for (uint16_t i = 0; i < achCount && a.ok; ++i) {
		SpaAchievement x;
		x.id = a.U16();
		x.name = str(a.U16());
		x.description = str(a.U16());
		x.unachieved = str(a.U16());
		x.imageId = a.U32();
		x.gamerscore = a.U16();
		a.U16();
		x.flags = a.U32();
		a.Skip(16);
		if (a.ok) {
			out->achievements.push_back(std::move(x));
		}
	}
	if (!a.ok) {
		return fail("truncated XACH section");
	}

	// XVC2: the shared view layouts (fields + XPBM property list), then the views that use them
	Section vc = sections[{kNsMetadata, Tag("XVC2")}];
	Reader v{vc.p, vc.size};
	if (!Header(v, Tag("XVC2"))) {
		return fail("no XVC2 section");
	}
	struct Shared {
		std::vector<SpaField> fields;
		std::vector<uint32_t> properties;
	};
	std::vector<Shared> shared(v.U16());
	for (Shared& s : shared) {
		uint16_t columns = v.U16(), rows = v.U16();
		v.Skip(8);
		for (int i = 0; i < columns + rows && v.ok; ++i) {
			size_t start = v.pos;
			uint32_t fieldSize = v.U32();
			SpaField f;
			f.propertyId = v.U32();
			f.hidden = (v.U32() & 1) != 0;
			f.attributeId = v.U16();
			f.name = str(v.U16());
			f.aggregation = v.U16();
			if (fieldSize < v.pos - start || !v.Need(start + fieldSize - v.pos)) {
				v.ok = false;
				break;
			}
			v.pos = start + fieldSize;
			if (f.propertyId) {
				s.fields.push_back(std::move(f));
			}
		}
		// XPBM: magic, version, size (not reliable), 4 bytes, u32 count, property ids
		if (v.U32() != Tag("XPBM")) {
			v.ok = false;
		}
		v.Skip(12);
		uint32_t props = v.U32();
		for (uint32_t i = 0; i < props && v.ok; ++i) {
			s.properties.push_back(v.U32());
		}
	}
	uint16_t viewCount = v.U16();
	for (uint16_t i = 0; i < viewCount && v.ok; ++i) {
		SpaView view;
		view.id = v.U32();
		view.flags = v.U32();
		uint16_t sharedIndex = v.U16();
		view.name = str(v.U16());
		v.U32();
		if (!v.ok || sharedIndex >= shared.size()) {
			v.ok = false;
			break;
		}
		view.fields = shared[sharedIndex].fields;
		view.properties = shared[sharedIndex].properties;
		out->views.push_back(std::move(view));
	}
	if (!v.ok) {
		return fail("truncated XVC2 section");
	}
	out->loaded = true;
	return true;
}

bool LoadSpaResource(HMODULE module, Spa* out, std::string* error) {
	if (!module) {
		module = GetModuleHandleW(nullptr);
	}
	// LP2 stores it under the resource type *named* "RT_RCDATA", not the numeric RT_RCDATA
	HRSRC res = FindResourceW(module, L"SPAFILE", L"RT_RCDATA");
	if (!res) {
		res = FindResourceW(module, L"SPAFILE", MAKEINTRESOURCEW(10));
	}
	HGLOBAL mem = res ? LoadResource(module, res) : nullptr;
	const BYTE* data = mem ? (const BYTE*)LockResource(mem) : nullptr;
	if (!data) {
		*error = "no SPAFILE resource in the game exe";
		return false;
	}
	return ParseSpa(data, SizeofResource(module, res), out, error);
}

}  // namespace lp2net
