// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/core/yeyito_hints/hints.h"

#include "third_party/blink/public/common/input/web_keyboard_event.h"
#include "third_party/blink/renderer/core/dom/document.h"
#include "third_party/blink/renderer/core/editing/editing_utilities.h"
#include "third_party/blink/renderer/core/frame/frame_overlay.h"
#include "third_party/blink/renderer/core/frame/local_frame.h"
#include "third_party/blink/renderer/core/frame/local_frame_view.h"
#include "third_party/blink/renderer/core/yeyito_hints/click_hints.h"
#include "third_party/blink/renderer/core/yeyito_hints/hover_hints.h"
#include "third_party/blink/renderer/core/yeyito_hints/labels.h"
#include "third_party/blink/renderer/core/yeyito_hints/overlay.h"
#include "third_party/blink/renderer/platform/windows_keyboard_codes.h"
#include "third_party/blink/renderer/platform/wtf/text/string_builder.h"
#include "ui/events/keycodes/dom/dom_key.h"

namespace blink {

namespace {

constexpr int kQutebrowserBrowserCommandWebModifier = 1 << 27;
constexpr int kQutebrowserPageInputWebModifier = 1 << 28;

bool IsQutebrowserBrowserCommand(const WebKeyboardEvent& event) {
  return event.is_browser_shortcut ||
         (event.GetModifiers() & kQutebrowserBrowserCommandWebModifier);
}

bool IsQutebrowserPageInput(const WebKeyboardEvent& event) {
  return event.GetModifiers() & kQutebrowserPageInputWebModifier;
}

bool HasNoKeyModifiers(const WebKeyboardEvent& event) {
  return (event.GetModifiers() & WebKeyboardEvent::kKeyModifiers) == 0;
}

bool HasOnlyNoOrShiftModifiers(const WebKeyboardEvent& event) {
  return (event.GetModifiers() &
          (WebInputEvent::kControlKey | WebInputEvent::kAltKey |
           WebInputEvent::kMetaKey)) == 0;
}

bool HasOnlyControlModifier(const WebKeyboardEvent& event) {
  return (event.GetModifiers() & WebKeyboardEvent::kKeyModifiers) ==
         WebInputEvent::kControlKey;
}

bool IsASCIIHintKey(UChar c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

UChar LowerASCIIHintKey(UChar c) {
  if (c >= 'A' && c <= 'Z') {
    return c - 'A' + 'a';
  }
  return c;
}

UChar CharacterFromEvent(const WebKeyboardEvent& event) {
  if (event.text[0]) {
    return event.text[0];
  }
  if (event.unmodified_text[0]) {
    return event.unmodified_text[0];
  }
  if (event.windows_key_code >= 'A' && event.windows_key_code <= 'Z') {
    return static_cast<UChar>(event.windows_key_code);
  }
  return 0;
}

bool IsEscapeKey(const WebKeyboardEvent& event) {
  return event.windows_key_code == VK_ESCAPE ||
         event.dom_key == static_cast<uint32_t>(ui::DomKey::ESCAPE);
}

}  // namespace

const char Hints::kSupplementName[] = "Hints";

Hints& Hints::From(LocalFrame& frame) {
  auto* hints = Supplement<LocalFrame>::From<Hints>(frame);
  if (!hints) {
    hints = MakeGarbageCollected<Hints>(frame);
    Supplement<LocalFrame>::ProvideTo(frame, hints);
  }
  return *hints;
}

Hints::Hints(LocalFrame& frame) : Supplement<LocalFrame>(frame) {}

void Hints::Dispose() {
  Stop();
}

WebInputEventResult Hints::HandleKeyEvent(const WebKeyboardEvent& event) {
  if (event.GetType() != WebInputEvent::Type::kRawKeyDown &&
      event.GetType() != WebInputEvent::Type::kKeyDown &&
      event.GetType() != WebInputEvent::Type::kChar) {
    return WebInputEventResult::kNotHandled;
  }

  if (IsQutebrowserPageInput(event)) {
    if (active_) {
      Stop();
    }
    return WebInputEventResult::kNotHandled;
  }

  if (event.GetType() == WebInputEvent::Type::kChar &&
      pending_char_to_suppress_) {
    UChar c = CharacterFromEvent(event);
    if (IsASCIIHintKey(c) &&
        LowerASCIIHintKey(c) == pending_char_to_suppress_) {
      pending_char_to_suppress_ = 0;
      return WebInputEventResult::kHandledSuppressed;
    }
    pending_char_to_suppress_ = 0;
  }

  if (!active_) {
    if (event.GetType() != WebInputEvent::Type::kRawKeyDown &&
        event.GetType() != WebInputEvent::Type::kKeyDown) {
      return WebInputEventResult::kNotHandled;
    }

    const bool is_browser_command = IsQutebrowserBrowserCommand(event);
    const HintEntry entry = EntryForKeyEvent(event, is_browser_command);
    if (!entry.is_entry ||
        (!is_browser_command && ShouldIgnoreEntryKeyForFocusedEditable())) {
      return WebInputEventResult::kNotHandled;
    }

    UChar entry_char = CharacterFromEvent(event);
    Start(entry.mode, entry.target);
    if (IsClickHintModeEntryKey(event) && IsASCIIHintKey(entry_char)) {
      pending_char_to_suppress_ = LowerASCIIHintKey(entry_char);
    }
    return WebInputEventResult::kHandledSuppressed;
  }

  if (event.GetType() == WebInputEvent::Type::kChar) {
    if (!AppendTypedCharacter(event)) {
      return WebInputEventResult::kHandledSuppressed;
    }
    return HandleTypedPrefix();
  }

  if (IsEscapeKey(event)) {
    Stop();
    return WebInputEventResult::kHandledSuppressed;
  }

  if (event.windows_key_code == VK_BACK) {
    if (!typed_prefix_.empty()) {
      typed_prefix_ = typed_prefix_.Left(typed_prefix_.length() - 1);
      ScheduleOverlayUpdate();
    }
    return WebInputEventResult::kHandledSuppressed;
  }

  if (!AppendTypedCharacter(event)) {
    return WebInputEventResult::kHandledSuppressed;
  }
  UChar c = CharacterFromEvent(event);
  if (IsASCIIHintKey(c)) {
    pending_char_to_suppress_ = LowerASCIIHintKey(c);
  }

  return HandleTypedPrefix();
}

WebInputEventResult Hints::HandleTypedPrefix() {
  HintCandidate* exact_match = nullptr;
  unsigned visible_matches = 0;
  for (auto& candidate : candidates_) {
    if (!candidate.element || !candidate.label.StartsWith(typed_prefix_)) {
      continue;
    }
    ++visible_matches;
    if (candidate.label == typed_prefix_) {
      exact_match = &candidate;
    }
  }

  if (exact_match && visible_matches == 1) {
    ActivateCandidate(*exact_match);
    return WebInputEventResult::kHandledSuppressed;
  }

  if (!visible_matches) {
    Stop();
    return WebInputEventResult::kHandledSuppressed;
  }

  ScheduleOverlayUpdate();
  return WebInputEventResult::kHandledSuppressed;
}

void Hints::Start(HintMode mode, ActivationTarget target) {
  active_ = true;
  hint_mode_ = mode;
  activation_target_ = target;
  pending_char_to_suppress_ = 0;
  typed_prefix_ = String();
  CollectCandidates();
  AssignLabels();

  if (candidates_.empty()) {
    Stop();
    return;
  }

  if (!frame_overlay_) {
    frame_overlay_ = MakeGarbageCollected<FrameOverlay>(
        GetSupplementable(), std::make_unique<HintOverlayDelegate>(*this));
  }
  ScheduleOverlayUpdate();
}

void Hints::Stop() {
  active_ = false;
  typed_prefix_ = String();
  candidates_.clear();
  if (frame_overlay_) {
    frame_overlay_.Release()->Destroy();
  }
  if (LocalFrame* frame = GetSupplementable()) {
    if (frame->View()) {
      frame->View()->SetVisualViewportOrOverlayNeedsRepaint();
      frame->View()->SetPaintArtifactCompositorNeedsUpdate();
      frame->View()->ScheduleAnimation();
    }
  }
}

void Hints::PaintOverlay(GraphicsContext& context) {
  if (frame_overlay_) {
    frame_overlay_->Paint(context);
  }
}

void Hints::CollectCandidates() {
  candidates_.clear();
  LocalFrame* frame = GetSupplementable();
  if (!frame) {
    return;
  }

  switch (hint_mode_) {
    case HintMode::kClick: {
      const click_hints::CandidateGroup group =
          activation_target_ == ActivationTarget::kRightClick
              ? click_hints::CandidateGroup::kRightClickables
              : click_hints::CandidateGroup::kClickables;
      click_hints::CollectCandidates(*frame, candidates_, group);
      return;
    }
    case HintMode::kHover:
      hover_hints::CollectCandidates(*frame, candidates_);
      return;
    case HintMode::kFocus:
      click_hints::CollectCandidates(*frame, candidates_,
                                     click_hints::CandidateGroup::kScrollables);
      return;
  }
}

void Hints::AssignLabels() {
  const wtf_size_t label_length =
      hint_labels::LabelLengthForCandidateCount(candidates_.size());
  for (wtf_size_t i = 0; i < candidates_.size(); ++i) {
    candidates_[i].label = hint_labels::LabelForIndex(i, label_length);
  }
}

Hints::HintEntry Hints::EntryForKeyEvent(
    const WebKeyboardEvent& event,
    bool is_browser_command) const {
  if (IsClickHintModeEntryKey(event)) {
    return {true, HintMode::kClick, ActivationTargetForClickEntryKey(event)};
  }

  if (IsRightClickHintModeEntryKey(event, is_browser_command)) {
    return {true, HintMode::kClick, ActivationTarget::kRightClick};
  }

  if (IsHoverHintModeEntryKey(event, is_browser_command)) {
    return {true, HintMode::kHover, ActivationTarget::kCurrentTab};
  }

  if (IsScrollableHintModeEntryKey(event, is_browser_command)) {
    return {true, HintMode::kFocus, ActivationTarget::kCurrentTab};
  }

  return {};
}

bool Hints::IsClickHintModeEntryKey(const WebKeyboardEvent& event) const {
  return HasOnlyNoOrShiftModifiers(event) && event.windows_key_code == VK_F;
}

bool Hints::IsRightClickHintModeEntryKey(const WebKeyboardEvent& event,
                                         bool is_browser_command) const {
  return is_browser_command && HasOnlyControlModifier(event) &&
         event.windows_key_code == VK_J;
}

bool Hints::IsHoverHintModeEntryKey(const WebKeyboardEvent& event,
                                    bool is_browser_command) const {
  return is_browser_command && HasOnlyControlModifier(event) &&
         event.windows_key_code == VK_K;
}

bool Hints::IsScrollableHintModeEntryKey(const WebKeyboardEvent& event,
                                         bool is_browser_command) const {
  return is_browser_command && HasOnlyControlModifier(event) &&
         event.windows_key_code == VK_SPACE;
}

Hints::ActivationTarget Hints::ActivationTargetForClickEntryKey(
    const WebKeyboardEvent& event) const {
  return HasNoKeyModifiers(event) ? ActivationTarget::kCurrentTab
                                  : ActivationTarget::kNewTab;
}

bool Hints::ShouldIgnoreEntryKeyForFocusedEditable() const {
  LocalFrame* frame = GetSupplementable();
  if (!frame || !frame->GetDocument()) {
    return true;
  }
  Node* focused = frame->GetDocument()->FocusedElement();
  return focused && IsEditable(*focused);
}

bool Hints::AppendTypedCharacter(const WebKeyboardEvent& event) {
  if (!HasOnlyNoOrShiftModifiers(event)) {
    return false;
  }

  UChar c = CharacterFromEvent(event);
  if (!IsASCIIHintKey(c)) {
    return false;
  }

  c = LowerASCIIHintKey(c);
  StringBuilder builder;
  builder.Append(typed_prefix_);
  builder.Append(c);
  typed_prefix_ = builder.ToString();
  return true;
}

void Hints::ActivateCandidate(HintCandidate& candidate) {
  LocalFrame* frame = GetSupplementable();
  Element* element = candidate.element.Get();
  const gfx::RectF rect = candidate.viewport_rect;
  const HintMode mode = hint_mode_;
  const ActivationTarget target = activation_target_;

  Stop();
  if (!frame || !element) {
    return;
  }

  switch (mode) {
    case HintMode::kClick: {
      click_hints::ActivationAction action =
          click_hints::ActivationAction::kLeftClick;
      if (target == ActivationTarget::kNewTab) {
        action = click_hints::ActivationAction::kLeftClickNewTab;
      } else if (target == ActivationTarget::kRightClick) {
        action = click_hints::ActivationAction::kRightClick;
      }
      click_hints::ActivateCandidate(*frame, *element, rect, action);
      return;
    }
    case HintMode::kHover:
      hover_hints::ActivateCandidate(*frame, *element, rect);
      return;
    case HintMode::kFocus:
      click_hints::ActivateCandidate(*frame, *element, rect,
                                     click_hints::ActivationAction::kFocus);
      return;
  }
}

void Hints::ScheduleOverlayUpdate() {
  if (!frame_overlay_) {
    return;
  }
  frame_overlay_->UpdatePrePaint();
}

void Hints::Trace(Visitor* visitor) const {
  visitor->Trace(candidates_);
  visitor->Trace(frame_overlay_);
  Supplement<LocalFrame>::Trace(visitor);
}

}  // namespace blink
