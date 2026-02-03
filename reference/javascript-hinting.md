Javascript Hint Target
======================

Overview
--------

qutebrowser now supports a new hint target called `javascript`. The target
allows bindings such as `config.bind('<Ctrl-Space>', 'hint all javascript focus.js')`
to execute a custom JavaScript snippet against the element selected via hinting.

File Lookup
-----------

- Script filenames are resolved relative to the user configuration
  directory (`standarddir.config()/js`) and fall back to the data directory
  (`standarddir.data()/js`).
- Paths may include subdirectories (e.g. `forms/focus.js`) but must not
  escape the `js/` root or be absolute.
- If a file cannot be found, hinting fails with a descriptive error listing the
  probed directories.

Runtime Behaviour
-----------------

1. When a hint is triggered with the `javascript` target, qutebrowser reads the
   referenced file (UTF-8) and passes any additional arguments from the binding
   to the script.
2. The hinted DOM node is tagged with a unique
   `data-qutebrowser-hint-target` attribute.
   - On QtWebEngine this uses the existing `webelem` JS channel with an async
     callback to ensure the attribute write finishes before execution.
   - On QtWebKit the attribute is written directly on the wrapped `QWebElement`.
3. A wrapper IIFE (`(function(){ ... })();`) runs inside the page's main world
   with no `unsafe-eval`, avoiding Content-Security-Policy violations.
   - The wrapper locates the tagged element via `document.querySelector`,
     cleans up the temporary attribute, and executes the script inside a
     `(function(element, args){ ... })(element, args);` block.
   - The provided script executes exactly as written; braces are no longer
     escaped so arbitrary JavaScript bodies work, including nested functions.
4. After execution the attribute is removed and any exception is surfaced via
   the status line and the browser console.

Script API
----------

- `element` – the hinted DOM element.
- `args` – a list of strings taken from the binding following the filename.

Example (`~/.config/qutebrowser/js/focus.js`):

```javascript
if (document.activeElement && document.activeElement !== element) {
    document.activeElement.blur();
}
element.focus({preventScroll: true});
```

Bind it with:

```python
config.bind('<Ctrl-f>', 'hint all javascript focus.js')
```

Implementation Notes
--------------------

- Core code lives in `qutebrowser/browser/hints.py` (`HintActions.call_javascript`).
- DOM storage/exposure relies on `webelem.js`; a new helper `get(id)` was
  added to keep the element table accessible from the injected wrapper.
- The identifier attribute name is defined as `_HINT_JS_ATTR` to keep it
  consistent and easy to audit.
- The wrapper executes in the main JS world for compatibility with page APIs
  (e.g. `focus`).
- `python3 -m compileall qutebrowser/browser/hints.py` was run to ensure the
  module compiles.

Limitations / Future Work
-------------------------

- Scripts run with the page's permissions; consider CSP interactions for
  advanced scenarios (e.g. network fetch APIs could still be blocked by the
  site).
- Error reporting is currently limited to console warnings/errors and status
  bar messages.

---

**Note for AI agents**: If you make changes that affect the accuracy of this document, please update it accordingly.
