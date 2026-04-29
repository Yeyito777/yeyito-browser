// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/core/yeyito_hints/yeyito_hint_overlay.h"

#include <algorithm>
#include <cmath>
#include <string>

#include "cc/paint/paint_canvas.h"
#include "cc/paint/paint_flags.h"
#include "skia/ext/font_utils.h"
#include "third_party/blink/renderer/core/frame/local_frame.h"
#include "third_party/blink/renderer/core/frame/local_frame_view.h"
#include "third_party/blink/renderer/core/yeyito_hints/yeyito_hint_candidate.h"
#include "third_party/blink/renderer/core/yeyito_hints/yeyito_hint_controller.h"
#include "third_party/blink/renderer/platform/graphics/graphics_context.h"
#include "third_party/blink/renderer/platform/graphics/paint/drawing_recorder.h"
#include "third_party/skia/include/core/SkFont.h"
#include "third_party/skia/include/core/SkFontMetrics.h"
#include "third_party/skia/include/core/SkTextBlob.h"
#include "third_party/skia/include/core/SkTypeface.h"

namespace blink {

namespace {

constexpr SkColor kHintBackground = SkColorSetRGB(0x1d, 0x9b, 0xf0);
constexpr SkColor kHintForeground = SkColorSetRGB(0x00, 0x05, 0x0f);
constexpr SkColor kHintMatchedForeground = SkColorSetRGB(0xff, 0xff, 0xff);

constexpr char kHintAlphabet[] = "asdfghjklqwertyuiopzxcvbnm";

bool LabelIsVisibleForPrefix(const String& label, const String& prefix) {
  return prefix.empty() || label.StartsWith(prefix);
}

SkScalar HintCellWidth(const SkFont& font) {
  SkScalar max_width = 0;
  for (const char* c = kHintAlphabet; *c; ++c) {
    max_width =
        std::max(max_width, font.measureText(c, 1, SkTextEncoding::kUTF8));
  }
  return max_width;
}

sk_sp<SkTypeface> HintTypeface() {
  static sk_sp<SkTypeface> typeface = [] {
    // Match qutebrowser's canonical app font from qutebrowser/app.py.
    // Keep monospace fallbacks so same-length hint labels get identical widths
    // even if the preferred family is unavailable.
    constexpr const char* kMonospaceFamilies[] = {
        "JetBrains Mono",  "monospace",      "DejaVu Sans Mono",
        "Liberation Mono", "Noto Sans Mono",
    };
    for (const char* family : kMonospaceFamilies) {
      sk_sp<SkTypeface> candidate = skia::MakeTypefaceFromName(
          family,
          SkFontStyle(SkFontStyle::kBold_Weight, SkFontStyle::kNormal_Width,
                      SkFontStyle::kUpright_Slant));
      if (candidate) {
        return candidate;
      }
    }
    return skia::DefaultTypeface();
  }();
  return typeface;
}

void DrawTextChunk(cc::PaintCanvas& canvas,
                   const std::string& text,
                   const SkFont& font,
                   SkColor color,
                   float x,
                   float baseline) {
  if (text.empty()) {
    return;
  }

  cc::PaintFlags text_flags;
  text_flags.setAntiAlias(true);
  text_flags.setColor(color);
  text_flags.setStyle(cc::PaintFlags::kFill_Style);
  canvas.drawTextBlob(SkTextBlob::MakeFromText(text.data(), text.size(), font,
                                               SkTextEncoding::kUTF8),
                      x, baseline, text_flags);
}

void PaintLabel(cc::PaintCanvas& canvas,
                const gfx::PointF& point,
                const String& label,
                const String& prefix) {
  std::string text = label.Utf8();
  if (text.empty()) {
    return;
  }

  SkFont font(HintTypeface());
  font.setSize(12.0f);

  const SkScalar text_width =
      font.measureText(text.data(), text.size(), SkTextEncoding::kUTF8);
  const SkScalar label_width = HintCellWidth(font) * text.size();
  SkFontMetrics metrics;
  font.getMetrics(&metrics);

  constexpr float kHorizontalPadding = 3.0f;
  constexpr float kExtraHeight = 5.0f;
  const float text_height = metrics.fDescent - metrics.fAscent;
  const float width =
      std::max(12.0f, std::ceil(label_width + 2 * kHorizontalPadding));
  const float height = std::ceil(std::max(12.0f, text_height) + kExtraHeight);

  const float left = std::max(0.0f, point.x());
  const float top = std::max(0.0f, point.y());
  const SkRect background = SkRect::MakeXYWH(left, top, width, height);
  const float text_x = left + (width - text_width) / 2.0f;
  const float baseline = top + (height - text_height) / 2.0f - metrics.fAscent;

  cc::PaintFlags background_flags;
  background_flags.setAntiAlias(true);
  background_flags.setColor(kHintBackground);
  background_flags.setStyle(cc::PaintFlags::kFill_Style);
  canvas.drawRect(background, background_flags);

  wtf_size_t matched_char_count = 0;
  if (!prefix.empty() && label.StartsWith(prefix)) {
    matched_char_count = std::min(prefix.length(), label.length());
  }
  const size_t matched_byte_count =
      std::min<size_t>(matched_char_count, text.size());
  const std::string matched_text = text.substr(0, matched_byte_count);
  const std::string rest_text = text.substr(matched_byte_count);
  const SkScalar matched_width = font.measureText(
      matched_text.data(), matched_text.size(), SkTextEncoding::kUTF8);

  DrawTextChunk(canvas, matched_text, font, kHintMatchedForeground, text_x,
                baseline);
  DrawTextChunk(canvas, rest_text, font, kHintForeground,
                text_x + matched_width, baseline);
}

}  // namespace

YeyitoHintOverlayDelegate::YeyitoHintOverlayDelegate(
    YeyitoHintController& controller)
    : controller_(&controller) {}

void YeyitoHintOverlayDelegate::PaintFrameOverlay(
    const FrameOverlay& frame_overlay,
    GraphicsContext& context,
    const gfx::Size& view_size) const {
  if (!controller_ || !controller_->IsActive()) {
    return;
  }

  if (DrawingRecorder::UseCachedDrawingIfPossible(context, frame_overlay,
                                                  DisplayItem::kFrameOverlay)) {
    return;
  }
  DrawingRecorder recorder(context, frame_overlay, DisplayItem::kFrameOverlay,
                           gfx::Rect(view_size));

  cc::PaintCanvas* canvas = context.Canvas();
  if (!canvas) {
    return;
  }

  const String& prefix = controller_->TypedPrefix();
  for (const auto& candidate : controller_->Candidates()) {
    if (!candidate.element) {
      continue;
    }
    const bool visible = LabelIsVisibleForPrefix(candidate.label, prefix);
    if (!visible && !prefix.empty()) {
      continue;
    }
    PaintLabel(*canvas, candidate.viewport_rect.origin(), candidate.label,
               prefix);
  }
}

void YeyitoHintOverlayDelegate::Invalidate() {
  if (!controller_) {
    return;
  }
  LocalFrame* frame = controller_->GetFrame();
  if (!frame || !frame->View()) {
    return;
  }
  frame->View()->SetVisualViewportOrOverlayNeedsRepaint();
  frame->View()->SetPaintArtifactCompositorNeedsUpdate();
  frame->View()->ScheduleAnimation();
}

}  // namespace blink
