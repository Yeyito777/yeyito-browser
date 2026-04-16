// SPDX-FileCopyrightText: Florian Bruhin (The Compiler) <mail@qutebrowser.org>
//
// SPDX-License-Identifier: GPL-3.0-or-later

"use strict";

window._qutebrowser.hovertrack = (function() {
    const funcs = {};

    const MARK_ATTR = "data-qutebrowser-hovertrack";
    const HOVER_EVENTS = new Set([
        "mouseenter", "mouseleave",
        "mouseover", "mouseout", "mousemove",
        "pointerenter", "pointerleave",
        "pointerover", "pointerout", "pointermove",
    ]);
    const listenerTypes = new WeakMap();

    function is_element(obj) {
        return typeof Element !== "undefined" && obj instanceof Element;
    }

    function normalize_event_type(type) {
        if (typeof type !== "string") {
            return null;
        }
        const normalized = type.toLowerCase();
        if (!HOVER_EVENTS.has(normalized)) {
            return null;
        }
        return normalized;
    }

    function normalize_prop_name(propName) {
        if (typeof propName !== "string" || !propName.startsWith("on")) {
            return null;
        }
        return normalize_event_type(propName.slice(2));
    }

    function tracked_listener_types(elem) {
        const existing = listenerTypes.get(elem);
        if (existing) {
            return existing;
        }
        const created = new Set();
        listenerTypes.set(elem, created);
        return created;
    }

    function remember_listener(target, type) {
        const normalized = normalize_event_type(type);
        if (!normalized || !is_element(target)) {
            return;
        }
        tracked_listener_types(target).add(normalized);
    }

    function patch_add_event_listener() {
        const proto = typeof Element !== "undefined" ? Element.prototype : null;
        if (!proto || proto.__qute_hovertrack_patched) {
            return;
        }

        const original = proto.addEventListener;
        Object.defineProperty(proto, "addEventListener", {
            value: function(type, listener, options) {
                const result = original.call(this, type, listener, options);
                remember_listener(this, type);
                return result;
            },
            enumerable: false,
            configurable: true,
            writable: true,
        });

        Object.defineProperty(proto, "__qute_hovertrack_patched", {
            value: true,
            enumerable: false,
            configurable: false,
            writable: false,
        });
    }

    function dom0_hover_types(elem) {
        const types = new Set();
        for (const eventName of HOVER_EVENTS) {
            const propName = `on${eventName}`;
            try {
                if (typeof elem[propName] === "function") {
                    types.add(eventName);
                }
            } catch (_error) {
                // Ignore accessors that throw.
            }
        }
        return types;
    }

    function framework_hover_types(elem) {
        const types = new Set();
        let propertyNames;
        try {
            propertyNames = Object.getOwnPropertyNames(elem);
        } catch (_error) {
            return types;
        }

        for (const propName of propertyNames) {
            if (!(
                propName.startsWith("__reactProps$") ||
                propName.startsWith("__reactEventHandlers$")
            )) {
                continue;
            }

            let props;
            try {
                props = elem[propName];
            } catch (_error) {
                continue;
            }

            if (!props || typeof props !== "object") {
                continue;
            }

            for (const key of Object.keys(props)) {
                const normalized = normalize_prop_name(key);
                if (normalized !== null) {
                    types.add(normalized);
                }
            }
        }

        return types;
    }

    function collect_types(elem) {
        const types = new Set();

        const tracked = listenerTypes.get(elem);
        if (tracked) {
            for (const type of tracked) {
                types.add(type);
            }
        }

        for (const type of dom0_hover_types(elem)) {
            types.add(type);
        }

        for (const type of framework_hover_types(elem)) {
            types.add(type);
        }

        return types;
    }

    function mark_element(elem) {
        const types = Array.from(collect_types(elem)).sort();
        if (types.length !== 0) {
            elem.setAttribute(MARK_ATTR, types.join(","));
            return 1;
        }

        elem.removeAttribute(MARK_ATTR);
        return 0;
    }

    function mark_container(container = document) {
        let count = 0;

        const mark_nested_shadow_roots = (root) => {
            for (const elem of root.querySelectorAll("*")) {
                count += mark_element(elem);
                if (elem.shadowRoot) {
                    mark_nested_shadow_roots(elem.shadowRoot);
                }
            }
        };

        if (container.querySelectorAll) {
            mark_nested_shadow_roots(container);
        }

        return count;
    }

    funcs.mark = () => {
        patch_add_event_listener();
        return mark_container(document);
    };

    funcs.mark_frames = () => {
        patch_add_event_listener();

        function mark_window(win) {
            let count = 0;
            try {
                const helper = win._qutebrowser && win._qutebrowser.hovertrack;
                if (helper && typeof helper.mark === "function") {
                    count += helper.mark();
                }
            } catch (_error) {
                return count;
            }

            for (const frame of Array.from(win.frames)) {
                try {
                    count += mark_window(frame);
                } catch (_error) {
                    // Ignore cross-origin frames.
                }
            }

            return count;
        }

        return mark_window(window);
    };

    patch_add_event_listener();
    return funcs;
})();
