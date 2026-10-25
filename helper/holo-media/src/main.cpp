// holo-media.exe: the media helper melange.asi starts out of process (docs/media-helper.md).
// Placeholder from Melange 0.12.0 C0: it parses nothing and plays nothing. C6a replaces it with the real helper
// (pipe client, mpv software render into the frame section, the hardened mpv and yt-dlp options, Low integrity).
#include <windows.h>

#include <cstdio>
#include <cwchar>

#include "bt/bt.h"
#include "media/media_proto.h"
#include "media/pipe_codec.h"
#include "media/shm_logic.h"

int wmain(int argc, wchar_t** argv) {
    for (int i = 1; i < argc; ++i)
        if (wcscmp(argv[i], L"--version") == 0) {
            printf("holo-media %s (protocol %d, placeholder)\n", HOLO_MEDIA_VERSION, melange::media::proto::kProto);
            return 0;
        }
    fprintf(stderr, "holo-media: placeholder build, no player\n");
    return 2;
}
