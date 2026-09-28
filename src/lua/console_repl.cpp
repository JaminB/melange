#include "lua/console_repl.h"

#include <cctype>

namespace melange::console {
namespace {
bool IsWordChar(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.' || c == ':';
}
}  // namespace

std::string ExpandShorthand(const std::string& code) {
    size_t b = 0, e = code.size();
    while (b < e && std::isspace(static_cast<unsigned char>(code[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(code[e - 1]))) --e;
    std::string trimmed = code.substr(b, e - b);
    if (!trimmed.empty() && trimmed[0] == '=') return "return " + trimmed.substr(1);
    return trimmed;
}

std::string CompletionWord(const std::string& text, size_t cursor) {
    if (cursor > text.size()) cursor = text.size();
    size_t start = cursor;
    while (start > 0 && IsWordChar(text[start - 1])) --start;
    // Trim leading separators/digits so the word starts at a valid identifier character.
    while (start < cursor && (text[start] == '.' || text[start] == ':' || std::isdigit(static_cast<unsigned char>(text[start]))))
        ++start;
    return text.substr(start, cursor - start);
}

std::string SpliceCompletion(const std::string& word, const std::string& candidate) {
    size_t sep = word.find_last_of(".:");
    if (sep == std::string::npos) return candidate;
    return word.substr(0, sep + 1) + candidate;
}
}  // namespace melange::console
