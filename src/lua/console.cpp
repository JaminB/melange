// LuaConsole: overlay REPL for the client VM (Sandbox) and the match VM (SimBridge).
#include <windows.h>
#include <shlobj.h>

#include <imgui.h>

#include <chrono>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

#include "core/config.h"
#include "core/events.h"
#include "core/game.h"
#include "core/log.h"
#include "core/module.h"
#include "lua/console_history.h"
#include "lua/console_repl.h"
#include "lua/engine50.h"
#include "lua/sandbox_internal.h"
#include "lua/sim/bridge_internal.h"
#include "melange/mods.h"
#include "melange/overlay.h"
#include "melange/sim.h"
#include "melange/testcmd.h"

namespace {
using melange::sandbox::EvalOut;

enum class Target { Client, Mod, Match };

struct LogEntry {
    std::string target, code, text;
    bool ok = false;
    double ms = 0;
};

constexpr const char* kPanelId = "console";
constexpr size_t kMaxLog = 500;
constexpr size_t kInputCap = 8192;

melange::console::History g_history(500);
std::vector<LogEntry> g_log;
char g_inputBuf[kInputCap] = {};
bool g_submit = false;
bool g_scrollToBottom = false;
Target g_target = Target::Client;
std::string g_targetMod;
bool g_matchConsoleOnline = false;
bool g_knownBuild = false;
// mods::Peers() is only ever nonzero once Handshake has joined a lobby, so [Handshake] Enabled=0 made every
// online match look offline here. NetSession's MatchStart/MatchEnd fire independently of Handshake.
bool g_online = false;

bool Online() { return g_online; }

std::string TargetLabel(Target t, const std::string& mod) {
    switch (t) {
    case Target::Client: return "client";
    case Target::Mod: return mod;
    case Target::Match: return "match";
    }
    return "?";
}

bool ResolveTarget(std::string_view tok, Target* kind, std::string* modId) {
    if (tok.empty()) return false;
    if (tok == "client") { *kind = Target::Client; return true; }
    if (tok == "match") { *kind = Target::Match; return true; }
    *kind = Target::Mod;
    *modId = std::string(tok);
    return true;
}

// Splits "<first token> <rest of the line>"; `rest` keeps internal whitespace (it may be Lua code).
bool SplitFirst(std::string_view args, std::string* first, std::string_view* rest) {
    size_t b = args.find_first_not_of(' ');
    if (b == std::string_view::npos) return false;
    size_t e = args.find(' ', b);
    if (e == std::string_view::npos) {
        *first = std::string(args.substr(b));
        *rest = {};
        return true;
    }
    *first = std::string(args.substr(b, e - b));
    size_t r = args.find_first_not_of(' ', e);
    *rest = r == std::string_view::npos ? std::string_view{} : args.substr(r);
    return true;
}

bool MatchAllowed(std::string* reason) {
    if (!melange::sim::InMatch()) {
        *reason = "not in a match";
        return false;
    }
    if (!g_knownBuild) {
        *reason = "engine Lua access is unavailable on this build";
        return false;
    }
    if (!g_matchConsoleOnline && Online()) {
        *reason = "match console is off in online matches ([LuaConsole] MatchConsoleOnline=1 to allow)";
        return false;
    }
    return true;
}

EvalOut RunTarget(Target kind, const std::string& modId, const std::string& code) {
    return kind == Target::Match ? melange::simbridge::EvalMatch(code)
                                  : melange::sandbox::Eval(kind == Target::Mod ? modId.c_str() : nullptr, code);
}

LogEntry RunEval(Target kind, const std::string& modId, const std::string& rawCode) {
    LogEntry e;
    e.target = TargetLabel(kind, modId);
    e.code = rawCode;
    if (kind == Target::Match) {
        std::string reason;
        if (!MatchAllowed(&reason)) {
            e.ok = false;
            e.text = reason;
            return e;
        }
    }
    std::string code = melange::console::ExpandShorthand(rawCode);
    auto t0 = std::chrono::steady_clock::now();
    // sandbox::Eval / simbridge::EvalMatch already try "return <code>" before falling back to raw code (so a
    // bare expression works); retrying that here on a runtime failure would just run code that already ran once
    // a second time.
    EvalOut out = RunTarget(kind, modId, code);
    e.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    e.ok = out.ok;
    e.text = out.text;
    return e;
}

void LogEvalToFile(const LogEntry& e) {
    LOG_INFO("[console] %s> %s", e.target.c_str(), e.code.c_str());
    size_t start = 0;
    while (start <= e.text.size()) {
        size_t nl = e.text.find('\n', start);
        std::string line = e.text.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
        if (e.ok) LOG_INFO("[console] %s", line.c_str());
        else LOG_WARN("[console] %s", line.c_str());
        if (nl == std::string::npos) break;
        start = nl + 1;
    }
}

void PushLog(LogEntry e) {
    LogEvalToFile(e);
    g_log.push_back(std::move(e));
    if (g_log.size() > kMaxLog) g_log.erase(g_log.begin());
    g_scrollToBottom = true;
}

void Complete(Target kind, const std::string& modId, const std::string& prefix, std::vector<std::string>* out) {
    if (kind == Target::Match) melange::simbridge::CompleteMatch(prefix, out);
    else melange::sandbox::Complete(kind == Target::Mod ? modId.c_str() : nullptr, prefix, out);
}

std::wstring HistoryPath() {
    PWSTR p = nullptr;
    std::wstring out;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &p)) && p) out = p;
    if (p) CoTaskMemFree(p);
    if (out.empty()) out = L".";
    return out + L"\\Melange\\console_history.txt";
}

void LoadHistory() {
    std::wstring path = HistoryPath();
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    LARGE_INTEGER sz{};
    if (!GetFileSizeEx(f, &sz) || sz.QuadPart < 0 || sz.QuadPart > (16 << 20)) {
        CloseHandle(f);
        return;
    }
    std::string data(static_cast<size_t>(sz.QuadPart), '\0');
    DWORD got = 0;
    BOOL ok = data.empty() || ReadFile(f, data.data(), static_cast<DWORD>(data.size()), &got, nullptr);
    CloseHandle(f);
    if (!ok) return;
    data.resize(got);
    g_history.Load(data);
}

void SaveHistory() {
    std::wstring path = HistoryPath();
    size_t slash = path.find_last_of(L'\\');
    if (slash != std::wstring::npos) CreateDirectoryW(path.substr(0, slash).c_str(), nullptr);
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    std::string data = g_history.Serialize();
    DWORD wrote = 0;
    WriteFile(f, data.data(), static_cast<DWORD>(data.size()), &wrote, nullptr);
    CloseHandle(f);
}

void Submit(const std::string& code) {
    if (code.empty()) return;
    PushLog(RunEval(g_target, g_targetMod, code));
    g_history.Add(code);
    SaveHistory();
    g_inputBuf[0] = '\0';
}

int InputCallback(ImGuiInputTextCallbackData* data) {
    if (data->EventFlag == ImGuiInputTextFlags_CallbackCharFilter) {
        if (data->EventChar == '\n' && !ImGui::GetIO().KeyShift) {
            g_submit = true;
            return 1;  // swallow: no newline inserted, Submit() runs after the widget returns
        }
        return 0;
    }
    if (data->EventFlag == ImGuiInputTextFlags_CallbackHistory) {
        const std::string* h = data->EventKey == ImGuiKey_UpArrow ? g_history.Older(std::string(data->Buf, static_cast<size_t>(data->BufTextLen)))
                                                                   : g_history.Newer();
        if (h) {
            data->DeleteChars(0, data->BufTextLen);
            data->InsertChars(0, h->c_str());
        }
        return 0;
    }
    if (data->EventFlag == ImGuiInputTextFlags_CallbackCompletion) {
        std::string text(data->Buf, static_cast<size_t>(data->BufTextLen));
        size_t cursor = static_cast<size_t>(data->CursorPos);
        std::string word = melange::console::CompletionWord(text, cursor);
        if (word.empty()) return 0;
        std::vector<std::string> cands;
        Complete(g_target, g_targetMod, word, &cands);
        if (cands.size() == 1) {
            std::string spliced = melange::console::SpliceCompletion(word, cands[0]);
            int start = static_cast<int>(cursor - word.size());
            data->DeleteChars(start, static_cast<int>(word.size()));
            data->InsertChars(start, spliced.c_str());
        } else if (!cands.empty()) {
            std::string joined;
            for (auto& c : cands) {
                if (!joined.empty()) joined += ", ";
                joined += c;
            }
            LogEntry e;
            e.target = TargetLabel(g_target, g_targetMod);
            e.code = "(complete) " + word;
            e.ok = true;
            e.text = joined;
            PushLog(std::move(e));
        }
        return 0;
    }
    if (data->EventFlag == ImGuiInputTextFlags_CallbackEdit) g_history.ResetCursor();
    return 0;
}

void DrawTargetSelector() {
    std::vector<melange::mods::ModInfo> mods;
    int total = melange::mods::List(nullptr, 0);
    if (total > 0) {
        mods.resize(static_cast<size_t>(total));
        total = melange::mods::List(mods.data(), total);
        mods.resize(static_cast<size_t>(total > 0 ? total : 0));
    }
    std::string label = g_target == Target::Client ? "Client" : g_target == Target::Match ? "Match" : "Client: " + g_targetMod;
    if (ImGui::BeginCombo("Target", label.c_str())) {
        if (ImGui::Selectable("Client", g_target == Target::Client)) {
            g_target = Target::Client;
            g_targetMod.clear();
        }
        for (const auto& m : mods) {
            if (m.state != melange::mods::State::Enabled || !m.hasClient) continue;
            std::string item = std::string("Client: ") + m.id;
            bool sel = g_target == Target::Mod && g_targetMod == m.id;
            if (ImGui::Selectable(item.c_str(), sel)) {
                g_target = Target::Mod;
                g_targetMod = m.id;
            }
        }
        bool inMatch = melange::sim::InMatch();
        ImGui::BeginDisabled(!inMatch);
        if (ImGui::Selectable("Match", g_target == Target::Match) && inMatch) {
            g_target = Target::Match;
            g_targetMod.clear();
        }
        ImGui::EndDisabled();
        ImGui::EndCombo();
    }
    if (g_target == Target::Match) {
        std::string reason;
        if (!MatchAllowed(&reason)) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1.f, .7f, .3f, 1.f), "(%s)", reason.c_str());
        }
    }
}

void DrawPanel(void*) {
    DrawTargetSelector();
    ImGui::Separator();
    ImGui::BeginChild("##scrollback", ImVec2(0, -ImGui::GetFrameHeightWithSpacing() * 3), false);
    for (const LogEntry& e : g_log) {
        ImGui::TextDisabled("%s> %s", e.target.c_str(), e.code.c_str());
        if (e.ok) {
            ImGui::TextWrapped("%s", e.text.c_str());
        } else {
            ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 110, 110, 255));
            ImGui::TextWrapped("%s", e.text.c_str());
            ImGui::PopStyleColor();
        }
        ImGui::TextDisabled("%.2f ms", e.ms);
    }
    if (g_scrollToBottom) {
        ImGui::SetScrollHereY(1.f);
        g_scrollToBottom = false;
    }
    ImGui::EndChild();

    g_submit = false;
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextMultiline("##input", g_inputBuf, sizeof(g_inputBuf), ImVec2(0, ImGui::GetFrameHeightWithSpacing() * 2),
                               ImGuiInputTextFlags_CallbackCharFilter | ImGuiInputTextFlags_CallbackHistory |
                                   ImGuiInputTextFlags_CallbackCompletion | ImGuiInputTextFlags_CallbackEdit,
                               &InputCallback);
    ImGui::TextDisabled("Enter runs, Shift+Enter for a new line, Tab completes, Up/Down recalls history");
    ImGui::SameLine();
    if (ImGui::SmallButton("Run")) g_submit = true;
    if (g_submit) Submit(g_inputBuf);
}

bool VerbOpen(std::string_view, void*) {
    melange::overlay::SetVisible(true);
    melange::overlay::OpenPanel(kPanelId, true);
    return true;
}

void HotkeyOpen(void*) {
    melange::overlay::SetVisible(true);
    melange::overlay::OpenPanel(kPanelId, true);
}

bool VerbRun(std::string_view args, void*) {
    std::string tok;
    std::string_view rest;
    if (!SplitFirst(args, &tok, &rest) || rest.empty()) {
        LOG_WARN("[console] console.run: usage <target> <code>");
        return false;
    }
    Target kind;
    std::string modId;
    if (!ResolveTarget(tok, &kind, &modId)) return false;
    LogEntry e = RunEval(kind, modId, std::string(rest));
    PushLog(e);
    return e.ok;
}

bool VerbComplete(std::string_view args, void*) {
    std::string tok;
    std::string_view rest;
    if (!SplitFirst(args, &tok, &rest)) {
        LOG_WARN("[console] console.complete: usage <target> <prefix>");
        return false;
    }
    Target kind;
    std::string modId;
    if (!ResolveTarget(tok, &kind, &modId)) return false;
    std::vector<std::string> cands;
    Complete(kind, modId, std::string(rest), &cands);
    std::string joined;
    for (auto& c : cands) {
        if (!joined.empty()) joined += ",";
        joined += c;
    }
    LOG_INFO("[console] complete %s '%.*s' -> %s", tok.c_str(), static_cast<int>(rest.size()), rest.data(), joined.c_str());
    return true;
}

class LuaConsole final : public melange::Module {
public:
    const char* Name() const override { return "LuaConsole"; }
    const char* Description() const override { return "Lua console in the overlay"; }
    int Order() const override { return 55; }
    bool Install() override {
        g_knownBuild = melange::game::IsKnownBuild() && melange::lua50::Check();
        if (melange::game::IsKnownBuild() && !g_knownBuild) LOG_WARN("[console] engine Lua check failed: the Match target stays off");

        g_matchConsoleOnline = Bool("MatchConsoleOnline", false);
        int histSize = Int("HistorySize", 500);
        g_history = melange::console::History(static_cast<size_t>(histSize > 0 ? histSize : 1));
        LoadHistory();

        std::string hotkey = StringKey("Hotkey", "Ctrl+Shift+F10");
        uint8_t dik = 0, mods = 0;
        if (!hotkey.empty() && melange::overlay::ParseHotkey(hotkey.c_str(), &dik, &mods)) {
            melange::overlay::AddHotkey(dik, mods, &HotkeyOpen, nullptr);
        } else if (!hotkey.empty()) {
            LOG_WARN("[console] [LuaConsole] Hotkey=%s not understood", hotkey.c_str());
        }

        melange::events::Subscribe(melange::events::Event::MatchStart, [] { g_online = true; });
        melange::events::Subscribe(melange::events::Event::MatchEnd, [] { g_online = false; });
        melange::overlay::AddPanel(kPanelId, "Lua/Console", &DrawPanel, nullptr);
        melange::testcmd::Register("console.open", &VerbOpen);
        melange::testcmd::Register("console.run", &VerbRun);
        melange::testcmd::Register("console.complete", &VerbComplete);
        return true;
    }
    void Uninstall() override { SaveHistory(); }

private:
    std::string StringKey(const char* key, const char* def) const {
        melange::config::EnsureKey(Name(), key, def);
        return melange::config::GetString(Name(), key, def);
    }
};
}  // namespace

MELANGE_MODULE(LuaConsole);
