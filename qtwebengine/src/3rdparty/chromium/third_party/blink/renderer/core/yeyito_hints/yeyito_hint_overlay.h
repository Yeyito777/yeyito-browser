// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef THIRD_PARTY_BLINK_RENDERER_CORE_YEYITO_HINTS_YEYITO_HINT_OVERLAY_H_
#define THIRD_PARTY_BLINK_RENDERER_CORE_YEYITO_HINTS_YEYITO_HINT_OVERLAY_H_

#include <memory>

#include "third_party/blink/renderer/core/frame/frame_overlay.h"
#include "third_party/blink/renderer/platform/heap/persistent.h"

namespace blink {

class YeyitoHintController;

class YeyitoHintOverlayDelegate final : public FrameOverlay::Delegate {
 public:
  explicit YeyitoHintOverlayDelegate(YeyitoHintController& controller);
  ~YeyitoHintOverlayDelegate() override = default;

  void PaintFrameOverlay(const FrameOverlay& frame_overlay,
                         GraphicsContext& context,
                         const gfx::Size& view_size) const override;
  void Invalidate() override;

 private:
  Persistent<YeyitoHintController> controller_;
};

}  // namespace blink

#endif  // THIRD_PARTY_BLINK_RENDERER_CORE_YEYITO_HINTS_YEYITO_HINT_OVERLAY_H_
