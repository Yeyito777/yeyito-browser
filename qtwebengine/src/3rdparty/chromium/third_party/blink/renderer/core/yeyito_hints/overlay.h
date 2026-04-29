// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef THIRD_PARTY_BLINK_RENDERER_CORE_YEYITO_HINTS_OVERLAY_H_
#define THIRD_PARTY_BLINK_RENDERER_CORE_YEYITO_HINTS_OVERLAY_H_

#include <memory>

#include "third_party/blink/renderer/core/frame/frame_overlay.h"
#include "third_party/blink/renderer/platform/heap/persistent.h"

namespace blink {

class Hints;

class HintOverlayDelegate final : public FrameOverlay::Delegate {
 public:
  explicit HintOverlayDelegate(Hints& hints);
  ~HintOverlayDelegate() override = default;

  void PaintFrameOverlay(const FrameOverlay& frame_overlay,
                         GraphicsContext& context,
                         const gfx::Size& view_size) const override;
  void Invalidate() override;

 private:
  Persistent<Hints> hints_;
};

}  // namespace blink

#endif  // THIRD_PARTY_BLINK_RENDERER_CORE_YEYITO_HINTS_OVERLAY_H_
