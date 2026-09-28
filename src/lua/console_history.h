#pragma once
#include <cstddef>
#include <string>
#include <vector>

// Command history for the console: Up/Down recall capped at a size, plus a text serialisation for
// persisting to console_history.txt. Pure logic, no file I/O (the module does that).
namespace melange::console {
class History {
public:
    explicit History(size_t capacity = 500);

    void Add(const std::string& line);  // no-op for an empty line or a repeat of the last one
    size_t Size() const { return lines_.size(); }
    const std::string& At(size_t i) const { return lines_[i]; }  // 0 = oldest

    void ResetCursor();  // call when the user edits the input directly, to stop browsing
    // Up: returns an older line, saving `draft` the first time; null once already at the oldest.
    const std::string* Older(const std::string& draft);
    // Down: returns a newer line, the saved draft past the newest entry, or null when not browsing.
    const std::string* Newer();

    // One entry per physical line; '\\' and embedded newlines are escaped so multi-line entries round-trip.
    std::string Serialize() const;
    static std::vector<std::string> Deserialize(const std::string& text);
    void Load(const std::string& text);  // replaces the current history, cursor reset

private:
    size_t capacity_;
    std::vector<std::string> lines_;
    size_t cursor_;  // lines_.size() means "not browsing"
    std::string draft_;
};
}  // namespace melange::console
