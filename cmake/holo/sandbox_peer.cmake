# sandbox_selftest additions for wum.lobby / wum.peer (owner: C3). Fakes for peerbus:: go in tests/sandbox_fakes_peer.inc.
# The bus itself (peerbus.cpp) is faked; the value codec is the real one.
target_sources(sandbox_selftest PRIVATE src/net/peerbus_wire.cpp)
