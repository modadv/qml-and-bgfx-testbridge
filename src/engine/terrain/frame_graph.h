// frame_graph.h
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>
#include <nlohmann/json.hpp>

// Minimal, linear, *declarative* frame-pass description.
//
// This is deliberately NOT a render-graph scheduler: there is no DAG solver, no
// resource aliasing, no automatic barrier insertion. The render pipeline here is
// a fixed, short, ordered sequence of passes (decode -> smap -> terrain ->
// overlay -> present). What was missing was making that ordering and each pass's
// read/write set *explicit* instead of implicit in the body of update(). This
// header provides exactly that: a recorded pass list that can be (a) validated
// for producer-before-consumer ordering and (b) introspected via the
// render.resources snapshot. It does not drive submission, so it carries no
// hot-path cost and cannot change rendering behavior.
namespace engine {

// Resources that flow between passes within a single frame. Bit flags so a pass
// can declare a set of reads/writes in one mask.
enum class PassResource : uint32_t
{
    Heightfield = 1u << 0, // raw decoded heightfield source (external input)
    Dmap        = 1u << 1, // displacement map texture (produced by decode/upload)
    Smap        = 1u << 2, // slope map texture (produced from Dmap)
    Diffuse     = 1u << 3, // diffuse/albedo texture (produced by upload)
    ColorTarget = 1u << 4, // offscreen color attachment
    DepthTarget = 1u << 5, // offscreen depth attachment
    OverlayMax  = 1u << 6  // overlay max-elevation buffer
};

inline uint32_t operator|(PassResource a, PassResource b)
{
    return uint32_t(a) | uint32_t(b);
}
inline uint32_t operator|(uint32_t a, PassResource b)
{
    return a | uint32_t(b);
}

// One linear pass. viewId < 0 marks a CPU/host pass (no bgfx view).
struct FramePass
{
    std::string name;
    int         viewId = -1;
    uint32_t    reads  = 0;
    uint32_t    writes = 0;
};

class FramePassList
{
public:
    void clear() { m_passes.clear(); }

    FramePassList& add(std::string name, int viewId, uint32_t reads, uint32_t writes)
    {
        FramePass p;
        p.name   = std::move(name);
        p.viewId = viewId;
        p.reads  = reads;
        p.writes = writes;
        m_passes.push_back(std::move(p));
        return *this;
    }

    const std::vector<FramePass>& passes() const { return m_passes; }

    // Validate the linear ordering: any resource that is *produced* (written by
    // some pass in this list) must not be *read* by an earlier pass than the one
    // that writes it. Resources never written by any pass are external inputs
    // (e.g. a heightfield loaded from disk) and are exempt. Returns true when
    // valid; otherwise fills outError with the first violation.
    bool validate(std::string& outError) const
    {
        uint32_t produced = 0;
        for (size_t i = 0; i < m_passes.size(); ++i)
            produced |= m_passes[i].writes;

        uint32_t available = 0;
        for (size_t i = 0; i < m_passes.size(); ++i)
        {
            const FramePass& p = m_passes[i];
            const uint32_t producedReads = p.reads & produced; // reads of intra-frame resources
            const uint32_t missing = producedReads & ~available;
            if (missing != 0)
            {
                outError = "pass '" + p.name
                    + "' reads resource(s) not yet produced: " + maskNames(missing);
                return false;
            }
            available |= p.writes;
        }
        outError.clear();
        return true;
    }

    nlohmann::json toJson() const
    {
        nlohmann::json arr = nlohmann::json::array();
        for (size_t i = 0; i < m_passes.size(); ++i)
        {
            const FramePass& p = m_passes[i];
            arr.push_back({
                {"name", p.name},
                {"viewId", p.viewId},
                {"reads", maskArray(p.reads)},
                {"writes", maskArray(p.writes)}
            });
        }
        std::string err;
        const bool ok = validate(err);
        return nlohmann::json{
            {"passes", arr},
            {"count", m_passes.size()},
            {"valid", ok},
            {"error", err}
        };
    }

private:
    struct NameEntry { PassResource r; const char* n; };

    static const NameEntry* names(size_t& count)
    {
        static const NameEntry kNames[] = {
            {PassResource::Heightfield, "Heightfield"},
            {PassResource::Dmap,        "Dmap"},
            {PassResource::Smap,        "Smap"},
            {PassResource::Diffuse,     "Diffuse"},
            {PassResource::ColorTarget, "ColorTarget"},
            {PassResource::DepthTarget, "DepthTarget"},
            {PassResource::OverlayMax,  "OverlayMax"},
        };
        count = sizeof(kNames) / sizeof(kNames[0]);
        return kNames;
    }

    static nlohmann::json maskArray(uint32_t mask)
    {
        size_t count = 0;
        const NameEntry* tbl = names(count);
        nlohmann::json out = nlohmann::json::array();
        for (size_t i = 0; i < count; ++i)
            if (mask & uint32_t(tbl[i].r))
                out.push_back(tbl[i].n);
        return out;
    }

    static std::string maskNames(uint32_t mask)
    {
        size_t count = 0;
        const NameEntry* tbl = names(count);
        std::string out;
        for (size_t i = 0; i < count; ++i)
        {
            if (mask & uint32_t(tbl[i].r))
            {
                if (!out.empty()) out += "|";
                out += tbl[i].n;
            }
        }
        return out.empty() ? std::string("<none>") : out;
    }

    std::vector<FramePass> m_passes;
};

} // namespace engine
