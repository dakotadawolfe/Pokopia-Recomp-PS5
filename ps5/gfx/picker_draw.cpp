// SPDX-License-Identifier: GPL-3.0-or-later
// Uppercase/digit glyphs adapted from ps5-native-app-boilerplate demo_renderer.cpp,
// Copyright (C) 2026 BlackBearReloaded, revision dd44bbdc75437332ed22e3ba95126733419ef25a.
// Punctuation and Vulkan drawing are local additions. This shares the runtime
// renderer's attachment and never opens another VideoOut surface.
#include "picker_draw.h"
#include "../app/folder_picker.h"
#include "nxrt/gfx.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <string>
#include <string_view>
#include <vector>

namespace nxrt::app {
namespace {
struct Glyph { char character; std::array<uint8_t, 7> rows; };
constexpr Glyph glyphs[] = {
    {' ', {0,0,0,0,0,0,0}}, {'0', {14,17,19,21,25,17,14}}, {'1', {4,12,4,4,4,4,14}},
    {'2', {14,17,1,2,4,8,31}}, {'3', {30,1,1,14,1,1,30}}, {'4', {2,6,10,18,31,2,2}},
    {'5', {31,16,16,30,1,1,30}}, {'6', {14,16,16,30,17,17,14}}, {'7', {31,1,2,4,8,8,8}},
    {'8', {14,17,17,14,17,17,14}}, {'9', {14,17,17,15,1,1,14}}, {'A', {14,17,17,31,17,17,17}},
    {'B', {30,17,17,30,17,17,30}}, {'C', {14,17,16,16,16,17,14}}, {'D', {30,17,17,17,17,17,30}},
    {'E', {31,16,16,30,16,16,31}}, {'F', {31,16,16,30,16,16,16}}, {'G', {14,17,16,23,17,17,14}},
    {'H', {17,17,17,31,17,17,17}}, {'I', {31,4,4,4,4,4,31}}, {'J', {7,2,2,2,18,18,12}},
    {'K', {17,18,20,24,20,18,17}}, {'L', {16,16,16,16,16,16,31}}, {'M', {17,27,21,21,17,17,17}},
    {'N', {17,25,21,19,17,17,17}}, {'O', {14,17,17,17,17,17,14}}, {'P', {30,17,17,30,16,16,16}},
    {'Q', {14,17,17,17,21,18,13}}, {'R', {30,17,17,30,20,18,17}}, {'S', {15,16,16,14,1,1,30}},
    {'T', {31,4,4,4,4,4,4}}, {'U', {17,17,17,17,17,17,14}}, {'V', {17,17,17,17,17,10,4}},
    {'W', {17,17,17,21,21,21,10}}, {'X', {17,17,10,4,10,17,17}}, {'Y', {17,17,10,4,4,4,4}},
    {'Z', {31,1,2,4,8,16,31}},
    {'/', {1,1,2,4,8,16,16}}, {'\\', {16,16,8,4,2,1,1}}, {'.', {0,0,0,0,0,12,12}},
    {'_', {0,0,0,0,0,0,31}}, {'-', {0,0,0,31,0,0,0}}, {':', {0,12,12,0,12,12,0}},
    {'[', {14,8,8,8,8,8,14}}, {']', {14,2,2,2,2,2,14}}, {'(', {2,4,8,8,8,4,2}},
    {')', {8,4,2,2,2,4,8}}, {'+', {0,4,4,31,4,4,0}}, {'>', {16,8,4,2,4,8,16}},
    {'<', {1,2,4,8,4,2,1}}, {'!', {4,4,4,4,4,0,4}}, {'?', {14,17,1,2,4,0,4}},
    {'=', {0,31,0,31,0,0,0}}, {',', {0,0,0,0,0,4,8}}, {'|', {4,4,4,4,4,4,4}}
};
const auto& glyph(char value) {
    const char upper = static_cast<char>(std::toupper(static_cast<unsigned char>(value)));
    for (const auto& g : glyphs) if (g.character == upper) return g.rows;
    for (const auto& g : glyphs) if (g.character == '?') return g.rows;
    return glyphs[0].rows;
}
std::string shorten(std::string_view value, size_t length, bool tail = false) {
    if (value.size() <= length) return std::string(value);
    return tail ? "..." + std::string(value.substr(value.size() - length + 3)) : std::string(value.substr(0, length - 3)) + "...";
}
struct Canvas {
    const gfx::FrameInfo& frame;
    std::vector<VkClearRect> rectangles;
    void rect(float x, float y, float width, float height) {
        const float sx = frame.extent.width / 1280.0f, sy = frame.extent.height / 720.0f;
        const int left = std::clamp(static_cast<int>(x * sx), 0, static_cast<int>(frame.extent.width));
        const int top = std::clamp(static_cast<int>(y * sy), 0, static_cast<int>(frame.extent.height));
        const int right = std::clamp(static_cast<int>((x + width) * sx), left, static_cast<int>(frame.extent.width));
        const int bottom = std::clamp(static_cast<int>((y + height) * sy), top, static_cast<int>(frame.extent.height));
        if (right > left && bottom > top) rectangles.push_back({{{left, top}, {static_cast<uint32_t>(right-left), static_cast<uint32_t>(bottom-top)}}, 0, 1});
    }
    void text(std::string_view value, float x, float y, int scale, size_t maximum) {
        for (const char ch : shorten(value, maximum)) {
            const auto& rows = glyph(ch);
            for (size_t r = 0; r < rows.size(); ++r) {
                unsigned column = 0;
                while (column < 5) {
                    if (!(rows[r] & (16u >> column))) { ++column; continue; }
                    const unsigned first = column;
                    while (column < 5 && (rows[r] & (16u >> column))) ++column;
                    rect(x + first * scale, y + r * scale, (column-first) * scale, scale);
                }
            }
            x += 6 * scale;
        }
    }
    void flush(float red, float green, float blue) {
        if (rectangles.empty()) return;
        VkClearAttachment attachment{};
        attachment.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        attachment.colorAttachment = 0;
        attachment.clearValue.color = {{red, green, blue, 1.0f}};
        vkCmdClearAttachments(frame.cmd, 1, &attachment, static_cast<uint32_t>(rectangles.size()), rectangles.data());
        rectangles.clear();
    }
};
}

void draw_folder_picker(const gfx::FrameInfo& frame, const PickerView& view) {
    VkRenderingAttachmentInfo attachment{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    attachment.imageView = frame.view;
    attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.clearValue.color = {{0.025f, 0.035f, 0.055f, 1.0f}};
    VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
    rendering.renderArea = {{0,0}, frame.extent};
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachments = &attachment;
    vkCmdBeginRendering(frame.cmd, &rendering);
    Canvas canvas{frame};
    canvas.text(view.title, 60, 46, 4, 47);
    canvas.flush(0.95f, 0.97f, 1.0f);
    canvas.text(shorten(view.location, 94, true), 60, 98, 2, 94);
    canvas.flush(0.52f, 0.68f, 0.78f);
    constexpr size_t visible = 12;
    for (size_t i = 0; i < view.rows.size() && i < visible; ++i) {
        const float y = 151.0f + static_cast<float>(i) * 31.0f;
        if (i == view.selected) { canvas.rect(48, y-7, 1184, 29); canvas.flush(0.10f, 0.26f, 0.34f); }
        canvas.text(i == view.selected ? ">" : " ", 60, y, 2, 1);
        canvas.text(view.rows[i], 86, y, 2, 92);
        canvas.flush(0.9f, 0.94f, 0.95f);
    }
    size_t split = view.status.size();
    if (split > 94) {
        split = view.status.rfind(' ', 94);
        if (split == std::string::npos) split = 94;
    }
    canvas.text(std::string_view(view.status).substr(0, split), 60, 558, 2, 94);
    if (split < view.status.size()) {
        const size_t next = view.status[split] == ' ' ? split+1 : split;
        canvas.text(std::string_view(view.status).substr(next), 60, 586, 2, 94);
    }
    canvas.flush(0.55f, 0.82f, 0.77f);
    if (!view.controller_connected) {
        canvas.text("CONNECT A CONTROLLER TO CONTINUE", 60, 618, 2, 94);
        canvas.flush(1.0f, 0.73f, 0.32f);
    }
    canvas.text(view.controls, 60, 658, 2, 94);
    canvas.flush(0.60f, 0.65f, 0.72f);
    vkCmdEndRendering(frame.cmd);
}
} // namespace nxrt::app
