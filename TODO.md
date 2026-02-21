# Bugs
- "Unknown error while getting elements" Is so fucking annoying when I press f
- The fake click that is sent on hinting may not click on the desired item
- Hinting must highlight elements if ANY part of their bounding box intersects with their screen, not just the top-left corner as I suspect it currently does.
- Fix the fact that when I rebuild qutebrowser oftentimes credentials are lost and I need to relog into discord for example.
- Stop CI in github please
- Body / app body in pages like youtube do not get shown through selectables so if we're scrolling in another element it's a pain to refocus the main scroll.
- Scrollbars are not toggled with the shader toggle enum
- You need to fix everything in the shdaer stress test

# Improvements
- zz centers selection on caret mode and highlight mode (/ and ?)
- Overhaul UIs like the crash report and the download so that they follow my terminal theme (Also make it so that it doesn't email the dude lol)
- Use TamperMonkey instead of GreaseMonkey or something that would help get vencord in discord
- A 'copy' hinting mode that allows for copying big, but independent blocks of text
- Be able to navigate the right click copy menu etc with keybinds / outright overhaul it

# Major aditions
- Really think how I could add $/0/w/W/f/F and others while writing text in insert mode. Maybe a special mode? Like insert-normal mode?

# Runtime tooling

## Network detail (`network.sh detail`)
- [x] C++ metadata (url, method, status, type, mimeType, timing, size, cached, netError, remoteEndpoint)
- [x] Response body via JS `fetch({cache: "force-cache"})`
- [x] Response headers via JS `fetch({cache: "force-cache"}).headers`
- [x] Request headers for sub-resource requests (scripts, stylesheets, images, fonts, fetch/XHR)
- [x] Request headers for document/navigation requests (captured from `NavigationHandle::GetRequestHeaders()` in `DidFinishNavigation`)
- [ ] Network-service-added request headers (Accept-Encoding, Host, Cookie — added after blink sends the request)
- [ ] Request body (POST payloads — not captured by `ResourceLoadComplete`)
- [ ] `set-cookie` response headers (stripped by `fetch().headers` per spec — needs C++ `HttpResponseHeaders` interception)
- [ ] Response body/headers for cross-origin without CORS (fetch fails — needs C++ response interception)
- [ ] Initiator (what triggered the request — not in `ResourceLoadComplete`, needs `Network.requestWillBeSent`)
- [ ] Redirect chain individual hops (only `url` vs `originalUrl` captured — needs `NavigationHandle` redirect tracking)
- [ ] Priority (not captured — needs `Network.resourceChangedPriority`)
- [ ] WebSocket frames (separate protocol — stub exists, needs DevTools `Network.webSocketFrame*` events)

### Testing
After C++ changes, rebuild with `./install.sh --dirty`, then restart qutebrowser by sending `:restart` through the IPC socket (or just relaunch it). Navigate to a page with mixed resource types (e.g. any site with scripts, stylesheets, images). Run `network.sh list` to grab request IDs, then `network.sh detail <id>` on both a sub-resource (script/stylesheet) and a document request to verify the new field shows up where expected. Pipe the resulting `request-<id>.json` through `jq` to inspect the specific field you changed.
