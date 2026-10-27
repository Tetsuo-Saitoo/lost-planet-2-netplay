#include "firewall.h"

#include "setup.h"

#include <netfw.h>
#include <wrl/client.h>

namespace lp2setup {
namespace {
using Microsoft::WRL::ComPtr;

const wchar_t* const kGameExes[] = {L"LP2DX9.exe", L"LP2DX11.exe"};

struct Bstr {
	BSTR b = nullptr;
	Bstr() = default;
	explicit Bstr(const std::wstring& s) : b(SysAllocStringLen(s.data(), (UINT)s.size())) {}
	Bstr(const Bstr&) = delete;
	Bstr& operator=(const Bstr&) = delete;
	~Bstr() { SysFreeString(b); }
	std::wstring Str() const { return b ? std::wstring(b, SysStringLen(b)) : std::wstring(); }
};

// Firewall rules keep program paths as typed (often lower case, sometimes with %ProgramFiles% and the like).
std::wstring Canonical(const std::wstring& path) {
	wchar_t buf[MAX_PATH * 2];
	DWORD n = ExpandEnvironmentStringsW(path.c_str(), buf, ARRAYSIZE(buf));
	std::wstring s = n && n <= ARRAYSIZE(buf) ? buf : path;
	CharLowerBuffW(s.data(), (DWORD)s.size());
	return s;
}

ComPtr<INetFwPolicy2> Policy() {
	ComPtr<INetFwPolicy2> policy;
	CoCreateInstance(__uuidof(NetFwPolicy2), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&policy));
	return policy;
}

struct GameRule {
	ComPtr<INetFwRule> rule;
	int exe;  // index into kGameExes
	bool enabled;
	bool block;
	std::wstring name;
};

// Every inbound rule whose program is one of the game's exes.
bool GameRules(INetFwPolicy2* policy, const std::wstring& dir, std::vector<GameRule>& out) {
	ComPtr<INetFwRules> rules;
	ComPtr<IUnknown> unk;
	ComPtr<IEnumVARIANT> it;
	if (FAILED(policy->get_Rules(&rules)) || FAILED(rules->get__NewEnum(&unk)) || FAILED(unk.As(&it))) {
		return false;
	}
	std::wstring paths[2];
	for (int i = 0; i < 2; ++i) {
		paths[i] = Canonical(dir + L"\\" + kGameExes[i]);
	}
	VARIANT v;
	VariantInit(&v);
	ULONG got = 0;
	while (it->Next(1, &v, &got) == S_OK && got == 1) {
		ComPtr<INetFwRule> rule;
		Bstr app;
		NET_FW_RULE_DIRECTION direction;
		if (v.vt == VT_DISPATCH && v.pdispVal && SUCCEEDED(v.pdispVal->QueryInterface(IID_PPV_ARGS(&rule))) &&
		    SUCCEEDED(rule->get_ApplicationName(&app.b)) && app.b && SUCCEEDED(rule->get_Direction(&direction)) &&
		    direction == NET_FW_RULE_DIR_IN) {
			std::wstring a = Canonical(app.Str());
			for (int i = 0; i < 2; ++i) {
				if (a != paths[i]) {
					continue;
				}
				VARIANT_BOOL enabled = VARIANT_FALSE;
				NET_FW_ACTION action = NET_FW_ACTION_ALLOW;
				Bstr name;
				rule->get_Enabled(&enabled);
				rule->get_Action(&action);
				rule->get_Name(&name.b);
				out.push_back({rule, i, enabled != VARIANT_FALSE, action == NET_FW_ACTION_BLOCK, name.Str()});
			}
		}
		VariantClear(&v);
	}
	return true;
}
}  // namespace

Firewall FirewallStatus(const std::wstring& dir, std::vector<std::wstring>* blockingRules) {
	ComPtr<INetFwPolicy2> policy = Policy();
	std::vector<GameRule> rules;
	if (!policy || !GameRules(policy.Get(), dir, rules)) {
		return Firewall::Unknown;
	}
	bool allowed[2] = {};
	bool blocked = false;
	for (const GameRule& r : rules) {
		if (!r.enabled) {
			continue;
		}
		if (r.block) {
			blocked = true;
			if (blockingRules) {
				blockingRules->push_back(r.name);
			}
		} else {
			allowed[r.exe] = true;
		}
	}
	if (blocked) {
		return Firewall::Blocked;
	}
	for (int i = 0; i < 2; ++i) {
		if (!allowed[i] && FileExists(dir + L"\\" + kGameExes[i])) {
			return Firewall::NoRule;
		}
	}
	return Firewall::Allowed;
}

std::wstring FirewallText(Firewall state, const std::vector<std::wstring>& blockingRules) {
	switch (state) {
	case Firewall::Allowed:
		return L"Windows Firewall lets the game through. Nothing to do.";
	case Firewall::Blocked: {
		std::wstring names;
		for (const std::wstring& n : blockingRules) {
			names += (names.empty() ? L"" : L", ") + n;
		}
		return L"Windows Firewall is blocking the game (rule: " + names +
		       L"). Other players may not be able to join you. Click the button to allow it.";
	}
	case Firewall::NoRule:
		return L"No firewall rule yet. Windows asks the first time the game goes online: click Allow. Or click "
		       L"the button now.";
	default:
		return L"Setup could not read the Windows Firewall settings.";
	}
}

bool FirewallAllow(const std::wstring& dir, bool apply, std::vector<std::wstring>& report) {
	ComPtr<INetFwPolicy2> policy = Policy();
	ComPtr<INetFwRules> rules;
	std::vector<GameRule> game;
	if (!policy || FAILED(policy->get_Rules(&rules)) || !GameRules(policy.Get(), dir, game)) {
		report.push_back(L"Could not reach Windows Firewall.");
		return false;
	}
	bool ok = true;
	for (const GameRule& r : game) {
		if (r.enabled && r.block) {
			report.push_back(L"Turn off the rule that blocks the game: " + r.name);
			if (apply && FAILED(r.rule->put_Enabled(VARIANT_FALSE))) {
				ok = false;
			}
		}
	}
	for (const wchar_t* exe : kGameExes) {
		std::wstring path = dir + L"\\" + exe;
		if (!FileExists(path)) {
			continue;
		}
		Bstr name(std::wstring(L"Lost Planet 2 (LP2 Netplay) ") + exe);
		report.push_back(L"Add the rule \"" + name.Str() + L"\": incoming UDP for " + path +
		                 L", private and public networks.");
		if (!apply) {
			continue;
		}
		ComPtr<INetFwRule> old;
		for (int i = 0; i < 16 && SUCCEEDED(rules->Item(name.b, &old)); ++i) {
			old.Reset();
			rules->Remove(name.b);
		}
		ComPtr<INetFwRule> rule;
		if (FAILED(CoCreateInstance(__uuidof(NetFwRule), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&rule)))) {
			ok = false;
			continue;
		}
		Bstr description(L"Added by LP2 Netplay Setup when you clicked \"Allow through firewall\".");
		Bstr program(path);
		Bstr group(L"LP2 Netplay");
		rule->put_Name(name.b);
		rule->put_Description(description.b);
		rule->put_ApplicationName(program.b);
		rule->put_Grouping(group.b);
		rule->put_Protocol(NET_FW_IP_PROTOCOL_UDP);
		rule->put_Direction(NET_FW_RULE_DIR_IN);
		rule->put_Action(NET_FW_ACTION_ALLOW);
		rule->put_Profiles(NET_FW_PROFILE2_PRIVATE | NET_FW_PROFILE2_PUBLIC);
		rule->put_Enabled(VARIANT_TRUE);
		if (FAILED(rules->Add(rule.Get()))) {
			ok = false;
		}
	}
	return ok;
}

}  // namespace lp2setup
