// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/core/yeyito_hints/hover_hints.h"

#include "third_party/blink/renderer/core/yeyito_hints/click_hints.h"

namespace blink::hover_hints {

void CollectCandidates(LocalFrame& frame,
                       HeapVector<HintCandidate>& candidates) {
  click_hints::CollectCandidates(frame, candidates,
                                 click_hints::CandidateGroup::kHoverables);
}

void ActivateCandidate(LocalFrame& controller_frame,
                       Element& element,
                       const gfx::RectF& rect) {
  click_hints::ActivateCandidate(controller_frame, element, rect,
                                 click_hints::ActivationAction::kHover);
}

}  // namespace blink::hover_hints
