#include "performance_monitor.h"

namespace engine {
namespace perf {

RollingPerformanceMonitor::RollingPerformanceMonitor(uint32_t reportEveryFrames)
    : m_reportEveryFrames(reportEveryFrames == 0 ? 120 : reportEveryFrames)
{
}

bool RollingPerformanceMonitor::push(const FramePerfSample& sample, FramePerfSample& outAverage)
{
    m_accum.updateMs += sample.updateMs;
    m_accum.uploadMs += sample.uploadMs;
    m_accum.renderSubmitMs += sample.renderSubmitMs;
    m_accum.textureLoadMs += sample.textureLoadMs;
    m_accum.smapMs += sample.smapMs;
    ++m_count;

    if (m_count < m_reportEveryFrames)
        return false;

    const float inv = 1.0f / float(m_count);
    outAverage.updateMs = m_accum.updateMs * inv;
    outAverage.uploadMs = m_accum.uploadMs * inv;
    outAverage.renderSubmitMs = m_accum.renderSubmitMs * inv;
    outAverage.textureLoadMs = m_accum.textureLoadMs * inv;
    outAverage.smapMs = m_accum.smapMs * inv;
    reset();
    return true;
}

void RollingPerformanceMonitor::reset()
{
    m_count = 0;
    m_accum = FramePerfSample{};
}

} // namespace perf
} // namespace engine
