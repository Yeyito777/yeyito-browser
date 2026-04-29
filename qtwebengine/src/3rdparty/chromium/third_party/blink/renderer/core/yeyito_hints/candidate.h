// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef THIRD_PARTY_BLINK_RENDERER_CORE_YEYITO_HINTS_CANDIDATE_H_
#define THIRD_PARTY_BLINK_RENDERER_CORE_YEYITO_HINTS_CANDIDATE_H_

#include "third_party/blink/renderer/core/dom/element.h"
#include "third_party/blink/renderer/platform/heap/member.h"
#include "third_party/blink/renderer/platform/wtf/text/wtf_string.h"
#include "ui/gfx/geometry/rect_f.h"

namespace blink {

struct HintCandidate {
  DISALLOW_NEW();

 public:
  void Trace(Visitor* visitor) const { visitor->Trace(element); }

  Member<Element> element;
  gfx::RectF viewport_rect;
  String label;
};

}  // namespace blink

WTF_ALLOW_CLEAR_UNUSED_SLOTS_WITH_MEM_FUNCTIONS(blink::HintCandidate)

#endif  // THIRD_PARTY_BLINK_RENDERER_CORE_YEYITO_HINTS_CANDIDATE_H_
