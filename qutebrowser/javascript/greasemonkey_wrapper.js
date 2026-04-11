(function() {
    const _qute_script_id = "__gm_{{ scriptName }}";

    function GM_log(text) {
        console.log(text);
    }

    const GM_info = {
        'script': {{ scriptInfo }},
        'scriptMetaStr': "{{ scriptMeta }}",
        'scriptWillUpdate': false,
        'version': "0.0.1",
        // so scripts don't expect exportFunction
        'scriptHandler': 'Tampermonkey',
    };

    function checkKey(key, funcName) {
        if (typeof key !== "string") {
          throw new Error(`${funcName} requires the first parameter to be of type string, not '${typeof key}'`);
        }
    }

    function GM_setValue(key, value) {
        checkKey(key, "GM_setValue");
        if (typeof value !== "string" &&
            typeof value !== "number" &&
            typeof value !== "boolean") {
          throw new Error(`GM_setValue requires the second parameter to be of type string, number or boolean, not '${typeof value}'`);
        }
        localStorage.setItem(_qute_script_id + key, value);
    }

    function GM_getValue(key, default_) {
        checkKey(key, "GM_getValue");
        return localStorage.getItem(_qute_script_id + key) || default_;
    }

    function GM_deleteValue(key) {
        checkKey(key, "GM_deleteValue");
        localStorage.removeItem(_qute_script_id + key);
    }

    function GM_listValues() {
        const keys = [];
        for (let i = 0; i < localStorage.length; i++) {
            if (localStorage.key(i).startsWith(_qute_script_id)) {
                keys.push(localStorage.key(i).slice(_qute_script_id.length));
            }
        }
        return keys;
    }

    function GM_openInTab(url) {
        window.open(url);
    }


    // Cross-origin HTTP proxy via qute-gm:// scheme.
    // Routes requests through qutebrowser's Python backend, bypassing the
    // page's Content-Security-Policy.  The qute-gm scheme is registered
    // with ContentSecurityPolicyIgnored so fetch() to it always succeeds.
    function GM_xmlhttpRequest(/* object */ details) {
        details.method = details.method ? details.method.toUpperCase() : "GET";

        if (!details.url) {
            throw new Error("GM_xmlhttpRequest requires a URL.");
        }

        const spec = {
            url: details.url,
            method: details.method,
            headers: details.headers || {},
            data: details.data || null,
        };

        const controller = new AbortController();

        const proxyUrl = "qutegm:/fetch?spec=" + encodeURIComponent(JSON.stringify(spec));

        fetch(proxyUrl, {
            signal: controller.signal,
        }).then(proxyResp => proxyResp.json()).then(r => {
            // Build a response object mimicking XMLHttpRequest/GM response
            const responseText = r.response || "";

            // Convert latin-1 string back to bytes for blob responses
            const bytes = new Uint8Array(responseText.length);
            for (let i = 0; i < responseText.length; i++) {
                bytes[i] = responseText.charCodeAt(i);
            }
            const blob = new Blob([bytes]);

            const resp = {
                readyState: 4,
                status: r.status,
                statusText: r.statusText,
                responseHeaders: r.responseHeaders || "",
                responseText: responseText,
                response: details.responseType === "blob" ? blob :
                          details.responseType === "arraybuffer" ? bytes.buffer :
                          details.responseType === "json" ? (() => { try { return JSON.parse(responseText); } catch(e) { return null; } })() :
                          responseText,
                finalUrl: r.finalUrl || details.url,
                // Convenience helpers matching fetch Response API
                blob: () => Promise.resolve(blob),
                arrayBuffer: () => blob.arrayBuffer(),
                text: () => Promise.resolve(responseText),
                json: async () => JSON.parse(responseText),
                headers: new Headers(),
                ok: r.status >= 200 && r.status < 300,
            };

            // Parse response headers into Headers object
            if (r.responseHeaders) {
                for (const line of r.responseHeaders.split("\r\n")) {
                    const idx = line.indexOf(":");
                    if (idx > 0) {
                        try {
                            resp.headers.append(line.slice(0, idx).trim(), line.slice(idx + 1).trim());
                        } catch(e) {}
                    }
                }
            }

            if (r.error) {
                if ("onerror" in details) details.onerror(resp);
            } else {
                if ("onload" in details) details.onload(resp);
            }
        }).catch(err => {
            if (err.name === "AbortError") {
                if ("onabort" in details) details.onabort();
            } else {
                const errResp = { readyState: 4, status: 0, statusText: String(err), responseText: "", response: "" };
                if ("onerror" in details) details.onerror(errResp);
            }
        });

        return { abort: () => controller.abort() };
    }

    function GM_addStyle(/* String */ styles) {
        const oStyle = document.createElement("style");
        oStyle.setAttribute("type", "text/css");
        oStyle.appendChild(document.createTextNode(styles));

        const head = document.getElementsByTagName("head")[0];
        if (head === undefined) {
            // no head yet, stick it wherever
            document.documentElement.appendChild(oStyle);
        } else {
            head.appendChild(oStyle);
        }
    }

    // Based on GreaseMonkey:
    // https://github.com/greasemonkey/greasemonkey/blob/4.11/src/bg/api-provider-source.js#L232-L249
    function GM_setClipboard(text) {
        function onCopy(event) {
            document.removeEventListener('copy', onCopy, true);

            event.stopImmediatePropagation();
            event.preventDefault();

            event.clipboardData.setData('text/plain', text);
        }

        document.addEventListener('copy', onCopy, true);
        document.execCommand('copy');
    }

    // Stub these two so that the gm4 polyfill script doesn't try to
    // create broken versions as attributes of window.
    function GM_getResourceText(caption, commandFunc, accessKey) {
        console.info(`${GM_info.script.name} called unimplemented GM_getResourceText`);
    }

    function GM_registerMenuCommand(caption, commandFunc, accessKey) {
        console.info(`${GM_info.script.name} called unimplemented GM_registerMenuCommand`);
    }

    // Mock the greasemonkey 4.0 async API.
    const GM = {};
    GM.info = GM_info;
    const entries = {
        'log': GM_log,
        'addStyle': GM_addStyle,
        'setClipboard': GM_setClipboard,
        'deleteValue': GM_deleteValue,
        'getValue': GM_getValue,
        'listValues': GM_listValues,
        'openInTab': GM_openInTab,
        'setValue': GM_setValue,
        'xmlHttpRequest': GM_xmlhttpRequest,
    }
    for (newKey in entries) {
        let old = entries[newKey];
        if (old && (typeof GM[newKey] == 'undefined')) {
            GM[newKey] = function(...args) {
                return new Promise((resolve, reject) => {
                    try {
                        resolve(old(...args));
                    } catch (e) {
                        reject(e);
                    }
                });
            };
        }
    };

    const unsafeWindow = window;
    {% if use_proxy %}
    /*
     * Try to give userscripts an environment that they expect. Which seems
     * to be that the global window object should look the same as the page's
     * one and that if a script writes to an attribute of window all other
     * scripts should be able to access that variable in the global scope.
     * Use a Proxy to stop scripts from actually changing the global window
     * (that's what unsafeWindow is for). Use the "with" statement to make
     * the proxy provide what looks like global scope.
     *
     * There are other Proxy functions that we may need to override.  set,
     * get and has are definitely required.
     */

    if (!window._qute_gm_window_proxy) {
        const qute_gm_window_shadow = {}; // stores local changes to window
        const qute_gm_windowProxyHandler = {
            get: function (target, prop) {
                if (prop in qute_gm_window_shadow)
                    return qute_gm_window_shadow[prop];
                if (prop in target) {
                    if (typeof target[prop] === 'function' && typeof target[prop].prototype == 'undefined')
                        // Getting TypeError: Illegal Execution when callers try
                        // to execute eg addEventListener from here because they
                        // were returned unbound
                        return target[prop].bind(target);
                    return target[prop];
                }
            },
            set: function(target, prop, val) {
                return qute_gm_window_shadow[prop] = val;
            },
            has: function(target, key) {
                return key in qute_gm_window_shadow || key in target;
            }
        };
        window._qute_gm_window_proxy = new Proxy(unsafeWindow, qute_gm_windowProxyHandler);
    }
    const qute_gm_window_proxy = window._qute_gm_window_proxy;
    with (qute_gm_window_proxy) {
        // We can't return `this` or `qute_gm_window_proxy` from
        // `qute_gm_window_proxy.get('window')` because the Proxy implementation
        // does typechecking on read-only things. So we have to shadow `window`
        // more conventionally here.
        const window = qute_gm_window_proxy;
        // ====== The actual user script source ====== //
{{ scriptSource }}
        // ====== End User Script ====== //
    };
    {% else %}
        // ====== The actual user script source ====== //
{{ scriptSource }}
        // ====== End User Script ====== //
    {% endif %}
})();
