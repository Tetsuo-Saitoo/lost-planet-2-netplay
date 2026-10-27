// Voice chat (xhv.cpp): GFWL's voice engine for the game, and what the overlay shows and changes.
#pragma once

#include "gfwl.h"

#include <string>
#include <vector>

namespace lp2net {

struct VoiceTalkerView {
	XUID xuid = 0;
	std::string name;
	bool talking = false;
	bool muted = false;      // on our mute list
	bool gameMuted = false;  // the game turned them off (playback priority "never")
	bool reachable = false;  // we know which peer they are, so our voice reaches them
};

struct VoiceView {
	bool engine = false;  // the game created its voice engine
	bool active = false;  // microphone open: in a session with other talkers, or the microphone test
	std::string source;   // where our voice comes from
	bool talking = false;
	bool micTest = false;
	std::vector<VoiceTalkerView> talkers;
	std::string counters;
};

VoiceView VoiceStatus();
void VoiceSetMuted(XUID xuid, bool muted);
void VoiceSetMicTest(bool on);  // hear yourself, without the game's help
void VoiceShutdown();           // XLiveUninitialize

}  // namespace lp2net
