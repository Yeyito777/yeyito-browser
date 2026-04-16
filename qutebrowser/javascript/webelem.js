// SPDX-FileCopyrightText: Florian Bruhin (The Compiler) <mail@qutebrowser.org>
//
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * The connection for web elements between Python and Javascript works like
 * this:
 *
 * - Python calls into Javascript and invokes a function to find elements (one
 *   of the find_* functions).
 * - Javascript gets the requested element, and calls serialize_elem on it.
 * - serialize_elem saves the javascript element object in "elements", gets some
 *   attributes from the element, and assigns an ID (index into 'elements') to
 *   it.
 * - Python gets this information and constructs a Python wrapper object with
 *   the information it got right away, and the ID.
 * - When Python wants to modify an element, it calls javascript again with the
 *   element ID.
 * - Javascript gets the element from the elements array, and modifies it.
 */

"use strict";

window._qutebrowser.webelem = (function() {
    const funcs = {};
    const elements = [];

    function get_frame_offset(frame) {
        if (frame === null) {
            // Dummy object with zero offset
            return {
                "top": 0,
                "right": 0,
                "bottom": 0,
                "left": 0,
                "height": 0,
                "width": 0,
            };
        }
        return frame.frameElement.getBoundingClientRect();
    }

    // Add an offset rect to a base rect, for use with frames
    function add_offset_rect(base, offset) {
        return {
            "top": base.top + offset.top,
            "left": base.left + offset.left,
            "bottom": base.bottom + offset.top,
            "right": base.right + offset.left,
            "height": base.height,
            "width": base.width,
        };
    }

    // Lightweight mode skips expensive serialization (outerHTML, textContent, attributes iteration)
    // Used for hint operations where we only need id + rects for positioning
    function serialize_elem(elem, frame = null, lightweight = false) {
        if (!elem) {
            return null;
        }

        const id = elements.length;
        elements[id] = elem;

        const out = {
            "id": id,
            "rects": [],  // Gets filled up later
            // Always include these with defaults (Python expects them)
            "caret_position": null,
            "is_content_editable": false,
            "class_name": "",
            "value": "",
            "outer_xml": "",
            "attributes": {},
        };

        // Always include tag_name (cheap and useful)
        if (typeof elem.tagName === "string") {
            out.tag_name = elem.tagName;
        } else if (typeof elem.nodeName === "string") {
            out.tag_name = elem.nodeName;
        } else {
            out.tag_name = "";
        }

        // Skip expensive operations in lightweight mode
        if (!lightweight) {
            out.caret_position = elem.selectionStart;
            out.is_content_editable = elem.isContentEditable || false;

            if (typeof elem.className === "string") {
                out.class_name = elem.className;
            }

            if (typeof elem.value === "string" || typeof elem.value === "number") {
                out.value = elem.value;
            }

            if (typeof elem.outerHTML === "string") {
                out.outer_xml = elem.outerHTML;
            }

            if (typeof elem.textContent === "string") {
                out.text = elem.textContent;
            } else if (typeof elem.text === "string") {
                out.text = elem.text;
            }

            const attributes = {};
            for (let i = 0; i < elem.attributes.length; ++i) {
                const attr = elem.attributes[i];
                attributes[attr.name] = attr.value;
            }
            out.attributes = attributes;
        }

        const client_rects = elem.getClientRects();
        const frame_offset_rect = get_frame_offset(frame);

        for (let k = 0; k < client_rects.length; ++k) {
            const rect = client_rects[k];
            out.rects.push(
                add_offset_rect(rect, frame_offset_rect)
            );
        }

        return out;
    }

    function is_hidden_css(elem) {
        // Check if the element is hidden via CSS
        const win = elem.ownerDocument.defaultView;
        const style = win.getComputedStyle(elem, null);

        const invisible = style.getPropertyValue("visibility") !== "visible";
        const none_display = style.getPropertyValue("display") === "none";
        const zero_opacity = style.getPropertyValue("opacity") === "0";

        const is_framework = (
            // ACE editor
            elem.classList.contains("ace_text-input") ||
            // bootstrap CSS
            elem.classList.contains("custom-control-input")
        );

        if (invisible || none_display || (zero_opacity && !is_framework)) {
            return true;
        }

        // Check if any ancestor has opacity: 0 (e.g., Discord's hover widgets)
        let parent = elem.parentElement;
        while (parent && parent !== document.body) {
            const parentStyle = win.getComputedStyle(parent, null);
            if (parentStyle.getPropertyValue("opacity") === "0") {
                return true;
            }
            parent = parent.parentElement;
        }

        return false;
    }

    function is_visible(elem, frame = null) {
        // Adopted from vimperator:
        // https://github.com/vimperator/vimperator-labs/blob/vimperator-3.14.0/common/content/hints.js#L259-L285
        // FIXME:qtwebengine we might need something more sophisticated like
        // the cVim implementation here?
        // https://github.com/1995eaton/chromium-vim/blob/1.2.85/content_scripts/dom.js#L74-L134

        if (is_hidden_css(elem)) {
            return false;
        }

        const offset_rect = get_frame_offset(frame);
        let rect = add_offset_rect(elem.getBoundingClientRect(), offset_rect);

        if (!rect ||
                rect.top > window.innerHeight ||
                rect.bottom < 0 ||
                rect.left > window.innerWidth ||
                rect.right < 0) {
            return false;
        }

        // Skip elements that are too small to be useful (e.g., 1px wide hidden elements)
        const MIN_SIZE = 4;
        if (rect.width < MIN_SIZE || rect.height < MIN_SIZE) {
            return false;
        }

        rect = elem.getClientRects()[0];
        return Boolean(rect);
    }

    // Returns true if the iframe is accessible without
    // cross domain errors, else false.
    function iframe_same_domain(frame) {
        try {
            frame.document; // eslint-disable-line no-unused-expressions
            return true;
        } catch (exc) {
            if (exc instanceof DOMException && exc.name === "SecurityError") {
                // FIXME:qtwebengine This does not work for cross-origin frames.
                return false;
            }
            throw exc;
        }
    }

    // Find elements that have CSS :hover rules defined in stylesheets
    // Returns an array of [element, frame] pairs
    function find_elements_with_css_hover(containers) {
        const uniqueSelectors = new Set();  // Collect unique base selectors first

        // Trivial CSS properties that don't indicate meaningful hover interactions
        const trivialProperties = new Set([
            "cursor",
            "outline", "outline-color", "outline-style", "outline-width", "outline-offset",
            "text-decoration", "text-decoration-color", "text-decoration-line",
            "text-decoration-style", "text-decoration-thickness",
            "-webkit-text-decoration", "-moz-text-decoration",
        ]);

        // Check if a CSS rule only modifies trivial properties
        function hasOnlyTrivialProperties(rule) {
            const style = rule.style;
            if (!style || style.length === 0) {
                return true;  // Empty rule is trivial
            }

            for (let i = 0; i < style.length; i++) {
                const prop = style[i];
                if (!trivialProperties.has(prop)) {
                    return false;  // Found a non-trivial property
                }
            }
            return true;  // All properties are trivial
        }

        // Helper to extract base selectors from a :hover selector
        // e.g., ".message:hover" -> ".message"
        // e.g., ".card:hover .icon" -> ".card"
        function extractBaseSelectors(selectorText) {
            const selectors = selectorText.split(",");
            for (const sel of selectors) {
                if (sel.includes(":hover")) {
                    const hoverIndex = sel.indexOf(":hover");
                    const basePart = sel.substring(0, hoverIndex).trim();
                    if (basePart) {
                        uniqueSelectors.add(basePart);
                    }
                }
            }
        }

        // Helper to process a CSS rule (just collects selectors, no DOM queries)
        function processRule(rule) {
            if (rule.type === CSSRule.STYLE_RULE && rule.selectorText) {
                if (rule.selectorText.includes(":hover")) {
                    // Skip rules that only change trivial properties
                    if (!hasOnlyTrivialProperties(rule)) {
                        extractBaseSelectors(rule.selectorText);
                    }
                }
            }
            // Handle nested rules (media queries, supports, etc.)
            else if (rule.cssRules) {
                for (const nestedRule of rule.cssRules) {
                    processRule(nestedRule);
                }
            }
        }

        // Phase 1: Scan all stylesheets and collect unique selectors
        for (const sheet of document.styleSheets) {
            let rules;
            try {
                rules = sheet.cssRules || sheet.rules;
            } catch (e) {
                // Cross-origin stylesheet, skip it
                continue;
            }

            if (!rules) {
                continue;
            }

            for (const rule of rules) {
                processRule(rule);
            }
        }

        // Phase 2: Query DOM once per unique selector
        const elemSet = new Set();
        const candidates = [];

        for (const selector of uniqueSelectors) {
            for (const [container, frame] of containers) {
                try {
                    for (const elem of container.querySelectorAll(selector)) {
                        if (!elemSet.has(elem)) {
                            elemSet.add(elem);
                            candidates.push([elem, frame]);
                        }
                    }
                } catch (e) {
                    // Invalid selector, skip
                }
            }
        }

        // Phase 3: Filter to elements that have hidden clickable children
        // Only apply this aggressive filter on pages with many candidates (e.g., Discord)
        // For simpler pages (e.g., Claude.ai), keep all candidates
        const CANDIDATE_THRESHOLD = 200;

        if (candidates.length <= CANDIDATE_THRESHOLD) {
            // Simple page: return all candidates without filtering
            return candidates;
        }

        // Complex page: filter to elements with hidden clickable children
        // (these are likely "actionable" hovers that reveal buttons/actions)
        const clickableSelector = [
            "a", "button",
            "[onclick]", "[onmousedown]",
            "[role='button']", "[role='link']", "[role='menuitem']",
            "[role='menuitemcheckbox']", "[role='menuitemradio']",
            "[tabindex]:not([tabindex='-1'])",
        ].join(", ");

        function hasHiddenClickableChild(elem) {
            const clickables = elem.querySelectorAll(clickableSelector);
            for (const child of clickables) {
                const style = window.getComputedStyle(child);
                const isHidden = (
                    style.visibility === "hidden" ||
                    style.display === "none" ||
                    style.opacity === "0"
                );
                if (isHidden) {
                    return true;
                }
            }
            return false;
        }

        const result = [];
        for (const [elem, frame] of candidates) {
            if (hasHiddenClickableChild(elem)) {
                result.push([elem, frame]);
            }
        }

        return result;
    }

    // Find elements pre-marked in the main world as having JS-driven hover behavior.
    // Returns an array of [element, frame] pairs.
    function find_elements_with_js_hover(containers) {
        const result = [];
        const selector = "[data-qutebrowser-hovertrack]";
        const interactiveSelector = [
            "button", "a", "input", "textarea", "select", "summary",
            "[role='button']", "[role='tab']", "[role='link']", "[role='menuitem']",
            "[tabindex]:not([tabindex='-1'])",
        ].join(", ");

        function parseHoverTypes(elem) {
            const value = elem.getAttribute("data-qutebrowser-hovertrack") || "";
            return new Set(value.split(",").map((part) => part.trim()).filter(Boolean));
        }

        function hasEnterType(types) {
            return [
                "mouseenter", "mouseover", "mousemove",
                "pointerenter", "pointerover", "pointermove",
            ].some((name) => types.has(name));
        }

        function rectArea(rect) {
            return Math.max(0, rect.width) * Math.max(0, rect.height);
        }

        function overlapRatio(a, b) {
            const left = Math.max(a.left, b.left);
            const right = Math.min(a.right, b.right);
            const top = Math.max(a.top, b.top);
            const bottom = Math.min(a.bottom, b.bottom);
            const width = Math.max(0, right - left);
            const height = Math.max(0, bottom - top);
            const intersection = width * height;
            const minArea = Math.min(rectArea(a), rectArea(b));
            if (minArea === 0) {
                return 0;
            }
            return intersection / minArea;
        }

        function areaRatio(a, b) {
            const areaA = rectArea(a);
            const areaB = rectArea(b);
            const minArea = Math.max(1, Math.min(areaA, areaB));
            const maxArea = Math.max(areaA, areaB);
            return maxArea / minArea;
        }

        function isNearDuplicateRect(a, b) {
            return (
                overlapRatio(a, b) >= 0.9 &&
                areaRatio(a, b) <= 1.35 &&
                Math.abs(a.left - b.left) <= 20 &&
                Math.abs(a.top - b.top) <= 20 &&
                Math.abs(a.width - b.width) <= 40 &&
                Math.abs(a.height - b.height) <= 40
            );
        }

        function isDecorative(elem) {
            return ["SPAN", "IMG", "SVG", "PATH", "USE"].includes(elem.tagName);
        }

        const candidates = [];
        const candidateMap = new Map();

        for (const [container, frame] of containers) {
            for (const elem of container.querySelectorAll(selector)) {
                if (candidateMap.has(elem)) {
                    continue;
                }

                const types = parseHoverTypes(elem);
                if (!hasEnterType(types)) {
                    continue;
                }

                const rect = elem.getBoundingClientRect();
                const candidate = {
                    elem,
                    frame,
                    rect,
                    types,
                    score: 0,
                };
                candidates.push(candidate);
                candidateMap.set(elem, candidate);
            }
        }

        function candidateScore(candidate) {
            const elem = candidate.elem;
            const rect = candidate.rect;
            const types = candidate.types;
            const win = elem.ownerDocument.defaultView || window;
            const viewportArea = Math.max(1, win.innerWidth * win.innerHeight);
            const area = rectArea(rect);
            let score = 0;

            if (types.has("mouseover") || types.has("pointerover")) {
                score += 4;
            }
            if (types.has("mouseenter") || types.has("pointerenter")) {
                score += 3;
            }
            if (types.has("mousemove") || types.has("pointermove")) {
                score += 1;
            }

            if (elem.matches(interactiveSelector)) {
                score += 6;
            }

            if (["DIV", "LI", "ARTICLE", "TR", "TD"].includes(elem.tagName)) {
                score += 2;
            }

            if (elem.innerText && elem.innerText.trim()) {
                score += 1;
            }

            if (isDecorative(elem)) {
                score -= 4;
            }

            if (rect.width < 20 || rect.height < 20) {
                score -= 2;
            }

            if (area > viewportArea * 0.5) {
                score -= 4;
            }
            if (area > viewportArea * 0.8) {
                score -= 8;
            }

            return score;
        }

        for (const candidate of candidates) {
            candidate.score = candidateScore(candidate);
        }

        const dropSet = new Set();
        for (const candidate of candidates) {
            let parent = candidate.elem.parentElement;
            while (parent) {
                const ancestor = candidateMap.get(parent);
                if (ancestor) {
                    if (isNearDuplicateRect(candidate.rect, ancestor.rect)) {
                        if (ancestor.score > candidate.score) {
                            dropSet.add(candidate.elem);
                        } else if (candidate.score > ancestor.score) {
                            dropSet.add(ancestor.elem);
                        } else if (rectArea(ancestor.rect) >= rectArea(candidate.rect)) {
                            dropSet.add(candidate.elem);
                        } else {
                            dropSet.add(ancestor.elem);
                        }
                    }
                    break;
                }
                parent = parent.parentElement;
            }
        }

        for (const candidate of candidates) {
            if (!dropSet.has(candidate.elem)) {
                result.push([candidate.elem, candidate.frame]);
            }
        }

        return result;
    }

    // Find elements that are scrollable (have overflow content and scroll/auto overflow)
    // Returns an array of [element, frame] pairs
    function find_scrollable_elements(containers) {
        const result = [];
        const elemSet = new Set();
        const scrollableOverflowRe = /auto|scroll|overlay/;

        function hasOverflowRange(elem) {
            return (
                elem.scrollHeight > elem.clientHeight ||
                elem.scrollWidth > elem.clientWidth
            );
        }

        function isDocumentScroller(elem) {
            const doc = elem.ownerDocument;
            return Boolean(
                doc && (
                    elem === doc.scrollingElement ||
                    elem === doc.documentElement ||
                    elem === doc.body
                )
            );
        }

        function getOverflow(elem) {
            const win = elem.ownerDocument.defaultView || window;
            const style = win.getComputedStyle(elem);
            return {
                y: style.overflowY || style.overflow,
                x: style.overflowX || style.overflow,
            };
        }

        function isScrollable(elem) {
            // Check if element has overflow content
            if (!hasOverflowRange(elem)) {
                return false;
            }

            // The document scrolling element is special: even when the page is
            // scrollable, its computed overflow is often "visible". However,
            // if both axes are explicitly blocked (hidden/clip), including it
            // creates a bogus top-level hint which obscures the real nested
            // scroll container on app-style pages.
            const overflow = getOverflow(elem);
            if (isDocumentScroller(elem)) {
                const blocksY = ["hidden", "clip"].includes(overflow.y);
                const blocksX = ["hidden", "clip"].includes(overflow.x);
                const canScrollY = elem.scrollHeight > elem.clientHeight && !blocksY;
                const canScrollX = elem.scrollWidth > elem.clientWidth && !blocksX;
                return canScrollX || canScrollY;
            }

            return (
                scrollableOverflowRe.test(overflow.y) ||
                scrollableOverflowRe.test(overflow.x)
            );
        }

        for (const [container, frame] of containers) {
            const doc = container.ownerDocument || container;
            const scrollEl = doc.scrollingElement;
            if (scrollEl && !elemSet.has(scrollEl) && isScrollable(scrollEl)) {
                elemSet.add(scrollEl);
                result.push([scrollEl, frame]);
            }

            // Find all scrollable elements in this container
            const allElements = container.querySelectorAll("*");
            for (const elem of allElements) {
                if (!elemSet.has(elem) && isScrollable(elem)) {
                    elemSet.add(elem);
                    result.push([elem, frame]);
                }
            }
        }

        return result;
    }

    // Recursively finds elements from DOM that have a shadowRoot
    // and returns the shadow roots in a list
    function find_shadow_roots(container = document) {
        const roots = [];

        for (const elem of container.querySelectorAll("*")) {
            if (elem.shadowRoot) {
                roots.push(elem.shadowRoot, ...find_shadow_roots(elem.shadowRoot));
            }
        }

        return roots;
    }

    funcs.find_css = (selector, only_visible) => {
        // Check for special :qb-hover marker to include CSS hover elements
        const includeCssHover = selector.includes(":qb-hover");
        // Check for special :qb-scrollable marker to include scrollable elements
        const includeScrollable = selector.includes(":qb-scrollable");

        // Remove magic markers from the selector
        if (includeCssHover || includeScrollable) {
            selector = selector
                .split(",")
                .map((s) => s.trim())
                .filter((s) => s !== ":qb-hover" && s !== ":qb-scrollable")
                .join(", ");
        }

        // Find all places where we need to look for elements:
        const containers = [[document, null]];
        // Same-domain iframes
        for (const frame of Array.from(window.frames)) {
            if (iframe_same_domain(frame)) {
                containers.push([frame.document, frame]);
            }
        }
        // Open shadow roots
        for (const root of find_shadow_roots()) {
            containers.push([root, null]);
        }

        // Find elements in all containers
        const elems = [];
        const elemSet = new Set();  // Track elements to avoid duplicates

        // Only query with selector if there's something left after removing :qb-hover
        if (selector) {
            for (const [container, frame] of containers) {
                try {
                    for (const elem of container.querySelectorAll(selector)) {
                        if (!elemSet.has(elem)) {
                            elems.push([elem, frame]);
                            elemSet.add(elem);
                        }
                    }
                } catch (ex) {
                    return {"success": false, "error": ex.toString()};
                }
            }
        }

        // If :qb-hover was specified, also find elements with CSS :hover rules
        // and elements pre-marked as JS hover targets in the main world.
        if (includeCssHover) {
            const hoverElems = find_elements_with_css_hover(containers);
            for (const [elem, frame] of hoverElems) {
                if (!elemSet.has(elem)) {
                    elems.push([elem, frame]);
                    elemSet.add(elem);
                }
            }

            const jsHoverElems = find_elements_with_js_hover(containers);
            for (const [elem, frame] of jsHoverElems) {
                if (!elemSet.has(elem)) {
                    elems.push([elem, frame]);
                    elemSet.add(elem);
                }
            }
        }

        // If :qb-scrollable was specified, also find scrollable elements
        // Track page scrolling elements so we can pin their hint to viewport top-left
        let scrollingElems = null;
        if (includeScrollable) {
            scrollingElems = new Set();
            for (const [container] of containers) {
                const doc = container.ownerDocument || container;
                if (doc.scrollingElement) {
                    scrollingElems.add(doc.scrollingElement);
                }
            }
            const scrollableElems = find_scrollable_elements(containers);
            for (const [elem, frame] of scrollableElems) {
                if (!elemSet.has(elem)) {
                    elems.push([elem, frame]);
                    elemSet.add(elem);
                }
            }
        }

        // Filter by visibility and serialize
        // Use lightweight serialization for hover/scrollable detection (skips outerHTML, textContent, attributes)
        const useLightweight = includeCssHover || includeScrollable;
        const out = [];
        for (const [elem, frame] of elems) {
            const isScrollingElem = scrollingElems && scrollingElems.has(elem);
            // Always include page scrolling elements (their rect spans the
            // whole document so is_visible would pass, but we pin their hint
            // to viewport top-left regardless of scroll position)
            if (isScrollingElem || !only_visible || is_visible(elem, frame)) {
                const serialized = serialize_elem(elem, frame, useLightweight);
                if (isScrollingElem) {
                    // Pin to viewport top-left so the hint is always visible
                    serialized.rects = [{
                        top: 0, left: 0,
                        bottom: window.innerHeight, right: window.innerWidth,
                        width: window.innerWidth, height: window.innerHeight,
                    }];
                }
                out.push(serialized);
            }
        }

        return {"success": true, "result": out};
    };

    // Runs a function in a frame until the result is not null, then return
    // If no frame succeeds, return null
    function run_frames(func) {
        for (let i = 0; i < window.frames.length; ++i) {
            const frame = window.frames[i];
            if (iframe_same_domain(frame)) {
                const result = func(frame);
                if (result) {
                    return result;
                }
            }
        }
        return null;
    }

    funcs.find_id = (id) => {
        const elem = document.getElementById(id);
        if (elem) {
            return serialize_elem(elem);
        }

        const serialized_elem = run_frames((frame) => {
            const element = frame.window.document.getElementById(id);
            return serialize_elem(element, frame);
        });

        if (serialized_elem) {
            return serialized_elem;
        }

        return null;
    };

    // Check if elem is an iframe, and if so, return the result of func on it.
    // If no iframes match, return null
    function call_if_frame(elem, func) {
        // Check if elem is a frame, and if so, call func on the window
        if ("contentWindow" in elem) {
            const frame = elem.contentWindow;
            if (iframe_same_domain(frame) &&
                "frameElement" in elem.contentWindow) {
                return func(frame);
            }
        }
        return null;
    }

    funcs.find_focused = () => {
        const elem = document.activeElement;

        if (!elem || elem === document.body) {
            // "When there is no selection, the active element is the page's
            // <body> or null."
            return null;
        }

        // Check if we got an iframe, and if so, recurse inside of it
        const frame_elem = call_if_frame(elem,
            (frame) => serialize_elem(frame.document.activeElement, frame));

        if (frame_elem !== null) {
            return frame_elem;
        }
        return serialize_elem(elem);
    };

    funcs.find_at_pos = (x, y) => {
        const elem = document.elementFromPoint(x, y);

        if (!elem) {
            return null;
        }

        // Check if we got an iframe, and if so, recurse inside of it
        const frame_elem = call_if_frame(elem,
            (frame) => {
                // Subtract offsets due to being in an iframe
                const frame_offset_rect =
                      frame.frameElement.getBoundingClientRect();
                return serialize_elem(frame.document.
                    elementFromPoint(x - frame_offset_rect.left,
                        y - frame_offset_rect.top), frame);
            });

        if (frame_elem !== null) {
            return frame_elem;
        }
        return serialize_elem(elem);
    };

    // Function for returning a selection or focus to python (so we can click
    // it). If nothing is selected but there is something focused, returns
    // "focused"
    funcs.find_selected_focused_link = () => {
        const elem = window.getSelection().anchorNode;
        if (elem) {
            return serialize_elem(elem.parentNode);
        }

        const serialized_frame_elem = run_frames((frame) => {
            const node = frame.window.getSelection().anchorNode;
            if (node) {
                return serialize_elem(node.parentNode, frame);
            }
            return null;
        });

        if (serialized_frame_elem) {
            return serialized_frame_elem;
        }
        return funcs.find_focused() && "focused";
    };

    funcs.set_value = (id, value) => {
        elements[id].value = value;
    };

    funcs.insert_text = (id, text) => {
        const elem = elements[id];
        elem.focus();
        document.execCommand("insertText", false, text);
    };

    funcs.dispatch_event = (id, event, bubbles = false,
        cancelable = false, composed = false) => {
        const elem = elements[id];
        elem.dispatchEvent(
            new Event(event, {"bubbles": bubbles,
                "cancelable": cancelable,
                "composed": composed}));
    };

    funcs.set_attribute = (id, name, value) => {
        elements[id].setAttribute(name, value);
    };

    funcs.remove_blank_target = (id) => {
        let elem = elements[id];
        while (elem !== null) {
            const tag = elem.tagName.toLowerCase();
            if (tag === "a" || tag === "area") {
                if (elem.getAttribute("target") === "_blank") {
                    elem.setAttribute("target", "_top");
                }
                break;
            }
            elem = elem.parentElement;
        }
    };

    funcs.click = (id) => {
        const elem = elements[id];
        elem.click();
    };

    funcs.focus = (id) => {
        const elem = elements[id];
        elem.focus();
    };

    funcs.focus_element = (id) => {
        const elem = elements[id];
        if (document.activeElement && document.activeElement !== elem) {
            document.activeElement.blur();
        }
        elem.style.outline = "none";
        elem.focus();
    };

    funcs.move_cursor_to_end = (id) => {
        const elem = elements[id];
        if (elem.value === undefined) {
            return;
        }
        elem.selectionStart = elem.value.length;
        elem.selectionEnd = elem.value.length;
    };

    funcs.delete = (id) => {
        const elem = elements[id];
        elem.remove();
    };

    funcs.get = (id) => {
        if (id === null || id === undefined) {
            return null;
        }
        const index = Number(id);
        if (!Number.isInteger(index) || index < 0) {
            return null;
        }
        return elements[index] || null;
    };

    return funcs;
})();
