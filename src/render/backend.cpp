#include "render/backend.h"

#include <windows.h>

#include <imgui.h>
#include <imgui_impl_opengl2.h>
#include <imgui_impl_win32.h>

namespace melange::render::backend {
namespace {
bool g_ready = false;
void* g_hwnd = nullptr;
std::string g_ini;  // io.IniFilename points into this; must outlive the context
}  // namespace

bool Init(void* hwnd, const char* iniUtf8, void (*onCreate)()) {
    if (g_ready) return true;
    if (!ImGui::GetCurrentContext()) {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        g_ini = iniUtf8 ? iniUtf8 : "";
        io.IniFilename = g_ini.empty() ? nullptr : g_ini.c_str();
        io.LogFilename = nullptr;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        ImGui::StyleColorsDark();
        if (onCreate) onCreate();
    }
    if (!ImGui_ImplWin32_Init(hwnd)) return false;
    if (!ImGui_ImplOpenGL2_Init()) {
        ImGui_ImplWin32_Shutdown();
        return false;
    }
    g_hwnd = hwnd;
    g_ready = true;
    return true;
}

void Shutdown(bool contextLost) {
    if (!g_ready) return;
    g_ready = false;
    if (contextLost) {
        // The textures belong to a context that is gone: forget the ids so that the backend's destroy pass deletes
        // nothing (glDeleteTextures of id 0 is ignored) and the next render uploads them again. Status Destroyed
        // with pixels still in memory turns into WantCreate (ImTextureData::SetStatus).
        for (ImTextureData* tex : ImGui::GetPlatformIO().Textures) {
            if (tex->Status == ImTextureStatus_Destroyed) continue;
            tex->SetTexID(ImTextureID_Invalid);
            tex->BackendUserData = nullptr;
            tex->SetStatus(ImTextureStatus_Destroyed);
        }
    }
    ImGui_ImplOpenGL2_Shutdown();
    ImGui_ImplWin32_Shutdown();
    g_hwnd = nullptr;
}

bool Ready() { return g_ready; }
void* Hwnd() { return g_hwnd; }
}  // namespace melange::render::backend
