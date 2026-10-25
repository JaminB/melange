# sandbox_selftest additions for wum.lobby / wum.peer (owner: C3). Fakes for peerbus:: go in tests/sandbox_fakes_peer.inc.
# The bus itself (peerbus.cpp) is faked; the value codec is the real one. The fakes need peerbus_api.h, which the frozen
# tests/sandbox_selftest.cpp does not include (it includes the fakes inside namespace melange), so it is force-included
# into that one file here.
target_sources(sandbox_selftest PRIVATE src/net/peerbus_wire.cpp)
set_property(SOURCE tests/sandbox_selftest.cpp APPEND PROPERTY COMPILE_OPTIONS "/FInet/peerbus_api.h")
