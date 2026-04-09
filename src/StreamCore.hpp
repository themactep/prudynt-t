#ifndef STREAM_CORE_HPP
#define STREAM_CORE_HPP

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

enum class StreamStartPolicy
{
    LiveEdge,
    LatestSync
};

template <typename FrameType>
struct StreamCoreTraits
{
    static bool is_sync(const FrameType &)
    {
        return false;
    }
};

template <typename FrameType>
class StreamCore
{
public:
    struct Cursor
    {
        std::size_t subscriberId{0};
        uint64_t nextSequence{0};
        uint64_t overflowCount{0};
        StreamStartPolicy startPolicy{StreamStartPolicy::LiveEdge};
        bool requireSync{false};
        bool started{false};
        bool active{false};
    };

    explicit StreamCore(std::size_t capacity)
        : capacity_{std::max<std::size_t>(capacity, 1)}
    {
    }

    Cursor registerSubscriber(std::function<void()> callback,
                              StreamStartPolicy startPolicy)
    {
        std::lock_guard<std::mutex> lock(mutex_);

        Cursor cursor;
        cursor.subscriberId = nextSubscriberId_++;
        cursor.startPolicy = startPolicy;
        cursor.requireSync = startPolicy == StreamStartPolicy::LatestSync;
        cursor.nextSequence = initialSequenceLocked(startPolicy);
        cursor.active = true;
        callbacks_[cursor.subscriberId] = std::move(callback);
        return cursor;
    }

    void unregisterSubscriber(Cursor &cursor)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!cursor.active)
            return;

        callbacks_.erase(cursor.subscriberId);
        cursor.active = false;
    }

    void publish(FrameType frame)
    {
        std::vector<std::function<void()>> callbacks;
        {
            std::lock_guard<std::mutex> lock(mutex_);

            if (entries_.size() >= capacity_)
            {
                entries_.pop_front();
                ++producerDropCount_;
            }

            entries_.push_back(Entry{
                ++lastSequence_,
                std::move(frame),
            });

            latestFrame_ = entries_.back().frame;
            if (StreamCoreTraits<FrameType>::is_sync(entries_.back().frame))
                latestSyncFrame_ = entries_.back().frame;

            callbacks.reserve(callbacks_.size());
            for (const auto &[_, callback] : callbacks_)
            {
                if (callback)
                    callbacks.push_back(callback);
            }
        }

        dataCv_.notify_all();
        for (auto &callback : callbacks)
            callback();
    }

    bool read(Cursor &cursor, FrameType *out)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return readLocked(cursor, out);
    }

    template <class Rep, class Period>
    bool waitReadFor(Cursor &cursor,
                     FrameType *out,
                     const std::chrono::duration<Rep, Period> &timeout)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        if (!dataCv_.wait_for(lock, timeout, [&]() {
                return hasReadableFrameLocked(cursor);
            }))
        {
            return false;
        }

        return readLocked(cursor, out);
    }

    bool latestFrame(FrameType *out) const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!latestFrame_.has_value())
            return false;
        *out = *latestFrame_;
        return true;
    }

    bool latestSyncFrame(FrameType *out) const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!latestSyncFrame_.has_value())
            return false;
        *out = *latestSyncFrame_;
        return true;
    }

    std::size_t subscriberCount() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return callbacks_.size();
    }

    std::size_t depth() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return entries_.size();
    }

    uint64_t producerDropCount() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return producerDropCount_;
    }

private:
    struct Entry
    {
        uint64_t sequence;
        FrameType frame;
    };

    uint64_t initialSequenceLocked(StreamStartPolicy startPolicy) const
    {
        if (entries_.empty())
            return lastSequence_ + 1;

        if (startPolicy == StreamStartPolicy::LatestSync)
        {
            for (auto it = entries_.rbegin(); it != entries_.rend(); ++it)
            {
                if (StreamCoreTraits<FrameType>::is_sync(it->frame))
                    return it->sequence;
            }

            return lastSequence_ + 1;
        }

        return entries_.back().sequence;
    }

    bool hasReadableFrameLocked(Cursor &cursor)
    {
        if (!cursor.active || entries_.empty())
            return false;

        normalizeCursorLocked(cursor);
        const auto backSequence = entries_.back().sequence;
        return cursor.nextSequence <= backSequence;
    }

    bool readLocked(Cursor &cursor, FrameType *out)
    {
        if (!cursor.active || entries_.empty())
            return false;

        normalizeCursorLocked(cursor);

        for (const auto &entry : entries_)
        {
            if (entry.sequence < cursor.nextSequence)
                continue;

            if (cursor.requireSync && !cursor.started
                && !StreamCoreTraits<FrameType>::is_sync(entry.frame))
            {
                cursor.nextSequence = entry.sequence + 1;
                continue;
            }

            *out = entry.frame;
            cursor.nextSequence = entry.sequence + 1;
            cursor.started = true;
            return true;
        }

        return false;
    }

    void normalizeCursorLocked(Cursor &cursor)
    {
        if (entries_.empty())
            return;

        const auto frontSequence = entries_.front().sequence;
        if (cursor.nextSequence < frontSequence)
        {
            cursor.overflowCount += frontSequence - cursor.nextSequence;
            cursor.nextSequence = frontSequence;

            if (cursor.requireSync)
            {
                for (const auto &entry : entries_)
                {
                    if (entry.sequence < cursor.nextSequence)
                        continue;
                    if (StreamCoreTraits<FrameType>::is_sync(entry.frame))
                    {
                        cursor.nextSequence = entry.sequence;
                        break;
                    }
                }
            }
        }
    }

    const std::size_t capacity_;
    mutable std::mutex mutex_;
    std::condition_variable dataCv_;
    std::deque<Entry> entries_;
    std::unordered_map<std::size_t, std::function<void()>> callbacks_;
    std::optional<FrameType> latestFrame_;
    std::optional<FrameType> latestSyncFrame_;
    std::size_t nextSubscriberId_{1};
    uint64_t lastSequence_{0};
    uint64_t producerDropCount_{0};
};

#endif // STREAM_CORE_HPP
