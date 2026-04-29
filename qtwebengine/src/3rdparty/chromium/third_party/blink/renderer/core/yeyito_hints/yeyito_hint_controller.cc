// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/core/yeyito_hints/yeyito_hint_controller.h"

#include <algorithm>
#include <memory>
#include <string>

#include "base/time/time.h"
#include "third_party/blink/public/common/input/web_keyboard_event.h"
#include "third_party/blink/public/common/input/web_mouse_event.h"
#include "third_party/blink/renderer/core/dom/document.h"
#include "third_party/blink/renderer/core/dom/element_traversal.h"
#include "third_party/blink/renderer/core/editing/editing_utilities.h"
#include "third_party/blink/renderer/core/frame/frame_overlay.h"
#include "third_party/blink/renderer/core/frame/local_frame.h"
#include "third_party/blink/renderer/core/frame/local_frame_view.h"
#include "third_party/blink/renderer/core/frame/visual_viewport.h"
#include "third_party/blink/renderer/core/html/forms/html_button_element.h"
#include "third_party/blink/renderer/core/html/forms/html_input_element.h"
#include "third_party/blink/renderer/core/html/forms/html_select_element.h"
#include "third_party/blink/renderer/core/html/forms/html_text_area_element.h"
#include "third_party/blink/renderer/core/html/html_anchor_element.h"
#include "third_party/blink/renderer/core/html/html_element.h"
#include "third_party/blink/renderer/core/html/html_summary_element.h"
#include "third_party/blink/renderer/core/html_names.h"
#include "third_party/blink/renderer/core/input/event_handler.h"
#include "third_party/blink/renderer/core/page/page.h"
#include "third_party/blink/renderer/core/yeyito_hints/yeyito_hint_overlay.h"
#include "third_party/blink/renderer/platform/windows_keyboard_codes.h"
#include "third_party/blink/renderer/platform/wtf/text/string_builder.h"
#include "ui/events/keycodes/dom/dom_key.h"

namespace blink {

namespace {

constexpr char kHintAlphabet[] = "asdfghjklqwertyuiopzxcvbnm";

bool HasNoKeyModifiers(const WebKeyboardEvent& event) {
  return (event.GetModifiers() & WebKeyboardEvent::kKeyModifiers) == 0;
}

bool HasOnlyNoOrShiftModifiers(const WebKeyboardEvent& event) {
  return (event.GetModifiers() &
          (WebInputEvent::kControlKey | WebInputEvent::kAltKey |
           WebInputEvent::kMetaKey)) == 0;
}

bool IsVisibleRect(const gfx::RectF& rect, const gfx::Size& viewport_size) {
  if (rect.IsEmpty()) {
    return false;
  }
  gfx::RectF viewport_rect(0, 0, viewport_size.width(), viewport_size.height());
  return rect.Intersects(viewport_rect);
}

wtf_size_t LabelLengthForCandidateCount(wtf_size_t count) {
  constexpr wtf_size_t kAlphabetSize = sizeof(kHintAlphabet) - 1;
  wtf_size_t length = 1;
  wtf_size_t capacity = kAlphabetSize;
  while (count > capacity) {
    ++length;
    capacity *= kAlphabetSize;
  }
  return length;
}

String LabelForIndex(wtf_size_t index, wtf_size_t length) {
  constexpr wtf_size_t kAlphabetSize = sizeof(kHintAlphabet) - 1;
  std::string label(length, kHintAlphabet[0]);
  for (wtf_size_t i = 0; i < length; ++i) {
    label[length - i - 1] = kHintAlphabet[index % kAlphabetSize];
    index = index / kAlphabetSize;
  }
  return String::FromUTF8(label);
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

const char YeyitoHintController::kSupplementName[] = "YeyitoHintController";

YeyitoHintController& YeyitoHintController::From(LocalFrame& frame) {
  auto* controller = Supplement<LocalFrame>::From<YeyitoHintController>(frame);
  if (!controller) {
    controller = MakeGarbageCollected<YeyitoHintController>(frame);
    Supplement<LocalFrame>::ProvideTo(frame, controller);
  }
  return *controller;
}

YeyitoHintController::YeyitoHintController(LocalFrame& frame)
    : Supplement<LocalFrame>(frame) {}

void YeyitoHintController::Dispose() {
  Stop();
}

WebInputEventResult YeyitoHintController::HandleKeyEvent(
    const WebKeyboardEvent& event) {
  if (event.GetType() != WebInputEvent::Type::kRawKeyDown &&
      event.GetType() != WebInputEvent::Type::kKeyDown &&
      event.GetType() != WebInputEvent::Type::kChar) {
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
    if (!IsHintModeEntryKey(event) ||
        ShouldIgnoreEntryKeyForFocusedEditable()) {
      return WebInputEventResult::kNotHandled;
    }
    Start();
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

WebInputEventResult YeyitoHintController::HandleTypedPrefix() {
  YeyitoHintCandidate* exact_match = nullptr;
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

void YeyitoHintController::Start() {
  active_ = true;
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
        GetSupplementable(),
        std::make_unique<YeyitoHintOverlayDelegate>(*this));
  }
  ScheduleOverlayUpdate();
}

void YeyitoHintController::Stop() {
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

void YeyitoHintController::PaintOverlay(GraphicsContext& context) {
  if (frame_overlay_) {
    frame_overlay_->Paint(context);
  }
}

void YeyitoHintController::CollectCandidates() {
  candidates_.clear();
  LocalFrame* frame = GetSupplementable();
  if (!frame || !frame->GetDocument() || !frame->View() || !frame->GetPage()) {
    return;
  }

  Document* document = frame->GetDocument();
  document->UpdateStyleAndLayout(DocumentUpdateReason::kInput);

  Element* root = document->documentElement();
  if (!root) {
    return;
  }

  const gfx::Size viewport_size = frame->GetPage()->GetVisualViewport().Size();
  for (Element& element : ElementTraversal::InclusiveDescendantsOf(*root)) {
    if (!IsCandidateElement(element)) {
      continue;
    }
    gfx::RectF rect(element.VisibleBoundsInLocalRoot());
    if (!IsVisibleRect(rect, viewport_size)) {
      continue;
    }

    YeyitoHintCandidate candidate;
    candidate.element = &element;
    candidate.viewport_rect = rect;
    candidates_.push_back(candidate);
  }
}

void YeyitoHintController::AssignLabels() {
  const wtf_size_t label_length =
      LabelLengthForCandidateCount(candidates_.size());
  for (wtf_size_t i = 0; i < candidates_.size(); ++i) {
    candidates_[i].label = LabelForIndex(i, label_length);
  }
}

bool YeyitoHintController::IsCandidateElement(Element& element) const {
  if (!element.GetLayoutObject() || element.IsDisabledFormControl()) {
    return false;
  }

  if (IsA<HTMLAnchorElement>(element) &&
      element.FastHasAttribute(html_names::kHrefAttr)) {
    return true;
  }
  if (IsA<HTMLButtonElement>(element) || IsA<HTMLInputElement>(element) ||
      IsA<HTMLTextAreaElement>(element) || IsA<HTMLSelectElement>(element) ||
      IsA<HTMLSummaryElement>(element)) {
    return true;
  }

  if (element.FastHasAttribute(html_names::kOnclickAttr)) {
    return true;
  }

  const AtomicString& role = element.FastGetAttribute(html_names::kRoleAttr);
  if (EqualIgnoringASCIICase(role, "button") ||
      EqualIgnoringASCIICase(role, "link") ||
      EqualIgnoringASCIICase(role, "menuitem") ||
      EqualIgnoringASCIICase(role, "tab")) {
    return true;
  }

  if (element.IsMouseFocusable(
          Element::UpdateBehavior::kAssertNoLayoutUpdates)) {
    return true;
  }

  return false;
}

bool YeyitoHintController::IsHintModeEntryKey(
    const WebKeyboardEvent& event) const {
  return HasNoKeyModifiers(event) && event.windows_key_code == VK_F;
}

bool YeyitoHintController::ShouldIgnoreEntryKeyForFocusedEditable() const {
  LocalFrame* frame = GetSupplementable();
  if (!frame || !frame->GetDocument()) {
    return true;
  }
  Node* focused = frame->GetDocument()->FocusedElement();
  return focused && IsEditable(*focused);
}

bool YeyitoHintController::AppendTypedCharacter(const WebKeyboardEvent& event) {
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

void YeyitoHintController::ActivateCandidate(YeyitoHintCandidate& candidate) {
  Element* element = candidate.element.Get();
  gfx::RectF rect = candidate.viewport_rect;
  Stop();

  if (!element || !GetSupplementable()) {
    return;
  }

  const gfx::PointF center(rect.x() + rect.width() / 2.0f,
                           rect.y() + rect.height() / 2.0f);

  WebMouseEvent mouse_down(WebInputEvent::Type::kMouseDown,
                           WebInputEvent::kNoModifiers, base::TimeTicks::Now());
  mouse_down.button = WebPointerProperties::Button::kLeft;
  mouse_down.click_count = 1;
  mouse_down.SetPositionInWidget(center);
  mouse_down.SetPositionInScreen(center);
  mouse_down.UpdateEventModifiersToMatchButton();

  WebMouseEvent mouse_up(WebInputEvent::Type::kMouseUp,
                         WebInputEvent::kNoModifiers, base::TimeTicks::Now());
  mouse_up.button = WebPointerProperties::Button::kLeft;
  mouse_up.click_count = 1;
  mouse_up.SetPositionInWidget(center);
  mouse_up.SetPositionInScreen(center);

  LocalFrame* frame = element->GetDocument().GetFrame();
  if (!frame) {
    return;
  }
  frame->GetEventHandler().HandleMousePressEvent(mouse_down);
  frame->GetEventHandler().HandleMouseReleaseEvent(mouse_up);
}

void YeyitoHintController::ScheduleOverlayUpdate() {
  if (!frame_overlay_) {
    return;
  }
  frame_overlay_->UpdatePrePaint();
}

void YeyitoHintController::Trace(Visitor* visitor) const {
  visitor->Trace(candidates_);
  visitor->Trace(frame_overlay_);
  Supplement<LocalFrame>::Trace(visitor);
}

}  // namespace blink
