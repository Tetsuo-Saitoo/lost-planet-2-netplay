// Reads the SPA (achievements, stats views) out of an installed Lost Planet 2 exe, the way xlive.dll does at
// runtime, and prints a summary. Checks the parser against the real data without starting the game.
//   spatest "<game dir>\LP2DX9.exe" [-v]   -> prints PASS or FAIL
#include "../src/xlive/spa.h"

#include <cstdio>
#include <cstring>

using namespace lp2net;

int main(int argc, char** argv) {
	if (argc < 2) {
		printf("usage: spatest <LP2DX9.exe> [-v]\n");
		return 2;
	}
	bool verbose = argc > 2 && !strcmp(argv[2], "-v");
	HMODULE exe = LoadLibraryExA(argv[1], nullptr, LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
	if (!exe) {
		printf("FAIL cannot open %s (%lu)\n", argv[1], GetLastError());
		return 1;
	}
	Spa spa;
	std::string error;
	if (!LoadSpaResource(exe, &spa, &error)) {
		printf("FAIL %s\n", error.c_str());
		return 1;
	}
	int gamerscore = 0, named = 0;
	for (const SpaAchievement& a : spa.achievements) {
		gamerscore += a.gamerscore;
		named += !a.name.empty() && !a.description.empty();
		if (verbose) {
			printf("ach %2u %3uG flags %02X  %s: %s\n", a.id, a.gamerscore, a.flags, a.name.c_str(),
			       a.description.c_str());
		}
	}
	int rated = 0;
	for (const SpaView& v : spa.views) {
		rated += v.Rating() != nullptr;
		if (verbose) {
			printf("view %08X flags %02X %-24s", v.id, v.flags, v.name.c_str());
			for (const SpaField& f : v.fields) {
				printf(" %X:%08X/%X%s", f.attributeId, f.propertyId, f.aggregation, f.hidden ? "h" : "");
			}
			printf("  [%zu props]\n", v.properties.size());
		}
	}
	printf("%zu achievements (%d with name and description, %d G), %zu stats views (%d rated)\n",
	       spa.achievements.size(), named, gamerscore, spa.views.size(), rated);
	const SpaView* battle = spa.View(1);
	const SpaView* skill = spa.View(0xFFFF0000);
	bool pass = spa.achievements.size() == 50 && named == 50 && battle && battle->Rating() &&
	            battle->Rating()->propertyId == 0x2000001D && skill && skill->properties.size() == 4;
	printf("%s\n", pass ? "PASS" : "FAIL");
	return pass ? 0 : 1;
}
