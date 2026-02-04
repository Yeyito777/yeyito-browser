# Element Shader

The element shader intercepts Blink's style resolution to transform CSS properties before rendering. Modifications are invisible to JavaScript - we modify the render, not the DOM.

## Injection Point

**File:** `qtwebengine/src/3rdparty/chromium/third_party/blink/renderer/core/css/resolver/style_resolver.cc`

The shader function lives in the anonymous namespace at the top of the file (after includes, inside `namespace blink { namespace {`):

```cpp
// Line ~148
// =============================================================================
// ELEMENT SHADER - Transforms computed styles before rendering
// =============================================================================

// Helper: create gradient with alpha from original gradient's first/last stops
StyleImage* CreateShaderGradient(float start_alpha, float end_alpha) {
  Color start_color(0x00, 0x05, 0x0f);
  Color end_color(0x09, 0x0d, 0x35);
  start_color.SetAlpha(start_alpha);
  end_color.SetAlpha(end_alpha);

  auto* gradient = MakeGarbageCollected<cssvalue::CSSLinearGradientValue>(
      nullptr, nullptr, nullptr, nullptr, nullptr,
      cssvalue::kNonRepeating, cssvalue::kCSSLinearGradient);

  cssvalue::CSSGradientColorStop stop1;
  stop1.color_ = cssvalue::CSSColor::Create(start_color);
  stop1.offset_ = CSSNumericLiteralValue::Create(0, CSSPrimitiveValue::UnitType::kPercentage);
  gradient->AddStop(stop1);

  cssvalue::CSSGradientColorStop stop2;
  stop2.color_ = cssvalue::CSSColor::Create(end_color);
  stop2.offset_ = CSSNumericLiteralValue::Create(100, CSSPrimitiveValue::UnitType::kPercentage);
  gradient->AddStop(stop2);

  return MakeGarbageCollected<StyleGeneratedImage>(
      *gradient, CSSToLengthConversionData::ContainerSizes());
}

void ApplyElementShader(StyleResolverState& state) {
  ComputedStyleBuilder& builder = state.StyleBuilder();
  // ... text color setup ...

  // Check for gradients in background layers and replace them
  FillLayer& bg_layers = builder.AccessBackgroundLayers();
  for (FillLayer* layer = &bg_layers; layer; layer = layer->Next()) {
    StyleImage* image = layer->GetImage();
    if (image && image->IsGeneratedImage()) {
      const auto* generated = DynamicTo<StyleGeneratedImage>(image);
      if (generated) {
        const CSSValue* css_value = generated->CssValue();
        if (css_value && css_value->IsGradientValue()) {
          // Extract alpha from original gradient
          float start_alpha = 1.0f, end_alpha = 1.0f;
          const auto* gradient_value = DynamicTo<cssvalue::CSSGradientValue>(css_value);
          if (gradient_value && gradient_value->StopCount() > 0 && state.ParentStyle()) {
            Vector<Color> stop_colors = gradient_value->GetStopColors(
                state.GetDocument(), *state.ParentStyle());
            if (!stop_colors.empty()) {
              start_alpha = stop_colors.front().Alpha();
              end_alpha = stop_colors.back().Alpha();
            }
          }
          layer->SetImage(CreateShaderGradient(start_alpha, end_alpha));
        }
      }
    }
  }

  // ... background color handling ...
  if (bg_style_color.IsCurrentColor()) {
    return;
  }

  // Get the actual color value
  Color bg_color = bg_style_color.GetColor();

  // Skip background modification if fully transparent
  if (bg_color.IsFullyTransparent()) {
    return;
  }

  // Background is not transparent, apply our target color
  builder.SetBackgroundColor(StyleColor(kTargetBackground));
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

### Accessing Element Info

```cpp
// Get the element being styled
Element& element = state.GetElement();

// Check element type
if (IsA<HTMLBodyElement>(element)) { ... }

// Get document
Document& doc = state.GetDocument();
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
| `platform/graphics/color.h` | `Color` class with `IsFullyTransparent()` |
| `css_gradient_value.h` | `CSSLinearGradientValue`, `CSSGradientColorStop` for gradients |
| `css_color.h` | `CSSColor::Create()` for gradient color stops |
| `css_numeric_literal_value.h` | `CSSNumericLiteralValue::Create()` for percentages |
| `style_generated_image.h` | `StyleGeneratedImage` wrapper for gradients |
| `fill_layer.h` | `FillLayer` for accessing background-image layers |

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

4. **CLI configuration** - Pass target colors from qutebrowser config via CLI flags (see `darkmode.py` for pattern).

---

**Note for AI agents**: This shader is confirmed working. Modify `ApplyElementShader()` to implement new color transformation logic. Always rebuild with `./install.sh --dirty` after changes. **IMPORTANT**: Modify this file if it it's outdated after any changes you make to the codebase.
