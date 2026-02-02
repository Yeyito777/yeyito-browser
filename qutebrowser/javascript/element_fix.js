// SPDX-FileCopyrightText: qutebrowser Contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later

"use strict";

(function() {
    // Prevent double initialization
    if (window._qutebrowser_fix_initialized) {
        return;
    }
    window._qutebrowser_fix_initialized = true;

    /**
     * Apply fix styles to an element based on its properties.
     */
    function fixElement(element) {
        if (!(element instanceof Element)) {
            return;
        }

        // Skip already processed elements
        if (element._qb_fixed) {
            return;
        }

        try {
            const computedStyle = window.getComputedStyle(element);

            // If the element has a gradient, fix it to use our colors
            fixGradient(element, computedStyle);

            // If the element has a non-transparent background
            // Then make its background this color: #00050f
            if (!isTransparent(computedStyle.backgroundColor)) {
                element.style.setProperty("background-color", "#00050f", "important");
            }

            // If the element has a border
            // Then make the border this color: #1d9bf0
            if (hasBorder(computedStyle)) {
                element.style.setProperty("border-color", "#1d9bf0", "important");
            }

            // If the element has text and is NOT a code block
            // Then make the text this color: #ffffff
            if (hasText(element) && !isCodeBlock(element)) {
                element.style.setProperty("color", "#ffffff", "important");
            }

            // If the element is an SVG or SVG child element
            // Then make it white
            if (isSvgElement(element)) {
                element.style.setProperty("fill", "#ffffff", "important");
                element.style.setProperty("stroke", "#ffffff", "important");
            }

            element._qb_fixed = true;
        } catch (e) {
            // Element might not be attached to DOM yet
        }
    }

    /**
     * Check if a background has a gradient and fix it.
     * Returns true if a gradient was found and fixed.
     */
    function fixGradient(element, computedStyle) {
        const backgroundImage = computedStyle.backgroundImage;

        if (!backgroundImage || backgroundImage === "none") {
            return false;
        }

        // Check if it contains a gradient
        if (backgroundImage.includes("gradient")) {
            // Replace the gradient with our standard gradient
            element.style.setProperty(
                "background-image",
                "linear-gradient(to bottom, #00050f, #090d35)",
                "important"
            );
            return true;
        }

        return false;
    }

    /**
     * Check if a background color is transparent.
     */
    function isTransparent(backgroundColor) {
        if (!backgroundColor || backgroundColor === "transparent") {
            return true;
        }

        const rgbaMatch = backgroundColor.match(
            /rgba?\s*\(\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*(?:,\s*([\d.]+))?\s*\)/
        );

        if (rgbaMatch) {
            const alpha = rgbaMatch[4] !== undefined ? parseFloat(rgbaMatch[4]) : 1;
            return alpha === 0;
        }

        return false;
    }

    /**
     * Check if an element has a visible border.
     */
    function hasBorder(computedStyle) {
        const sides = ["Top", "Right", "Bottom", "Left"];
        for (const side of sides) {
            const width = parseFloat(computedStyle["border" + side + "Width"]);
            const style = computedStyle["border" + side + "Style"];
            if (width > 0 && style && style !== "none") {
                return true;
            }
        }
        return false;
    }

    /**
     * Check if an element has direct text content.
     */
    function hasText(element) {
        for (const node of element.childNodes) {
            if (node.nodeType === Node.TEXT_NODE && node.textContent.trim().length > 0) {
                return true;
            }
        }
        return false;
    }

    /**
     * Check if an element is a code block or inside one.
     * Matches the CSS selector logic:
     * *:not(pre):not(code):not(pre *):not(code *):not(.hljs):not(.hljs *)
     * :not([class*="language-"]):not([class*="language-"] *):not(.textLayer):not(.textLayer *)
     */
    function isCodeBlock(element) {
        const tagName = element.tagName.toLowerCase();

        // Direct code block elements
        if (tagName === "code" || tagName === "pre") {
            return true;
        }

        // Check for code-related classes on the element itself
        const className = element.className || "";
        if (typeof className === "string") {
            if (className.includes("hljs") ||
                className.includes("language-") ||
                className.includes("textLayer")) {
                return true;
            }
        }

        // Check if element is inside a code block or code-related container
        let parent = element.parentElement;
        while (parent) {
            const parentTag = parent.tagName.toLowerCase();
            if (parentTag === "code" || parentTag === "pre") {
                return true;
            }

            const parentClass = parent.className || "";
            if (typeof parentClass === "string") {
                if (parentClass.includes("hljs") ||
                    parentClass.includes("language-") ||
                    parentClass.includes("textLayer")) {
                    return true;
                }
            }

            parent = parent.parentElement;
        }

        return false;
    }

    /**
     * Check if an element is an SVG or SVG child element.
     */
    function isSvgElement(element) {
        return element.namespaceURI === "http://www.w3.org/2000/svg";
    }

    /**
     * Process an element and all its descendants, including Shadow DOM.
     */
    function processElement(element) {
        if (!element) {
            return;
        }

        fixElement(element);

        // Process regular children
        if (element.querySelectorAll) {
            const children = element.querySelectorAll("*");
            for (let i = 0; i < children.length; i++) {
                fixElement(children[i]);
                // Check for shadow roots on each child
                if (children[i].shadowRoot) {
                    processElement(children[i].shadowRoot);
                    observeShadowRoot(children[i].shadowRoot);
                }
            }
        }

        // Process shadow root if present
        if (element.shadowRoot) {
            processElement(element.shadowRoot);
            observeShadowRoot(element.shadowRoot);
        }
    }

    /**
     * Handle mutations.
     */
    function handleMutations(mutations) {
        for (const mutation of mutations) {
            // Handle added nodes
            for (let i = 0; i < mutation.addedNodes.length; i++) {
                const node = mutation.addedNodes[i];
                if (node.nodeType === Node.ELEMENT_NODE) {
                    processElement(node);
                }
            }

            // Handle attribute changes (style or class changes)
            if (mutation.type === "attributes") {
                const target = mutation.target;
                if (target.nodeType === Node.ELEMENT_NODE) {
                    // Reset the fixed flag so we can re-check
                    target._qb_fixed = false;
                    fixElement(target);
                }
            }
        }
    }

    // Keep track of observed shadow roots to avoid duplicates
    const observedShadowRoots = new WeakSet();

    /**
     * Observe a shadow root for mutations.
     */
    function observeShadowRoot(shadowRoot) {
        if (observedShadowRoots.has(shadowRoot)) {
            return;
        }
        observedShadowRoots.add(shadowRoot);

        const observer = new MutationObserver(handleMutations);
        observer.observe(shadowRoot, {
            childList: true,
            subtree: true,
            attributes: true,
            attributeFilter: ["style", "class"]
        });
    }

    /**
     * Intercept attachShadow to observe new shadow roots.
     */
    const originalAttachShadow = Element.prototype.attachShadow;
    Element.prototype.attachShadow = function(init) {
        const shadowRoot = originalAttachShadow.call(this, init);
        observeShadowRoot(shadowRoot);
        return shadowRoot;
    };

    // Main observer for the document
    const mainObserver = new MutationObserver(handleMutations);

    /**
     * Start observing.
     */
    function startObserving() {
        const target = document.documentElement || document.body;
        if (target) {
            // Process all existing elements first
            processElement(target);

            // Watch for new elements and attribute changes
            mainObserver.observe(target, {
                childList: true,
                subtree: true,
                attributes: true,
                attributeFilter: ["style", "class"]
            });

            // Re-process periodically to catch dynamically styled elements
            setInterval(function() {
                processElement(document.documentElement || document.body);
            }, 1000);
        } else {
            requestAnimationFrame(startObserving);
        }
    }

    // Start as early as possible
    if (document.documentElement) {
        startObserving();
    } else {
        document.addEventListener("DOMContentLoaded", startObserving, { once: true });
        requestAnimationFrame(startObserving);
    }
})();
