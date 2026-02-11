dwm persist integration — _DWM_SAVE_ARGV registration
======================================================

Qutebrowser registers itself with dwm's persist mode so it can be
restored across dwm restarts. This is done by setting the
`_DWM_SAVE_ARGV` X11 property on the main window.


How it works
------------

`MainWindow._set_dwm_save_argv()` in `mainwindow/mainwindow.py` runs
`xprop` as a subprocess to set the `_DWM_SAVE_ARGV` property on the
Qt window. dwm watches for this property and writes a `.save` file
to `~/.runtime/dwm/<tag>/` containing the restore command.

### Registered argv

    qutebrowser --basedir /home/yeyito/.runtime/qutebrowser-yeyito

Built from the literal string `"qutebrowser"` (the launcher script in
`~/.local/bin/qutebrowser`) plus `objects.args.basedir` if set.

**Important**: `sys.argv[0]` is NOT used because the launcher runs
`python -m qutebrowser`, making `sys.argv[0]` resolve to the venv
`__main__.py` path, which is not a valid restore command.


### xprop invocation

    xprop -id <wid> -f _DWM_SAVE_ARGV 8u -set _DWM_SAVE_ARGV "<argv>"

- `-f ... 8u` sets the property type as `UTF8_STRING`
- `<wid>` comes from `int(self.winId())` — the X11 window ID


Guards
------

- **First window only** (`win_id == 0`) — qutebrowser uses IPC to
  deduplicate instances, so only the first window matters. Session
  autosave restores all windows/tabs from the first instance.

- **X11 only** — skipped on Wayland via `qtutils.is_wayland()`.

- **Once per window** — `showEvent()` uses a `_dwm_save_registered`
  flag so the xprop call fires exactly once on first show.

- **try/except** — failures logged at debug level, never crash.


Hook point
----------

Called from `showEvent()` after `super().showEvent(e)`. This is when
the Qt window has a valid native window ID (`winId()`).

Note: there is a race condition between when xprop sets the property
and when dwm calls `XSelectInput(PropertyChangeMask)` in `manage()`.
dwm handles this by also checking for existing `_DWM_SAVE_ARGV` in
`manage()` right after `XSelectInput()`, so properties set before
dwm starts listening are still picked up.


Session restore
---------------

dwm restores qutebrowser by running the saved argv. Qutebrowser's
built-in session autosave (`data/sessions/`) handles restoring all
tabs and windows — no `--restore` flag or URL args needed in the
save argv.


Files
-----

- `qutebrowser/mainwindow/mainwindow.py` — `_set_dwm_save_argv()`,
  `showEvent()`
- dwm side: `reference/persist-mode.md` in `~/Config/dwm/`
