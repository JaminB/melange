#pragma once
// Every fixed engine address Mirage uses (build #1077), behind RequiresKnownBuild and Expect checks.
#include <cstdint>

namespace melange::mirage::engine {
bool Check();                // the Expect checks passed (run once); every accessor returns 0/false otherwise
uintptr_t Rm();              // *0x9796f0
uintptr_t Sort();            // rm+0x54
uintptr_t Cam();             // **(rm+0x60)
uintptr_t ShaderMgr();       // *0x984ce0
uintptr_t PostProcess();     // *0x961d7c
int Pass();                  // rm+0x100
int BucketCount();
uintptr_t CreateSceneFunc(void(__cdecl* fn)(void*, void*));  // 0x639b83(0x8905bc) + AddRef + fn at +0x14
bool SetBucketFunc(int id, bool post, uintptr_t sceneFunc);   // rm->vtbl[0x90/0x94]
uintptr_t BucketFunc(int id, bool post);                      // current slot content
struct CgProg { uintptr_t self, program; int type; const char* path; const char* entry; bool reload, failed; uint32_t binds; };
int ForEachCgProg(void (*fn)(const CgProg&, void*), void* user);  // mgr+0x368..+0x36c
bool MarkReload(uintptr_t cgprog);   // +0x44 = 1, +0x45 = 0
uintptr_t CgContext();               // *(mgr+0x14)
int CgProfile(int type);             // mgr+0x18 / +0x1c
bool FxaaOn();                       // *(0x95a100)+0x74
bool SetFxaa(bool on);               // writes it; refused unless SSAA is 1x1 (+0x6c/+0x70)
bool MsaaOn();                       // pp+0x7a: the scene renders into multisampled renderbuffers (/SSAA's hardware AA)
uintptr_t AppOptions();              // *0x95a100
bool Supersample(int* x, int* y);    // the /SSAA factors (+0x6c wide, +0x70 high)
bool SceneSize(int* w, int* h);      // pp+0x7c/+0x80: the scene targets' size
}
