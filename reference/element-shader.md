# Element Shader

The element shader intercepts Blink's style resolution to transform CSS properties before rendering. Modifications are invisible to JavaScript - we modify the render, not the DOM.

## Injection Point

**File:** `qtwebengine/src/3rdparty/chromium/third_party/blink/renderer/core/css/resolver/style_resolver.cc`

The shader function lives in the anonymous namespace at the top of the file (after includes, inside `namespace blink { namespace {`):

```cpp
// Line ~143
// =============================================================================
// ELEMENT SHADER - Transforms computed styles before rendering
// =============================================================================
void ApplyElementShader(StyleResolverState& state) {
  ComputedStyleBuilder& builder = state.StyleBuilder();

  // Target colors (hardcoded for now)
  const Color kTargetBackground(0x00, 0x05, 0x0f);  // #00050f
  const Color kTargetText(0xff, 0xff, 0xff);        // #ffffff

  // Force background color to target
  builder.SetBackgroundColor(StyleColor(kTargetBackground));

  // Force text color to target
  builder.SetColor(StyleColor(kTargetText));
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

### Reading Style Properties (currently protected - see TODO)

The getters like `BackgroundColor()`, `Color()` are **protected** in `ComputedStyleBuilderBase`. To read before modifying, we need to either:
1. Make them public
2. Add the shader as a friend class
3. Use `ColorPropertyFunctions` (already a friend)

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
| `color_property_functions.cc` | Reference for color get/set patterns |
| `platform/graphics/color.h` | `Color` class |

## TODO

1. **Unprotect style getters** - `BackgroundColor()`, `Color()`, and other getters in `ComputedStyleBuilderBase` are protected. Options:
   - Make them public in `gen/third_party/blink/renderer/core/style/computed_style_base.h` (auto-generated, need to find generator)
   - Add `ApplyElementShader` as a friend function to `ComputedStyleBuilder`
   - Create a helper class that's already a friend (like `ColorPropertyFunctions`)

2. **Preserve transparency** - Currently we overwrite all backgrounds including transparent ones. Need to read alpha before deciding to modify.

3. **Handle gradients** - `background-image` with gradients needs separate handling via `FillLayer`.

4. **CLI configuration** - Pass target colors from qutebrowser config via CLI flags (see `darkmode.py` for pattern).

---

**Note for AI agents**: This shader is confirmed working. Modify `ApplyElementShader()` to implement new color transformation logic. Always rebuild with `./install.sh --dirty` after changes. **IMPORTANT**: Modify this file if it it's outdated after any changes you make to the codebase.
