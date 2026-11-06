// SPDX-License-Identifier: GPL-3.0-or-later
#include "test_framework.h"
#include "nvn_copy_span.h"
NXTEST_GROUP(copy_span)
using namespace nxrt::nvn::be;
NXTEST(array_slices_exclude_trailing_padding) {
    CopySpan s;
    CHECK(copy_span(3, 2, 4, 1, 1, 4, 16, 48, s));
    CHECK(s.bytes == 172); // last slice at 144, last row at 160, 12 bytes copied
    CHECK(s.row_texels == 4);
    CHECK(s.image_texels == 3);
    CHECK(copy_span(3, 2, 4, 1, 1, 4, 0, 0, s));
    CHECK(s.bytes == 96);
}
NXTEST(compressed_edge_blocks) {
    CopySpan s;
    CHECK(copy_span(7, 5, 2, 4, 4, 16, 64, 192, s));
    CHECK(s.bytes == 288);
    CHECK(s.row_texels == 16);
    CHECK(s.image_texels == 12);
    CHECK(copy_span(1, 1, 1, 4, 4, 8, 0, 0, s));
    CHECK(s.bytes == 8);
}
NXTEST(invalid_pitches_and_overflow) {
    CopySpan s;
    CHECK(!copy_span(4, 4, 1, 1, 1, 4, 12, 0, s));
    CHECK(!copy_span(4, 4, 1, 1, 1, 4, 17, 0, s));
    CHECK(!copy_span(4, 4, 1, 1, 1, 4, 16, 63, s));
    CHECK(!copy_span(4, 4, 1, 1, 1, 4, 16, 65, s));
    CHECK(!copy_span(4, 4, 1, 1, 1, 4, -1, 0, s));
    CHECK(!copy_span(0, 4, 1, 1, 1, 4, 0, 0, s));
    CHECK(!copy_span(1, 1, 0, 1, 1, 4, 0, 0, s));
    CHECK(!copy_span(1, 1, UINT64_MAX, 1, 1, 4, 0, 0, s));
    CHECK(!copy_span(UINT32_MAX, UINT32_MAX, 2, 1, 1, 16, 0, 0, s));
    CHECK(s.bytes == 0);
}
