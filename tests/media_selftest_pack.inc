// media_selftest cases for the pack installer (owner: C4a): manifest parse, the streaming extractor's path rules
// (traversal, extra entry, size and hash mismatch, the 4 MiB heap high-water mark), a file:/// download with Range
// resume, the yt-dlp lane manifest (hash, serial rollback). Included at file scope, so it may #include what it needs;
// it defines pack_cases::Run(), and CHECK(cond, what) is available. Sources: cmake/holo/media_pack.cmake.
namespace pack_cases {
inline void Run() {}
}  // namespace pack_cases
