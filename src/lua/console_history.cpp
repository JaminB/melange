#include "lua/console_history.h"

namespace melange::console {
History::History(size_t capacity) : capacity_(capacity ? capacity : 1), cursor_(0) {}

void History::Add(const std::string& line) {
    if (line.empty()) return;
    if (!lines_.empty() && lines_.back() == line) {
        ResetCursor();
        return;
    }
    lines_.push_back(line);
    if (lines_.size() > capacity_) lines_.erase(lines_.begin());
    ResetCursor();
}

void History::ResetCursor() {
    cursor_ = lines_.size();
    draft_.clear();
}

const std::string* History::Older(const std::string& draft) {
    if (lines_.empty() || cursor_ == 0) return nullptr;
    if (cursor_ == lines_.size()) draft_ = draft;
    --cursor_;
    return &lines_[cursor_];
}

const std::string* History::Newer() {
    if (cursor_ >= lines_.size()) return nullptr;
    ++cursor_;
    return cursor_ == lines_.size() ? &draft_ : &lines_[cursor_];
}

namespace {
std::string Escape(const std::string& line) {
    std::string out;
    out.reserve(line.size());
    for (char c : line) {
        if (c == '\\') out += "\\\\";
        else if (c == '\n') out += "\\n";
        else if (c == '\r') continue;
        else out += c;
    }
    return out;
}

std::string Unescape(const std::string& line) {
    std::string out;
    out.reserve(line.size());
    for (size_t i = 0; i < line.size(); ++i) {
        if (line[i] == '\\' && i + 1 < line.size()) {
            char n = line[++i];
            out += n == 'n' ? '\n' : n;
        } else {
            out += line[i];
        }
    }
    return out;
}
}  // namespace

std::string History::Serialize() const {
    std::string out;
    for (const std::string& l : lines_) {
        out += Escape(l);
        out += '\n';
    }
    return out;
}

std::vector<std::string> History::Deserialize(const std::string& text) {
    std::vector<std::string> out;
    size_t start = 0;
    while (start <= text.size()) {
        size_t nl = text.find('\n', start);
        if (nl == std::string::npos) {
            if (start < text.size()) out.push_back(Unescape(text.substr(start)));
            break;
        }
        out.push_back(Unescape(text.substr(start, nl - start)));
        start = nl + 1;
    }
    return out;
}

void History::Load(const std::string& text) {
    lines_ = Deserialize(text);
    if (lines_.size() > capacity_)
        lines_.erase(lines_.begin(), lines_.begin() + static_cast<std::vector<std::string>::difference_type>(lines_.size() - capacity_));
    ResetCursor();
}
}  // namespace melange::console
