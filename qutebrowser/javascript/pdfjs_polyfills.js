/* eslint-disable strict */
/* (this file gets used as a snippet) */

/*
SPDX-FileCopyrightText: Florian Bruhin (The Compiler) <mail@qutebrowser.org>
SPDX-License-Identifier: GPL-3.0-or-later
*/

(function() {
    // Chromium 119 / QtWebEngine 6.8
    // https://caniuse.com/mdn-javascript_builtins_promise_withresolvers
    if (typeof Promise.withResolvers === "undefined") {
        Promise.withResolvers = function() {
            let resolve, reject
            const promise = new Promise((res, rej) => {
                resolve = res
                reject = rej
            })
            return { promise, resolve, reject }
        }
    }

    // Chromium 126 / QtWebEngine 6.9
    // https://caniuse.com/mdn-api_url_parse_static
    if (typeof URL.parse === "undefined") {
        URL.parse = function(url, base) {
            try {
                return new URL(url, base);
            } catch (ex) {
                return null;
            }
        }
    }

    // Chromium 140
    // https://caniuse.com/mdn-javascript_builtins_uint8array_tohex
    if (typeof Uint8Array.prototype.toHex === "undefined") {
        Uint8Array.prototype.toHex = function() {
            return Array.from(this, b => b.toString(16).padStart(2, '0')).join('')
        }
    }

    // Chromium 140
    // https://caniuse.com/mdn-javascript_builtins_uint8array_fromhex
    if (typeof Uint8Array.fromHex === "undefined") {
        Uint8Array.fromHex = function(hexString) {
            const bytes = new Uint8Array(hexString.length / 2)
            for (let i = 0; i < hexString.length; i += 2) {
                bytes[i / 2] = parseInt(hexString.substring(i, i + 2), 16)
            }
            return bytes
        }
    }

    // Chromium 140
    // https://caniuse.com/mdn-javascript_builtins_uint8array_tobase64
    if (typeof Uint8Array.prototype.toBase64 === "undefined") {
        Uint8Array.prototype.toBase64 = function() {
            let binary = ''
            for (let i = 0; i < this.length; i++) {
                binary += String.fromCharCode(this[i])
            }
            return btoa(binary)
        }
    }

    // Chromium 140
    // https://caniuse.com/mdn-javascript_builtins_uint8array_frombase64
    if (typeof Uint8Array.fromBase64 === "undefined") {
        Uint8Array.fromBase64 = function(base64String) {
            const binary = atob(base64String)
            const bytes = new Uint8Array(binary.length)
            for (let i = 0; i < binary.length; i++) {
                bytes[i] = binary.charCodeAt(i)
            }
            return bytes
        }
    }
})();
