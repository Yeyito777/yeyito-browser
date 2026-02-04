# Element Shader

The element shader intercepts Blink's style resolution to transform CSS properties before rendering. Modifications are invisible to JavaScript - we modify the render, not the DOM.

## Injection Point

**File:** `qtwebengine/src/3rdparty/chromium/third_party/blink/renderer/core/css/resolver/style_resolver.cc`

The shader function lives in the anonymous namespace at the top of the file (after includes, inside `namespace blink { namespace {`):

```cpp
// Line ~144
// =============================================================================
// ELEMENT SHADER - Transforms computed styles before rendering
// =============================================================================
void ApplyElementShader(StyleResolverState& state) {
  ComputedStyleBuilder& builder = state.StyleBuilder();

  // Target colors
  const Color kTargetBackground(0x00, 0x05, 0x0f);  // #00050f
  const Color kTargetText(0xff, 0xff, 0xff);        // #ffffff
  const StyleColor kTargetTextStyle(kTargetText);

  // Set all text-related colors to white
  builder.SetColor(kTargetTextStyle);                        // Main text color
  builder.SetTextFillColor(kTargetTextStyle);                // -webkit-text-fill-color (overrides color)
  builder.SetInternalVisitedColor(kTargetTextStyle);         // Visited link color
  builder.SetInternalVisitedTextFillColor(kTargetTextStyle); // Visited link fill color

  // Get the current background color
  OptionalStyleColor bg_opt = ColorPropertyFunctions::GetUnvisitedColor(
      GetCSSPropertyBackgroundColor(), builder);

  if (!bg_opt.has_value()) {
    return;  // No background color property
  }

  const StyleColor& bg_style_color = bg_opt.value();

  // If it's currentcolor, skip background modification
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

## TODO

1. ~~**Unprotect style getters**~~ - SOLVED: Using `ColorPropertyFunctions::GetUnvisitedColor()` which is already a friend class.

2. ~~**Preserve transparency**~~ - DONE: Now checks `IsFullyTransparent()` before modifying background.

3. **Handle gradients** - `background-image` with gradients needs separate handling via `FillLayer`.

4. **CLI configuration** - Pass target colors from qutebrowser config via CLI flags (see `darkmode.py` for pattern).

---

**Note for AI agents**: This shader is confirmed working. Modify `ApplyElementShader()` to implement new color transformation logic. Always rebuild with `./install.sh --dirty` after changes. **IMPORTANT**: Modify this file if it it's outdated after any changes you make to the codebase.
