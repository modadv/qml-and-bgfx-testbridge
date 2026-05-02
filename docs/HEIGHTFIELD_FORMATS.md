# Heightfield Format Support

This engine treats heightfields as a CPU-side `uint16_t` elevation grid before
uploading a DMap texture. The offscreen bgfx-to-Qt framebuffer architecture is
unchanged.

## Supported Now

| Format | Input | Notes |
|---|---|---|
| 8-bit grayscale image | PNG/JPEG/BMP/etc. through bimg | Expanded to 16-bit by multiplying by 257. |
| 16-bit grayscale image | PNG/TIFF/etc. through bimg when decoded as `R16` | Uploaded as R16 DMap. |
| RGB/RGBA image | PNG/JPEG/BMP/etc. through bimg | Converted using luma. RGBA/BGRA can still carry a raw payload for legacy GPU decode. |
| RAW R16 | `.raw`, `.r16`, `.r16le`, `.r16be` | Headerless unsigned 16-bit grid. Dimensions are inferred for square grids or passed through loader options. |
| SRTM HGT | `.hgt` | Big-endian signed 16-bit square grid. `-32768` is treated as void. |

## External References Used

- USGS states that modern US elevation DEMs are distributed as Cloud Optimized
  GeoTIFF, with legacy compatibility through GeoTIFF:
  https://www.usgs.gov/faqs/what-types-elevation-datasets-are-available-what-formats-do-they-come-and-where-can-i-download
- USGS standard DEM is an ASCII elevation file with metadata records and
  profile records: https://www.umesc.usgs.gov/data_library/other/15_min_dem.html
- SRTM `.hgt` is a simple binary height grid used by NASA/SRTM tooling:
  https://bwinkel.github.io/pycraf/pathprof/working_with_srtm.html
- A CC0 terrain heightfield PNG is available on Wikimedia Commons and is useful
  as a public visual smoke-test reference:
  https://commons.wikimedia.org/wiki/File:Hand_made_terrain_heightmap.png

## Next Formats

GeoTIFF/COG and Arc ASCII Grid should be implemented behind a separate optional
geospatial asset provider, probably with GDAL or a deliberately small TIFF
reader. They are intentionally not folded into the core renderer in this pass,
because the current app is a lightweight bgfx/Qt test harness rather than a GIS
runtime.
