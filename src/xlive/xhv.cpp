// Voice chat: GFWL's XHV engine (XHVCreateEngine #5008, IXHVEngine) and the mute list (XUserMuteListQuery #5314).
//
// How LP2 drives it (NetworkXboxVoice, read from the analysis copy): one engine for the whole run, one local talker
// (user 0) in voice chat mode (loopback is mode 1, voice chat 2), up to 30 remote talkers registered by XUID as
// session members come and go. Each frame it checks GetDataReadyFlags, pulls GetLocalChatData into a 100-byte queue
// and sends that to every unmuted member once it holds more than 70 bytes or 0.2 s have passed; receivers hand the
// bytes to SubmitIncomingChatData. That channel was sized for the Xbox 360 codec (10-byte packets, about 4 kbit/s):
// the game drops bytes when its queue overflows, and Steam's Opus voice needs several times that rate. So the audio
// travels on LP2Net's own transport (the core socket, or the Steam relay, see net.cpp) and the game's queue stays
// empty. The game still decides who is a talker (Register/UnregisterRemoteTalker), who is muted
// (SetPlaybackPriority, XUserMuteListQuery) and shows who talks (IsLocalTalking, IsRemoteTalking, IsHeadsetPresent).
//
// Inside, voice is 16 kHz mono 16-bit. The microphone is Steam's (StartVoiceRecording/GetVoice, compressed by
// Steam) when Steam runs, else the Windows default recording device (waveIn, sent as IMA ADPCM), or a WAV file for
// testing. Playback mixes every talker into the Windows default output (waveOut). A peer announces its XUIDs every
// 2 s; we send our voice only to peers whose XUID the game registered as a talker and has not muted.
#include "xhv.h"

#include "config.h"
#include "log.h"
#include "net.h"
#include "steam.h"
#include "util.h"
#include "xnotify.h"

#include <mmsystem.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <deque>
#include <map>
#include <mutex>
#include <set>

namespace lp2net {
void EnsureInit();

namespace {
// ---- GFWL definitions (xhv2.h) -----------------------------------------------------------------------------------
using XHV_PROCESSING_MODE = DWORD;
constexpr XHV_PROCESSING_MODE XHV_LOOPBACK_MODE = 1, XHV_VOICECHAT_MODE = 2;
constexpr DWORD XHV_PLAYBACK_PRIORITY_NEVER = 0xFFFFFFFF;
constexpr DWORD XN_SYS_MUTELISTCHANGED = XNID(0, 0, 0x0011);

struct XHV_INIT_PARAMS {
	DWORD dwMaxRemoteTalkers;
	DWORD dwMaxLocalTalkers;
	const XHV_PROCESSING_MODE* localTalkerEnabledModes;
	DWORD dwNumLocalTalkerEnabledModes;
	const XHV_PROCESSING_MODE* remoteTalkerEnabledModes;
	DWORD dwNumRemoteTalkerEnabledModes;
	BOOL bCustomVADProvided;
	BOOL bRelaxPrivileges;
	void* pfnMicrophoneRawDataReady;
	HWND hwndFocus;
};
static_assert(sizeof(XHV_INIT_PARAMS) == 40);

// ---- audio format and timing -------------------------------------------------------------------------------------
constexpr int kRate = 16000;
constexpr int kBlock = kRate / 50;       // 20 ms
constexpr int kPacketSamples = kBlock * 2;  // 40 ms of audio per packet
constexpr size_t kPrebuffer = kBlock * 5;   // a talker starts playing once 100 ms are buffered
constexpr size_t kMaxBuffered = kRate / 2;  // beyond 0.5 s the oldest audio goes
constexpr int kOutBuffers = 5;              // 100 ms queued at the sound card
constexpr ULONGLONG kTalkingHold = 250;     // a talker still counts as talking this long after audible audio
constexpr ULONGLONG kVadHangover = 400;     // keep sending this long after the microphone level drops
constexpr double kVadLevel = 200.0;         // RMS (of 32768) that counts as speech
constexpr ULONGLONG kAnnounceEvery = 2000;
constexpr ULONGLONG kStatsEvery = 10000;

// Milliseconds since t. Several threads stamp times, so t may be a little later than the caller's own `now`.
ULONGLONG Age(ULONGLONG t, ULONGLONG now) {
	return now > t ? now - t : 0;
}

// ---- wire format: PacketType::Voice body --------------------------------------------------------------------------
#pragma pack(push, 1)
struct VoiceMsg {
	uint8_t kind;
	uint8_t codec;
	uint8_t flags;
	uint8_t reserved;
	uint16_t seq;
	uint16_t cbData;       // bytes after this header
	uint64_t xuid;         // the sender's XUID as the game sees it (online XUID when signed in online)
	uint64_t xuidOffline;  // and its offline one, whichever the other side's game registered
};
#pragma pack(pop)
enum : uint8_t { kKindAnnounce = 0, kKindAudio = 1 };  // announce: 16-byte name follows
enum : uint8_t { kCodecSteam = 1, kCodecAdpcm = 2 };
constexpr uint8_t kAnnounceHeadset = 1;
constexpr uint8_t kAudioSpurtStart = 1;  // first packet after a pause: the receiver ran dry on purpose

// ---- IMA ADPCM, 4 bits a sample. Block: int16 predictor, uint8 step index, uint8 0, then the samples, low nibble
// first. Each packet carries its own start state, so a lost packet costs only its own 40 ms. ----------------------
const int kImaStep[89] = {
    7,     8,     9,     10,    11,    12,    13,    14,    16,    17,    19,    21,    23,    25,    28,
    31,    34,    37,    41,    45,    50,    55,    60,    66,    73,    80,    88,    97,    107,   118,
    130,   143,   157,   173,   190,   209,   230,   253,   279,   307,   337,   371,   408,   449,   494,
    544,   598,   658,   724,   796,   876,   963,   1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,
    2272,  2499,  2749,  3024,  3327,  3660,  4026,  4428,  4871,  5358,  5894,  6484,  7132,  7845,  8630,
    9493,  10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};
const int kImaIndex[16] = {-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8};

struct Adpcm {
	int pred = 0;
	int index = 0;

	int Step(int nib) {
		int step = kImaStep[index];
		int delta = step >> 3;
		if (nib & 4) {
			delta += step;
		}
		if (nib & 2) {
			delta += step >> 1;
		}
		if (nib & 1) {
			delta += step >> 2;
		}
		pred = std::clamp(pred + ((nib & 8) ? -delta : delta), -32768, 32767);
		index = std::clamp(index + kImaIndex[nib], 0, 88);
		return pred;
	}

	int EncodeOne(int s) {
		int step = kImaStep[index];
		int diff = s - pred;
		int nib = 0;
		if (diff < 0) {
			nib = 8;
			diff = -diff;
		}
		if (diff >= step) {
			nib |= 4;
			diff -= step;
		}
		if (diff >= step >> 1) {
			nib |= 2;
			diff -= step >> 1;
		}
		if (diff >= step >> 2) {
			nib |= 1;
		}
		Step(nib);
		return nib;
	}
};

std::vector<BYTE> AdpcmEncode(Adpcm& st, const int16_t* pcm, int n) {
	std::vector<BYTE> out(4 + (n + 1) / 2);
	int16_t pred = (int16_t)st.pred;
	memcpy(out.data(), &pred, 2);
	out[2] = (BYTE)st.index;
	for (int i = 0; i < n; ++i) {
		int nib = st.EncodeOne(pcm[i]);
		out[4 + i / 2] |= (BYTE)((i & 1) ? nib << 4 : nib);
	}
	return out;
}

bool AdpcmDecode(const BYTE* in, size_t cb, std::vector<int16_t>* pcm) {
	if (cb < 5 || in[2] > 88) {
		return false;
	}
	Adpcm st;
	int16_t pred;
	memcpy(&pred, in, 2);
	st.pred = pred;
	st.index = in[2];
	for (size_t i = 4; i < cb; ++i) {
		pcm->push_back((int16_t)st.Step(in[i] & 0x0F));
		pcm->push_back((int16_t)st.Step(in[i] >> 4));
	}
	return true;
}

double Rms(const int16_t* s, size_t n) {
	if (!n) {
		return 0;
	}
	double sum = 0;
	for (size_t i = 0; i < n; ++i) {
		sum += (double)s[i] * s[i];
	}
	return std::sqrt(sum / (double)n);
}

// ---- WAV files (debug feed and dump) ------------------------------------------------------------------------------
std::wstring ResolvePath(const std::string& p) {
	std::wstring w = Widen(p);
	bool absolute = w.size() > 2 && (w[1] == L':' || (w[0] == L'\\' && w[1] == L'\\'));
	return absolute ? w : GameDir() + w;
}

// Any PCM WAV (8 or 16 bit, any rate, any channel count) as 16 kHz mono.
bool LoadWav(const std::wstring& path, std::vector<int16_t>* out) {
	FILE* f = _wfopen(path.c_str(), L"rb");
	if (!f) {
		return false;
	}
	std::vector<BYTE> file;
	BYTE buf[65536];
	for (size_t n; (n = fread(buf, 1, sizeof(buf), f)) > 0;) {
		file.insert(file.end(), buf, buf + n);
	}
	fclose(f);
	if (file.size() < 12 || memcmp(file.data(), "RIFF", 4) != 0 || memcmp(file.data() + 8, "WAVE", 4) != 0) {
		return false;
	}
	uint16_t format = 0, channels = 0, bits = 0;
	uint32_t rate = 0;
	const BYTE* data = nullptr;
	uint32_t cbData = 0;
	for (size_t at = 12; at + 8 <= file.size();) {
		uint32_t cb;
		memcpy(&cb, &file[at + 4], 4);
		const BYTE* body = &file[at + 8];
		size_t avail = file.size() - at - 8;
		if (!memcmp(&file[at], "fmt ", 4) && cb >= 16 && avail >= 16) {
			memcpy(&format, body, 2);
			memcpy(&channels, body + 2, 2);
			memcpy(&rate, body + 4, 4);
			memcpy(&bits, body + 14, 2);
			if (format == 0xFFFE && cb >= 26 && avail >= 26) {
				memcpy(&format, body + 24, 2);  // WAVE_FORMAT_EXTENSIBLE: first two bytes of the subformat GUID
			}
		}
		else if (!memcmp(&file[at], "data", 4)) {
			data = body;
			cbData = (uint32_t)std::min<size_t>(cb, avail);
		}
		at += 8 + cb + (cb & 1);
	}
	if (format != 1 || !channels || !rate || (bits != 8 && bits != 16) || !data) {
		LOG("voice: %ls is not a PCM WAV file (format %u, %u bit)", path.c_str(), format, bits);
		return false;
	}
	size_t frame = (size_t)channels * bits / 8;
	size_t frames = cbData / frame;
	std::vector<float> mono(frames);
	for (size_t i = 0; i < frames; ++i) {
		float sum = 0;
		for (int c = 0; c < channels; ++c) {
			const BYTE* p = data + i * frame + c * bits / 8;
			int16_t s16;
			memcpy(&s16, p, 2);
			sum += bits == 8 ? (float)((int)*p - 128) * 256.0f : (float)s16;
		}
		mono[i] = sum / channels;
	}
	out->clear();
	double step = (double)rate / kRate;
	for (double pos = 0; pos + 1 < (double)frames; pos += step) {
		size_t i = (size_t)pos;
		double t = pos - (double)i;
		out->push_back((int16_t)std::clamp(mono[i] * (1 - t) + mono[i + 1] * t, -32768.0, 32767.0));
	}
	return !out->empty();
}

class WavWriter {
public:
	bool Open(const std::wstring& path) {
		f_ = _wfopen(path.c_str(), L"wb");
		if (!f_) {
			return false;
		}
		BYTE header[44] = {};
		fwrite(header, 1, sizeof(header), f_);
		Fix();
		return true;
	}
	bool IsOpen() const { return f_ != nullptr; }
	void Write(const int16_t* s, size_t n) {
		fwrite(s, 2, n, f_);
		samples_ += (uint32_t)n;
		if (++writes_ % 50 == 0) {
			Fix();  // keep the header current, so the file stays readable if the game is killed
		}
	}
	void Close() {
		if (f_) {
			Fix();
			fclose(f_);
			f_ = nullptr;
		}
	}
	uint32_t Samples() const { return samples_; }

private:
	void Fix() {
		uint32_t cbData = samples_ * 2, riff = 36 + cbData, fmtSize = 16, byteRate = kRate * 2, rate = kRate;
		uint16_t pcm = 1, channels = 1, align = 2, bits = 16;
		BYTE h[44];
		memcpy(h, "RIFF", 4);
		memcpy(h + 4, &riff, 4);
		memcpy(h + 8, "WAVEfmt ", 8);
		memcpy(h + 16, &fmtSize, 4);
		memcpy(h + 20, &pcm, 2);
		memcpy(h + 22, &channels, 2);
		memcpy(h + 24, &rate, 4);
		memcpy(h + 28, &byteRate, 4);
		memcpy(h + 32, &align, 2);
		memcpy(h + 34, &bits, 2);
		memcpy(h + 36, "data", 4);
		memcpy(h + 40, &cbData, 4);
		long at = ftell(f_);
		fseek(f_, 0, SEEK_SET);
		fwrite(h, 1, sizeof(h), f_);
		fseek(f_, at, SEEK_SET);
		fflush(f_);
	}
	FILE* f_ = nullptr;
	uint32_t samples_ = 0;
	uint32_t writes_ = 0;
};

WAVEFORMATEX VoiceFormat() {
	WAVEFORMATEX fmt{};
	fmt.wFormatTag = WAVE_FORMAT_PCM;
	fmt.nChannels = 1;
	fmt.nSamplesPerSec = kRate;
	fmt.wBitsPerSample = 16;
	fmt.nBlockAlign = 2;
	fmt.nAvgBytesPerSec = kRate * 2;
	return fmt;
}

// ---- Windows microphone (default recording device) ---------------------------------------------------------------
class WaveIn {
public:
	bool Open(HANDLE event) {
		WAVEFORMATEX fmt = VoiceFormat();
		MMRESULT r = waveInOpen(&h_, WAVE_MAPPER, &fmt, (DWORD_PTR)event, 0, CALLBACK_EVENT);
		if (r != MMSYSERR_NOERROR) {
			LOG("voice: no Windows microphone (waveInOpen %u)", r);
			h_ = nullptr;
			return false;
		}
		for (int i = 0; i < kBuffers; ++i) {
			hdr_[i] = WAVEHDR{};
			hdr_[i].lpData = (LPSTR)data_[i];
			hdr_[i].dwBufferLength = sizeof(data_[i]);
			waveInPrepareHeader(h_, &hdr_[i], sizeof(WAVEHDR));
			waveInAddBuffer(h_, &hdr_[i], sizeof(WAVEHDR));
		}
		next_ = 0;
		waveInStart(h_);
		return true;
	}
	void Read(std::vector<int16_t>* out) {
		while (h_ && (hdr_[next_].dwFlags & WHDR_DONE)) {
			WAVEHDR& w = hdr_[next_];
			out->insert(out->end(), data_[next_], data_[next_] + w.dwBytesRecorded / 2);
			w.dwFlags &= ~WHDR_DONE;
			waveInAddBuffer(h_, &w, sizeof(WAVEHDR));
			next_ = (next_ + 1) % kBuffers;
		}
	}
	void Close() {
		if (!h_) {
			return;
		}
		waveInReset(h_);
		for (auto& w : hdr_) {
			waveInUnprepareHeader(h_, &w, sizeof(WAVEHDR));
		}
		waveInClose(h_);
		h_ = nullptr;
	}

private:
	static constexpr int kBuffers = 8;
	HWAVEIN h_ = nullptr;
	WAVEHDR hdr_[kBuffers]{};
	int16_t data_[kBuffers][kBlock]{};
	int next_ = 0;
};

// ---- speakers (default output device) -----------------------------------------------------------------------------
class WaveOut {
public:
	bool Open(HANDLE event) {
		WAVEFORMATEX fmt = VoiceFormat();
		MMRESULT r = waveOutOpen(&h_, WAVE_MAPPER, &fmt, (DWORD_PTR)event, 0, CALLBACK_EVENT);
		if (r != MMSYSERR_NOERROR) {
			LOG("voice: no sound output (waveOutOpen %u); voice is still mixed for the dump", r);
			h_ = nullptr;
			return false;
		}
		for (int i = 0; i < kOutBuffers; ++i) {
			hdr_[i] = WAVEHDR{};
			hdr_[i].lpData = (LPSTR)data_[i];
			hdr_[i].dwBufferLength = sizeof(data_[i]);
			waveOutPrepareHeader(h_, &hdr_[i], sizeof(WAVEHDR));
			hdr_[i].dwFlags |= WHDR_DONE;  // free until first written
		}
		next_ = 0;
		return true;
	}
	bool IsOpen() const { return h_ != nullptr; }
	// The next buffer the device has finished with, or nullptr.
	int16_t* Free() { return (hdr_[next_].dwFlags & WHDR_DONE) ? data_[next_] : nullptr; }
	void Submit() {
		hdr_[next_].dwFlags &= ~WHDR_DONE;
		waveOutWrite(h_, &hdr_[next_], sizeof(WAVEHDR));
		next_ = (next_ + 1) % kOutBuffers;
	}
	void Close() {
		if (!h_) {
			return;
		}
		waveOutReset(h_);
		for (auto& w : hdr_) {
			waveOutUnprepareHeader(h_, &w, sizeof(WAVEHDR));
		}
		waveOutClose(h_);
		h_ = nullptr;
	}

private:
	HWAVEOUT h_ = nullptr;
	WAVEHDR hdr_[kOutBuffers]{};
	int16_t data_[kOutBuffers][kBlock]{};
	int next_ = 0;
};

// ---- engine state -------------------------------------------------------------------------------------------------
// Audio waiting to be played for one talker (a remote player, or ourselves for loopback and the microphone test).
struct Playout {
	std::deque<int16_t> pcm;
	bool playing = false;
	bool starved = false;  // ran dry while playing; more of the same talk spurt after that = an underrun
	ULONGLONG lastAudible = 0;
	ULONGLONG lastPacket = 0;
	uint16_t nextSeq = 0;
	bool haveSeq = false;

	void Push(const std::vector<int16_t>& s, bool spurtStart, std::atomic<uint32_t>* underruns) {
		if (starved && !spurtStart) {
			++*underruns;
		}
		starved = false;
		pcm.insert(pcm.end(), s.begin(), s.end());
		while (pcm.size() > kMaxBuffered) {
			pcm.pop_front();
		}
	}
};

struct RemoteTalker {
	DWORD priority = 0;  // XHV_PLAYBACK_PRIORITY: 0 = highest, NEVER = muted by the game
	bool voiceChat = false;  // StartRemoteProcessingModes(voice chat)
	Playout out;
};

// What a peer told us about itself (announcements and audio packets).
struct PeerVoice {
	XUID xuid = 0, xuidOffline = 0;
	std::string name;
	bool headset = false;
	ULONGLONG lastSeen = 0;
};

enum class Source { None, Steam, WindowsMic, WavFeed };
const char* SourceName(Source s) {
	switch (s) {
	case Source::Steam: return "Steam microphone";
	case Source::WindowsMic: return "Windows microphone";
	case Source::WavFeed: return "WAV file (debug)";
	default: return "off";
	}
}

struct Counters {
	std::atomic<uint32_t> captured{0};     // 20 ms blocks from the microphone/feed, or Steam voice reads with data
	std::atomic<uint32_t> sentPackets{0}, sentBytes{0};
	std::atomic<uint32_t> recvPackets{0}, recvBytes{0};
	std::atomic<uint32_t> decoded{0}, decodeErrors{0};
	std::atomic<uint32_t> lost{0};         // gaps in a talker's packet sequence
	std::atomic<uint32_t> underruns{0};
	std::atomic<uint32_t> notTalker{0};    // audio from someone the game did not register (not in our session)
	std::atomic<uint32_t> mutedDrops{0};
	std::atomic<uint32_t> played{0};       // 20 ms blocks with someone's voice in them
	std::atomic<uint32_t> gameChannel{0};  // bytes the game passed to SubmitIncomingChatData (nothing should)
};

std::mutex g_mutex;
LONG g_refs = 0;  // engine references; 0 = no engine
DWORD g_maxRemote = 0;
bool g_localRegistered = false;
DWORD g_localModes = 0;  // bit per processing mode: 1 << XHV_LOOPBACK_MODE, 1 << XHV_VOICECHAT_MODE
std::map<XUID, RemoteTalker> g_talkers;
std::map<uint32_t, PeerVoice> g_peerVoice;  // by peer instance id
std::set<XUID> g_muted;
bool g_mutedLoaded = false;
Playout g_self;  // our own voice, played back in loopback mode / microphone test
std::atomic<bool> g_micTest{false};
std::atomic<Source> g_source{Source::None};
std::atomic<ULONGLONG> g_localTalkingAt{0};
std::atomic<bool> g_haveMic{false};
std::atomic<bool> g_announceNow{false};
Counters g_n;
HANDLE g_thread = nullptr;
std::atomic<bool> g_stop{false};

void LoadMuted() {
	if (!g_mutedLoaded) {
		g_mutedLoaded = true;
		g_muted.insert(Cfg().muted.begin(), Cfg().muted.end());
	}
}

bool ModeOn(XHV_PROCESSING_MODE m) {
	return (g_localModes & (1u << m)) != 0;
}

bool TalkerMuted(XUID x, const RemoteTalker& t) {
	return t.priority == XHV_PLAYBACK_PRIORITY_NEVER || g_muted.count(x) != 0;
}

const PeerVoice* PeerForXuid(XUID x, uint32_t* id = nullptr) {
	for (const auto& [pid, pv] : g_peerVoice) {
		if (x && (pv.xuid == x || pv.xuidOffline == x)) {
			if (id) {
				*id = pid;
			}
			return &pv;
		}
	}
	return nullptr;
}

std::string TalkerName(XUID x) {
	const PeerVoice* pv = PeerForXuid(x);
	if (pv && !pv->name.empty()) {
		return pv->name;
	}
	char buf[32];
	snprintf(buf, sizeof(buf), "XUID %016llX", x);
	return buf;
}

std::string ModeList(const XHV_PROCESSING_MODE* modes, DWORD n) {
	std::string s;
	for (DWORD i = 0; modes && i < n && i < 8; ++i) {
		s += (s.empty() ? "" : ",") + std::to_string(modes[i]);
	}
	return s;
}

// ---- sending -------------------------------------------------------------------------------------------------------
VoiceMsg MakeMsg(uint8_t kind, uint8_t codec, uint16_t seq, size_t cbData) {
	VoiceMsg m{};
	m.kind = kind;
	m.codec = codec;
	m.seq = seq;
	m.cbData = (uint16_t)cbData;
	m.xuid = MySessionXuid();
	m.xuidOffline = Me().xuid;
	return m;
}

// Our voice goes to the peers the game registered as talkers and has not muted.
void SendAudio(uint8_t codec, const BYTE* data, size_t cb) {
	static uint16_t seq = 0;
	static ULONGLONG lastSent = 0;
	ULONGLONG now = GetTickCount64();
	bool spurtStart = Age(lastSent, now) > 150;  // packets are 40 ms apart while someone talks
	lastSent = now;
	std::vector<uint32_t> to;
	{
		std::lock_guard lock(g_mutex);
		for (const auto& [x, t] : g_talkers) {
			uint32_t id = 0;
			if (!TalkerMuted(x, t) && PeerForXuid(x, &id) && std::find(to.begin(), to.end(), id) == to.end()) {
				to.push_back(id);
			}
		}
	}
	std::vector<BYTE> pkt(sizeof(VoiceMsg) + cb);
	VoiceMsg m = MakeMsg(kKindAudio, codec, seq++, cb);
	m.flags = spurtStart ? kAudioSpurtStart : 0;
	memcpy(pkt.data(), &m, sizeof(m));
	memcpy(pkt.data() + sizeof(m), data, cb);
	for (uint32_t id : to) {
		SendVoice(id, pkt.data(), pkt.size());
		++g_n.sentPackets;
		g_n.sentBytes += (uint32_t)pkt.size();
	}
}

void Announce() {
	BYTE pkt[sizeof(VoiceMsg) + 16] = {};
	VoiceMsg m = MakeMsg(kKindAnnounce, 0, 0, 16);
	m.flags = g_haveMic && Cfg().voice ? kAnnounceHeadset : 0;
	memcpy(pkt, &m, sizeof(m));
	strncpy_s((char*)pkt + sizeof(m), 16, Me().name.c_str(), _TRUNCATE);
	for (uint32_t id : VoicePeers()) {
		SendVoice(id, pkt, sizeof(pkt));
	}
}

// ---- receiving (net worker thread) -------------------------------------------------------------------------------
void OnVoicePacket(uint32_t from, const BYTE* body, int cb) {
	VoiceMsg m;
	if (cb < (int)sizeof(m)) {
		return;
	}
	memcpy(&m, body, sizeof(m));
	if (m.cbData > cb - (int)sizeof(m)) {
		return;
	}
	const BYTE* data = body + sizeof(m);
	ULONGLONG now = GetTickCount64();
	XUID talker = 0;
	{
		std::lock_guard lock(g_mutex);
		if (!g_refs) {
			return;
		}
		PeerVoice& pv = g_peerVoice[from];
		bool isNew = !pv.lastSeen;
		pv.xuid = m.xuid;
		pv.xuidOffline = m.xuidOffline;
		pv.lastSeen = now;
		if (m.kind == kKindAnnounce) {
			pv.headset = (m.flags & kAnnounceHeadset) != 0;
			if (m.cbData >= 16) {
				pv.name.assign((const char*)data, strnlen((const char*)data, 16));
			}
		}
		if (isNew) {
			LOG("voice: peer %08X is %s (xuid %016llX / %016llX)%s", from, pv.name.c_str(), m.xuid, m.xuidOffline,
			    pv.headset ? ", has a microphone" : "");
		}
		if (m.kind != kKindAudio || !Cfg().voice) {
			return;
		}
		++g_n.recvPackets;
		g_n.recvBytes += (uint32_t)cb;
		for (XUID x : {m.xuid, m.xuidOffline}) {
			auto it = g_talkers.find(x);
			if (it != g_talkers.end() && it->second.voiceChat) {
				talker = x;
				if (TalkerMuted(x, it->second)) {
					++g_n.mutedDrops;
					return;
				}
				break;
			}
		}
		if (!talker) {
			++g_n.notTalker;
			return;
		}
	}
	std::vector<int16_t> pcm;
	bool ok = false;
	if (m.codec == kCodecAdpcm) {
		ok = AdpcmDecode(data, m.cbData, &pcm);
	}
	else if (m.codec == kCodecSteam) {
		static thread_local std::vector<int16_t> buf(kRate);  // 1 s
		int n = SteamVoiceDecompress(data, m.cbData, buf.data(), (uint32_t)(buf.size() * 2), kRate);
		ok = n >= 0;
		if (ok) {
			pcm.assign(buf.begin(), buf.begin() + n / 2);
		}
	}
	if (!ok) {
		++g_n.decodeErrors;
		return;
	}
	++g_n.decoded;
	std::lock_guard lock(g_mutex);
	auto it = g_talkers.find(talker);
	if (it == g_talkers.end()) {
		return;
	}
	Playout& o = it->second.out;
	if (o.haveSeq && m.seq != o.nextSeq) {
		uint16_t gap = (uint16_t)(m.seq - o.nextSeq);
		if (gap < 1000) {
			g_n.lost += gap;
		}
	}
	o.haveSeq = true;
	o.nextSeq = (uint16_t)(m.seq + 1);
	o.lastPacket = now;
	o.Push(pcm, (m.flags & kAudioSpurtStart) != 0, &g_n.underruns);
}

// ---- the voice thread: microphone, playback, announcements ---------------------------------------------------------
bool PushToTalkHeld() {
	UINT vk = Cfg().pushToTalk;
	if (!vk) {
		return true;
	}
	DWORD pid = 0;
	GetWindowThreadProcessId(GetForegroundWindow(), &pid);
	return pid == GetCurrentProcessId() && (GetAsyncKeyState((int)vk) & 0x8000) != 0;
}

bool FeedForThisCopy() {
	return !Cfg().voiceFeedWav.empty() && (Cfg().voiceFeedCopy == 0 || Cfg().voiceFeedCopy == Me().slot + 1);
}

// IsHeadsetPresent: the game tells the other players whether we can talk.
void UpdateHaveMic() {
	g_haveMic = Cfg().voice && (FeedForThisCopy() || (Cfg().voiceSteamMic && SteamReady()) || waveInGetNumDevs() > 0);
}

std::wstring DumpPath() {
	std::wstring p = ResolvePath(Cfg().voiceDumpWav);
	if (Me().slot > 0) {
		size_t dot = p.find_last_of(L'.');
		size_t slash = p.find_last_of(L"\\/");
		std::wstring suffix = L"-copy" + std::to_wstring(Me().slot + 1);
		p.insert(dot != std::wstring::npos && (slash == std::wstring::npos || dot > slash) ? dot : p.size(), suffix);
	}
	return p;
}

class VoiceWorker {
public:
	void Run() {
		HANDLE events[2] = {CreateEventW(nullptr, FALSE, FALSE, nullptr), CreateEventW(nullptr, FALSE, FALSE, nullptr)};
		inEvent_ = events[0];
		outEvent_ = events[1];
		LOG("voice: thread up; Steam voice sample rate %u", SteamVoiceSampleRate());
		while (!g_stop) {
			bool busy = cap_ != Source::None || out_.IsOpen() || playClock_;
			WaitForMultipleObjects(2, events, FALSE, busy ? 10 : 50);
			ULONGLONG now = GetTickCount64();
			Capture(now);
			Playback(now);
			if (now - lastAnnounce_ >= kAnnounceEvery || g_announceNow.exchange(false)) {
				lastAnnounce_ = now;
				UpdateHaveMic();
				if (Cfg().voice) {
					Announce();
				}
				std::lock_guard lock(g_mutex);
				std::erase_if(g_peerVoice, [&](const auto& kv) { return Age(kv.second.lastSeen, now) > 30000; });
			}
			if (now - lastStats_ >= kStatsEvery) {
				lastStats_ = now;
				LogStats();
			}
		}
		Open(Source::None, GetTickCount64());
		out_.Close();
		dump_.Close();
		CloseHandle(events[0]);
		CloseHandle(events[1]);
		LOG("voice: thread down");
	}

private:
	// Microphone open while we are a registered talker with someone to talk to (or in loopback / microphone test).
	Source Wanted() {
		{
			std::lock_guard lock(g_mutex);
			bool loop = ModeOn(XHV_LOOPBACK_MODE) || g_micTest;
			bool chat = ModeOn(XHV_VOICECHAT_MODE) && !g_talkers.empty();
			if (!Cfg().voice || !g_localRegistered || !(loop || chat)) {
				return Source::None;
			}
		}
		if (FeedForThisCopy()) {
			return Source::WavFeed;
		}
		if (Cfg().voiceSteamMic && SteamReady()) {
			return Source::Steam;
		}
		return Source::WindowsMic;
	}

	void Open(Source s, ULONGLONG now) {
		if (s == cap_ || (s == Source::WindowsMic && now < micRetryAt_)) {
			return;
		}
		if (cap_ == Source::Steam) {
			SteamVoiceRecord(false);
		}
		mic_.Close();
		pending_.clear();
		cap_ = Source::None;
		if (s == Source::WavFeed) {
			std::wstring path = ResolvePath(Cfg().voiceFeedWav);
			if (feed_.empty() && !LoadWav(path, &feed_)) {
				LOG("voice: cannot read %ls; using the microphone", path.c_str());
				Cfg().voiceFeedWav.clear();  // the next round picks the microphone
				s = Source::None;
			}
			else {
				LOG("voice: feeding %ls (%.1f s at 16 kHz) instead of the microphone", path.c_str(),
				    feed_.size() / (double)kRate);
				feedClock_ = now;
			}
		}
		else if (s == Source::Steam) {
			SteamVoiceRecord(true);
		}
		else if (s == Source::WindowsMic && !mic_.Open(inEvent_)) {
			micRetryAt_ = now + 10000;  // no recording device: look again in a while
			s = Source::None;
		}
		if (s != cap_ || s != g_source) {
			LOG("voice: microphone %s", SourceName(s));
		}
		cap_ = s;
		g_source = s;
	}

	bool Loopback() {
		std::lock_guard lock(g_mutex);
		return ModeOn(XHV_LOOPBACK_MODE) || g_micTest;
	}

	void PlaySelf(const std::vector<int16_t>& pcm) {
		std::lock_guard lock(g_mutex);
		g_self.Push(pcm, true, &g_n.underruns);
	}

	void Capture(ULONGLONG now) {
		Open(Wanted(), now);
		bool ptt = PushToTalkHeld();
		switch (cap_) {
		case Source::WavFeed: {
			size_t n = (size_t)std::min<ULONGLONG>(now - feedClock_, 200) * kRate / 1000;
			feedClock_ = now;
			for (size_t i = 0; i < n; ++i) {
				pending_.push_back(feed_[feedPos_]);
				feedPos_ = (feedPos_ + 1) % feed_.size();
			}
			break;
		}
		case Source::WindowsMic:
			mic_.Read(&pending_);
			break;
		case Source::Steam: {
			int result = 0;
			int n = SteamVoiceGet(steamBuf_, sizeof(steamBuf_), &result);
			if (result != lastSteamResult_) {
				LOG("voice: Steam voice result %d", result);  // 0 ok, 2 not recording, 3 no data, 6 restricted
				lastSteamResult_ = result;
			}
			if (n > 0) {
				++g_n.captured;
				if (ptt) {
					g_localTalkingAt = now;
					SendAudio(kCodecSteam, steamBuf_, (size_t)n);
					if (Loopback()) {
						std::vector<int16_t> pcm(kRate);
						int got = SteamVoiceDecompress(steamBuf_, (uint32_t)n, pcm.data(), kRate * 2, kRate);
						if (got > 0) {
							pcm.resize((size_t)got / 2);
							PlaySelf(pcm);
						}
					}
				}
			}
			break;
		}
		default:
			break;
		}
		// PCM sources: 40 ms packets, sent while the level says someone speaks
		while (pending_.size() >= kPacketSamples) {
			std::vector<int16_t> pkt(pending_.begin(), pending_.begin() + kPacketSamples);
			pending_.erase(pending_.begin(), pending_.begin() + kPacketSamples);
			g_n.captured += kPacketSamples / kBlock;
			if (Rms(pkt.data(), pkt.size()) > kVadLevel) {
				vadUntil_ = now + kVadHangover;
			}
			if (!ptt || now >= vadUntil_) {
				continue;
			}
			g_localTalkingAt = now;
			std::vector<BYTE> enc = AdpcmEncode(adpcm_, pkt.data(), (int)pkt.size());
			SendAudio(kCodecAdpcm, enc.data(), enc.size());
			if (Loopback()) {
				PlaySelf(pkt);
			}
		}
	}

	// Take up to one block from a talker; false when it has nothing to play.
	static bool Pull(Playout& o, int32_t* acc, ULONGLONG now) {
		if (!o.playing) {
			if (o.pcm.size() < kPrebuffer) {
				return false;
			}
			o.playing = true;
		}
		size_t n = std::min<size_t>(o.pcm.size(), kBlock);
		double sum = 0;
		for (size_t i = 0; i < n; ++i) {
			int16_t s = o.pcm.front();
			o.pcm.pop_front();
			acc[i] += s;
			sum += (double)s * s;
		}
		if (n && std::sqrt(sum / (double)n) > kVadLevel) {
			o.lastAudible = now;
		}
		if (o.pcm.empty()) {
			o.playing = false;
			o.starved = true;
		}
		return n > 0;
	}

	void Mix(int16_t* out, ULONGLONG now) {
		int32_t acc[kBlock] = {};
		bool any = false;
		{
			std::lock_guard lock(g_mutex);
			for (auto& [x, t] : g_talkers) {
				any |= Pull(t.out, acc, now);
			}
			any |= Pull(g_self, acc, now);
		}
		double gain = Cfg().voiceVolume / 100.0;
		for (int i = 0; i < kBlock; ++i) {
			out[i] = (int16_t)std::clamp((double)acc[i] * gain, -32768.0, 32767.0);
		}
		if (any) {
			++g_n.played;
		}
		if (!Cfg().voiceDumpWav.empty()) {
			if (!dump_.IsOpen() && any && !dumpFailed_) {
				std::wstring p = DumpPath();
				dumpFailed_ = !dump_.Open(p);
				LOG("voice: %s %ls", dumpFailed_ ? "cannot write" : "dumping playback to", p.c_str());
			}
			if (dump_.IsOpen()) {
				dump_.Write(out, kBlock);
			}
		}
	}

	void Playback(ULONGLONG now) {
		bool wanted;
		{
			std::lock_guard lock(g_mutex);
			wanted = Cfg().voice && (!g_talkers.empty() || ModeOn(XHV_LOOPBACK_MODE) || g_micTest);
		}
		if (wanted && !out_.IsOpen() && !playClock_) {
			if (!out_.Open(outEvent_)) {
				playClock_ = now;  // no sound card: keep time ourselves so the dump still runs
			}
			else {
				LOG("voice: playback open (16 kHz mono, %d ms queued)", kOutBuffers * 20);
			}
		}
		if (out_.IsOpen()) {
			while (int16_t* buf = out_.Free()) {
				Mix(buf, now);
				out_.Submit();
			}
		}
		else if (playClock_) {
			for (; playClock_ + 20 <= now; playClock_ += 20) {
				int16_t buf[kBlock];
				Mix(buf, now);
			}
		}
	}

	void LogStats() {
		uint32_t sig = g_n.captured + g_n.sentPackets + g_n.recvPackets + g_n.played + g_n.notTalker + g_n.gameChannel;
		if (sig == lastSig_) {
			return;
		}
		lastSig_ = sig;
		LOG("voice: mic %s, captured %u blocks; sent %u packets / %u B; received %u packets / %u B; decoded %u "
		    "(errors %u), lost %u, underruns %u; played %u blocks; dropped: not a talker %u, muted %u; game channel "
		    "%u B%s",
		    SourceName(cap_), (uint32_t)g_n.captured, (uint32_t)g_n.sentPackets, (uint32_t)g_n.sentBytes,
		    (uint32_t)g_n.recvPackets, (uint32_t)g_n.recvBytes, (uint32_t)g_n.decoded, (uint32_t)g_n.decodeErrors,
		    (uint32_t)g_n.lost, (uint32_t)g_n.underruns, (uint32_t)g_n.played, (uint32_t)g_n.notTalker,
		    (uint32_t)g_n.mutedDrops, (uint32_t)g_n.gameChannel, dump_.IsOpen() ? "; dump open" : "");
	}

	Source cap_ = Source::None;
	HANDLE inEvent_ = nullptr, outEvent_ = nullptr;
	WaveIn mic_;
	WaveOut out_;
	ULONGLONG playClock_ = 0;
	std::vector<int16_t> pending_;
	std::vector<int16_t> feed_;
	size_t feedPos_ = 0;
	ULONGLONG feedClock_ = 0;
	ULONGLONG vadUntil_ = 0;
	ULONGLONG micRetryAt_ = 0;
	Adpcm adpcm_;
	BYTE steamBuf_[8192];
	int lastSteamResult_ = -1;
	WavWriter dump_;
	bool dumpFailed_ = false;
	ULONGLONG lastAnnounce_ = 0, lastStats_ = 0;
	uint32_t lastSig_ = 0;
};

void StartThread() {
	if (g_thread) {
		return;
	}
	g_stop = false;
	SetVoiceHandler(OnVoicePacket);
	g_thread = CreateThread(nullptr, 0, [](void*) -> DWORD {
		auto* w = new VoiceWorker;  // ~8 KB of buffers: keep it off the thread's stack
		w->Run();
		delete w;
		return 0;
	}, nullptr, 0, nullptr);
}

void StopThread() {
	if (!g_thread) {
		return;
	}
	g_stop = true;
	WaitForSingleObject(g_thread, 3000);
	CloseHandle(g_thread);
	g_thread = nullptr;
}

// ---- IXHVEngine ----------------------------------------------------------------------------------------------------
// The layout is the COM interface the game calls through: slots in this exact order, __stdcall, no destructor slot.
class XhvEngine {
public:
	virtual ULONG __stdcall AddRef() {
		std::lock_guard lock(g_mutex);
		return (ULONG)++g_refs;
	}

	virtual ULONG __stdcall Release() {
		LONG left;
		{
			std::lock_guard lock(g_mutex);
			left = g_refs > 0 ? --g_refs : 0;
		}
		if (left == 0) {
			LOG("IXHVEngine::Release: engine gone");
			StopThread();
			std::lock_guard lock(g_mutex);
			g_talkers.clear();
			g_localRegistered = false;
			g_localModes = 0;
			g_self = Playout{};
		}
		return (ULONG)left;
	}

	virtual HRESULT __stdcall Lock(DWORD) { return S_OK; }  // our state has its own lock

	virtual HRESULT __stdcall StartLocalProcessingModes(DWORD user, const XHV_PROCESSING_MODE* modes, DWORD n) {
		return LocalModes(user, modes, n, true);
	}

	virtual HRESULT __stdcall StopLocalProcessingModes(DWORD user, const XHV_PROCESSING_MODE* modes, DWORD n) {
		return LocalModes(user, modes, n, false);
	}

	virtual HRESULT __stdcall StartRemoteProcessingModes(XUID xuid, const XHV_PROCESSING_MODE* modes, DWORD n) {
		return RemoteModes(xuid, modes, n, true);
	}

	virtual HRESULT __stdcall StopRemoteProcessingModes(XUID xuid, const XHV_PROCESSING_MODE* modes, DWORD n) {
		return RemoteModes(xuid, modes, n, false);
	}

	virtual HRESULT __stdcall SetMaxDecodePackets(DWORD) { return S_OK; }

	virtual HRESULT __stdcall RegisterLocalTalker(DWORD user) {
		if (user >= XUSER_MAX_COUNT) {
			return E_INVALIDARG;
		}
		std::lock_guard lock(g_mutex);
		LOG("IXHVEngine::RegisterLocalTalker(%lu)", user);
		g_localRegistered = user == 0 ? true : g_localRegistered;  // the game only has user 0
		return S_OK;
	}

	virtual HRESULT __stdcall UnregisterLocalTalker(DWORD user) {
		if (user >= XUSER_MAX_COUNT) {
			return E_INVALIDARG;
		}
		std::lock_guard lock(g_mutex);
		LOG("IXHVEngine::UnregisterLocalTalker(%lu)", user);
		if (user == 0) {
			g_localRegistered = false;
			g_localModes = 0;
		}
		return S_OK;
	}

	virtual HRESULT __stdcall RegisterRemoteTalker(XUID xuid, void*, void*, void*) {
		std::lock_guard lock(g_mutex);
		if (!g_talkers.count(xuid) && g_talkers.size() >= std::max<DWORD>(g_maxRemote, 1)) {
			return E_FAIL;
		}
		g_talkers[xuid];
		g_announceNow = true;  // so they learn who we are without waiting for the next round
		LOG("IXHVEngine::RegisterRemoteTalker(%016llX) %s, %zu talkers", xuid, TalkerName(xuid).c_str(),
		    g_talkers.size());
		return S_OK;
	}

	virtual HRESULT __stdcall UnregisterRemoteTalker(XUID xuid) {
		std::lock_guard lock(g_mutex);
		size_t erased = g_talkers.erase(xuid);
		LOG("IXHVEngine::UnregisterRemoteTalker(%016llX)%s, %zu talkers", xuid, erased ? "" : " (not registered)",
		    g_talkers.size());
		return S_OK;
	}

	virtual HRESULT __stdcall GetRemoteTalkers(DWORD* count, XUID* xuids) {
		if (!count) {
			return E_POINTER;
		}
		std::lock_guard lock(g_mutex);
		DWORD cap = *count, i = 0;
		for (const auto& [x, t] : g_talkers) {
			if (i < cap && xuids) {
				xuids[i] = x;
			}
			++i;
		}
		*count = std::min(i, cap);
		return S_OK;
	}

	virtual BOOL __stdcall IsHeadsetPresent(DWORD user) { return user == 0 && g_haveMic && Cfg().voice; }

	virtual BOOL __stdcall IsLocalTalking(DWORD user) {
		return user == 0 && g_source != Source::None && Age(g_localTalkingAt, GetTickCount64()) < kTalkingHold;
	}

	virtual BOOL __stdcall IsRemoteTalking(XUID xuid) {
		std::lock_guard lock(g_mutex);
		auto it = g_talkers.find(xuid);
		return it != g_talkers.end() && !TalkerMuted(xuid, it->second) &&
		       Age(it->second.out.lastAudible, GetTickCount64()) < kTalkingHold;
	}

	// Never anything for the game to carry: our voice travels on LP2Net's transport.
	virtual DWORD __stdcall GetDataReadyFlags() { return 0; }

	virtual HRESULT __stdcall GetLocalChatData(DWORD user, BYTE*, DWORD* size, DWORD* packets) {
		if (user >= XUSER_MAX_COUNT || !size) {
			return E_INVALIDARG;
		}
		*size = 0;
		if (packets) {
			*packets = 0;
		}
		return S_OK;
	}

	virtual HRESULT __stdcall SetPlaybackPriority(XUID xuid, DWORD, DWORD priority) {
		std::lock_guard lock(g_mutex);
		auto it = g_talkers.find(xuid);
		if (it == g_talkers.end()) {
			return E_INVALIDARG;
		}
		if (it->second.priority != priority) {
			LOG("IXHVEngine::SetPlaybackPriority(%016llX %s) %s", xuid, TalkerName(xuid).c_str(),
			    priority == XHV_PLAYBACK_PRIORITY_NEVER ? "never (muted)" : std::to_string(priority).c_str());
			it->second.priority = priority;
			if (priority == XHV_PLAYBACK_PRIORITY_NEVER) {
				it->second.out.pcm.clear();
			}
		}
		return S_OK;
	}

	// Bytes the game received on its own channel: only another GFWL engine would send any. Accepted and dropped.
	virtual HRESULT __stdcall SubmitIncomingChatData(XUID xuid, const BYTE*, DWORD* size) {
		if (size && *size) {
			if (g_n.gameChannel == 0) {
				LOG("IXHVEngine::SubmitIncomingChatData(%016llX, %lu bytes): ignored", xuid, *size);
			}
			g_n.gameChannel += *size;
		}
		return S_OK;
	}

private:
	static HRESULT LocalModes(DWORD user, const XHV_PROCESSING_MODE* modes, DWORD n, bool on) {
		if (user >= XUSER_MAX_COUNT || (!modes && n)) {
			return E_INVALIDARG;
		}
		std::lock_guard lock(g_mutex);
		for (DWORD i = 0; i < n; ++i) {
			if (modes[i] >= 32 || user != 0) {
				continue;
			}
			if (on) {
				g_localModes |= 1u << modes[i];
			}
			else {
				g_localModes &= ~(1u << modes[i]);
			}
		}
		LOG("IXHVEngine::%sLocalProcessingModes(%lu, [%s]) -> modes 0x%lX", on ? "Start" : "Stop", user,
		    ModeList(modes, n).c_str(), g_localModes);
		return S_OK;
	}

	static HRESULT RemoteModes(XUID xuid, const XHV_PROCESSING_MODE* modes, DWORD n, bool on) {
		std::lock_guard lock(g_mutex);
		auto it = g_talkers.find(xuid);
		if (it == g_talkers.end()) {
			return E_INVALIDARG;
		}
		for (DWORD i = 0; modes && i < n; ++i) {
			if (modes[i] == XHV_VOICECHAT_MODE) {
				it->second.voiceChat = on;
			}
		}
		return S_OK;
	}
};

XhvEngine g_engine;
}  // namespace

// ---- overlay -------------------------------------------------------------------------------------------------------
VoiceView VoiceStatus() {
	VoiceView v;
	ULONGLONG now = GetTickCount64();
	std::lock_guard lock(g_mutex);
	LoadMuted();
	v.engine = g_refs > 0;
	v.active = g_source != Source::None;
	v.source = SourceName(g_source);
	v.talking = v.active && Age(g_localTalkingAt, now) < kTalkingHold;
	v.micTest = g_micTest;
	for (const auto& [x, t] : g_talkers) {
		VoiceTalkerView tv;
		tv.xuid = x;
		tv.name = TalkerName(x);
		tv.muted = g_muted.count(x) != 0;
		tv.gameMuted = t.priority == XHV_PLAYBACK_PRIORITY_NEVER;
		tv.talking = !TalkerMuted(x, t) && Age(t.out.lastAudible, now) < kTalkingHold;
		tv.reachable = PeerForXuid(x) != nullptr;
		v.talkers.push_back(tv);
	}
	char buf[256];
	snprintf(buf, sizeof(buf), "sent %u packets (%u KB), received %u (%u KB), decoded %u, lost %u, underruns %u",
	         (uint32_t)g_n.sentPackets, (uint32_t)g_n.sentBytes / 1024, (uint32_t)g_n.recvPackets,
	         (uint32_t)g_n.recvBytes / 1024, (uint32_t)g_n.decoded, (uint32_t)g_n.lost, (uint32_t)g_n.underruns);
	v.counters = buf;
	return v;
}

void VoiceSetMuted(XUID xuid, bool muted) {
	{
		std::lock_guard lock(g_mutex);
		LoadMuted();
		if (muted) {
			g_muted.insert(xuid);
		}
		else {
			g_muted.erase(xuid);
		}
		Cfg().muted.assign(g_muted.begin(), g_muted.end());
		auto it = g_talkers.find(xuid);
		if (muted && it != g_talkers.end()) {
			it->second.out.pcm.clear();
		}
		LOG("voice: %s %s", muted ? "muted" : "unmuted", TalkerName(xuid).c_str());
	}
	SaveConfig();
	PostNotification(XN_SYS_MUTELISTCHANGED, 0);  // the game asks XUserMuteListQuery again and updates its talkers
}

void VoiceSetMicTest(bool on) {
	if (g_micTest.exchange(on) != on) {
		LOG("voice: microphone test %s", on ? "on" : "off");
		if (!on) {
			std::lock_guard lock(g_mutex);
			g_self = Playout{};
		}
	}
}

void VoiceShutdown() {
	StopThread();
}

}  // namespace lp2net

using namespace lp2net;

extern "C" HRESULT WINAPI XHVCreateEngine(const XHV_INIT_PARAMS* pParams, HANDLE* phWorkerThread, void** ppEngine) {
	EnsureInit();
	if (!pParams || !ppEngine) {
		return E_INVALIDARG;
	}
	LOG("XHVCreateEngine(maxRemote=%lu maxLocal=%lu localModes=[%s] remoteModes=[%s] customVAD=%d relax=%d "
	    "rawCallback=%p hwnd=%p)%s",
	    pParams->dwMaxRemoteTalkers, pParams->dwMaxLocalTalkers,
	    ModeList(pParams->localTalkerEnabledModes, pParams->dwNumLocalTalkerEnabledModes).c_str(),
	    ModeList(pParams->remoteTalkerEnabledModes, pParams->dwNumRemoteTalkerEnabledModes).c_str(),
	    pParams->bCustomVADProvided, pParams->bRelaxPrivileges, pParams->pfnMicrophoneRawDataReady, pParams->hwndFocus,
	    Cfg().voice ? "" : " - voice is off in lp2net.ini");
	if (phWorkerThread) {
		*phWorkerThread = nullptr;  // we run our own thread
	}
	{
		std::lock_guard lock(g_mutex);
		LoadMuted();
		g_maxRemote = pParams->dwMaxRemoteTalkers;
		++g_refs;
	}
	UpdateHaveMic();
	StartThread();
	*ppEngine = &g_engine;
	return S_OK;
}

extern "C" DWORD WINAPI XUserMuteListQuery(DWORD dwUserIndex, XUID XuidRemoteTalker, BOOL* pfOnMuteList) {
	if (!pfOnMuteList) {
		return ERROR_INVALID_PARAMETER;
	}
	std::lock_guard lock(g_mutex);
	LoadMuted();
	*pfOnMuteList = g_muted.count(XuidRemoteTalker) != 0;
	// the game asks for every member each frame: log answers that changed
	static std::map<XUID, BOOL> answered;
	auto it = answered.find(XuidRemoteTalker);
	if (it == answered.end() || it->second != *pfOnMuteList) {
		answered[XuidRemoteTalker] = *pfOnMuteList;
		LOG("XUserMuteListQuery(user=%lu, %016llX) -> %s", dwUserIndex, XuidRemoteTalker,
		    *pfOnMuteList ? "muted" : "not muted");
	}
	return ERROR_SUCCESS;
}
