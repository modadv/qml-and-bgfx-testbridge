#include "terrain/heightfield_asset.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

namespace {

std::string tempPath(const char* name)
{
    char buffer[L_tmpnam];
    std::tmpnam(buffer);
    return std::string(buffer) + "_" + name;
}

void writeBytes(const std::string& path, const std::vector<uint8_t>& bytes)
{
    std::ofstream out(path, std::ios::binary);
    assert(out.is_open());
    out.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
}

void appendLe16(std::vector<uint8_t>& out, uint16_t v)
{
    out.push_back(uint8_t(v & 0xff));
    out.push_back(uint8_t((v >> 8) & 0xff));
}

void appendBe16(std::vector<uint8_t>& out, int16_t v)
{
    const uint16_t u = uint16_t(v);
    out.push_back(uint8_t((u >> 8) & 0xff));
    out.push_back(uint8_t(u & 0xff));
}

void testRawR16()
{
    const std::string path = tempPath("height.r16");
    std::vector<uint8_t> bytes;
    appendLe16(bytes, 0);
    appendLe16(bytes, 32768);
    appendLe16(bytes, 65535);
    appendLe16(bytes, 1024);
    writeBytes(path, bytes);

    engine::terrain::HeightfieldLoadOptions options;
    options.rawWidth = 2;
    options.rawHeight = 2;
    const auto result = engine::terrain::loadHeightfieldAsset(path, false, options);
    std::remove(path.c_str());

    assert(result.success);
    assert(result.asset.encoding == engine::terrain::HeightfieldEncoding::RawR16);
    assert(result.asset.width == 2);
    assert(result.asset.height == 2);
    assert(result.asset.samples.size() == 4);
    assert(result.asset.samples[0] == 0);
    assert(result.asset.samples[1] == 32768);
    assert(result.asset.samples[2] == 65535);
    assert(result.asset.samples[3] == 1024);
}

void testSrtmHgt()
{
    const std::string path = tempPath("N00E000.hgt");
    const int16_t values[9] = {
        -100, 0, 100,
        200, -32768, 300,
        400, 500, 600
    };
    std::vector<uint8_t> bytes;
    for (int16_t v : values)
        appendBe16(bytes, v);
    writeBytes(path, bytes);

    const auto result = engine::terrain::loadHeightfieldAsset(path);
    std::remove(path.c_str());

    assert(result.success);
    assert(result.asset.encoding == engine::terrain::HeightfieldEncoding::SrtmHgt);
    assert(result.asset.sampleType == engine::terrain::HeightSampleType::ElevationMeters);
    assert(result.asset.width == 3);
    assert(result.asset.height == 3);
    assert(result.asset.heightMin == -100.0f);
    assert(result.asset.heightMax == 600.0f);
    assert(result.asset.samples[0] == 0);
    assert(result.asset.samples[8] == 65535);
}

void testPngRgbaLuma()
{
    const std::string path = tempPath("height.png");
    const std::vector<uint8_t> png = {
        0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d,
        0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x02,
        0x08, 0x06, 0x00, 0x00, 0x00, 0x72, 0xb6, 0x0d, 0x24, 0x00, 0x00, 0x00,
        0x18, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9c, 0x63, 0x60, 0x60, 0x60, 0xf8,
        0x1f, 0x1a, 0x1a, 0xfa, 0x9f, 0x61, 0xd5, 0xaa, 0x55, 0xff, 0x41, 0x00,
        0x00, 0x40, 0xd1, 0x09, 0xf7, 0x48, 0xad, 0x44, 0x57, 0x00, 0x00, 0x00,
        0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82
    };
    writeBytes(path, png);

    const auto result = engine::terrain::loadHeightfieldAsset(path);
    std::remove(path.c_str());

    assert(result.success);
    assert(result.asset.encoding == engine::terrain::HeightfieldEncoding::Image);
    assert(result.asset.width == 2);
    assert(result.asset.height == 2);
    assert(result.asset.samples.size() == 4);
    assert(result.asset.samples[0] == 0);
    assert(result.asset.samples[3] == 65535);
}

} // namespace

int main()
{
    testRawR16();
    testSrtmHgt();
    testPngRgbaLuma();
    return 0;
}
