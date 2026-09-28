#pragma once
#include <cstddef>
#include <memory>
#include <string>

#include "oasis/core/server.h"

namespace melange::oasis::core {
// Serves the entries of a zip held in memory (the bundle embedded in the binary). `data` must outlive the
// result. A "<path>.gz" entry is served as-is to clients that accept gzip. nullptr when `data` is not a zip.
std::unique_ptr<Files> ZipFiles(const void* data, size_t size);
// Serves a folder, re-read on every request (development, hot reload).
std::unique_ptr<Files> DirFiles(const std::wstring& dir);
// The text of one small entry of an in-memory zip, "" when absent.
std::string ZipEntryText(const void* data, size_t size, const char* name);
}  // namespace melange::oasis::core
