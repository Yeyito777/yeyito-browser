(function() {
    "use strict";
    const expectedHelpers = {{ expected_helpers | safe }};

    if (!window.hasOwnProperty("_qutebrowser") ||
        !window._qutebrowser ||
        typeof window._qutebrowser !== "object") {
        window._qutebrowser = {};
    }

    if (!window._qutebrowser.initialized ||
        typeof window._qutebrowser.initialized !== "object") {
        window._qutebrowser.initialized = {};
    }

    const helpersPresent = expectedHelpers.every((helperName) => {
        const helper = window._qutebrowser[helperName];
        return helper !== undefined && helper !== null;
    });

    if (window._qutebrowser.initialized["{{name}}"] && helpersPresent) {
        return;
    }

    {{code}}
    window._qutebrowser.initialized["{{name}}"] = true;
})();
