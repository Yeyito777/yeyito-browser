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

        return (invisible || none_display || (zero_opacity && !is_framework));
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
        if (includeCssHover) {
            // Remove the marker from the selector
            // Handle both ", :qb-hover" and ":qb-hover, " and standalone ":qb-hover"
            selector = selector
                .split(",")
                .map((s) => s.trim())
                .filter((s) => s !== ":qb-hover")
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
        if (includeCssHover) {
            const hoverElems = find_elements_with_css_hover(containers);
            for (const [elem, frame] of hoverElems) {
                if (!elemSet.has(elem)) {
                    elems.push([elem, frame]);
                    elemSet.add(elem);
                }
            }
        }

        // Filter by visibility and serialize
        // Use lightweight serialization for hover detection (skips outerHTML, textContent, attributes)
        const out = [];
        for (const [elem, frame] of elems) {
            if (!only_visible || is_visible(elem, frame)) {
                out.push(serialize_elem(elem, frame, includeCssHover));
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
