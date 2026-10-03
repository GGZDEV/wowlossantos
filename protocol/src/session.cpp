#include "gamebridge/session.h"

#include "gamebridge/protocol.h"

namespace gamebridge {

bool parseRequestCounter(std::string const& requestId, std::int64_t& counter)
{
    std::size_t dash = requestId.rfind('-');
    if (dash == std::string::npos || dash == 0 || dash + 1 >= requestId.size())
        return false;
    std::size_t digits = requestId.size() - dash - 1;
    if (digits > 16)
        return false;
    std::int64_t v = 0;
    for (std::size_t i = dash + 1; i < requestId.size(); ++i)
    {
        char c = requestId[i];
        if (c < '0' || c > '9')
            return false;
        v = v * 10 + (c - '0');
    }
    if (v > kMaxSafeInteger)
        return false;
    counter = v;
    return true;
}

RequestCache::Verdict RequestCache::check(std::string const& requestId, std::uint64_t hash, Entry const** existing)
{
    std::int64_t counter;
    if (!parseRequestCounter(requestId, counter))
        return Verdict::Malformed;

    auto it = _entries.find(counter);
    if (it != _entries.end())
    {
        if (it->second.first != requestId || it->second.second.payloadHash != hash)
            return Verdict::PayloadMismatch;
        if (existing)
            *existing = &it->second.second;
        return Verdict::Duplicate;
    }
    // Unknown counter: anything at/below an evicted counter or below the highest
    // accepted counter is a replay (new requests must use a larger counter).
    if (counter <= _floor || counter <= _highest)
        return Verdict::Replay;

    _highest = counter;
    Entry e;
    e.payloadHash = hash;
    _entries.emplace(counter, std::make_pair(requestId, e));
    while (_entries.size() > _capacity)
    {
        auto oldest = _entries.begin();
        if (oldest->first > _floor)
            _floor = oldest->first;
        _entries.erase(oldest);
    }
    return Verdict::New;
}

void RequestCache::complete(std::string const& requestId, std::string resultJson)
{
    std::int64_t counter;
    if (!parseRequestCounter(requestId, counter))
        return;
    auto it = _entries.find(counter);
    if (it == _entries.end() || it->second.first != requestId)
        return;
    it->second.second.resultJson = std::move(resultJson);
    it->second.second.completed = true;
}

void RequestCache::reset()
{
    _entries.clear();
    _floor = -1;
    _highest = -1;
}

bool RevisionGate::accept(std::string const& entityId, std::int64_t generation, std::int64_t revision)
{
    auto it = _last.find(entityId);
    if (it != _last.end())
    {
        if (generation < it->second.first)
            return false;  // late state for an older incarnation
        if (generation == it->second.first && revision <= it->second.second)
            return false;  // equal/older state cannot roll back newer state
    }
    _last[entityId] = std::make_pair(generation, revision);
    return true;
}

std::int64_t RevisionGate::last(std::string const& entityId) const
{
    auto it = _last.find(entityId);
    return it == _last.end() ? -1 : it->second.second;
}

bool EventDedupe::firstTime(std::string const& eventId)
{
    if (_set.count(eventId))
        return false;
    _set.insert(eventId);
    _order.push_back(eventId);
    while (_order.size() > _capacity)
    {
        _set.erase(_order.front());
        _order.pop_front();
    }
    return true;
}

} // namespace gamebridge
