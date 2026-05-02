#include "log_sink.h"
#include "spdlog/details/log_msg.h"
#include <chrono>

namespace testbridge {

std::shared_ptr<LogSink> LogSink::instance()
{
    static auto s = std::make_shared<LogSink>();
    return s;
}

void LogSink::sink_it_(const spdlog::details::log_msg& msg)
{
    spdlog::memory_buf_t buf;
    formatter_->format(msg, buf);
    std::string text(buf.data(), buf.size());
    if (!text.empty() && text.back() == '\n') text.pop_back();

    Entry e;
    e.level  = spdlog::level::to_string_view(msg.level).data();
    e.text   = text;
    e.ts_ms  = std::chrono::duration_cast<std::chrono::milliseconds>(
                   msg.time.time_since_epoch()).count();

    {
        std::lock_guard<std::mutex> lk(m_ring_mutex);
        m_ring.push_back(std::move(e));
        if (m_ring.size() > kMaxLines)
            m_ring.pop_front();
    }
    m_cv.notify_all();
}

nlohmann::json LogSink::recent(std::size_t max, const std::string& regex) const
{
    std::lock_guard<std::mutex> lk(m_ring_mutex);
    nlohmann::json arr = nlohmann::json::array();
    bool use_re = !regex.empty();
    std::regex re;
    if (use_re) re = std::regex(regex);

    // iterate from newest
    std::size_t count = 0;
    for (auto it = m_ring.rbegin(); it != m_ring.rend() && count < max; ++it) {
        if (use_re && !std::regex_search(it->text, re)) continue;
        nlohmann::json obj;
        obj["level"] = it->level;
        obj["text"]  = it->text;
        obj["ts_ms"] = it->ts_ms;
        arr.insert(arr.begin(), obj); // maintain chronological order
        ++count;
    }
    return arr;
}

nlohmann::json LogSink::wait(const std::string& regex, int timeout_ms) const
{
    std::regex re(regex);
    auto deadline = std::chrono::steady_clock::now()
                  + std::chrono::milliseconds(timeout_ms);

    std::unique_lock<std::mutex> lk(m_ring_mutex);
    while (true) {
        // Check existing entries from newest
        for (auto it = m_ring.rbegin(); it != m_ring.rend(); ++it) {
            if (std::regex_search(it->text, re)) {
                nlohmann::json obj;
                obj["level"] = it->level;
                obj["text"]  = it->text;
                obj["ts_ms"] = it->ts_ms;
                return obj;
            }
        }
        auto now = std::chrono::steady_clock::now();
        if (now >= deadline) break;
        m_cv.wait_until(lk, deadline);
    }
    return nullptr;
}

} // namespace testbridge
