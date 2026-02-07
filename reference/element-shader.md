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
  // Otherwise: set to #ffffff
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
  if (!used_chromatic) { /* set all text colors to #ffffff */ }

  // --- Border recoloring (unchanged) ---
  // --- Border radius removal (unchanged) ---
  // --- Gradient replacement (unchanged) ---

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
  // Otherwise: #00050f + original alpha
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
  } else {
    Color target_with_alpha(0x00, 0x05, 0x0f);
    target_with_alpha.SetAlpha(bg_color.Alpha());
    builder.SetBackgroundColor(StyleColor(target_with_alpha));
  }
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

Pages that use `::-webkit-scrollbar` CSS get their scrollbars styled through Blink pseudo-elements. These are handled via a CSS override file injected by qutebrowser:

**File:** `~/.config/qutebrowser/cssoverrides/default.css`

```css
::-webkit-scrollbar { background: #00050f !important; }
::-webkit-scrollbar-thumb {
  background: #00050f !important;
  border: 1px solid #1d9bf0 !important;
  border-radius: 0 !important;
}
::-webkit-scrollbar-track { background: #00050f !important; border-radius: 0 !important; }
::-webkit-scrollbar-track-piece { background: #00050f !important; border-radius: 0 !important; }
::-webkit-scrollbar-corner { background: #00050f !important; }
::-webkit-scrollbar-button { background: #00050f !important; border-radius: 0 !important; }
```

### 2. Native Scrollbars (NativeTheme paint overrides)

The majority of scrollbars (including DevTools) are painted by the native theme engine, bypassing CSS entirely. These are themed by modifying the paint methods directly:

**Files:**
- `ui/native_theme/native_theme_aura.cc` — Aura scrollbars (standard + overlay)
- `ui/native_theme/native_theme_fluent.cc` — Fluent scrollbars

**Modified methods** (in both files):

| Method | Change |
|--------|--------|
| `PaintScrollbarTrack` | Fill with `#00050f` |
| `PaintScrollbarThumb` | Fill `#00050f` + 1px `#1d9bf0` stroke border, 0 radius |
| `PaintScrollbarCorner` | Fill with `#00050f` |
| `PaintArrowButton` | Background `#00050f`, arrow color `#1d9bf0`, 0 radius |

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
| `ui/native_theme/native_theme_aura.cc` | Native scrollbar painting (Aura/overlay) |
| `ui/native_theme/native_theme_fluent.cc` | Native scrollbar painting (Fluent) |

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

2. ~~**Preserve transparency**~~ - DONE: Now checks `IsFullyTransparent()` before modifying background.

3. ~~**Handle gradients**~~ - DONE: Detects gradients via `FillLayer` and replaces them with custom linear gradient (#00050f to #090d35). Preserves alpha from original gradient's first/last stops.

4. ~~**Border recoloring**~~ - DONE: Recolors all borders to #1d9bf0. Alpha preservation is optional via `kPreserveBorderAlpha` (currently disabled).

5. ~~**Border radius removal**~~ - DONE: Sets border-radius to 0 on all elements.

6. **CLI configuration** - Pass target colors from qutebrowser config via CLI flags (see `darkmode.py` for pattern).

7. ~~**Runtime toggle**~~ - DONE: `:shader-off` / `:shader-on` commands toggle the shader at runtime.

8. ~~**Chromatic text preservation**~~ - DONE: Detects chromatic text (chroma > 25) and boosts via HSL (lightness floor 0.70, saturation floor 0.70) instead of forcing white. Non-chromatic text stays #ffffff.

9. ~~**Chromatic background preservation**~~ - DONE: Small chromatic elements get darkened backgrounds (HSL lightness cap 0.15, saturation floor 0.50) instead of flat #00050f. Large elements (html/body or layout area > 200k px²) are forced to #00050f. Alpha always preserved.

10. ~~**Scrollbar theming**~~ - DONE: Two-layer approach: CSS overrides for `::-webkit-scrollbar-*` pseudo-elements, plus native theme paint overrides in `NativeThemeAura` and `NativeThemeFluent` for all other scrollbars. Theme: `#00050f` background, `#1d9bf0` 1px border, 0 radius.

## Runtime Toggle (shader-on / shader-off)

The shader can be toggled at runtime via qutebrowser commands:

- `:shader-off` — disables the shader on all open tabs and future pages
- `:shader-on` — re-enables the shader on all open tabs and future pages
- Calling the same command twice is a no-op (idempotent)

### How it works

**C++ side** (`style_resolver.cc`): `ApplyElementShader()` checks the document element for a `data-no-shader` attribute. If present, it returns early (skips all shader logic).

```cpp
Element* root = state.GetDocument().documentElement();
if (root) {
  DEFINE_STATIC_LOCAL(AtomicString, no_shader_attr, ("data-no-shader"));
  if (root->hasAttribute(no_shader_attr)) {
    return;
  }
}
```

**Python side** (`qutebrowser/components/shadercommands.py`):

1. **Existing tabs**: Runs JavaScript on all open tabs to set/remove the `data-no-shader` attribute, plus injects a `<style>` element that toggles a CSS custom property (`--__shader_state`) to force Blink to do a full style recalculation.
2. **New pages**: Installs/removes a profile-level `QWebEngineScript` (DocumentCreation injection point) that sets the attribute before styles are resolved.

### Key files

| File | Purpose |
|------|---------|
| `style_resolver.cc` (line ~195) | C++ attribute check in `ApplyElementShader()` |
| `qutebrowser/components/shadercommands.py` | Python commands and JS injection |

---

**Note for AI agents**: This shader is confirmed working. Modify `ApplyElementShader()` to implement new color transformation logic. Always rebuild with `./install.sh --dirty` after changes. **IMPORTANT**: Modify this file if it it's outdated after any changes you make to the codebase.
