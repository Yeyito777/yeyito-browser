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

    // ============ DIAGNOSTIC LOGGING ============
    const DEBUG = true;
    const LOG_PREFIX = "[qb-element-fix]";

    // Stats tracking
    const stats = {
        elementsProcessed: 0,
        elementsFixed: 0,
        mutationBatches: 0,
        mutationsTotal: 0,
        errors: 0,
        mutationsPerSecond: 0,
        isProcessing: false,
        processingDepth: 0
    };

    // Track mutations per second to detect runaway loops
    let mutationCountWindow = [];
    const MUTATION_WINDOW_MS = 1000;
    const MUTATION_THRESHOLD = 500; // Warn if more than this many mutations/sec

    function log(...args) {
        if (DEBUG) {
            console.log(LOG_PREFIX, ...args);
        }
    }

    function warn(...args) {
        console.warn(LOG_PREFIX, ...args);
    }

    function error(...args) {
        console.error(LOG_PREFIX, ...args);
    }

    function trackMutation(count) {
        const now = Date.now();
        mutationCountWindow.push({ time: now, count: count });

        // Remove old entries
        mutationCountWindow = mutationCountWindow.filter(
            entry => now - entry.time < MUTATION_WINDOW_MS
        );

        // Calculate mutations per second
        const total = mutationCountWindow.reduce((sum, e) => sum + e.count, 0);
        stats.mutationsPerSecond = total;

        if (total > MUTATION_THRESHOLD) {
            warn("HIGH MUTATION RATE:", total, "mutations/sec - possible feedback loop!");
        }
    }

    function logStats() {
        log("Stats:", JSON.stringify(stats, null, 2));
    }

    // Expose stats globally for console inspection
    window._qb_fix_stats = stats;
    window._qb_fix_log_stats = logStats;

    // Kill switch - allows disabling the script at runtime
    let disabled = false;
    let shadowRootCount = 0;

    window._qb_fix_disable = function() {
        disabled = true;
        warn("element_fix.js DISABLED - no more processing will occur");
        warn("To re-enable, call window._qb_fix_enable()");
    };

    window._qb_fix_enable = function() {
        disabled = false;
        warn("element_fix.js ENABLED");
    };

    window._qb_fix_status = function() {
        console.log("=== element_fix.js Status ===");
        console.log("Disabled:", disabled);
        console.log("Stats:", JSON.stringify(stats, null, 2));
        console.log("Shadow roots observed:", shadowRootCount);
        console.log("URL:", window.location.href);
    };

    log("Initializing element_fix.js on", window.location.href);
    // ============ END DIAGNOSTIC LOGGING ============

    /**
     * Apply fix styles to an element based on its properties.
     */
    function fixElement(element) {
        if (disabled) {
            return;
        }

        if (!(element instanceof Element)) {
            return;
        }

        // Skip already processed elements
        if (element._qb_fixed) {
            return;
        }

        stats.elementsProcessed++;

        try {
            const computedStyle = window.getComputedStyle(element);

            let wasFixed = false;

            // If the element has a gradient, fix it to use our colors
            if (fixGradient(element, computedStyle)) {
                wasFixed = true;
            }

            // If the element has a non-transparent background
            // Then make its background this color: #00050f
            if (!isTransparent(computedStyle.backgroundColor)) {
                element.style.setProperty("background-color", "#00050f", "important");
                wasFixed = true;
            }

            // If the element has a border
            // Then make the border this color: #1d9bf0
            if (hasBorder(computedStyle)) {
                element.style.setProperty("border-color", "#1d9bf0", "important");
                wasFixed = true;
            }

            // If the element has text and is NOT a code block
            // Then make the text this color: #ffffff
            if (hasText(element) && !isCodeBlock(element)) {
                element.style.setProperty("color", "#ffffff", "important");
                wasFixed = true;
            }

            // If the element is an SVG or SVG child element
            // Then make it white
            if (isSvgElement(element)) {
                element.style.setProperty("fill", "#ffffff", "important");
                element.style.setProperty("stroke", "#ffffff", "important");
                wasFixed = true;
            }

            if (wasFixed) {
                stats.elementsFixed++;
            }

            element._qb_fixed = true;
        } catch (e) {
            stats.errors++;
            error("Error fixing element:", e.message, "Element:", element.tagName, element.className);
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
            let children;
            try {
                children = element.querySelectorAll("*");
            } catch (e) {
                error("querySelectorAll failed:", e.message);
                return;
            }

            // Warn about large element counts
            if (children.length > 5000) {
                warn("Processing large element tree:", children.length, "elements");
            }

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
        if (disabled) {
            return;
        }

        // Detect re-entrancy (mutations triggered by our own style changes)
        if (stats.isProcessing) {
            stats.processingDepth++;
            if (stats.processingDepth > 10) {
                error("CRITICAL: Processing depth exceeded 10 - likely infinite loop! Aborting.");
                return;
            }
            warn("Re-entrant mutation detected, depth:", stats.processingDepth);
        }

        stats.isProcessing = true;
        stats.mutationBatches++;
        stats.mutationsTotal += mutations.length;
        trackMutation(mutations.length);

        const startTime = performance.now();
        let addedCount = 0;

        try {
            for (const mutation of mutations) {
                // Only handle added nodes - don't reprocess on attribute changes
                // (reprocessing on class changes causes feedback loops with some sites)
                for (let i = 0; i < mutation.addedNodes.length; i++) {
                    const node = mutation.addedNodes[i];
                    if (node.nodeType === Node.ELEMENT_NODE) {
                        addedCount++;
                        processElement(node);
                    }
                }
            }
        } catch (e) {
            error("Error in handleMutations:", e.message, e.stack);
        }

        const elapsed = performance.now() - startTime;

        // Log if processing took too long (potential jank)
        if (elapsed > 50) {
            warn("Slow mutation batch:", elapsed.toFixed(2), "ms for", mutations.length,
                 "mutations,", addedCount, "elements added");
        }

        // Log periodically
        if (stats.mutationBatches % 100 === 0) {
            log("Processed", stats.mutationBatches, "mutation batches,",
                stats.elementsProcessed, "elements,", stats.errors, "errors");
        }

        stats.isProcessing = false;
        stats.processingDepth = 0;
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
        shadowRootCount++;

        log("Observing shadow root #" + shadowRootCount + " on",
            shadowRoot.host ? shadowRoot.host.tagName : "unknown");

        const observer = new MutationObserver(handleMutations);
        observer.observe(shadowRoot, {
            childList: true,
            subtree: true
        });
    }

    /**
     * Intercept attachShadow to observe new shadow roots.
     */
    const originalAttachShadow = Element.prototype.attachShadow;
    Element.prototype.attachShadow = function(init) {
        let shadowRoot;
        try {
            shadowRoot = originalAttachShadow.call(this, init);
        } catch (e) {
            error("attachShadow failed:", e.message);
            throw e;
        }
        log("attachShadow intercepted on", this.tagName);
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
            log("Starting observation on", target.tagName);

            // Process all existing elements first
            const initStart = performance.now();
            processElement(target);
            const initElapsed = performance.now() - initStart;
            log("Initial processing took", initElapsed.toFixed(2), "ms,",
                stats.elementsProcessed, "elements processed");

            // Watch for new elements only (not attribute changes - causes feedback loops)
            mainObserver.observe(target, {
                childList: true,
                subtree: true
            });
            log("MutationObserver active (watching new elements only)");
        } else {
            log("Waiting for document element...");
            requestAnimationFrame(startObserving);
        }
    }

    // Start as early as possible
    if (document.documentElement) {
        log("Document element available, starting immediately");
        startObserving();
    } else {
        log("Document element not available, waiting...");
        document.addEventListener("DOMContentLoaded", startObserving, { once: true });
        requestAnimationFrame(startObserving);
    }

    // Log that initialization is complete
    log("element_fix.js initialization complete");
    log("Access window._qb_fix_stats for live stats");
    log("Call window._qb_fix_log_stats() for formatted output");
})();
