// holo-media.exe: the media helper melange.asi starts out of process (docs/media-helper.md). It plays one mpv core
// per player with the software renderer into the player's frame section, resolves YouTube links with yt-dlp, and
// streams torrent files from the bt::Engine, all driven over the core's pipe. It runs at Low integrity in a job the
// core owns, and exits when the game does or on `quit`, never because pings stop.
#include <windows.h>

#include <cstdio>
#include <cwchar>

#include "app.h"
#include "args.h"
#include "media/media_proto.h"

namespace holo {
int RunSelftest(const Args& a);
}

int wmain(int argc, wchar_t** argv) {
    // No crash or missing-file dialogs: a broken helper must die quietly (the core restarts it).
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    // DLLs from system32 and the application folder only; never the current directory or PATH.
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    SetDllDirectoryW(L"");
    if (argc == 2 && wcscmp(argv[1], L"--version") == 0) {
        printf("holo-media %s (protocol %d)\n", HOLO_MEDIA_VERSION, melange::media::proto::kProto);
        return 0;
    }
    holo::Args args;
    std::string why;
    if (!holo::ParseArgs(argc, argv, &args, &why)) {
        fprintf(stderr, "holo-media: %s\n", why.c_str());
        return 2;
    }
    if (args.selftest) return holo::RunSelftest(args);
    holo::App app(args);
    const int code = app.Run();
    // Every player, the engine and the pipe are shut down by now; end without running library destructors, which could
    // wait on a thread a broken decoder left behind.
    TerminateProcess(GetCurrentProcess(), static_cast<UINT>(code));
    return code;
}
