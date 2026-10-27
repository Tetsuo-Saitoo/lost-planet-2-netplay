// The game's own title data (SPA): achievements, stats views and their English strings, read at runtime from the
// "SPAFILE" resource in the game exe. Nothing of Capcom's is shipped with LP2 Netplay.
//
// The SPA is an XDBF container (big-endian). Namespace 1 holds the sections we read (XACH achievements, XVC2 stats
// views), namespace 3 the string tables by locale (1 = English). Layouts are in MODLOG.md, "Stats, leaderboards and
// achievements". No dependencies beyond Windows, so tests can link it on its own.
#pragma once

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

namespace lp2net {

struct SpaAchievement {
	uint16_t id = 0;
	std::string name, description, unachieved;  // UTF-8
	uint32_t imageId = 0;
	uint16_t gamerscore = 0;
	uint32_t flags = 0;  // low 3 bits: type; 0x8: show while locked
};

// One column of a stats view.
struct SpaField {
	uint32_t propertyId = 0;  // the XUSER_PROPERTY id the game writes
	uint16_t attributeId = 0;  // the column id the game reads (0xFFFF rank, 0xFFFE rating, 0xFFFD gamertag)
	uint16_t aggregation = 0;  // kAggregateLast, kAggregateMax, ...
	bool hidden = false;
	std::string name;
};
constexpr uint16_t kAggregateLast = 0x8001;
constexpr uint16_t kAggregateMax = 0x800B;
constexpr uint16_t kColumnRank = 0xFFFF;
constexpr uint16_t kColumnRating = 0xFFFE;
constexpr uint16_t kColumnGamertag = 0xFFFD;

struct SpaView {
	uint32_t id = 0;
	uint32_t flags = 0;  // 0x30 on the arbitrated (hidden) views
	std::string name;
	std::vector<SpaField> fields;     // shared by every view with the same layout; the empty "row" entry dropped
	std::vector<uint32_t> properties;  // every property id written to this view (the XPBM list)
	const SpaField* Rating() const;
	const SpaField* ByColumn(uint16_t column) const;  // by attribute id, else by the property id's low word
	const SpaField* ByProperty(uint32_t propertyId) const;
};

struct Spa {
	bool loaded = false;
	std::vector<SpaAchievement> achievements;
	std::vector<SpaView> views;
	const SpaAchievement* Achievement(uint32_t id) const;
	const SpaView* View(uint32_t id) const;
};

// Parse an SPA image; false (and a reason) on anything unexpected.
bool ParseSpa(const BYTE* data, size_t size, Spa* out, std::string* error);
// The "SPAFILE" resource of `module` (the game exe when nullptr).
bool LoadSpaResource(HMODULE module, Spa* out, std::string* error);

// The game's SPA, loaded on first use (logs the outcome).
const Spa& GameSpa();

}  // namespace lp2net
