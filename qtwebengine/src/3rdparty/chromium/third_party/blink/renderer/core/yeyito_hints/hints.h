// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef THIRD_PARTY_BLINK_RENDERER_CORE_YEYITO_HINTS_HINTS_H_
#define THIRD_PARTY_BLINK_RENDERER_CORE_YEYITO_HINTS_HINTS_H_

#include "third_party/blink/public/common/input/web_input_event.h"
#include "third_party/blink/public/platform/web_input_event_result.h"
#include "third_party/blink/renderer/core/core_export.h"
#include "third_party/blink/renderer/core/frame/frame_overlay.h"
#include "third_party/blink/renderer/core/yeyito_hints/candidate.h"
#include "third_party/blink/renderer/platform/heap/collection_support/heap_vector.h"
#include "third_party/blink/renderer/platform/heap/member.h"
#include "third_party/blink/renderer/platform/supplementable.h"

namespace blink {

class Element;
class GraphicsContext;
class LocalFrame;
class WebKeyboardEvent;

class CORE_EXPORT Hints final : public GarbageCollected<Hints>,
                                public Supplement<LocalFrame> {
  USING_PRE_FINALIZER(Hints, Dispose);

 public:
  static const char kSupplementName[];
  static Hints& From(LocalFrame& frame);

  explicit Hints(LocalFrame& frame);
  Hints(const Hints&) = delete;
  Hints& operator=(const Hints&) = delete;

  void Dispose();
  WebInputEventResult HandleKeyEvent(const WebKeyboardEvent& event);
  void PaintOverlay(GraphicsContext& context);

  bool IsActive() const { return active_; }
  LocalFrame* GetFrame() const { return GetSupplementable(); }
  const HeapVector<HintCandidate>& Candidates() const { return candidates_; }
  const String& TypedPrefix() const { return typed_prefix_; }

  void Trace(Visitor* visitor) const override;

 private:
  void Start();
  void Stop();
  void CollectCandidates();
  void AssignLabels();
  bool IsHintModeEntryKey(const WebKeyboardEvent& event) const;
  bool ShouldIgnoreEntryKeyForFocusedEditable() const;
  bool AppendTypedCharacter(const WebKeyboardEvent& event);
  WebInputEventResult HandleTypedPrefix();
  void ActivateCandidate(HintCandidate& candidate);
  void ScheduleOverlayUpdate();

  bool active_ = false;
  UChar pending_char_to_suppress_ = 0;
  String typed_prefix_;
  HeapVector<HintCandidate> candidates_;
  Member<FrameOverlay> frame_overlay_;
};

}  // namespace blink

#endif  // THIRD_PARTY_BLINK_RENDERER_CORE_YEYITO_HINTS_HINTS_H_
