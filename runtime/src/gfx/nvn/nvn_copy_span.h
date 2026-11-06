// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <limits>

namespace nxrt::nvn::be {
struct CopySpan {
    uint64_t bytes = 0;
    uint32_t row_texels = 0, image_texels = 0;
};

// Vulkan addresses only the last copied block, not trailing row/slice padding.
// The caller supplies depth slices * array layers after choosing subresources.
inline bool copy_span(uint32_t width, uint32_t height, uint64_t slices,
                      uint32_t block_w, uint32_t block_h, uint32_t block_bytes,
                      int64_t row_stride, int64_t image_stride, CopySpan& out) {
    out = {};
    if (!width || !height || !slices || !block_w || !block_h || !block_bytes ||
        row_stride < 0 || image_stride < 0) return false;
    constexpr uint64_t limit = std::numeric_limits<uint64_t>::max();
    auto mul = [](uint64_t a, uint64_t b, uint64_t& result) {
        if (b && a > std::numeric_limits<uint64_t>::max() / b) return false;
        result = a * b;
        return true;
    };
    const uint64_t columns = (uint64_t(width) + block_w - 1) / block_w;
    const uint64_t rows = (uint64_t(height) + block_h - 1) / block_h;
    uint64_t row_bytes;
    if (!mul(columns, block_bytes, row_bytes)) return false;
    const uint64_t stride = row_stride ? uint64_t(row_stride) : row_bytes;
    if (stride < row_bytes || stride % block_bytes) return false;
    uint64_t minimum_slice;
    if (!mul(stride, rows, minimum_slice)) return false;
    const uint64_t slice = image_stride ? uint64_t(image_stride) : minimum_slice;
    if (slice < minimum_slice || slice % stride) return false;
    uint64_t row_texels, image_texels, preceding_slices, preceding_rows;
    if (!mul(stride / block_bytes, block_w, row_texels) ||
        !mul(slice / stride, block_h, image_texels) ||
        row_texels > UINT32_MAX || image_texels > UINT32_MAX ||
        !mul(slices - 1, slice, preceding_slices) || !mul(rows - 1, stride, preceding_rows) ||
        preceding_rows > limit - row_bytes || preceding_slices > limit - preceding_rows - row_bytes) return false;
    out = {preceding_slices + preceding_rows + row_bytes, uint32_t(row_texels), uint32_t(image_texels)};
    return true;
}
} // namespace nxrt::nvn::be
