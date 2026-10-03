// GameBridge protocol v1 - per-epoch lifecycle helpers shared by both endpoints:
// sequence checking, request deduplication with a bounded replay window,
// state revision gating, presentation-event dedupe and bounded queues.
#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <utility>

namespace gamebridge {

// seq must be strictly increasing per sender per epoch.
class SeqTracker
{
public:
    bool accept(std::int64_t seq)
    {
        if (_started && seq <= _last)
            return false;
        _started = true;
        _last = seq;
        return true;
    }
    void reset() { _started = false; _last = -1; }
    std::int64_t last() const { return _last; }

private:
    bool _started = false;
    std::int64_t _last = -1;
};

// Gameplay request IDs have the form "<prefix>-<counter>" where <counter> is a
// decimal safe integer, strictly increasing per epoch for new requests. A retry
// of the same request reuses the same ID. This makes the replay window bounded:
// IDs with a counter at or below the eviction floor can never be accepted again.
bool parseRequestCounter(std::string const& requestId, std::int64_t& counter);

class RequestCache
{
public:
    enum class Verdict { New, Duplicate, PayloadMismatch, Replay, Malformed };

    struct Entry
    {
        std::uint64_t payloadHash = 0;
        std::string resultJson;   // last response payload (may be empty while in progress)
        bool completed = false;
    };

    explicit RequestCache(std::size_t capacity = 256) : _capacity(capacity) { }

    // Classify an incoming request; on New the entry is recorded (in progress).
    Verdict check(std::string const& requestId, std::uint64_t payloadHash, Entry const** existing = nullptr);
    void complete(std::string const& requestId, std::string resultJson);
    void reset();

    std::size_t size() const { return _entries.size(); }
    std::int64_t floor() const { return _floor; }

private:
    std::size_t _capacity;
    std::map<std::int64_t, std::pair<std::string, Entry>> _entries; // counter -> (id, entry)
    std::int64_t _floor = -1;       // highest evicted counter
    std::int64_t _highest = -1;     // highest accepted counter
};

// Absolute state is applied only when strictly newer than what is applied.
class RevisionGate
{
public:
    bool accept(std::string const& entityId, std::int64_t generation, std::int64_t revision);
    void forget(std::string const& entityId) { _last.erase(entityId); }
    void reset() { _last.clear(); }
    std::int64_t last(std::string const& entityId) const;

private:
    std::map<std::string, std::pair<std::int64_t, std::int64_t>> _last; // id -> (generation, revision)
};

// Presentation events are played once; duplicates are ignored. Bounded FIFO.
class EventDedupe
{
public:
    explicit EventDedupe(std::size_t capacity = 512) : _capacity(capacity) { }
    bool firstTime(std::string const& eventId);
    void reset() { _set.clear(); _order.clear(); }

private:
    std::size_t _capacity;
    std::set<std::string> _set;
    std::deque<std::string> _order;
};

// Bounded queue. Items with a non-empty coalesce key replace an older queued
// item with the same key (telemetry). Critical items (empty key) are never
// dropped: if the queue is full the queue latches `overflowed` and rejects the
// push; the owner must then freeze the bridge and resynchronise.
template <typename T>
class BoundedQueue
{
public:
    explicit BoundedQueue(std::size_t capacity) : _capacity(capacity) { }

    bool push(T item, std::string const& coalesceKey = std::string())
    {
        if (_overflowed)
            return false;
        if (!coalesceKey.empty())
            for (auto& q : _q)
                if (q.first == coalesceKey)
                {
                    q.second = std::move(item);
                    return true;
                }
        if (_q.size() >= _capacity)
        {
            _overflowed = true;
            return false;
        }
        _q.emplace_back(coalesceKey, std::move(item));
        return true;
    }

    bool pop(T& out)
    {
        if (_q.empty())
            return false;
        out = std::move(_q.front().second);
        _q.pop_front();
        return true;
    }

    bool overflowed() const { return _overflowed; }
    std::size_t size() const { return _q.size(); }
    std::size_t capacity() const { return _capacity; }
    void clear() { _q.clear(); _overflowed = false; }

private:
    std::size_t _capacity;
    std::deque<std::pair<std::string, T>> _q;
    bool _overflowed = false;
};

} // namespace gamebridge
