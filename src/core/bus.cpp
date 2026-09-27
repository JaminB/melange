// Stub for component B (event bus). B replaces this file with the real hooks of docs/m0-design.md SS3 "B".
//
// NOTE (added by component C, 2026-09-27): the doc comment above originally shipped by E promised a file that
// "compiles and does nothing", but it was only an #include with no function bodies at all. That is enough as
// long as nothing calls into wumfix/bus.h, but component C's event-bus adapter (jlog_adapters.cpp) and its
// viewer panel (log_viewer.cpp) must be written against this frozen contract now, per the merge order in
// docs/m0-design.md SS4 ("C rebases on B+A"). Every function below is a neutral, side-effect-free stand-in
// (Subscribe* return failure, Installed()/RegistryReady() are false, decoders never fire) so the plugin links
// while B is still unimplemented in this worktree. None of this is component B's real logic - no hooks are
// installed, no message is ever observed - and every one of these bodies is expected to be discarded outright
// when B replaces this file wholesale. See the M0 report for component C for the exact list.
#include "wumfix/bus.h"

namespace wf::bus {

bool MessageView::Read(uint32_t /*offset*/, void* /*out*/, uint32_t /*n*/) const { return false; }

SubId Subscribe(MsgId /*id*/, Path /*path*/, Handler /*fn*/, void* /*user*/) { return 0; }
SubId SubscribeName(const char* /*name*/, Path /*path*/, Handler /*fn*/, void* /*user*/) { return 0; }
SubId SubscribeAll(Path /*path*/, Handler /*fn*/, void* /*user*/) { return 0; }
void Unsubscribe(SubId /*id*/) {}

bool RegistryReady() { return false; }
MsgId IdOf(std::string_view /*name*/) { return kInvalidId; }
const char* NameOf(MsgId /*id*/) { return "?"; }
size_t Capacity() { return 0; }

bool RegisterDecoder(uintptr_t /*vtable*/, const char* /*className*/, Decoder /*fn*/) { return false; }
bool Decode(const MessageView& /*m*/, JsonOut& /*out*/) { return false; }

Stats GetStats() { return Stats{}; }
uint32_t CountOf(MsgId /*id*/, Path /*path*/) { return 0; }
bool Installed() { return false; }

}  // namespace wf::bus
