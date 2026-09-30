#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "erg/scene.h"
#include "xom/xom.h"

// Object-level edits on a parsed XOM document: new objects from the schema, TYPE entries, and inserts/removals that
// keep every reference (and the root) pointing at the same objects. No game or OS calls.
namespace melange::erg::xomutil {
// An object of `cls` with every field the document's TYPE versions serialise, at its zero value.
bool NewObject(const xom::Document& doc, std::string_view cls, xom::Object* out, std::string* err);
// Adds a TYPE entry for `cls` (count 0) before `before` (or at the end); true when it is present afterwards.
bool EnsureType(xom::Document& doc, std::string_view cls, std::string_view before);
// Inserts `obj` as object #at (1-based); every reference >= at moves up by one.
bool InsertObject(xom::Document& doc, uint32_t at, xom::Object obj);
// Removes object #at; references to it become 0 (null) and later ones move down by one.
bool RemoveObject(xom::Document& doc, uint32_t at);
// The same for many objects with one pass over the references: InsertObjects puts `objs` at #at.. in order;
// RemoveObjects drops every object in `ats`, and RemovedMap(sorted ats, r) is where reference r lands (0 if removed).
bool InsertObjects(xom::Document& doc, uint32_t at, std::vector<xom::Object> objs);
bool RemoveObjects(xom::Document& doc, std::vector<uint32_t> ats);
uint32_t RemovedMap(const std::vector<uint32_t>& sortedAts, uint32_t r);

xom::Value RefValue(uint32_t ref);
std::string Str(const xom::Object& o, std::string_view field);
bool SetStr(xom::Object& o, std::string_view field, std::string_view value);
bool GetVec(const xom::Object& o, std::string_view field, Vec3* out);
bool SetVec(xom::Object& o, std::string_view field, const Vec3& v);
int64_t Int(const xom::Object& o, std::string_view field, int64_t def = 0);
}  // namespace melange::erg::xomutil
