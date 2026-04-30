// SPDX-FileCopyrightText: Florian Bruhin (The Compiler) <mail@qutebrowser.org>
//
// SPDX-License-Identifier: GPL-3.0-or-later

"use strict";

window._qutebrowser.scroll = (function() {
    const funcs = {};

    funcs.to_perc = (x, y) => {
        const dx = x !== undefined ? 1 : 0;
        const dy = y !== undefined ? 1 : 0;

        // Try focused scrollable element first
        const active = _deepActiveElement();
        if (active && active !== document.body &&
            active !== document.documentElement) {
            const scrollTarget = _findScrollable(active, dx, dy);
            if (scrollTarget) {
                if (x !== undefined) {
                    scrollTarget.scrollLeft =
                        (scrollTarget.scrollWidth - scrollTarget.clientWidth) /
                        100 * x;
                }
                if (y !== undefined) {
                    scrollTarget.scrollTop =
                        (scrollTarget.scrollHeight - scrollTarget.clientHeight) /
                        100 * y;
                }
                return;
            }
        }

        // Fall back to window scroll
        let x_px = window.scrollX;
        let y_px = window.scrollY;

        const width = Math.max(
            document.body.scrollWidth,
            document.body.offsetWidth,
            document.documentElement.scrollWidth,
            document.documentElement.offsetWidth
        );
        const height = Math.max(
            document.body.scrollHeight,
            document.body.offsetHeight,
            document.documentElement.scrollHeight,
            document.documentElement.offsetHeight
        );

        if (x !== undefined) {
            x_px = (width - window.innerWidth) / 100 * x;
        }

        if (y !== undefined) {
            y_px = (height - window.innerHeight) / 100 * y;
        }

        window.scroll(x_px, y_px);
    };

    funcs.delta_page = (x, y) => {
        const dx = window.innerWidth * x;
        const dy = window.innerHeight * y;
        window.scrollBy(dx, dy);
    };

    const _scrollableOverflows = new Set(["auto", "scroll", "overlay"]);

    const _isScrollable = (elem, dx, dy) => {
        const style = window.getComputedStyle(elem);
        const canScrollY = dy !== 0 && _scrollableOverflows.has(style.overflowY) &&
            elem.scrollHeight > elem.clientHeight;
        const canScrollX = dx !== 0 && _scrollableOverflows.has(style.overflowX) &&
            elem.scrollWidth > elem.clientWidth;
        return canScrollX || canScrollY;
    };

    const _deepActiveElement = (root = document) => {
        const active = root.activeElement;
        if (!active) {
            return active;
        }

        if (active.shadowRoot && active.shadowRoot.activeElement) {
            return _deepActiveElement(active.shadowRoot);
        }

        return active;
    };

    const _findScrollable = (start, dx, dy) => {
        const doc = document.documentElement;
        const body = document.body;
        let current = start;

        while (current && current !== body && current !== doc) {
            if (_isScrollable(current, dx, dy)) {
                return current;
            }
            current = current.parentElement;
        }

        return null;
    };

    const _selectedScrollable = () => {
        const attr = "data-qutebrowser-scroll-target";
        const selector = `[${attr}="1"]`;
        const findIn = (root) => {
            if (!root || !root.querySelector) {
                return null;
            }
            const found = root.querySelector(selector);
            if (found) {
                return found;
            }
            const all = root.querySelectorAll ? root.querySelectorAll("*") : [];
            for (const elem of all) {
                if (elem.shadowRoot) {
                    const shadowFound = findIn(elem.shadowRoot);
                    if (shadowFound) {
                        return shadowFound;
                    }
                }
            }
            return null;
        };
        return findIn(document);
    };

    const _isDocumentScroller = (elem) => {
        const doc = elem && elem.ownerDocument;
        return !!doc && (elem === doc.scrollingElement ||
            elem === doc.documentElement || elem === doc.body);
    };

    const _scrollTargetCenter = (target, dx, dy) => {
        if (!target) {
            return null;
        }
        if (_isDocumentScroller(target)) {
            // Let the native compositor scroller target the viewport directly
            // instead of hit-testing whatever child is under the viewport center.
            return [-1, -1];
        }
        const scrollTarget = _isScrollable(target, dx, dy) ? target :
            _findScrollable(target, dx, dy);
        if (!scrollTarget) {
            return null;
        }
        const rect = scrollTarget.getBoundingClientRect();
        return [Math.round(rect.left + rect.width / 2),
                Math.round(rect.top + rect.height / 2)];
    };

    // Smooth scroll accumulator — batches rapid scroll calls into one
    // continuous animation instead of fighting independent CSS animations.
    // All steps are truncated to whole pixels to avoid sub-pixel rendering
    // asymmetry (floor-based pixel snapping makes up/down behave differently
    // for fractional scrollBy values).
    let _scrollState = null;
    let _scrollRaf = null;
    let _scrollFactor = 0.3;

    const _animateScroll = () => {
        const s = _scrollState;
        if (!s) { _scrollRaf = null; return; }

        let stepX = Math.trunc(s.dx * _scrollFactor);
        let stepY = Math.trunc(s.dy * _scrollFactor);

        // Minimum ±1px step while ≥1px remains, avoids large final snaps
        if (stepX === 0 && Math.abs(s.dx) >= 1) stepX = s.dx > 0 ? 1 : -1;
        if (stepY === 0 && Math.abs(s.dy) >= 1) stepY = s.dy > 0 ? 1 : -1;

        if (stepX === 0 && stepY === 0) {
            _scrollState = null;
            _scrollRaf = null;
            return;
        }

        s.target.scrollBy({left: stepX, top: stepY, behavior: "instant"});
        s.dx -= stepX;
        s.dy -= stepY;

        _scrollRaf = requestAnimationFrame(_animateScroll);
    };

    const _accumulateScroll = (target, dx, dy, factor) => {
        if (factor !== undefined) _scrollFactor = factor;
        if (_scrollState && _scrollState.target === target) {
            _scrollState.dx += dx;
            _scrollState.dy += dy;
        } else {
            if (_scrollRaf) cancelAnimationFrame(_scrollRaf);
            _scrollState = {target, dx, dy};
            _scrollRaf = requestAnimationFrame(_animateScroll);
        }
    };

    funcs.get_scroll_target_center = (dx, dy) => {
        const selected = _scrollTargetCenter(_selectedScrollable(), dx, dy);
        if (selected) {
            return selected;
        }

        const active = _deepActiveElement();
        if (active && active !== document.body &&
            active !== document.documentElement) {
            return _scrollTargetCenter(active, dx, dy);
        }
        return null;
    };

    funcs.scroll_focused = (dx, dy, factor) => {
        const active = _deepActiveElement();

        if (!active || active === document.body || active === document.documentElement) {
            return false;
        }

        const scrollTarget = _findScrollable(active, dx, dy);

        if (!scrollTarget) {
            return false;
        }

        _accumulateScroll(scrollTarget, dx, dy, factor);
        return true;
    };

    funcs.scroll_delta = (dx, dy, factor) => {
        const active = _deepActiveElement();
        if (active && active !== document.body &&
            active !== document.documentElement) {
            const scrollTarget = _findScrollable(active, dx, dy);
            if (scrollTarget) {
                _accumulateScroll(scrollTarget, dx, dy, factor);
                return;
            }
        }
        _accumulateScroll(window, dx, dy, factor);
    };

    return funcs;
})();
