// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/core/yeyito_hints/click_hints.h"

#include "base/time/time.h"
#include "build/build_config.h"
#include "third_party/blink/public/common/input/web_mouse_event.h"
#include "third_party/blink/renderer/core/dom/document.h"
#include "third_party/blink/renderer/core/dom/element_traversal.h"
#include "third_party/blink/renderer/core/dom/flat_tree_traversal.h"
#include "third_party/blink/renderer/core/dom/shadow_root.h"
#include "third_party/blink/renderer/core/editing/editing_utilities.h"
#include "third_party/blink/renderer/core/event_type_names.h"
#include "third_party/blink/renderer/core/events/ui_event_with_key_state.h"
#include "third_party/blink/renderer/core/frame/local_frame.h"
#include "third_party/blink/renderer/core/frame/local_frame_view.h"
#include "third_party/blink/renderer/core/frame/visual_viewport.h"
#include "third_party/blink/renderer/core/html/html_anchor_element.h"
#include "third_party/blink/renderer/core/html_names.h"
#include "third_party/blink/renderer/core/input/context_menu_allowed_scope.h"
#include "third_party/blink/renderer/core/input/event_handler.h"
#include "third_party/blink/renderer/core/page/page.h"
#include "third_party/blink/renderer/core/style/computed_style.h"

namespace blink::click_hints {

namespace {

constexpr float kMinimumHintSize = 4.0f;

AtomicString ConfiguredSelector(CandidateGroup group) {
  switch (group) {
    case CandidateGroup::kAll:
      // Port of qutebrowser's default hints.selectors['all'].  The old
      // qutebrowser-only :qb-click marker is represented by the native
      // HasClickBehavior() check below.
      return AtomicString(
          "a, area, textarea, select, input:not([type=\"hidden\"]), button, "
          "frame, iframe, img, link, summary, "
          "[contenteditable]:not([contenteditable=\"false\"]), [onclick], "
          "[onmousedown], [role=\"link\"], [role=\"option\"], "
          "[role=\"button\"], [role=\"tab\"], [role=\"checkbox\"], "
          "[role=\"switch\"], [role=\"menuitem\"], "
          "[role=\"menuitemcheckbox\"], [role=\"menuitemradio\"], "
          "[role=\"treeitem\"], [aria-haspopup], [ng-click], [ngClick], "
          "[data-ng-click], [x-ng-click], [tabindex]:not([tabindex=\"-1\"])");
    case CandidateGroup::kLinks:
      return AtomicString("a[href], area[href], link[href], [role=\"link\"][href]");
    case CandidateGroup::kImages:
      return AtomicString("img");
    case CandidateGroup::kMedia:
      return AtomicString("audio, img, video");
    case CandidateGroup::kUrl:
      return AtomicString("[src], [href]");
    case CandidateGroup::kInputs:
      return AtomicString(
          "input[type=\"text\"], input[type=\"date\"], "
          "input[type=\"datetime-local\"], input[type=\"email\"], "
          "input[type=\"month\"], input[type=\"number\"], "
          "input[type=\"password\"], input[type=\"search\"], "
          "input[type=\"tel\"], input[type=\"time\"], "
          "input[type=\"url\"], input[type=\"week\"], input:not([type]), "
          "[contenteditable]:not([contenteditable=\"false\"]), textarea");
    case CandidateGroup::kHoverables:
      // Port of hints.selectors['hoverables'] without the old :qb-hover marker;
      // native event-listener checks cover JS hover listeners below.
      return AtomicString(
          "[title], [data-tooltip], [data-tip], [aria-describedby], "
          "[onmouseover], [onmouseenter], [onmousemove], [role=\"article\"], "
          "[aria-roledescription], abbr, acronym, iframe, frame, video");
    case CandidateGroup::kRightClickables:
      return AtomicString(
          "a, area, link, img, audio, video, frame, iframe, textarea, "
          "input:not([type=\"hidden\"]), "
          "[contenteditable]:not([contenteditable=\"false\"]), "
          "[oncontextmenu], [onauxclick], [onmousedown], [onmouseup], "
          "[onpointerdown], [onpointerup], [data-context-menu], "
          "[data-contextmenu]");
    case CandidateGroup::kScrollables:
      return AtomicString();
  }
  return AtomicString();
}

bool IsVisibleRect(const gfx::RectF& rect, const gfx::Size& viewport_size) {
  if (rect.IsEmpty() || rect.width() < kMinimumHintSize ||
      rect.height() < kMinimumHintSize) {
    return false;
  }
  gfx::RectF viewport_rect(0, 0, viewport_size.width(), viewport_size.height());
  return rect.Intersects(viewport_rect);
}

bool AllowsZeroOpacity(Element& element) {
  return element.HasClassName(AtomicString("ace_text-input")) ||
         element.HasClassName(AtomicString("custom-control-input"));
}

bool HasVisibleStyle(Element& element) {
  for (Node& ancestor : FlatTreeTraversal::InclusiveAncestorsOf(element)) {
    auto* ancestor_element = DynamicTo<Element>(ancestor);
    if (!ancestor_element) {
      continue;
    }

    const ComputedStyle* style = ancestor_element->GetComputedStyle();
    if (!style) {
      return false;
    }
    if (style->Visibility() != EVisibility::kVisible) {
      return false;
    }
    if (style->Opacity() == 0.0f &&
        (ancestor_element != &element || !AllowsZeroOpacity(element))) {
      return false;
    }
  }
  return true;
}

bool HasClickBehavior(Element& element) {
  return element.HasEventListeners(event_type_names::kClick) ||
         element.HasEventListeners(event_type_names::kAuxclick) ||
         element.HasEventListeners(event_type_names::kDblclick) ||
         element.HasEventListeners(event_type_names::kMousedown) ||
         element.HasEventListeners(event_type_names::kMouseup) ||
         element.HasEventListeners(event_type_names::kPointerdown) ||
         element.HasEventListeners(event_type_names::kPointerup) ||
         element.HasEventListeners(event_type_names::kTouchstart) ||
         element.HasEventListeners(event_type_names::kTouchend);
}

bool HasHoverBehavior(Element& element) {
  return element.HasEventListeners(event_type_names::kMouseenter) ||
         element.HasEventListeners(event_type_names::kMouseover) ||
         element.HasEventListeners(event_type_names::kMousemove) ||
         element.HasEventListeners(event_type_names::kPointerenter) ||
         element.HasEventListeners(event_type_names::kPointerover) ||
         element.HasEventListeners(event_type_names::kPointermove);
}

bool HasRightClickBehavior(Element& element) {
  return element.HasEventListeners(event_type_names::kContextmenu) ||
         element.HasEventListeners(event_type_names::kAuxclick) ||
         element.HasEventListeners(event_type_names::kMousedown) ||
         element.HasEventListeners(event_type_names::kMouseup) ||
         element.HasEventListeners(event_type_names::kPointerdown) ||
         element.HasEventListeners(event_type_names::kPointerup);
}

bool IsDocumentScroller(Element& element) {
  Document& document = element.GetDocument();
  return document.ScrollingElementNoLayout() == &element ||
         document.documentElement() == &element || document.body() == &element;
}

bool OverflowBlocksScrolling(EOverflow overflow) {
  return overflow == EOverflow::kHidden || overflow == EOverflow::kClip;
}

bool IsScrollableElement(Element& element) {
  const bool has_vertical_range = element.scrollHeight() > element.clientHeight();
  const bool has_horizontal_range = element.scrollWidth() > element.clientWidth();
  if (!has_vertical_range && !has_horizontal_range) {
    return false;
  }

  const ComputedStyle* style = element.GetComputedStyle();
  if (!style) {
    return false;
  }

  if (IsDocumentScroller(element)) {
    return (has_vertical_range && !OverflowBlocksScrolling(style->OverflowY())) ||
           (has_horizontal_range && !OverflowBlocksScrolling(style->OverflowX()));
  }

  return ComputedStyle::ScrollsOverflow(style->OverflowY()) ||
         ComputedStyle::ScrollsOverflow(style->OverflowX());
}

bool MatchesCandidateGroup(Element& element,
                           CandidateGroup group,
                           const AtomicString& selector) {
  if (group == CandidateGroup::kScrollables) {
    return IsScrollableElement(element);
  }

  if (!selector.IsNull() && element.matches(selector)) {
    return true;
  }

  switch (group) {
    case CandidateGroup::kAll:
      return HasClickBehavior(element);
    case CandidateGroup::kHoverables:
      return HasHoverBehavior(element);
    case CandidateGroup::kRightClickables:
      return HasRightClickBehavior(element);
    case CandidateGroup::kLinks:
    case CandidateGroup::kImages:
    case CandidateGroup::kMedia:
    case CandidateGroup::kUrl:
    case CandidateGroup::kInputs:
    case CandidateGroup::kScrollables:
      return false;
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

void MaybeAppendCandidate(Element& element,
                          const gfx::Size& viewport_size,
                          CandidateGroup group,
                          const AtomicString& selector,
                          HeapVector<HintCandidate>& candidates) {
  if (!MatchesCandidateGroup(element, group, selector) ||
      !HasVisibleStyle(element)) {
    return;
  }

  gfx::RectF rect = CandidateRect(element, viewport_size);
  if (rect.IsEmpty()) {
    return;
  }

  HintCandidate candidate;
  candidate.element = &element;
  candidate.viewport_rect = rect;
  candidates.push_back(candidate);
}

void CollectContainerDescendants(ContainerNode& root,
                                 const gfx::Size& viewport_size,
                                 CandidateGroup group,
                                 const AtomicString& selector,
                                 HeapVector<HintCandidate>& candidates);

void CollectElementAndShadowTrees(Element& element,
                                  const gfx::Size& viewport_size,
                                  CandidateGroup group,
                                  const AtomicString& selector,
                                  HeapVector<HintCandidate>& candidates) {
  MaybeAppendCandidate(element, viewport_size, group, selector, candidates);
  if (ShadowRoot* shadow_root = element.GetShadowRoot()) {
    CollectContainerDescendants(*shadow_root, viewport_size, group, selector,
                                candidates);
  }
}

void CollectContainerDescendants(ContainerNode& root,
                                 const gfx::Size& viewport_size,
                                 CandidateGroup group,
                                 const AtomicString& selector,
                                 HeapVector<HintCandidate>& candidates) {
  for (Element& element : ElementTraversal::DescendantsOf(root)) {
    CollectElementAndShadowTrees(element, viewport_size, group, selector,
                                 candidates);
  }
}

}  // namespace

void CollectCandidates(LocalFrame& frame,
                       HeapVector<HintCandidate>& candidates,
                       CandidateGroup group) {
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

  const AtomicString selector = ConfiguredSelector(group);
  const gfx::Size viewport_size = frame.GetPage()->GetVisualViewport().Size();
  for (Element& element : ElementTraversal::InclusiveDescendantsOf(*root)) {
    CollectElementAndShadowTrees(element, viewport_size, group, selector,
                                 candidates);
  }
}

void ActivateCandidate(LocalFrame&,
                       Element& element,
                       const gfx::RectF& rect,
                       ActivationAction action) {
  const gfx::PointF center(rect.x() + rect.width() / 2.0f,
                           rect.y() + rect.height() / 2.0f);
  const bool open_in_new_tab = action == ActivationAction::kLeftClickNewTab;
  const bool right_click = action == ActivationAction::kRightClick;

  LocalFrame* frame = element.GetDocument().GetFrame();
  if (!frame) {
    return;
  }

  if (action == ActivationAction::kFocus) {
    element.Focus();
    return;
  }

  if (action == ActivationAction::kHover) {
    WebMouseEvent mouse_move(WebInputEvent::Type::kMouseMove,
                             WebInputEvent::kNoModifiers,
                             base::TimeTicks::Now());
    mouse_move.button = WebPointerProperties::Button::kNoButton;
    mouse_move.SetPositionInWidget(center);
    mouse_move.SetPositionInScreen(center);
    frame->GetEventHandler().HandleMouseMoveEvent(mouse_move,
                                                  Vector<WebMouseEvent>(),
                                                  Vector<WebMouseEvent>());
    return;
  }

#if BUILDFLAG(IS_MAC)
  constexpr int kNewTabModifier = WebInputEvent::kMetaKey;
  constexpr bool kNewTabCtrlKey = false;
  constexpr bool kNewTabMetaKey = true;
#else
  constexpr int kNewTabModifier = WebInputEvent::kControlKey;
  constexpr bool kNewTabCtrlKey = true;
  constexpr bool kNewTabMetaKey = false;
#endif
  const int click_modifiers = open_in_new_tab ? kNewTabModifier
                                              : WebInputEvent::kNoModifiers;
  const WebPointerProperties::Button button =
      right_click ? WebPointerProperties::Button::kRight
                  : WebPointerProperties::Button::kLeft;

  WebMouseEvent mouse_down(WebInputEvent::Type::kMouseDown, click_modifiers,
                           base::TimeTicks::Now());
  mouse_down.button = button;
  mouse_down.click_count = 1;
  mouse_down.SetPositionInWidget(center);
  mouse_down.SetPositionInScreen(center);
  mouse_down.UpdateEventModifiersToMatchButton();

  WebMouseEvent mouse_up(WebInputEvent::Type::kMouseUp, click_modifiers,
                         base::TimeTicks::Now());
  mouse_up.button = button;
  mouse_up.click_count = 1;
  mouse_up.SetPositionInWidget(center);
  mouse_up.SetPositionInScreen(center);

  if (open_in_new_tab) {
    // Chromium normally upgrades synthetic background-tab clicks to foreground
    // tabs to prevent page-created tab-unders. Native hint activation is a
    // browser command, not page script, so mark the new-tab modifier as coming
    // from a trusted/isolated source and let QtWebEngine/qutebrowser apply the
    // normal tabs.background policy.
    UIEventWithKeyState::DidCreateEventInIsolatedWorld(
        kNewTabCtrlKey, false, false, kNewTabMetaKey);
  }
  frame->GetEventHandler().HandleMousePressEvent(mouse_down);
  if (right_click) {
    mouse_down.menu_source_type = kMenuSourceMouse;
    ContextMenuAllowedScope scope;
    frame->GetEventHandler().SendContextMenuEvent(mouse_down, &element);
  }
  frame->GetEventHandler().HandleMouseReleaseEvent(mouse_up);
  if (open_in_new_tab) {
    UIEventWithKeyState::ClearNewTabModifierSetFromIsolatedWorld();
  }
}

}  // namespace blink::click_hints
