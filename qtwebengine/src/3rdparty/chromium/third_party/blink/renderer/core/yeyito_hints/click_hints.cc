// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/core/yeyito_hints/click_hints.h"

#include "base/time/time.h"
#include "third_party/blink/public/common/input/web_mouse_event.h"
#include "third_party/blink/renderer/core/dom/document.h"
#include "third_party/blink/renderer/core/dom/element_traversal.h"
#include "third_party/blink/renderer/core/frame/local_frame.h"
#include "third_party/blink/renderer/core/frame/local_frame_view.h"
#include "third_party/blink/renderer/core/frame/visual_viewport.h"
#include "third_party/blink/renderer/core/html/forms/html_button_element.h"
#include "third_party/blink/renderer/core/html/forms/html_input_element.h"
#include "third_party/blink/renderer/core/html/forms/html_select_element.h"
#include "third_party/blink/renderer/core/html/forms/html_text_area_element.h"
#include "third_party/blink/renderer/core/html/html_anchor_element.h"
#include "third_party/blink/renderer/core/html/html_summary_element.h"
#include "third_party/blink/renderer/core/html_names.h"
#include "third_party/blink/renderer/core/input/event_handler.h"
#include "third_party/blink/renderer/core/page/page.h"

namespace blink::click_hints {

namespace {

bool IsVisibleRect(const gfx::RectF& rect, const gfx::Size& viewport_size) {
  if (rect.IsEmpty()) {
    return false;
  }
  gfx::RectF viewport_rect(0, 0, viewport_size.width(), viewport_size.height());
  return rect.Intersects(viewport_rect);
}

bool IsCandidateElement(Element& element) {
  if (element.IsDisabledFormControl()) {
    return false;
  }

  // Some modern pages, including Google search result titles, use anchors whose
  // own layout box is not the best visible click target (and can occasionally
  // be absent, e.g. display: contents). Treat href anchors as candidates first,
  // then choose a better descendant rect below.
  if (IsA<HTMLAnchorElement>(element) &&
      element.FastHasAttribute(html_names::kHrefAttr)) {
    return true;
  }

  if (!element.GetLayoutObject()) {
    return false;
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

float RectArea(const gfx::RectF& rect) {
  return rect.width() * rect.height();
}

bool IsHeadingElement(const Element& element) {
  return element.HasTagName(html_names::kH1Tag) ||
         element.HasTagName(html_names::kH2Tag) ||
         element.HasTagName(html_names::kH3Tag) ||
         element.HasTagName(html_names::kH4Tag) ||
         element.HasTagName(html_names::kH5Tag) ||
         element.HasTagName(html_names::kH6Tag);
}

gfx::RectF FirstVisibleHeadingRect(Element& anchor,
                                   const gfx::Size& viewport_size) {
  for (Element& descendant : ElementTraversal::DescendantsOf(anchor)) {
    if (!IsHeadingElement(descendant)) {
      continue;
    }

    gfx::RectF rect(descendant.VisibleBoundsInLocalRoot());
    if (IsVisibleRect(rect, viewport_size)) {
      return rect;
    }
  }
  return gfx::RectF();
}

gfx::RectF LargestVisibleDescendantRect(Element& element,
                                        const gfx::Size& viewport_size) {
  gfx::RectF best_rect;
  float best_area = 0.0f;
  for (Element& descendant : ElementTraversal::DescendantsOf(element)) {
    if (!descendant.GetLayoutObject()) {
      continue;
    }

    gfx::RectF rect(descendant.VisibleBoundsInLocalRoot());
    if (!IsVisibleRect(rect, viewport_size)) {
      continue;
    }

    const float area = RectArea(rect);
    if (area > best_area) {
      best_area = area;
      best_rect = rect;
    }
  }
  return best_rect;
}

gfx::RectF CandidateRect(Element& element, const gfx::Size& viewport_size) {
  if (IsA<HTMLAnchorElement>(element)) {
    gfx::RectF heading_rect = FirstVisibleHeadingRect(element, viewport_size);
    if (!heading_rect.IsEmpty()) {
      return heading_rect;
    }
  }

  gfx::RectF rect(element.VisibleBoundsInLocalRoot());
  if (IsVisibleRect(rect, viewport_size)) {
    return rect;
  }

  if (IsA<HTMLAnchorElement>(element)) {
    return LargestVisibleDescendantRect(element, viewport_size);
  }

  return gfx::RectF();
}

}  // namespace

void CollectCandidates(LocalFrame& frame,
                       HeapVector<HintCandidate>& candidates) {
  candidates.clear();
  if (!frame.GetDocument() || !frame.View() || !frame.GetPage()) {
    return;
  }

  Document* document = frame.GetDocument();
  document->UpdateStyleAndLayout(DocumentUpdateReason::kInput);

  Element* root = document->documentElement();
  if (!root) {
    return;
  }

  const gfx::Size viewport_size = frame.GetPage()->GetVisualViewport().Size();
  for (Element& element : ElementTraversal::InclusiveDescendantsOf(*root)) {
    if (!IsCandidateElement(element)) {
      continue;
    }
    gfx::RectF rect = CandidateRect(element, viewport_size);
    if (rect.IsEmpty()) {
      continue;
    }

    HintCandidate candidate;
    candidate.element = &element;
    candidate.viewport_rect = rect;
    candidates.push_back(candidate);
  }
}

void ActivateCandidate(LocalFrame&, Element& element, const gfx::RectF& rect) {
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

  LocalFrame* frame = element.GetDocument().GetFrame();
  if (!frame) {
    return;
  }
  frame->GetEventHandler().HandleMousePressEvent(mouse_down);
  frame->GetEventHandler().HandleMouseReleaseEvent(mouse_up);
}

}  // namespace blink::click_hints
