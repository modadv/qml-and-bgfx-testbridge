#pragma once

#include <bx/timer.h>

#include <cstdint>

namespace engine {
namespace perf {

class ScopeTimer
{
public:
    explicit ScopeTimer(float& outMs)
        : m_out(outMs)
        , m_start(bx::getHPCounter())
    {
    }

    ~ScopeTimer()
    {
        const int64_t end = bx::getHPCounter();
        m_out = float((end - m_start) / double(bx::getHPFrequency()) * 1000.0);
    }

private:
    float& m_out;
    int64_t m_start;
};

struct FramePerfSample
{
    float updateMs = 0.0f;
    float uploadMs = 0.0f;
    float renderSubmitMs = 0.0f;
    float textureLoadMs = 0.0f;
    float smapMs = 0.0f;
};

class RollingPerformanceMonitor
{
public:
    explicit RollingPerformanceMonitor(uint32_t reportEveryFrames = 120);

    bool push(const FramePerfSample& sample, FramePerfSample& outAverage);
    void reset();

private:
    uint32_t m_reportEveryFrames = 120;
    uint32_t m_count = 0;
    FramePerfSample m_accum;
};

} // namespace perf
} // namespace engine
