#include "oasis/core/queue.h"

#include <algorithm>

#include "tools/json_mini.h"

namespace melange::oasis::core {
Outbox::Chan* Outbox::Find(ChannelId ch) {
    for (auto& c : chans_)
        if (c.id == ch) return &c;
    return nullptr;
}

const Outbox::Chan* Outbox::Find(ChannelId ch) const {
    for (const auto& c : chans_)
        if (c.id == ch) return &c;
    return nullptr;
}

void Outbox::Subscribe(ChannelId ch, std::string_view name, Overflow ov, size_t maxBytes) {
    if (Chan* c = Find(ch)) {
        c->ov = ov;
        c->maxBytes = maxBytes;
        return;
    }
    Chan c;
    c.id = ch;
    c.name = std::string(name);
    c.ov = ov;
    c.maxBytes = maxBytes;
    chans_.push_back(std::move(c));
}

void Outbox::Unsubscribe(ChannelId ch) {
    for (size_t i = 0; i < chans_.size(); ++i) {
        if (chans_[i].id != ch) continue;
        chanBytes_ -= chans_[i].bytes;
        chans_.erase(chans_.begin() + static_cast<ptrdiff_t>(i));
        return;
    }
}

bool Outbox::Subscribed(ChannelId ch) const { return Find(ch) != nullptr; }

bool Outbox::PushControl(std::string msg) {
    controlBytes_ += msg.size();
    control_.push_back(std::move(msg));
    return controlBytes_ <= lim_.connectionBytes;
}

void Outbox::DropFront(Chan& c) {
    c.bytes -= c.items.front().json.size();
    chanBytes_ -= c.items.front().json.size();
    c.items.pop_front();
}

bool Outbox::Publish(ChannelId ch, std::string_view json, uint64_t* dropped) {
    Chan* c = Find(ch);
    if (!c) return false;
    uint64_t lost = 0;
    const uint64_t seq = ++c->seq;
    if (c->ov == Overflow::Coalesce) {
        while (!c->items.empty()) DropFront(*c);
    }
    c->items.push_back(Item{seq, std::string(json)});
    c->bytes += json.size();
    chanBytes_ += json.size();
    if (c->ov == Overflow::DropOldest) {
        while (c->items.size() > 1 && (c->bytes > c->maxBytes || Bytes() > lim_.connectionBytes)) {
            DropFront(*c);
            ++lost;
        }
    }
    c->dropped += lost;
    if (dropped) *dropped = lost;
    return true;
}

uint32_t Outbox::Take(uint32_t now, std::vector<std::string>* out) {
    while (!control_.empty()) {
        controlBytes_ -= control_.front().size();
        out->push_back(std::move(control_.front()));
        control_.pop_front();
    }
    uint32_t wait = UINT32_MAX;
    for (auto& c : chans_) {
        if (c.items.empty() && !c.dropped) continue;
        const uint32_t since = now - c.lastFlush;
        if (c.flushedOnce && since < lim_.flushMs) {
            wait = std::min(wait, lim_.flushMs - since);
            continue;
        }
        c.lastFlush = now;
        c.flushedOnce = true;
        if (c.dropped) {
            out->push_back(jsonmini::Obj().Str("t", "drop").Str("ch", c.name).UInt("n", c.dropped).Str("why", "queue").End());
            c.dropped = 0;
        }
        while (!c.items.empty()) {
            const size_t n = std::min(c.items.size(), lim_.batchItems);
            std::string m = "{\"t\":\"ev\",\"ch\":\"" + jsonmini::Escape(c.name) + "\",\"seq\":" + std::to_string(c.items.front().seq);
            if (n == 1) {
                m += ",\"d\":";
                m += c.items.front().json;
            } else {
                m += ",\"b\":[";
                for (size_t i = 0; i < n; ++i) {
                    if (i) m += ',';
                    m += c.items[i].json;
                }
                m += ']';
            }
            m += '}';
            for (size_t i = 0; i < n; ++i) DropFront(c);
            out->push_back(std::move(m));
        }
    }
    return wait;
}
}  // namespace melange::oasis::core
