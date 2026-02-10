# Adding a New QWebEngineSettings Attribute

This documents the complete pipeline for adding a new boolean setting that flows from Python through Qt C++ through Chromium's multi-process IPC to Blink's renderer. There are **10 mandatory touch points** across 9 files. Missing any one of them causes silent failures that are expensive to debug.

## Why This Is Hard

Chromium is multi-process. The **browser process** (where Qt/Python lives) and the **renderer process** (where Blink/style resolution lives) are separate OS processes. They communicate via **Mojo IPC**. A setting change in the browser process must be:

1. Stored in the Qt settings layer
2. Written into a `WebPreferences` C++ struct
3. **Serialized** into a Mojo IPC message (wire format)
4. **Deserialized** in the renderer process
5. Applied to Blink's internal `Settings` object
6. Read by the consumer code (e.g., style resolver)

If you add the setting to the struct but forget the Mojo serialization (steps 3-4), the value will be correct in the browser process but the renderer will always see the default value. There are no compile errors, no runtime errors, no warnings. The only symptom is that the setting "doesn't work."

## The 10 Touch Points

Below is every file that must be modified, in pipeline order. Use `ElementShaderEnabled` / `element_shader_enabled` as the reference pattern — search for the neighboring entry (`ForceDarkMode` / `force_dark_mode_enabled`) to find the right insertion point in each file.

### Overview

```
Python (setAttribute)
  │
  ▼
[1] SIP bindings enum          ← pyqt6-webengine/sip/.../qwebenginesettings.sip
  │
  ▼
[2] C++ enum                   ← qtwebengine/src/core/api/qwebenginesettings.h
  │
  ▼
[3] Default value              ← qtwebengine/src/core/web_engine_settings.cpp (initDefaults)
  │
  ▼
[4] Qt → WebPreferences map    ← qtwebengine/src/core/web_engine_settings.cpp (applySettingsToWebPreferences)
  │
  ▼
[5] WebPreferences struct      ← chromium/.../web_preferences.h
  │
  ▼
[6] Mojom IPC definition       ← chromium/.../web_preferences.mojom        ← EASY TO FORGET
  │
  ▼
[7] Mojom serialize (getter)   ← chromium/.../web_preferences_mojom_traits.h  ← EASY TO FORGET
  │
  ▼
[8] Mojom deserialize          ← chromium/.../web_preferences_mojom_traits.cc ← EASY TO FORGET
  │
  ▼  (IPC boundary — browser process → renderer process)
  │
[9] Apply to Blink Settings    ← chromium/.../web_view_impl.cc
  │
  ▼
[10] Blink Settings schema     ← chromium/.../settings.json5
  │
  ▼
Consumer code (e.g. style_resolver.cc reads settings->GetYourSettingName())
```

### [1] SIP Bindings Enum

**File:** `pyqt6-webengine/sip/QtWebEngineCore/qwebenginesettings.sip`

This is what Python sees. The enum value must match the C++ enum (same position = same integer value). Add at the end, before the closing `};`.

```sip
%If (QtWebEngine_6_10_0 -)
        BackForwardCacheEnabled,
%End
        ElementShaderEnabled,        ← add here (no version guard = always available)
    };
```

**Rule:** The name must exactly match the C++ enum name. Position must match (enums are integer-ordered). If you add it outside a `%If` version guard, it's always available. If inside a guard, it's only available on that Qt version or later.

### [2] C++ Enum

**File:** `qtwebengine/src/core/api/qwebenginesettings.h`

The public Qt C++ API enum. Add at the end, before the closing `};`. Must match the SIP file exactly.

```cpp
        BackForwardCacheEnabled,
        ElementShaderEnabled,        ← add here
    };
```

### [3] Default Value

**File:** `qtwebengine/src/core/web_engine_settings.cpp` — `initDefaults()` function

Sets the default value when no page or user code has called `setAttribute()`. Search for `s_defaultAttributes.insert` block.

```cpp
        s_defaultAttributes.insert(QWebEngineSettings::BackForwardCacheEnabled, false);
        s_defaultAttributes.insert(QWebEngineSettings::ElementShaderEnabled, false);  ← add here
```

### [4] Qt to WebPreferences Mapping

**File:** `qtwebengine/src/core/web_engine_settings.cpp` — `applySettingsToWebPreferences()` function

Reads the Qt attribute value and writes it into the `WebPreferences` struct that will be sent over IPC. Search for `prefs->force_dark_mode_enabled` as a neighbor.

```cpp
    prefs->force_dark_mode_enabled = testAttribute(QWebEngineSettings::ForceDarkMode);
    prefs->element_shader_enabled = testAttribute(QWebEngineSettings::ElementShaderEnabled);  ← add here
```

### [5] WebPreferences Struct Field

**File:** `chromium/third_party/blink/public/common/web_preferences/web_preferences.h`

The C++ struct that holds all preferences. This is the "data bag" that gets serialized. Search for `force_dark_mode_enabled`.

```cpp
  // Enable forcibly modifying content rendering to result in a light on dark
  // color scheme.
  bool force_dark_mode_enabled = false;

  // Enable the element shader (custom style transforms in style resolution).
  bool element_shader_enabled = false;    ← add here (with default value)
```

**Rule:** The default value here should match what you set in step [3]. This default is used if the mojom message doesn't include the field (backwards compatibility).

### [6] Mojom IPC Wire Format

**File:** `chromium/third_party/blink/public/mojom/webpreferences/web_preferences.mojom`

**THIS IS THE STEP MOST LIKELY TO BE FORGOTTEN.** Defines the field in the IPC wire format. Without this, the value is never sent from browser to renderer. Search for `force_dark_mode_enabled`.

```mojom
  // Enable forcibly modifying content rendering to result in a light on dark
  // color scheme.
  bool force_dark_mode_enabled;

  // Enable the element shader (custom style transforms in style resolution).
  bool element_shader_enabled;           ← add here (no default value in mojom)
```

**Rule:** Field order in the mojom struct must match the order in `web_preferences.h`. No default values in `.mojom` files — that's a Mojo convention.

### [7] Mojom Serialization Getter

**File:** `chromium/third_party/blink/public/common/web_preferences/web_preferences_mojom_traits.h`

Tells Mojo how to read the field from the C++ struct for serialization. This is a static method in the `StructTraits` class. Search for `force_dark_mode_enabled`.

```cpp
  static bool force_dark_mode_enabled(
      const blink::web_pref::WebPreferences& r) {
    return r.force_dark_mode_enabled;
  }

  static bool element_shader_enabled(                    ← add here
      const blink::web_pref::WebPreferences& r) {
    return r.element_shader_enabled;
  }
```

**Rule:** Method name must exactly match the mojom field name. Return type must match the mojom type.

### [8] Mojom Deserialization

**File:** `chromium/third_party/blink/common/web_preferences/web_preferences_mojom_traits.cc`

Reads the value from the incoming IPC message and writes it into the C++ struct on the renderer side. This is inside the `StructTraits::Read()` method. Search for `force_dark_mode_enabled`.

```cpp
  out->force_dark_mode_enabled = data.force_dark_mode_enabled();
  out->element_shader_enabled = data.element_shader_enabled();   ← add here
```

**Rule:** `data.field_name()` is auto-generated from the mojom definition. If the mojom field doesn't exist, this line won't compile (which is actually helpful — it catches a missing step [6]).

### [9] Apply to Blink Settings

**File:** `chromium/third_party/blink/renderer/core/exported/web_view_impl.cc`

Takes the deserialized `WebPreferences` struct and applies each field to Blink's internal `Settings` object. This is in the `ApplyPreferences()` function. Search for `SetForceDarkModeEnabled`.

```cpp
  settings->SetForceDarkModeEnabled(prefs.force_dark_mode_enabled);
  settings->SetElementShaderEnabled(prefs.element_shader_enabled);   ← add here
```

**Rule:** The setter name (`SetElementShaderEnabled`) is auto-generated from `settings.json5` (step 10). If the setting isn't in `settings.json5`, the setter won't exist and this line won't compile.

### [10] Blink Settings Schema

**File:** `chromium/third_party/blink/renderer/core/frame/settings.json5`

Defines the setting in Blink's internal settings system. This auto-generates the getter (`GetElementShaderEnabled()`) and setter (`SetElementShaderEnabled()`) methods.

```json5
    //
    // Element shader
    //
    {
      name: "elementShaderEnabled",      ← camelCase name
      initial: false,                     ← default value in Blink
      invalidate: ["Style", "Paint"],    ← what to invalidate on change
    },
```

**`invalidate` options:**
- `"Style"` — triggers style recalculation (needed if the setting affects CSS/computed styles)
- `"Paint"` — triggers repaint
- `"Layout"` — triggers relayout
- `[]` — no automatic invalidation

**Rule:** `name` is camelCase. The auto-generated getter/setter are `Get`/`Set` + PascalCase version. `initial` should match the default in `web_preferences.h`.

## Reading the Setting in Consumer Code

After all 10 steps, the setting is available in any Blink renderer code via the document's `Settings` object:

```cpp
const Settings* settings = state.GetDocument().GetSettings();
if (!settings || !settings->GetElementShaderEnabled()) {
  return;  // setting is off or unavailable
}
```

The getter `GetElementShaderEnabled()` is auto-generated from `settings.json5`.

## Debugging Checklist

If a setting "doesn't work" (value appears correct in Python but has no effect in the renderer):

1. **Add `fprintf(stderr, ...)` at each pipeline stage** to trace the value:
   - After `testAttribute()` in `web_engine_settings.cpp` (browser process)
   - After `prefs.your_field` in `web_view_impl.cc` (renderer process)
   - In the consumer code (renderer process)

2. **If browser shows correct value but renderer shows default:**
   - You forgot the mojom layer (steps 6, 7, 8)
   - The value is being set correctly in the browser process but never serialized over IPC

3. **If renderer shows correct value but feature doesn't work:**
   - Check the consumer code (e.g., `style_resolver.cc`)
   - Check `settings.json5` invalidation — if `invalidate` is empty, changing the setting won't trigger a restyle

4. **If Python throws `AttributeError: no attribute 'YourSetting'`:**
   - The SIP bindings (step 1) weren't rebuilt, or the enum is missing
   - Run `./install.sh --dirty` to rebuild Phase 4

## File Paths Quick Reference

All Chromium paths are relative to `qtwebengine/src/3rdparty/chromium/`:

| Step | File | Short description |
|------|------|-------------------|
| 1 | `pyqt6-webengine/sip/QtWebEngineCore/qwebenginesettings.sip` | Python-visible enum |
| 2 | `qtwebengine/src/core/api/qwebenginesettings.h` | C++ public enum |
| 3 | `qtwebengine/src/core/web_engine_settings.cpp` (`initDefaults`) | Default value |
| 4 | `qtwebengine/src/core/web_engine_settings.cpp` (`applySettingsToWebPreferences`) | Qt → struct mapping |
| 5 | `third_party/blink/public/common/web_preferences/web_preferences.h` | Struct field |
| 6 | `third_party/blink/public/mojom/webpreferences/web_preferences.mojom` | IPC wire format |
| 7 | `third_party/blink/public/common/web_preferences/web_preferences_mojom_traits.h` | Serialize getter |
| 8 | `third_party/blink/common/web_preferences/web_preferences_mojom_traits.cc` | Deserialize |
| 9 | `third_party/blink/renderer/core/exported/web_view_impl.cc` | Apply to Blink |
| 10 | `third_party/blink/renderer/core/frame/settings.json5` | Blink schema |

## Common Mistakes

1. **Forgetting the mojom layer** (steps 6-8): No compile error, no runtime error. The setting just silently stays at its default in the renderer. This is the #1 mistake because the `web_preferences.h` struct and the mojom definition look similar but serve different purposes — the struct is a C++ data bag, the mojom is the IPC serialization schema.

2. **Field order mismatch between `.h` and `.mojom`**: The mojom field order should match the struct order. While Mojo doesn't strictly require this (it uses field names, not positions), keeping them in sync makes the code easier to maintain and audit.

3. **Enum position mismatch between `.sip` and `.h`**: SIP enums are positional (they map to integers). If the SIP enum has entries in a different order than the C++ enum, the wrong integer value gets passed to C++. Always keep them in sync.

4. **Forgetting to rebuild both C++ and SIP bindings**: If you add a new enum value, you need both `./install.sh --dirty` (rebuilds C++) and Phase 4 (rebuilds SIP bindings). The `--dirty` flag handles both.

5. **Wrong `invalidate` in `settings.json5`**: If you set `invalidate: []`, changing the setting won't trigger any visual update. For anything that affects rendering, use at least `["Style", "Paint"]`.

---

**Note for AI agents**: When asked to add a new QWebEngineSettings attribute, work through ALL 10 steps. Do not stop at step 5 (the struct field). The mojom steps (6-8) are required for the value to cross the process boundary. Use `force_dark_mode_enabled` as the neighboring anchor when searching for insertion points.
