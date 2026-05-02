#pragma once

#include <bimg/bimg.h>

#include <cstdint>
#include <string>
#include <vector>

namespace engine {
namespace terrain {

enum class HeightfieldEncoding : uint8_t
{
    Unknown = 0,
    Image,
    RawR16,
    SrtmHgt
};

enum class HeightSampleType : uint8_t
{
    NormalizedUnsigned = 0,
    ElevationMeters
};

struct HeightfieldLoadOptions
{
    uint32_t rawWidth = 0;
    uint32_t rawHeight = 0;
    bool rawBigEndian = false;
    bool rawSigned = false;
};

struct HeightfieldAsset
{
    std::string path;
    std::string formatName;
    HeightfieldEncoding encoding = HeightfieldEncoding::Unknown;
    HeightSampleType sampleType = HeightSampleType::NormalizedUnsigned;
    uint32_t width = 0;
    uint32_t height = 0;
    float heightMin = 0.0f;
    float heightMax = 0.0f;
    float aspectRatio = 1.0f;
    std::vector<uint16_t> samples;

    // Optional raw image payload for legacy GPU decode paths.
    std::vector<uint8_t> rawData;
    bimg::TextureFormat::Enum rawFormat = bimg::TextureFormat::Count;
    bool rawIsBGRA = false;
};

struct HeightfieldLoadResult
{
    bool success = false;
    std::string error;
    HeightfieldAsset asset;
};

HeightfieldLoadResult loadHeightfieldAsset(const std::string& path,
                                       bool keepGpuDecodePayload = false,
                                       const HeightfieldLoadOptions& options = {});

} // namespace terrain
} // namespace engine
