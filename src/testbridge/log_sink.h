#pragma once

#include <mutex>
#include <deque>
#include <string>
#include <condition_variable>
#include <regex>

#include "spdlog/sinks/base_sink.h"
#include <nlohmann/json.hpp>

namespace testbridge {

class LogSink : public spdlog::sinks::base_sink<std::mutex>
{
public:
    static constexpr std::size_t kMaxLines = 4096;

    static std::shared_ptr<LogSink> instance();

    // Returns up to `max` recent log lines, optionally filtered by regex.
    nlohmann::json recent(std::size_t max, const std::string& regex = {}) const;

    // Blocks until a line matching `regex` appears or timeout_ms elapses.
    // Returns the matched line or null on timeout.
    nlohmann::json wait(const std::string& regex, int timeout_ms) const;

protected:
    void sink_it_(const spdlog::details::log_msg& msg) override;
    void flush_() override {}

private:
    struct Entry {
        std::string level;
        std::string text;
        long long   ts_ms;
    };

    mutable std::mutex              m_ring_mutex;
    std::deque<Entry>               m_ring;
    mutable std::condition_variable m_cv;
};

} // namespace testbridge
