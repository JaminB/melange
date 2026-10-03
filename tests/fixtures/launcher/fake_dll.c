// A stand-in for a third-party dinput8.dll: never loaded, only read from disk by the launcher self-test.
#pragma comment(linker, "/EXPORT:DirectInput8Create=_DirectInput8Create@20")
long __stdcall DirectInput8Create(void* a, unsigned long b, const void* c, void** d, void* e) {
    (void)a; (void)b; (void)c; (void)d; (void)e;
    return -1;
}
