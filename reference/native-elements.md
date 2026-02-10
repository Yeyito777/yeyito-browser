# Native Elements (NativeTheme)

Chromium paints native form controls (checkboxes, radios, buttons, text fields, sliders, progress bars, select dropdowns, scrollbars) through its `NativeTheme` system, bypassing CSS entirely. These elements are drawn directly via Skia canvas calls.

## Architecture

### Class Hierarchy

```
ui::NativeTheme (pure virtual base)
  └── ui::NativeThemeBase (default paint implementations)
        ├── ui::NativeThemeAura (Aura overrides — scrollbars, menus)
        └── ui::NativeThemeFluent (Fluent overrides — scrollbars only)
```

All files live in `qtwebengine/src/3rdparty/chromium/ui/native_theme/`.

### Which Theme Is Active?

```cpp
// native_theme_aura.cc — the decision point for web content
NativeTheme* NativeTheme::GetInstanceForWeb() {
  if (IsFluentScrollbarEnabled()) {
    return NativeThemeFluent::web_instance();
  }
  return NativeThemeAura::web_instance();
}
```

- `IsFluentScrollbarEnabled()` is true on Windows/Linux when the `kFluentScrollbar` or `kFluentOverlayScrollbar` feature flags are enabled
- Fluent only overrides **scrollbar** paint methods — all other controls fall through to `NativeThemeBase`
- Aura overrides scrollbars + menus — other controls also fall through to `NativeThemeBase`

**Implication**: To modify non-scrollbar controls (checkboxes, radios, buttons, text fields, sliders, progress bars), you modify `NativeThemeBase` directly. Both Aura and Fluent inherit these methods.

### Override Matrix

Which class actually paints each Part:

| Part | NativeThemeBase | NativeThemeAura | NativeThemeFluent |
|------|:-:|:-:|:-:|
| `kCheckbox` | **paints** | inherits | inherits |
| `kRadio` | **paints** | inherits | inherits |
| `kPushButton` | **paints** | inherits | inherits |
| `kTextField` | **paints** | inherits | inherits |
| `kMenuList` | **paints** | inherits | inherits |
| `kSliderTrack` | **paints** | inherits | inherits |
| `kSliderThumb` | **paints** | inherits | inherits |
| `kProgressBar` | **paints** | inherits | inherits |
| `kInnerSpinButton` | **paints** | inherits | inherits |
| `kMenuPopupBackground` | default | **overrides** | inherits |
| `kMenuItemBackground` | no-op | **overrides** | inherits |
| `kMenuPopupSeparator` | **paints** | inherits | inherits |
| `kScrollbar*Arrow` | default | **overrides** | **overrides** |
| `kScrollbar*Thumb` | default | **overrides** | **overrides** |
| `kScrollbar*Track` | default | **overrides** | **overrides** |
| `kScrollbarCorner` | no-op | **overrides** | **overrides** |
| `kFrameTopArea` | **paints** (Linux) | inherits | inherits |
| `kTabPanelBackground` | NOTIMPLEMENTED | — | — |
| `kTrackbar*` | NOTIMPLEMENTED | — | — |

---

## Multi-Process Architecture

**This is the most important thing to understand.** Chromium is multi-process. The browser process and renderer process(es) each have their own `NativeTheme` singleton instances with **separate memory spaces**.

Native elements can be painted in **either** process:
- **Renderer process**: Most web page form controls
- **Browser process**: Some UI elements, DevTools scrollbars

### The Static Flag Pattern

Any toggle flag on `NativeTheme` must be **static** (shared across all instances within a process) and set from **both** processes:

```cpp
// native_theme.h — declaration
static bool element_shader_enabled() { return element_shader_enabled_; }
static void set_element_shader_enabled(bool enabled) {
  element_shader_enabled_ = enabled;
}
// ...
static bool element_shader_enabled_;

// native_theme.cc — definition
bool NativeTheme::element_shader_enabled_ = false;
```

Set from the **browser process** (`web_engine_settings.cpp:390-391`):
```cpp
prefs->element_shader_enabled = testAttribute(QWebEngineSettings::ElementShaderEnabled);
ui::NativeTheme::set_element_shader_enabled(prefs->element_shader_enabled);
```

Set from the **renderer process** (`web_view_impl.cc:1813-1814`):
```cpp
settings->SetElementShaderEnabled(prefs.element_shader_enabled);
ui::NativeTheme::set_element_shader_enabled(prefs.element_shader_enabled);
```

The renderer-process path is critical because the preferences pipeline (`WebPreferences` → Mojom IPC → Blink `Settings`) already carries the boolean across the process boundary. `web_view_impl.cc` is the renderer-side endpoint where deserialized prefs are applied.

**If you add a new flag**: Follow the same pattern — static member on `NativeTheme`, set from both `web_engine_settings.cpp` and `web_view_impl.cc`. See `reference/adding-a-web-setting.md` for the full 9-file pipeline if you need a new WebPreferences field.

### Include Guard Gotcha

In `web_view_impl.cc`, the `#include "ui/native_theme/native_theme.h"` was originally behind `#if BUILDFLAG(IS_CHROMEOS)`. We added an unconditional include at line 190. If you need other `ui/` headers in Blink files, watch for similar platform guards.

---

## Part Enum

All native element types that can be painted (`native_theme.h:62-106`):

```cpp
enum Part {
  kCheckbox,
  kFrameTopArea,           // Linux only
  kInnerSpinButton,
  kMenuList,               // <select> dropdown
  kMenuPopupBackground,
  kMenuCheck,              // Windows only
  kMenuCheckBackground,    // Windows only
  kMenuPopupArrow,         // Windows only
  kMenuPopupGutter,        // Windows only
  kMenuPopupSeparator,
  kMenuItemBackground,
  kProgressBar,
  kPushButton,
  kRadio,
  kScrollbarDownArrow,
  kScrollbarLeftArrow,
  kScrollbarRightArrow,
  kScrollbarUpArrow,
  kScrollbarHorizontalThumb,
  kScrollbarVerticalThumb,
  kScrollbarHorizontalTrack,
  kScrollbarVerticalTrack,
  kScrollbarHorizontalGripper,  // no-op
  kScrollbarVerticalGripper,    // no-op
  kScrollbarCorner,
  kSliderTrack,
  kSliderThumb,
  kTabPanelBackground,     // NOTIMPLEMENTED
  kTextField,
  kTrackbarThumb,          // NOTIMPLEMENTED
  kTrackbarTrack,          // NOTIMPLEMENTED
  kWindowResizeGripper,    // NOTIMPLEMENTED
  kMaxPart,
};
```

## State Enum

```cpp
enum State {
  kDisabled = 0,
  kHovered  = 1,
  kNormal   = 2,
  kPressed  = 3,
  kNumStates = 4,
};
```

---

## Paint Dispatch

The main `Paint()` method (`native_theme_base.cc:235-342`) dispatches on `Part`:

```
Part                          → Paint Method                    → ExtraParams Type
─────────────────────────────────────────────────────────────────────────────────
kCheckbox                     → PaintCheckbox()                 → ButtonExtraParams
kRadio                        → PaintRadio()                    → ButtonExtraParams
kPushButton                   → PaintButton()                   → ButtonExtraParams
kTextField                    → PaintTextField()                → TextFieldExtraParams
kMenuList                     → PaintMenuList()                 → MenuListExtraParams
kProgressBar                  → PaintProgressBar()              → ProgressBarExtraParams
kSliderTrack                  → PaintSliderTrack()              → SliderExtraParams
kSliderThumb                  → PaintSliderThumb()              → SliderExtraParams
kInnerSpinButton              → PaintInnerSpinButton()          → InnerSpinButtonExtraParams
kMenuPopupBackground          → PaintMenuPopupBackground()      → MenuBackgroundExtraParams
kMenuItemBackground           → PaintMenuItemBackground()       → MenuItemExtraParams
kMenuPopupSeparator           → PaintMenuSeparator()            → MenuSeparatorExtraParams
kFrameTopArea                 → PaintFrameTopArea()             → FrameTopAreaExtraParams
kScrollbar{Down,Up,L,R}Arrow → PaintArrowButton()              → ScrollbarArrowExtraParams
kScrollbar{H,V}Thumb         → PaintScrollbarThumb()           → ScrollbarThumbExtraParams
kScrollbar{H,V}Track         → PaintScrollbarTrack()           → ScrollbarTrackExtraParams
kScrollbarCorner              → PaintScrollbarCorner()          → ScrollbarTrackExtraParams
```

---

## ExtraParams Reference

Each Part receives a typed ExtraParams struct via `absl::variant`. Here are the fields for the controls you're most likely to modify:

### ButtonExtraParams (checkbox, radio, push button)
```cpp
struct ButtonExtraParams {
  bool checked = false;           // Checkbox/radio is checked
  bool indeterminate = false;     // Indeterminate state (checkbox only)
  bool is_default = false;        // Default button
  bool is_focused = false;        // Has keyboard focus
  bool has_border = false;        // Has visible border
  SkColor background_color = gfx::kPlaceholderColor;
  float zoom = 0;
};
```

### TextFieldExtraParams
```cpp
struct TextFieldExtraParams {
  bool is_text_area = false;      // <textarea> vs <input>
  bool is_listbox = false;        // <select multiple>
  SkColor background_color = gfx::kPlaceholderColor;
  bool is_read_only = false;
  bool is_focused = false;
  bool fill_content_area = false;
  bool draw_edges = false;
  bool has_border = false;
  bool auto_complete_active = false;
  float zoom = 0;
};
```

### SliderExtraParams
```cpp
struct SliderExtraParams {
  bool vertical = false;
  bool in_drag = false;
  int thumb_x = 0;               // Thumb position (for value bar calc)
  int thumb_y = 0;
  float zoom = 0;
  bool right_to_left = false;
};
```

### ProgressBarExtraParams
```cpp
struct ProgressBarExtraParams {
  double animated_seconds = 0;    // For indeterminate animation
  bool determinate = false;       // Determinate vs indeterminate
  int value_rect_x = 0;          // Filled portion rect
  int value_rect_y = 0;
  int value_rect_width = 0;
  int value_rect_height = 0;
  float zoom = 0;
  bool is_horizontal = false;
};
```

### MenuListExtraParams (<select> dropdown)
```cpp
struct MenuListExtraParams {
  bool has_border = false;
  bool has_border_radius = false;
  int arrow_x = 0;
  int arrow_y = 0;
  int arrow_size = 0;
  ArrowDirection arrow_direction = ArrowDirection::kDown;
  SkColor arrow_color = gfx::kPlaceholderColor;
  SkColor background_color = gfx::kPlaceholderColor;
  float zoom = 0;
};
```

### InnerSpinButtonExtraParams
```cpp
struct InnerSpinButtonExtraParams {
  bool spin_up = false;           // Up vs down button
  bool read_only = false;
  SpinArrowsDirection spin_arrows_direction = SpinArrowsDirection::kUpDown;
};
```

### ScrollbarArrowExtraParams
```cpp
struct ScrollbarArrowExtraParams {
  bool is_hovering = false;
  float zoom = 0;
  bool needs_rounded_corner = false;
  bool right_to_left = false;
  std::optional<SkColor> thumb_color;   // CSS override
  std::optional<SkColor> track_color;   // CSS override
};
```

### ScrollbarTrackExtraParams
```cpp
struct ScrollbarTrackExtraParams {
  bool is_upper = false;
  int track_x, track_y, track_width, track_height;
  std::optional<SkColor> track_color;   // CSS override
};
```

### ScrollbarThumbExtraParams
```cpp
struct ScrollbarThumbExtraParams {
  bool is_hovering = false;
  std::optional<SkColor> thumb_color;   // CSS override
  std::optional<SkColor> track_color;   // CSS override
  bool is_thumb_minimal_mode = false;   // Overlay minimal mode
  bool is_web_test = false;
};
```

---

## Color System

### Three-Level Color Resolution

When a paint method needs a color, the resolution order is:

1. **ColorProvider** (if available) — calls `GetControlColorFromColorProvider()` which maps to `kColorWebNativeControl*` color IDs
2. **Dark mode hardcoded** — `GetDarkModeControlColor()` if `ColorScheme::kDark`
3. **Light mode hardcoded** — fallback switch statement in `GetControlColor()`

### ControlColorId Enum (`native_theme_base.h:48-92`)

These are the abstract color IDs used by paint methods:

**Borders:**
`kBorder`, `kDisabledBorder`, `kHoveredBorder`, `kPressedBorder`

**Button Borders:**
`kButtonBorder`, `kButtonDisabledBorder`, `kButtonHoveredBorder`, `kButtonPressedBorder`

**Accents (checked state, progress bar fill):**
`kAccent`, `kDisabledAccent`, `kHoveredAccent`, `kPressedAccent`

**Fills (background of controls):**
`kFill`, `kDisabledFill`, `kHoveredFill`, `kPressedFill`

**Button Fills:**
`kButtonFill`, `kButtonDisabledFill`, `kButtonHoveredFill`, `kButtonPressedFill`

**Sliders:**
`kSlider`, `kDisabledSlider`, `kHoveredSlider`, `kPressedSlider`

**Scrollbar:**
`kScrollbarArrow`, `kScrollbarArrowHovered`, `kScrollbarArrowPressed`,
`kScrollbarArrowBackground`, `kScrollbarArrowBackgroundHovered`, `kScrollbarArrowBackgroundPressed`,
`kScrollbarTrack`, `kScrollbarThumb`, `kScrollbarThumbHovered`, `kScrollbarThumbPressed`, `kScrollbarThumbInactive`,
`kScrollbarCornerControlColorId`

**Other:**
`kBackground`, `kDisabledBackground`, `kAutoCompleteBackground`, `kLightenLayer`, `kProgressValue`

### Convenience Methods

Instead of calling `GetControlColor()` directly, paint methods typically use state-aware helpers:

```cpp
ControlsAccentColorForState(State, ColorScheme, ColorProvider*)    // kAccent/kHovered.../kPressed...
ControlsSliderColorForState(State, ColorScheme, ColorProvider*)    // kSlider/kHovered.../kPressed...
ControlsBorderColorForState(State, ColorScheme, ColorProvider*)    // kBorder/kHovered.../kPressed...
ControlsFillColorForState(State, ColorScheme, ColorProvider*)      // kFill/kHovered.../kPressed...
ButtonBorderColorForState(State, ColorScheme, ColorProvider*)      // kButtonBorder/kHovered.../kPressed...
ButtonFillColorForState(State, ColorScheme, ColorProvider*)        // kButtonFill/kHovered.../kPressed...
ControlsBackgroundColorForState(State, ColorScheme, ColorProvider*) // kBackground/kDisabledBackground
GetArrowColor(State, ColorScheme, ColorProvider*)                  // kScrollbarArrow/Hovered/Pressed
```

Each maps the current `State` to the appropriate `ControlColorId` variant (e.g., `kHovered` → `kHoveredAccent`).

### Hardcoded Light Mode Colors (from `GetControlColor()`)

| ControlColorId | Light Mode | Hex |
|---|---|---|
| kBorder | `(0x76, 0x76, 0x76)` | #767676 |
| kHoveredBorder | `(0x4F, 0x4F, 0x4F)` | #4F4F4F |
| kAccent | `(0x00, 0x75, 0xFF)` | #0075FF |
| kHoveredAccent | `(0x00, 0x5C, 0xC8)` | #005CC8 |
| kFill | `(0xEF, 0xEF, 0xEF)` | #EFEFEF |
| kBackground | `SK_ColorWHITE` | #FFFFFF |
| kScrollbarThumb | `ARGB(0x33, 0, 0, 0)` | 20% black |
| kScrollbarTrack | `(0xF1, 0xF1, 0xF1)` | #F1F1F1 |

### Hardcoded Dark Mode Colors (from `GetDarkModeControlColor()`)

| ControlColorId | Dark Mode | Hex |
|---|---|---|
| kAccent | `(0x99, 0xC8, 0xFF)` | #99C8FF |
| kHoveredAccent | `(0xD1, 0xE6, 0xFF)` | #D1E6FF |
| kFill | `(0x3B, 0x3B, 0x3B)` | #3B3B3B |
| kBorder | `(0x85, 0x85, 0x85)` | #858585 |
| kScrollbarThumb | `ARGB(0x33, 0xFF, 0xFF, 0xFF)` | 20% white |
| kScrollbarTrack | `(0x42, 0x42, 0x42)` | #424242 |

---

## Paint Method Anatomy

Every paint method follows the same pattern. Here's a template for how to add shader-conditional painting:

### Template: Modifying a Paint Method

```cpp
void NativeThemeBase::PaintSomething(
    cc::PaintCanvas* canvas,
    const ColorProvider* color_provider,
    State state,
    const gfx::Rect& rect,
    const SomethingExtraParams& extra,
    ColorScheme color_scheme,
    const std::optional<SkColor>& accent_color) const {

  // ---- Shader path ----
  if (element_shader_enabled()) {
    cc::PaintFlags flags;
    flags.setAntiAlias(true);

    // Fill background
    flags.setColor(SkColorSetRGB(0x00, 0x05, 0x0f));
    flags.setStyle(cc::PaintFlags::kFill_Style);
    SkRect skrect = gfx::RectToSkRect(rect);
    canvas->drawRect(skrect, flags);

    // Draw border
    flags.setColor(SkColorSetRGB(0x1d, 0x9b, 0xf0));
    flags.setStyle(cc::PaintFlags::kStroke_Style);
    flags.setStrokeWidth(1);
    canvas->drawRect(skrect, flags);

    return;  // Skip the original code path
  }

  // ---- Original Chromium code below (unchanged) ----
  // ... existing implementation ...
}
```

### Key Points

1. **Check `element_shader_enabled()` first** — early return after shader painting
2. **The `return` is critical** — without it, the original code runs too
3. **Preserve the original code verbatim** — it's the fallback when shader is off
4. **Anti-aliasing**: Set `flags.setAntiAlias(true)` for rounded shapes, `false` for pixel-aligned rects
5. **Inset for strokes**: Skia strokes are centered on the path. Inset by `strokeWidth / 2` to keep inside bounds:
   ```cpp
   SkRect inset = skrect;
   inset.inset(0.5f, 0.5f);  // For 1px stroke
   canvas->drawRect(inset, flags);
   ```

---

## Skia Drawing Primitives

All paint methods use `cc::PaintCanvas` (wrapper around Skia) and `cc::PaintFlags` (wrapper around `SkPaint`).

### cc::PaintFlags

```cpp
cc::PaintFlags flags;

// Style
flags.setStyle(cc::PaintFlags::kFill_Style);       // Fill
flags.setStyle(cc::PaintFlags::kStroke_Style);      // Stroke/outline
flags.setStyle(cc::PaintFlags::kStrokeAndFill_Style);

// Color
flags.setColor(SkColorSetRGB(0x00, 0x05, 0x0f));   // RGB
flags.setColor(SkColorSetARGB(0x80, 0x00, 0x05, 0x0f)); // RGBA
flags.setColor(SK_ColorWHITE);                      // Predefined
flags.setColor(SK_ColorBLACK);
flags.setColor(SK_ColorTRANSPARENT);

// Stroke
flags.setStrokeWidth(1.0f);
flags.setStrokeCap(cc::PaintFlags::kRound_Cap);

// Anti-aliasing
flags.setAntiAlias(true);

// Blend mode
flags.setBlendMode(SkBlendMode::kSrc);             // Replace
flags.setBlendMode(SkBlendMode::kSrcOver);          // Normal alpha blending (default)
```

### Canvas Operations

```cpp
// Rectangles
canvas->drawRect(SkRect, flags);                    // Axis-aligned rectangle
canvas->drawIRect(SkIRect, flags);                  // Integer rectangle
canvas->drawRoundRect(SkRect, rx, ry, flags);       // Rounded rectangle
canvas->drawRRect(SkRRect, flags);                  // Rounded rectangle (complex)

// Paths (arrows, checkmarks, custom shapes)
SkPath path;
path.moveTo(x, y);
path.lineTo(x, y);
path.close();
canvas->drawPath(path, flags);

// Canvas fill
canvas->drawColor(SkColor4f::FromColor(color), SkBlendMode::kSrc);

// Clipping
canvas->save();
canvas->clipRect(SkRect);
canvas->clipRRect(SkRRect);
// ... draw inside clip ...
canvas->restore();

// Lines (used by scrollbar helpers)
canvas->drawLine(x1, y1, x2, y2, flags);
```

### Rect Conversions

```cpp
// gfx::Rect → Skia types
SkRect skrect = gfx::RectToSkRect(rect);            // Float rect
SkIRect skrect = gfx::RectToSkIRect(rect);          // Integer rect

// Make rounded rect
SkRRect rrect;
rrect.setRectXY(skrect, radius, radius);

// Inset rect (shrink)
skrect.inset(dx, dy);
SkRect inset = skrect.makeInset(dx, dy);            // Non-mutating
```

### Color Construction

```cpp
SkColorSetRGB(0x1d, 0x9b, 0xf0);                   // #1d9bf0
SkColorSetARGB(0x80, 0x00, 0x05, 0x0f);             // 50% alpha #00050f
SK_ColorWHITE                                        // #ffffff
SK_ColorBLACK                                        // #000000
SK_ColorTRANSPARENT                                  // fully transparent
SkColorGetR(color)                                    // Extract red
SkColorGetA(color)                                    // Extract alpha
SkColorSetA(color, alpha)                             // Set alpha on existing color
```

---

## Constants

### NativeThemeBase Constants (`native_theme_base.cc`)

```cpp
const SkScalar kBorderWidth = 1.f;
const SkScalar kSliderTrackHeight = 8.f;
const SkScalar kSliderThumbBorderWidth = 1.f;
const SkScalar kSliderThumbBorderHoveredWidth = 1.f;
const SkScalar kTrackBlockRatio = 8.0f / 16;       // Progress bar
const SkScalar kMenuListArrowStrokeWidth = 2.f;
const int kSliderThumbSize = 16;
const double kAccentLuminanceAdjust = 0.11;         // Hover/press luminance shift
```

### NativeThemeFluent Constants (`native_theme_constants_fluent.h`)

```cpp
constexpr int kFluentScrollbarThickness = 15;
constexpr int kFluentScrollbarThumbThickness = 9;
constexpr int kFluentScrollbarPartsRadius = 999;        // Full round
constexpr int kFluentScrollbarMinimalThumbLength = 17;
constexpr int kFluentScrollbarButtonSideLength = 18;
constexpr int kFluentScrollbarArrowRectLength = 9;
constexpr int kFluentScrollbarPressedArrowRectLength = 8;
constexpr int kFluentScrollbarArrowOffset = 1;
constexpr float kFluentScrollbarTrackOutlineWidth = 1.0f;
constexpr int kFluentPaintedScrollbarTrackInset = 1;
```

### Border Radius Defaults (`GetBorderRadiusForPart()`)

| Part | Radius |
|------|--------|
| `kCheckbox` | 2.0f |
| `kTextField` | 2.0f |
| `kPushButton` | 2.0f |
| `kRadio` | 50% (half of width/height) |
| `kSliderThumb` | 50% (circular) |
| `kProgressBar` | 40.0f |
| `kSliderTrack` | 40.0f |
| Everything else | 0.0f |

---

## Walkthrough: How Each Control Is Painted

### Checkbox (`PaintCheckbox`, `native_theme_base.cc:666-720`)

1. Calls `PaintCheckboxRadioCommon()` → draws border and background fill
2. If `checked` or `indeterminate`: fills with accent color (`ControlsAccentColorForState()`)
3. If `indeterminate`: draws a white dash (horizontal bar)
4. If `checked`: draws a white checkmark via `SkPath` with stroke
5. Border radius: 2.0f

### Radio (`PaintRadio`, `native_theme_base.cc:808-841`)

1. Calls `PaintCheckboxRadioCommon()` with 50% border radius (circle)
2. If `checked`: draws a smaller accent-colored dot (20% inset from each side)

### Push Button (`PaintButton`, `native_theme_base.cc:843-883`)

1. If rect < 5px: fills with solid fill color, returns
2. Draws `PaintLightenLayer()` for disabled state
3. Fills rounded rect with `ButtonFillColorForState()`
4. If `has_border`: strokes rounded rect with `ButtonBorderColorForState()`
5. Border radius: 2.0f, adjusted by zoom

### Text Field (`PaintTextField`, `native_theme_base.cc:885-924`)

1. Draws `PaintLightenLayer()` for disabled state
2. If `background_color != 0`: fills with `ControlsBackgroundColorForState()` or `kAutoCompleteBackground`
3. If `has_border`: strokes with `ControlsBorderColorForState()`
4. Border radius: 2.0f, adjusted by zoom

### Menu List / Select (`PaintMenuList`, `native_theme_base.cc:926-1011`)

1. If no border-radius: delegates background/border to `PaintTextField()`
2. Draws dropdown arrow via `SkPath` (down/left/right triangle)
3. Arrow color from `extra.arrow_color`
4. Arrow stroke width: 2.0f

### Progress Bar (`PaintProgressBar`, `native_theme_base.cc:1186-1262`)

1. Draws full track with fill color, border radius 40.0f
2. Clips to track rounded rect
3. Draws value bar within clip using accent color
4. Minimum value bar size: 2px
5. Draws border stroke with 0x80 alpha (unless high contrast or dark mode)

### Slider Track (`PaintSliderTrack`, `native_theme_base.cc:1052-1109`)

1. Draws full track with `ControlsFillColorForState()`, track height 8.0f * zoom
2. Clips to value bar extent (up to thumb position)
3. Draws value bar with accent/slider color
4. Draws border stroke with 0x80 alpha

### Slider Thumb (`PaintSliderThumb`, `native_theme_base.cc:1111-1141`)

1. Fills a circle (50% radius) with accent/slider color
2. That's it — simple circle, no border in default implementation

### Inner Spin Button (`PaintInnerSpinButton`, `native_theme_base.cc:1143-1184`)

1. Draws two halves (up/down or left/right based on `spin_arrows_direction`)
2. Each half has its own hover/press state based on `spin_up`
3. Fills background, draws separator line between halves
4. Draws arrow glyph in each half

---

## Modifying Controls: Step-by-Step

### 1. Identify Which File to Edit

- **Scrollbars**: `native_theme_aura.cc` AND `native_theme_fluent.cc` (both override)
- **Menus**: `native_theme_aura.cc` (overrides popup background and item background)
- **Everything else**: `native_theme_base.cc` (checkboxes, radios, buttons, text fields, sliders, progress bars, spin buttons, menu list/select)

### 2. Add the Shader Conditional

```cpp
if (element_shader_enabled()) {
  // Your shader painting code
  return;
}
// Original code stays below, untouched
```

### 3. Handle State

Each control receives a `State` parameter. Your shader code should typically handle at least:

```cpp
if (element_shader_enabled()) {
  SkColor bg = SkColorSetRGB(0x00, 0x05, 0x0f);
  SkColor accent = SkColorSetRGB(0x1d, 0x9b, 0xf0);

  // Optionally differentiate states
  if (state == kDisabled) {
    accent = SkColorSetARGB(0x4D, 0x1d, 0x9b, 0xf0);  // 30% opacity
  }
  // ... paint ...
  return;
}
```

### 4. Build and Test

```bash
./install.sh --dirty
~/.local/bin/qutebrowser
```

Build time for `native_theme_base.cc` changes: ~1-5 minutes (single file recompile).

### 5. Test All States

Create a test HTML page:

```html
<input type="checkbox">
<input type="checkbox" checked>
<input type="checkbox" disabled>
<input type="checkbox" checked disabled>
<input type="radio">
<input type="radio" checked>
<button>Button</button>
<button disabled>Disabled</button>
<input type="text" value="Text field">
<input type="text" disabled value="Disabled">
<input type="range">
<progress value="0.5"></progress>
<progress></progress>  <!-- indeterminate -->
<select><option>Dropdown</option></select>
<input type="number">  <!-- spin button -->
```

---

## File Reference

| File | Purpose |
|------|---------|
| `native_theme.h` | Base class, Part/State/ExtraParams enums, static shader flag |
| `native_theme.cc` | Static member definitions, utility methods |
| `native_theme_base.h` | `ControlColorId` enum, all virtual paint method declarations |
| `native_theme_base.cc` | Default paint implementations for all controls, color resolution |
| `native_theme_aura.h` | Aura class definition, overridden methods |
| `native_theme_aura.cc` | Aura scrollbar + menu overrides, `GetInstanceForWeb()` |
| `native_theme_fluent.h` | Fluent class definition |
| `native_theme_fluent.cc` | Fluent scrollbar overrides |
| `native_theme_constants_fluent.h` | Fluent scrollbar dimensions and constants |
| `native_theme_features.h/cc` | Feature flag checks (`IsFluentScrollbarEnabled()`) |
| `web_engine_settings.cpp` | Browser-process flag setter |
| `web_view_impl.cc` | Renderer-process flag setter |

All Chromium files are relative to `qtwebengine/src/3rdparty/chromium/`.

---

**Note for AI agents**: This document describes the NativeTheme paint system for native form controls. When modifying native elements, always add the `element_shader_enabled()` check with the original code as fallback. Build with `./install.sh --dirty`. **IMPORTANT**: Update this file if it becomes outdated after changes to the codebase.
