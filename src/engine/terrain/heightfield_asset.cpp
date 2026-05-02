#include "heightfield_asset.h"

#include <bimg/decode.h>
#include <bx/allocator.h>
#include <bx/error.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <fstream>
#include <limits>

namespace engine {
namespace terrain {
namespace {

constexpr uint64_t kMaxHeightfieldBytes = 1024ull * 1024ull * 1024ull;
constexpr float kUnitlessDisplayScale = 0.3f;
constexpr int16_t kSrtmVoidSample = -32768;

std::string lowerExt(const std::string& path)
{
    const std::size_t dot = path.find_last_of('.');
    if (dot == std::string::npos)
        return {};
    std::string ext = path.substr(dot);
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return char(std::tolower(c)); });
    return ext;
}

bool readFile(const std::string& path, std::vector<uint8_t>& out, std::string& error)
{
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.is_open())
    {
        error = "failed to open file";
        return false;
    }

    const std::streamoff size = file.tellg();
    if (size <= 0 || uint64_t(size) > kMaxHeightfieldBytes)
    {
        error = "invalid file size";
        return false;
    }

    out.resize(std::size_t(size));
    file.seekg(0, std::ios::beg);
    if (!file.read(reinterpret_cast<char*>(out.data()), size))
    {
        error = "failed to read file";
        return false;
    }
    return true;
}

uint16_t readU16(const uint8_t* p, bool bigEndian)
{
    if (bigEndian)
        return uint16_t((uint16_t(p[0]) << 8) | uint16_t(p[1]));
    return uint16_t((uint16_t(p[1]) << 8) | uint16_t(p[0]));
}

int16_t readI16(const uint8_t* p, bool bigEndian)
{
    return int16_t(readU16(p, bigEndian));
}

bool inferSquareDimensions(std::size_t sampleCount, uint32_t& width, uint32_t& height)
{
    const double root = std::sqrt(double(sampleCount));
    const uint32_t dim = uint32_t(root + 0.5);
    if (uint64_t(dim) * uint64_t(dim) != sampleCount)
        return false;
    width = dim;
    height = dim;
    return dim > 0;
}

void finalizeUnitless(HeightfieldAsset& asset)
{
    asset.aspectRatio = asset.height > 0 ? float(asset.width) / float(asset.height) : 1.0f;
    asset.heightMin = 0.0f;
    asset.heightMax = kUnitlessDisplayScale;
    asset.sampleType = HeightSampleType::NormalizedUnsigned;
}

void normalizeFloatSamples(const std::vector<float>& values,
                           float minValue,
                           float maxValue,
                           std::vector<uint16_t>& out)
{
    out.resize(values.size(), 0);
    const float range = maxValue - minValue;
    if (!(range > 0.0f))
        return;

    for (std::size_t i = 0; i < values.size(); ++i)
    {
        const float v = values[i];
        if (!std::isfinite(v))
        {
            out[i] = 0;
            continue;
        }
        float n = (v - minValue) / range;
        n = std::max(0.0f, std::min(1.0f, n));
        out[i] = uint16_t(n * 65535.0f + 0.5f);
    }
}

HeightfieldLoadResult loadRawR16(const std::string& path,
                               const std::vector<uint8_t>& bytes,
                               const HeightfieldLoadOptions& options)
{
    HeightfieldLoadResult result;
    HeightfieldAsset& asset = result.asset;
    asset.path = path;
    asset.encoding = HeightfieldEncoding::RawR16;
    asset.formatName = "RAW_R16";

    if ((bytes.size() % 2) != 0)
    {
        result.error = "RAW R16 byte count is not even";
        return result;
    }

    const std::size_t sampleCount = bytes.size() / 2;
    uint32_t width = options.rawWidth;
    uint32_t height = options.rawHeight;
    if (width == 0 || height == 0)
    {
        if (!inferSquareDimensions(sampleCount, width, height))
        {
            result.error = "RAW R16 dimensions were not provided and sample count is not square";
            return result;
        }
    }
    if (uint64_t(width) * uint64_t(height) != sampleCount)
    {
        result.error = "RAW R16 dimensions do not match file size";
        return result;
    }

    asset.width = width;
    asset.height = height;
    asset.samples.resize(sampleCount);

    if (options.rawSigned)
    {
        std::vector<float> values(sampleCount, 0.0f);
        float minValue = std::numeric_limits<float>::infinity();
        float maxValue = -std::numeric_limits<float>::infinity();
        for (std::size_t i = 0; i < sampleCount; ++i)
        {
            const int16_t v = readI16(bytes.data() + i * 2, options.rawBigEndian);
            values[i] = float(v);
            minValue = std::min(minValue, values[i]);
            maxValue = std::max(maxValue, values[i]);
        }
        normalizeFloatSamples(values, minValue, maxValue, asset.samples);
        asset.heightMin = minValue;
        asset.heightMax = maxValue;
        asset.sampleType = HeightSampleType::ElevationMeters;
    }
    else
    {
        for (std::size_t i = 0; i < sampleCount; ++i)
            asset.samples[i] = readU16(bytes.data() + i * 2, options.rawBigEndian);
        finalizeUnitless(asset);
    }

    asset.aspectRatio = asset.height > 0 ? float(asset.width) / float(asset.height) : 1.0f;
    result.success = true;
    return result;
}

HeightfieldLoadResult loadSrtmHgt(const std::string& path, const std::vector<uint8_t>& bytes)
{
    HeightfieldLoadOptions options;
    options.rawBigEndian = true;
    options.rawSigned = true;

    HeightfieldLoadResult result;
    HeightfieldAsset& asset = result.asset;
    asset.path = path;
    asset.encoding = HeightfieldEncoding::SrtmHgt;
    asset.formatName = "SRTM_HGT";
    asset.sampleType = HeightSampleType::ElevationMeters;

    if ((bytes.size() % 2) != 0)
    {
        result.error = "HGT byte count is not even";
        return result;
    }

    const std::size_t sampleCount = bytes.size() / 2;
    uint32_t width = 0;
    uint32_t height = 0;
    if (!inferSquareDimensions(sampleCount, width, height))
    {
        result.error = "HGT sample count is not square";
        return result;
    }

    asset.width = width;
    asset.height = height;
    asset.aspectRatio = 1.0f;

    std::vector<float> values(sampleCount, 0.0f);
    float minValue = std::numeric_limits<float>::infinity();
    float maxValue = -std::numeric_limits<float>::infinity();
    for (std::size_t i = 0; i < sampleCount; ++i)
    {
        const int16_t v = readI16(bytes.data() + i * 2, true);
        if (v == kSrtmVoidSample)
        {
            values[i] = std::numeric_limits<float>::quiet_NaN();
            continue;
        }
        values[i] = float(v);
        minValue = std::min(minValue, values[i]);
        maxValue = std::max(maxValue, values[i]);
    }

    if (!std::isfinite(minValue) || !std::isfinite(maxValue))
    {
        result.error = "HGT contains no valid elevation samples";
        return result;
    }

    normalizeFloatSamples(values, minValue, maxValue, asset.samples);
    asset.heightMin = minValue;
    asset.heightMax = maxValue;
    result.success = true;
    return result;
}

HeightfieldLoadResult loadImage(const std::string& path,
                              const std::vector<uint8_t>& bytes,
                              bool keepGpuDecodePayload)
{
    HeightfieldLoadResult result;
    HeightfieldAsset& asset = result.asset;
    asset.path = path;
    asset.encoding = HeightfieldEncoding::Image;
    asset.formatName = "IMAGE";

    static thread_local bx::DefaultAllocator allocator;
    bx::Error error;
    bimg::ImageContainer* image = bimg::imageParse(
        &allocator,
        bytes.data(),
        uint32_t(bytes.size()),
        bimg::TextureFormat::Count,
        &error);
    if (image == nullptr)
    {
        result.error = "failed to parse image heightfield";
        return result;
    }

    asset.width = image->m_width;
    asset.height = image->m_height;
    asset.aspectRatio = asset.height > 0 ? float(asset.width) / float(asset.height) : 1.0f;
    const std::size_t pixelCount = std::size_t(asset.width) * std::size_t(asset.height);
    const uint8_t* src = static_cast<const uint8_t*>(image->m_data);
    asset.samples.assign(pixelCount, 0);

    switch (image->m_format)
    {
    case bimg::TextureFormat::R16:
        asset.formatName = "IMAGE_R16";
        if (image->m_size >= pixelCount * sizeof(uint16_t))
            std::memcpy(asset.samples.data(), src, pixelCount * sizeof(uint16_t));
        break;
    case bimg::TextureFormat::R8:
        asset.formatName = "IMAGE_R8";
        for (std::size_t i = 0; i < pixelCount; ++i)
            asset.samples[i] = uint16_t(src[i]) * 257u;
        break;
    case bimg::TextureFormat::RGBA8:
    case bimg::TextureFormat::BGRA8:
    case bimg::TextureFormat::RGB8:
    {
        asset.formatName = image->m_format == bimg::TextureFormat::RGB8
            ? "IMAGE_RGB8"
            : (image->m_format == bimg::TextureFormat::RGBA8 ? "IMAGE_RGBA8" : "IMAGE_BGRA8");
        const int channels = int(bimg::getBitsPerPixel(image->m_format)) / 8;
        const bool bgra = image->m_format == bimg::TextureFormat::BGRA8;
        for (std::size_t i = 0; i < pixelCount; ++i)
        {
            const uint8_t* p = src + i * std::size_t(channels);
            const uint8_t r = bgra ? p[2] : p[0];
            const uint8_t g = p[1];
            const uint8_t b = bgra ? p[0] : p[2];
            const uint16_t luma = uint16_t((uint32_t(r) * 299u + uint32_t(g) * 587u + uint32_t(b) * 114u) / 1000u);
            asset.samples[i] = uint16_t(luma * 257u);
        }
        if (keepGpuDecodePayload && channels == 4)
        {
            asset.rawData.resize(image->m_size);
            std::memcpy(asset.rawData.data(), src, image->m_size);
            asset.rawFormat = image->m_format;
            asset.rawIsBGRA = bgra;
        }
        break;
    }
    default:
        bimg::imageFree(image);
        result.error = "unsupported image texture format";
        return result;
    }

    finalizeUnitless(asset);
    bimg::imageFree(image);
    result.success = true;
    return result;
}

} // namespace

HeightfieldLoadResult loadHeightfieldAsset(const std::string& path,
                                       bool keepGpuDecodePayload,
                                       const HeightfieldLoadOptions& options)
{
    HeightfieldLoadResult result;
    if (path.empty() || path == "." || path == ".." || path == "/")
    {
        result.error = "invalid heightfield path";
        return result;
    }

    std::vector<uint8_t> bytes;
    if (!readFile(path, bytes, result.error))
        return result;

    const std::string ext = lowerExt(path);
    if (ext == ".hgt")
        return loadSrtmHgt(path, bytes);
    if (ext == ".raw" || ext == ".r16" || ext == ".r16le" || ext == ".r16be")
    {
        HeightfieldLoadOptions rawOptions = options;
        if (ext == ".r16be")
            rawOptions.rawBigEndian = true;
        return loadRawR16(path, bytes, rawOptions);
    }

    return loadImage(path, bytes, keepGpuDecodePayload);
}

} // namespace terrain
} // namespace engine
