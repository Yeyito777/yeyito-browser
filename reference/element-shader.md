# Element Shader

The element shader intercepts Blink's style resolution to transform CSS properties before rendering. Modifications are invisible to JavaScript - we modify the render, not the DOM.

## Injection Point

**File:** `qtwebengine/src/3rdparty/chromium/third_party/blink/renderer/core/css/resolver/style_resolver.cc`

The shader function lives in the anonymous namespace at the top of the file (after includes, inside `namespace blink { namespace {`):

```cpp
// Line ~150
// =============================================================================
// ELEMENT SHADER - Transforms computed styles before rendering
// =============================================================================

// Helper: create gradient with alpha from original gradient's first/last stops
StyleImage* CreateShaderGradient(float start_alpha, float end_alpha) { ... }

void ApplyElementShader(StyleResolverState& state) {
  // 1. Check if shader is disabled via data-no-shader attribute
  // 2. Handle ::selection pseudo — set bg to #4f5258, return early
  // 3. Set up target colors: bg=#00050f, text=#ffffff, border=#1d9bf0

  // --- Text color: chromatic preservation ---
  // Reads original text color, computes chroma (max-min of RGB channels).
  // If chroma > 25: boost via HSL (lightness >= 0.70, saturation >= 0.70)
  // Otherwise: set to #ffffff (fully transparent text stays transparent)
  bool used_chromatic = false;
  OptionalStyleColor text_opt = ColorPropertyFunctions::GetUnvisitedColor(
      GetCSSPropertyColor(), builder);
  if (text_opt.has_value() && !text_opt.value().IsCurrentColor()) {
    Color orig = text_opt.value().GetColor();
    int chroma = std::max({orig.Red(), orig.Green(), orig.Blue()})
               - std::min({orig.Red(), orig.Green(), orig.Blue()});
    if (chroma > 25) {
      double h, s, l;
      orig.GetHSL(h, s, l);       // All 0-1 range
      l = std::max(l, 0.70);      // Lightness floor
      s = std::max(s, 0.70);      // Saturation floor
      Color boosted = Color::FromHSLA(
          static_cast<float>(h) * 360.0f,  // FromHSLA wants degrees
          static_cast<float>(s),            // 0-1 range
          static_cast<float>(l),            // 0-1 range
          orig.Alpha());
      // Set on all text color properties
      used_chromatic = true;
    }
  }
  if (!used_chromatic) { /* #ffffff, or transparent if original was fully transparent */ }

  // --- Border recoloring (unchanged) ---
  // --- Border radius removal (unchanged) ---
  // --- Gradient replacement (unchanged) ---
  // --- Drop shadow recoloring: #090d35, alpha preserved ---

  // --- Background color: chromatic preservation with area gating ---
  // Large elements always get #00050f (no chromatic preservation):
  //   - <html> and <body> elements
  //   - Elements with layout area > kMaxChromaticBgArea (200000 px², ~450x450)
  const float kMaxChromaticBgArea = 200000.0f;
  bool force_dark = false;

  Element& element = state.GetElement();
  if (IsA<HTMLHtmlElement>(element) || IsA<HTMLBodyElement>(element)) {
    force_dark = true;
  }
  if (!force_dark) {
    LayoutObject* layout_obj = element.GetLayoutObject();
    if (layout_obj && layout_obj->IsBox()) {
      auto* layout_box = To<LayoutBox>(layout_obj);
      float w = layout_box->OffsetWidth().ToFloat();
      float h = layout_box->OffsetHeight().ToFloat();
      if (w * h > kMaxChromaticBgArea) force_dark = true;
    }
  }

  // If small + chromatic (chroma > 25): darken via HSL (lightness <= 0.15, saturation >= 0.50)
  // Otherwise: #00050f + original alpha (or #090d35 for highlight-state elements)
  if (!force_dark && bg_chroma > 25) {
    double h, s, l;
    bg_color.GetHSL(h, s, l);
    l = std::min(l, 0.15);
    s = std::max(s, 0.50);
    Color darkened = Color::FromHSLA(
        static_cast<float>(h) * 360.0f,
        static_cast<float>(s),
        static_cast<float>(l),
        bg_color.Alpha());  // Alpha preserved
    builder.SetBackgroundColor(StyleColor(darkened));
    builder.SetInternalVisitedBackgroundColor(StyleColor(darkened));
  } else {
    Color target_with_alpha(0x00, 0x05, 0x0f);
    // Brightness-based highlight detection: elements with bg brightness
    // in 0.5–0.95 range whose parent bg is transparent or post-shader dark
    // get #090d35 (covers :hover, :active, and class-based states like
    // .page-nav-active). Large elements are excluded via area check (layout
    // object from previous frame) or CSS dimension estimation (first render).
    if (!force_dark && !bg_color.IsFullyTransparent()) {
      bool too_large = false;
      LayoutObject* lo = element.GetLayoutObject();
      if (lo && lo->IsBox()) {
        auto* box = To<LayoutBox>(lo);
        if (box->OffsetWidth().ToFloat() * box->OffsetHeight().ToFloat()
            > kMaxChromaticBgArea)
          too_large = true;
      } else {
        // No layout object — estimate from CSS width/height + viewport
        const auto& viewport =
            state.GetDocument().GetStyleEngine().GetViewportSize();
        const Length& css_w = builder.Width();
        const Length& css_h = builder.Height();
        float est_w = 0, est_h = 0;
        if (css_w.IsFixed()) est_w = css_w.Pixels();
        else if (css_w.IsPercent())
          est_w = css_w.Percent() * viewport.Width() / 100.0;
        else if (css_w.IsAuto()) {
          EDisplay d = builder.Display();
          if (d == EDisplay::kBlock || d == EDisplay::kFlex ||
              d == EDisplay::kGrid || d == EDisplay::kTable)
            est_w = viewport.Width();
        }
        if (css_h.IsFixed()) est_h = css_h.Pixels();
        else if (css_h.IsPercent())
          est_h = css_h.Percent() * viewport.Height() / 100.0;
        if (est_w * est_h > kMaxChromaticBgArea) too_large = true;
      }
      float bg_brightness = (bg_r + bg_g + bg_b) / (3.0f * 255.0f);
      if (!too_large && bg_brightness > 0.5f && bg_brightness < 0.95f) {
        const ComputedStyle* parent_style = state.ParentStyle();
        if (parent_style) {
          Color parent_bg = parent_style->VisitedDependentColor(
              GetCSSPropertyBackgroundColor());
          if (parent_bg.IsFullyTransparent() ||
              (parent_bg.Red() < 40 && parent_bg.Green() < 40 &&
               parent_bg.Blue() < 40))
            target_with_alpha = Color(0x09, 0x0d, 0x35);
        }
      }
    }
    target_with_alpha.SetAlpha(bg_color.Alpha());
    builder.SetBackgroundColor(StyleColor(target_with_alpha));
    builder.SetInternalVisitedBackgroundColor(StyleColor(target_with_alpha));
  }

  // Force visited links to use unvisited (shader-transformed) colors.
  // Without this, VisitedDependentColor() reads InternalVisited* properties
  // which may not reflect our shader overrides in all pipeline paths.
  builder.SetInsideLink(EInsideLink::kNotInsideLink);
}
// =============================================================================
```

The shader is called in `StyleResolver::ResolveStyle()` right before the style is returned:

```cpp
// Line ~1385
  state.LoadPendingResources();

  // Apply element shader (transforms colors before rendering)
  ApplyElementShader(state);

  // Now return the style.
  return state.TakeStyle();
}
```

## Scrollbar Theming

Scrollbars are themed via **two separate mechanisms** to achieve full coverage:

### 1. CSS Custom Scrollbars (`::-webkit-scrollbar-*`)

Pages that use `::-webkit-scrollbar` CSS get their scrollbars styled through Blink pseudo-elements. The shader has a **dedicated code path** for all scrollbar pseudo-elements (`kPseudoIdScrollbar`, `kPseudoIdScrollbarThumb`, `kPseudoIdScrollbarButton`, `kPseudoIdScrollbarTrack`, `kPseudoIdScrollbarTrackPiece`, `kPseudoIdScrollbarCorner`) in `ApplyElementShader()` that forces the shader scrollbar theme directly on the computed style:

| Pseudo-element | background | border | border-radius |
|----------------|-----------|--------|---------------|
| All scrollbar parts | `#00050f` | — | `0` |
| `::scrollbar-thumb` | `#00050f` | `1px solid #1d9bf0` | `0` |

This is baked in at shader-time, overriding whatever CSS the page declares. External CSS overrides (`cssoverrides/default.css`) are no longer needed for this but may still provide a fallback when the shader is disabled.

### 2. Native Scrollbars (NativeTheme paint overrides)

The majority of scrollbars (including DevTools) are painted by the native theme engine, bypassing CSS entirely. These are themed by modifying the paint methods directly:

**Files:**
- `ui/native_theme/native_theme.h` — Base class: static `element_shader_enabled_` flag with getter/setter
- `ui/native_theme/native_theme.cc` — Static member definition
- `ui/native_theme/native_theme_aura.cc` — Aura scrollbars (standard + overlay)
- `ui/native_theme/native_theme_fluent.cc` — Fluent scrollbars

**Modified methods** (in both Aura and Fluent files):

| Method | Shader ON | Shader OFF |
|--------|-----------|------------|
| `PaintScrollbarTrack` | Fill with `#00050f` | Original Chromium behavior |
| `PaintScrollbarThumb` | Fill `#00050f` + 1px `#1d9bf0` stroke border, 0 radius | Original Chromium behavior |
| `PaintScrollbarCorner` | Fill with `#00050f` | Original Chromium behavior |
| `PaintArrowButton` | Background `#00050f`, arrow color `#1d9bf0`, 0 radius | Original Chromium behavior |
| `PaintArrow` (Fluent only) | Arrow color `#1d9bf0` | Original Chromium behavior |

Each method checks `element_shader_enabled()` at the top and either uses shader colors or falls back to the original Chromium code.

**Toggle mechanism**: The `NativeTheme` base class has a **static** `element_shader_enabled_` flag. It must be static because Chromium is multi-process — browser and renderer processes each have separate `NativeTheme` singleton instances with separate memory spaces. The flag is set from two places:

1. **Browser process**: `web_engine_settings.cpp` calls `ui::NativeTheme::set_element_shader_enabled()` when preferences are applied
2. **Renderer process**: `web_view_impl.cc` calls `ui::NativeTheme::set_element_shader_enabled()` when preferences are deserialized from IPC

Both calls are necessary because scrollbar painting can happen in either process.

**Why two mechanisms?** Chromium has two completely separate scrollbar rendering paths:
- **CSS custom scrollbars**: Only active when a page declares `::-webkit-scrollbar` rules. Rendered as pseudo-elements through Blink's style resolver. Rare.
- **Native scrollbars**: The default for most pages. Painted directly by `NativeThemeAura`/`NativeThemeFluent` via Skia canvas calls, completely bypassing CSS. This includes DevTools, most web pages, and all internal Chrome UI.

## Available APIs

### Writing Style Properties (public setters on ComputedStyleBuilder)

```cpp
ComputedStyleBuilder& builder = state.StyleBuilder();

// Colors
builder.SetBackgroundColor(StyleColor(Color(r, g, b)));
builder.SetColor(StyleColor(Color(r, g, b)));  // text color
builder.SetBorderTopColor(StyleColor(...));
builder.SetBorderBottomColor(StyleColor(...));
builder.SetBorderLeftColor(StyleColor(...));
builder.SetBorderRightColor(StyleColor(...));
builder.SetOutlineColor(StyleColor(...));
builder.SetCaretColor(StyleAutoColor(...));
builder.SetTextDecorationColor(StyleColor(...));
builder.SetTextEmphasisColor(StyleColor(...));
builder.SetTextStrokeColor(StyleColor(...));
builder.SetColumnRuleColor(GapDataList<StyleColor>(...));
```

### Reading Style Properties

Use `ColorPropertyFunctions` (a friend class) to read protected style properties:

```cpp
#include "third_party/blink/renderer/core/animation/color_property_functions.h"

// Get background color from builder
OptionalStyleColor bg_opt = ColorPropertyFunctions::GetUnvisitedColor(
    GetCSSPropertyBackgroundColor(), builder);

if (bg_opt.has_value()) {
  const StyleColor& style_color = bg_opt.value();
  if (!style_color.IsCurrentColor()) {
    Color color = style_color.GetColor();
    if (!color.IsFullyTransparent()) {
      // color has non-transparent background
    }
  }
}
```

### Color Construction

```cpp
#include "third_party/blink/renderer/platform/graphics/color.h"

// RGB (0-255)
Color color(0x1a, 0x1a, 0x1a);  // #1a1a1a

// RGBA
Color color(0x1a, 0x1a, 0x1a, 0x80);  // 50% alpha

// Wrap in StyleColor for setters
StyleColor style_color(color);
builder.SetBackgroundColor(style_color);
```

### HSL Color Manipulation

```cpp
// GetHSL returns all values in 0.0-1.0 range
double h, s, l;
color.GetHSL(h, s, l);

// FromHSLA takes: hue in degrees (0-360), s/l in 0-1, alpha in 0-1
Color result = Color::FromHSLA(
    static_cast<float>(h) * 360.0f,  // convert 0-1 → degrees
    static_cast<float>(s),
    static_cast<float>(l),
    color.Alpha());

// Chroma detection (distinguishes chromatic from gray/white/black)
int chroma = std::max({color.Red(), color.Green(), color.Blue()})
           - std::min({color.Red(), color.Green(), color.Blue()});
// chroma > 25 means "has real color" (white=0, black=0, #333=0, #cc3333=153)
```

### Accessing Element Info

```cpp
// Get the element being styled
Element& element = state.GetElement();

// Check element type
if (IsA<HTMLBodyElement>(element)) { ... }

// Get document
Document& doc = state.GetDocument();

// Get layout dimensions (available on restyle, nullptr on first paint)
LayoutObject* layout_obj = element.GetLayoutObject();
if (layout_obj && layout_obj->IsBox()) {
  auto* layout_box = To<LayoutBox>(layout_obj);
  float w = layout_box->OffsetWidth().ToFloat();
  float h = layout_box->OffsetHeight().ToFloat();
}

// CSS dimensions from the style being built (always available)
const Length& css_w = builder.Width();   // public on ComputedStyleBase
const Length& css_h = builder.Height();
// Length methods: IsFixed(), IsPercent(), IsAuto(), Pixels(), Percent()

// Viewport size (always available during style resolution)
const auto& viewport = state.GetDocument().GetStyleEngine().GetViewportSize();
double vw = viewport.Width();   // pixels
double vh = viewport.Height();

// Display type
EDisplay disp = builder.Display();
// EDisplay::kBlock, kFlex, kGrid, kTable, kInline, etc.

// Parent style (post-shader for already-resolved parents)
const ComputedStyle* parent_style = state.ParentStyle();
// Use VisitedDependentColor() for public color access:
Color parent_bg = parent_style->VisitedDependentColor(
    GetCSSPropertyBackgroundColor());
```

## Build & Test

```bash
# Edit the shader
vim qtwebengine/src/3rdparty/chromium/third_party/blink/renderer/core/css/resolver/style_resolver.cc

# Build (1-5 min for single file change)
./install.sh --dirty

# Test
~/.local/bin/qutebrowser
```

## Key Files Reference

| File | Purpose |
|------|---------|
| `style_resolver.cc` | Shader injection point |
| `style_resolver_state.h` | `StyleResolverState` class - provides `StyleBuilder()` |
| `computed_style.h` | `ComputedStyleBuilder` class (line ~2696) |
| `computed_style_base.h` | Auto-generated base with getters/setters |
| `color_property_functions.h` | `ColorPropertyFunctions` for reading color properties |
| `longhands.h` | `GetCSSPropertyBackgroundColor()` and other property accessors |
| `platform/graphics/color.h` | `Color` class with `IsFullyTransparent()`, `GetHSL()`, `FromHSLA()` |
| `layout/layout_box.h` | `LayoutBox` for element dimensions (`OffsetWidth()`, `OffsetHeight()`) |
| `css_gradient_value.h` | `CSSLinearGradientValue`, `CSSGradientColorStop` for gradients |
| `css_color.h` | `CSSColor::Create()` for gradient color stops |
| `css_numeric_literal_value.h` | `CSSNumericLiteralValue::Create()` for percentages |
| `style_generated_image.h` | `StyleGeneratedImage` wrapper for gradients |
| `fill_layer.h` | `FillLayer` for accessing background-image layers |
| `ui/native_theme/native_theme.h` | NativeTheme base class: static `element_shader_enabled_` flag |
| `ui/native_theme/native_theme.cc` | Static member definition for shader flag |
| `ui/native_theme/native_theme_aura.cc` | Native scrollbar painting (Aura/overlay), conditional on shader flag |
| `ui/native_theme/native_theme_fluent.cc` | Native scrollbar painting (Fluent), conditional on shader flag |
| `css/style_engine.h` | `GetViewportSize()` for viewport dimensions during style resolution |
| `platform/geometry/length.h` | `Length` class: `IsFixed()`, `IsPercent()`, `IsAuto()`, `Pixels()`, `Percent()` |
| `compositor_animations.cc` | Blocks compositor bg-color animation when shader enabled |

## Compositor Background-Color Fix

Composited `background-color` animations bypass the shader (compositor thread interpolates directly from CSS keyframe values). To prevent this, `CheckCanStartEffectOnCompositor()` in `compositor_animations.cc` blocks compositor promotion for `background-color` when `GetElementShaderEnabled()` is true, forcing the animation to run on the main thread where the shader transforms every frame.

Additionally, the shader uses `#090d35` (instead of `#00050f`) for highlight-state elements — detected via brightness-based comparison against parent background. This catches `:hover`, `:active`, and class-based selection states (e.g., `.page-nav-active`). Large elements are excluded via layout area check or CSS dimension estimation against viewport.

## Visited Link Color Fix

Blink uses `VisitedDependentColor()` at paint time to pick between unvisited and visited color variants for links. The shader transforms the unvisited `BackgroundColor` (confirmed by `getComputedStyle`), but the visited variant (`InternalVisitedBackgroundColor`) was painted directly from the CSS cascade, bypassing the shader's override.

**Root cause**: `SetInternalVisitedBackgroundColor()` on the builder does not reliably persist through all pipeline paths (base computed style cache, transition system, etc.).

**Fix**: Instead of trying to set every `InternalVisited*` property, the shader forces `builder.SetInsideLink(EInsideLink::kNotInsideLink)` at the end of `ApplyElementShader()`. This tells `VisitedDependentColor()` to always return the unvisited color — which IS properly shader-transformed. This effectively disables visited-link color differentiation for shader-processed elements, which is correct since the shader overrides all colors anyway.

## Gradient Handling

Gradients are part of `background-image` and stored in `FillLayer`. To modify them:

### Required Includes

```cpp
#include "third_party/blink/renderer/core/css/css_color.h"
#include "third_party/blink/renderer/core/css/css_gradient_value.h"
#include "third_party/blink/renderer/core/css/css_numeric_literal_value.h"
#include "third_party/blink/renderer/core/style/style_generated_image.h"
```

### Detecting Gradients

```cpp
FillLayer& bg_layers = builder.AccessBackgroundLayers();
for (FillLayer* layer = &bg_layers; layer; layer = layer->Next()) {
  StyleImage* image = layer->GetImage();
  if (image && image->IsGeneratedImage()) {
    const auto* generated = DynamicTo<StyleGeneratedImage>(image);
    if (generated && generated->CssValue()->IsGradientValue()) {
      // This layer has a gradient
    }
  }
}
```

### Creating a Gradient

```cpp
// Create linear gradient
auto* gradient = MakeGarbageCollected<cssvalue::CSSLinearGradientValue>(
    nullptr, nullptr, nullptr, nullptr, nullptr,  // direction params (nullptr = top to bottom)
    cssvalue::kNonRepeating, cssvalue::kCSSLinearGradient);

// Add color stops
cssvalue::CSSGradientColorStop stop;
stop.color_ = cssvalue::CSSColor::Create(Color(0x00, 0x05, 0x0f));
stop.offset_ = CSSNumericLiteralValue::Create(0, CSSPrimitiveValue::UnitType::kPercentage);
gradient->AddStop(stop);

// Wrap in StyleGeneratedImage and set on layer
layer->SetImage(MakeGarbageCollected<StyleGeneratedImage>(
    *gradient, CSSToLengthConversionData::ContainerSizes()));
```

### Gradient Types

| Class | CSS Function |
|-------|--------------|
| `CSSLinearGradientValue` | `linear-gradient()` |
| `CSSRadialGradientValue` | `radial-gradient()` |
| `CSSConicGradientValue` | `conic-gradient()` |

## TODO

1. ~~**Unprotect style getters**~~ - SOLVED: Using `ColorPropertyFunctions::GetUnvisitedColor()` which is already a friend class.

2. ~~**Preserve transparency**~~ - DONE: All elements are processed through the shader pipeline; alpha is always preserved from the original background, so transparent elements naturally stay transparent.

3. ~~**Handle gradients**~~ - DONE: Detects gradients via `FillLayer` and replaces them with custom linear gradient (#00050f to #090d35). Preserves alpha from original gradient's first/last stops.

4. ~~**Border recoloring**~~ - DONE: Recolors all borders to #1d9bf0. Alpha preservation is optional via `kPreserveBorderAlpha` (currently disabled).

5. ~~**Border radius removal**~~ - DONE: Sets border-radius to 0 on all elements.

6. **CLI configuration** - Pass target colors from qutebrowser config via CLI flags (see `darkmode.py` for pattern).

7. ~~**Runtime toggle**~~ - DONE: Full pipeline operational. Custom PyQt6-WebEngine bindings include `ElementShaderEnabled` enum. Mojom IPC serialization carries the setting from browser→renderer process. The C++ settings pipeline, Python bindings, and IPC layer are all aligned. Needs debug print cleanup (SHADER-DEBUG-* in 4 files).

8. ~~**Chromatic text preservation**~~ - DONE: Detects chromatic text (chroma > 25) and boosts via HSL (lightness floor 0.70, saturation floor 0.70) instead of forcing white. Non-chromatic text stays #ffffff.

9. ~~**Chromatic background preservation**~~ - DONE: Small chromatic elements get darkened backgrounds (HSL lightness cap 0.15, saturation floor 0.50) instead of flat #00050f. Large elements (html/body or layout area > 200k px²) are forced to #00050f. Alpha always preserved.

10. ~~**Scrollbar theming**~~ - DONE: Two-layer approach: CSS overrides for `::-webkit-scrollbar-*` pseudo-elements, plus native theme paint overrides in `NativeThemeAura` and `NativeThemeFluent` for all other scrollbars. Theme: `#00050f` background, `#1d9bf0` 1px border, 0 radius. Native scrollbar theming respects the shader toggle via a static `element_shader_enabled_` flag on `NativeTheme`, set from both browser process (`web_engine_settings.cpp`) and renderer process (`web_view_impl.cc`).

11. ~~**Drop shadow recoloring**~~ - DONE: Reads `BoxShadow()` from the style builder, recolors each shadow entry to `#090d35` while preserving original alpha and opacity. Shadow geometry (offsets, blur, spread, inset) is untouched.

12. ~~**Build custom PyQt6-WebEngine bindings**~~ - DONE: PyQt6-WebEngine source lives in `pyqt6-webengine/`, with `ElementShaderEnabled` added to `qwebenginesettings.sip`. Phase 4 of `install.sh` builds and installs the custom bindings using `sip-install` against our custom Qt headers. The venv's PyQt6 module now loads our custom `.abi3.so` which correctly marshals the enum value to C++.

13. ~~**Compositor bg-color bypass**~~ - DONE: Composited `background-color` animations bypass the shader (compositor thread interpolates directly). Fixed by blocking compositor promotion for bg-color when shader is enabled (`compositor_animations.cc`).

15. ~~**Highlight-state detection (#090d35)**~~ - DONE: Non-chromatic elements with bg brightness 0.5–0.95, whose parent bg is transparent or post-shader dark, get `#090d35` instead of `#00050f`. This catches `:hover`, `:active`, and class-based selection states (e.g., `.page-nav-active`). Large elements are excluded via two-tier size check: layout object area (for restyled elements) or CSS dimension estimation against viewport (for first render, handles fixed px, percentages, and auto width on block/flex/grid elements).

14. ~~**Visited link colors**~~ - DONE: Visited links use `VisitedDependentColor()` at paint time which reads `InternalVisited*` properties, bypassing the shader's color overrides. Fixed by setting `InsideLink` to `kNotInsideLink` at the end of the shader, forcing paint to use unvisited (shader-transformed) colors.

## Runtime Toggle (shader-on / shader-off)

The shader can be toggled at runtime via qutebrowser commands:

- `:shader-off` — disables the shader on all open tabs and future pages
- `:shader-on` — re-enables the shader on all open tabs and future pages
- `:shader-toggle` — toggles between on/off
- `:shader-reload` — cycles off then on (forces full re-apply)
- Calling `:shader-off` / `:shader-on` when already in that state is a no-op (idempotent)

### How it works

Uses the native **QWebEngineSettings → WebPreferences → Blink Settings** pipeline. No JavaScript injection or DOM attributes needed.

**C++ side** (`style_resolver.cc`): `ApplyElementShader()` reads the `elementShaderEnabled` setting from the document's `Settings` object:

```cpp
const Settings* settings = state.GetDocument().GetSettings();
if (!settings || !settings->GetElementShaderEnabled()) {
  return;
}
```

**Settings pipeline** (9 files carry the boolean from Python to Blink):

| Layer | File | What |
|-------|------|------|
| Qt API | `qwebenginesettings.h` | `ElementShaderEnabled` enum value |
| Qt internal | `web_engine_settings.cpp` | Default `false`, maps to `WebPreferences` |
| Bridge struct | `web_preferences.h` | `bool element_shader_enabled = false` |
| Mojom IPC def | `web_preferences.mojom` | Wire format field for browser→renderer IPC |
| Mojom serialize | `web_preferences_mojom_traits.h` | Getter for serialization |
| Mojom deserialize | `web_preferences_mojom_traits.cc` | Reads field from IPC message |
| Apply mapping | `web_view_impl.cc` | `SetElementShaderEnabled(prefs.element_shader_enabled)` |
| Blink schema | `settings.json5` | `elementShaderEnabled`, invalidates Style + Paint |
| Style resolver | `style_resolver.cc` | `GetElementShaderEnabled()` check |

**Critical**: The mojom IPC layer (3 files) is required because Chromium is multi-process. The browser process sets WebPreferences, but the renderer process (Blink) consumes them. Without the mojom serialization, the value never crosses the process boundary.

**Config option** (`content.element_shader`): Boolean, default `false`. Set `c.content.element_shader = True` in `config.py` to enable the shader on startup. The config is wired through `webenginesettings.py` to `QWebEngineSettings.setAttribute()`, which propagates through the full pipeline.

**Python side** (`qutebrowser/components/shadercommands.py`): Toggles the setting on all profiles via `QWebEngineSettings.setAttribute()`. The settings pipeline automatically propagates to all existing tabs, new tabs, and page refreshes. The `:shader-*` commands override the config value at runtime; on restart, the config value takes effect again.

**Status**: Fully operational. Custom PyQt6-WebEngine bindings (built from `pyqt6-webengine/`) include the `ElementShaderEnabled` enum value. Mojom IPC serialization carries the value from the browser process to the renderer process. `QWebEngineSettings.WebAttribute.ElementShaderEnabled` is accessible from Python and correctly marshals to C++ value 38, propagating through to Blink's `Settings::GetElementShaderEnabled()`.

### Key files

| File | Purpose |
|------|---------|
| `style_resolver.cc` (line ~198) | C++ Settings check in `ApplyElementShader()` |
| `qutebrowser/components/shadercommands.py` | Python commands via `QWebEngineSettings` |
| `qwebenginesettings.h` | C++ enum definition — `ElementShaderEnabled = 38` |
| `web_engine_settings.cpp` | Qt→WebPreferences mapping |
| `web_preferences.h` | `bool element_shader_enabled = true` struct field |
| `web_preferences.mojom` | Mojom IPC wire format definition |
| `web_preferences_mojom_traits.h` | Mojom serialization getter |
| `web_preferences_mojom_traits.cc` | Mojom deserialization |
| `web_view_impl.cc` | Applies deserialized prefs to Blink Settings |
| `settings.json5` | Blink Settings schema (invalidates Style + Paint) |

---

**Note for AI agents**: This shader is confirmed working. Modify `ApplyElementShader()` to implement new color transformation logic. Always rebuild with `./install.sh --dirty` after changes. **IMPORTANT**: Modify this file if it it's outdated after any changes you make to the codebase.
