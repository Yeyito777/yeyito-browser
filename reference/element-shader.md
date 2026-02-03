# Element-Wise Shader Implementation

## Goal

Implement an "element-wise shader" that intercepts elements before they're rendered, allowing us to:
1. **Read** computed style properties (e.g., background color, transparency)
2. **Write** modified style properties (e.g., set background to blue)

The key requirement: **modifications must be invisible to the page's JavaScript**. We're not modifying the DOM - we're modifying our render of it.

## Shader Requirements

The shader must transform colors using a "magnetic pole" approach - pulling element colors toward user-defined target colors.

### Properties to Modify

| Property | Description |
|----------|-------------|
| `background-color` | Transform toward user's target background |
| `color` | Transform toward user's target text color |
| `background-image` (gradients) | Transform gradient color stops |
| `::before` / `::after` | Pseudo-elements go through same style resolution |
| `:hover` / `:focus` / etc. | State changes trigger re-resolution, automatically handled |

### Configuration (Python Side)

Users configure in qutebrowser's `config.py`:

```python
c.shader.background = "#1a1a1a"  # Target background color
c.shader.text = "#e0e0e0"        # Target text color
```

These get passed as CLI flags (like dark mode):

```
qutebrowser config.py          qutebrowser/config/qtargs.py         Chromium/Blink
─────────────────────          ────────────────────────────         ─────────────
c.shader.background = "#1a1a1a"  →  --element-shader-settings=      →  Parse flags
c.shader.text = "#e0e0e0"            TargetBg=1a1a1a,TargetText=e0e0e0   Apply in style resolution
```

### Algorithm

The transformation algorithm is **pluggable**. Start with linear interpolation, iterate from there. The hook framework is what matters - the algorithm is a parameter.

Initial algorithm ideas:
- Linear interpolation toward target
- Contrast-preserving remapping
- Luminance-based pole attraction

## Mental Model

```
Page's DOM + CSSOM
        ↓
   Style Resolution (Blink computes final styles)
        ↓
   ┌─────────────────────────────────────────┐
   │  DESIRED INTERCEPTION POINT             │
   │  Read ComputedStyle → Apply transform   │
   └─────────────────────────────────────────┘
        ↓
   Layout (positions and sizes calculated)
        ↓
   Paint (drawing instructions generated)
        ↓
   Compositor → GPU → Pixels
```

The "message" we want to intercept is the `ComputedStyle` object that Blink creates for each element after resolving all CSS rules.

## Investigation Summary

### What QtWebEngine Exposes

QtWebEngine is a wrapper around Chromium's Blink engine. It exposes:

| API | Level | Can Read Styles? | Can Modify Invisibly? |
|-----|-------|------------------|----------------------|
| JavaScript injection | DOM | Yes (`getComputedStyle`) | No (page can detect) |
| `--blink-settings` flags | Blink engine | No | Yes (but fixed algorithms) |
| `--dark-mode-settings` flags | Blink engine | No | Yes (but fixed algorithms) |
| Network interceptor | HTTP | No | N/A |
| DevTools Protocol | CSSOM | Yes | No (modifies CSSOM) |

### How Dark Mode Works (The Closest Thing)

Qutebrowser's `darkmode.py` passes command-line flags to Chromium:

```
--blink-settings=forceDarkModeEnabled=true
--dark-mode-settings=InversionAlgorithm=4,ImagePolicy=2
```

This operates at the Blink level - the page cannot detect these color transformations. However:
- Only predefined algorithms available (`brightness-rgb`, `lightness-hsl`, `lightness-cielab`)
- Global application, no per-element logic
- No ability to read element properties and make decisions
- Settings are fixed at browser startup

### JavaScript Injection Approach (Rejected)

We previously attempted a JavaScript-based "shader" (`element_fix.js`) that:
1. Injected at `DocumentCreation` (before page scripts)
2. Intercepted `MutationObserver` to hide our changes from site code
3. Used `getComputedStyle()` to read properties
4. Modified `element.style` to change appearance

**Problems with this approach:**

1. **DOM Modification is Visible**
   - Even with MutationObserver interception, the DOM is modified
   - Page can detect changes via other means (polling, getComputedStyle comparison)
   - Violates the principle of "modify render, not DOM"

2. **CSS Loading is Asynchronous**
   - Elements may render before their styles are fully loaded
   - `getComputedStyle()` returns intermediate values
   - Results in flash of incorrectly styled content (FOISC)
   - No reliable way to know when styles are "final"

3. **iframes and Shadow DOM**
   - Each iframe is a separate document requiring separate injection
   - Cross-origin iframes cannot be accessed
   - Shadow DOM requires intercepting `attachShadow()` and observing each shadow root
   - Complex, fragile, and incomplete coverage

4. **Performance and Stability**
   - MutationObserver batching causes visible delays
   - Risk of infinite loops if site reacts to our changes
   - Previous implementation caused tab crashes

## The Core Problem

Chromium does not expose any API to:
- Hook into style resolution
- Modify `ComputedStyle` objects before layout/paint
- Register per-element render callbacks
- Inject custom logic into the rendering pipeline

The rendering pipeline is completely encapsulated within Blink. QtWebEngine provides no access to it beyond what Chromium's command-line flags offer.

## The Solution: Modify QtWebEngine

To achieve a true element-wise shader, we must modify Chromium's Blink engine. **QtWebEngine bundles Chromium** - you don't download them separately.

### What to Download

**Only QtWebEngine source is needed** (it contains Chromium/Blink):

```bash
git clone https://code.qt.io/qt/qtwebengine.git
cd qtwebengine
git submodule update --init --recursive  # Pulls Chromium (~20-25GB)
```

### Target Files in QtWebEngine

The style resolution pipeline lives in:

```
qtwebengine/src/3rdparty/chromium/third_party/blink/renderer/core/css/
├── resolver/
│   ├── style_resolver.cc          ← Main style resolution logic
│   ├── style_resolver.h
│   └── style_resolver_state.cc
├── computed_style.cc              ← ComputedStyle object
└── css_computed_style_declaration.cc
```

Additional files for CLI flag support:

```
qtwebengine/src/3rdparty/chromium/
├── third_party/blink/public/common/switches.cc    ← Add CLI flag definition
├── third_party/blink/renderer/core/css/properties/ ← CSS property accessors
```

Key classes:
- `StyleResolver` - Resolves CSS rules to compute final styles
- `ComputedStyle` - Immutable object holding resolved style values
- `StyleResolverState` - Mutable state during resolution

### Proposed Hook Point

In `style_resolver.cc`, the `StyleResolver::ResolveStyle()` method computes the final `ComputedStyle` for an element. We could add a hook:

```cpp
// In StyleResolver::ResolveStyle() or similar
scoped_refptr<ComputedStyle> StyleResolver::ResolveStyle(Element* element, ...) {
    // ... existing style resolution logic ...

    scoped_refptr<ComputedStyle> computed_style = /* resolved style */;

    // NEW: Element shader hook
    if (element_shader_callback_) {
        computed_style = element_shader_callback_(element, computed_style);
    }

    return computed_style;
}
```

### What the Hook Would Provide

```cpp
struct ElementShaderInput {
    // Read-only element info
    const Element* element;
    const ComputedStyle* original_style;

    // Contextual info
    bool is_in_shadow_dom;
    bool is_in_iframe;
    Document* document;
};

struct ElementShaderOutput {
    // Modified style properties
    std::optional<Color> background_color;
    std::optional<Color> color;
    std::optional<Color> border_color;
    // ... other properties
};

using ElementShaderCallback = std::function<ElementShaderOutput(const ElementShaderInput&)>;
```

### Integration with qutebrowser

Using CLI flags (like dark mode) - no QtWebEngine API changes needed beyond Blink modifications.

**Python side (minimal changes):**

1. Add config options in `qutebrowser/config/configdata.yml`:
   ```yaml
   shader.background:
     type: QtColor
     default: null
     desc: Target background color for element shader

   shader.text:
     type: QtColor
     default: null
     desc: Target text color for element shader
   ```

2. Add flag generation in `qutebrowser/config/qtargs.py` (similar to `darkmode.py`):
   ```python
   def _shader_settings(settings):
       # Generate --element-shader-settings flag from config
       ...
   ```

### Build Requirements

QtWebEngine build:
- ~25GB disk space for source
- ~100GB with build artifacts
- 16GB+ RAM recommended
- 2-6 hours build time depending on hardware
- Must rebuild when Qt updates Chromium version

## Implementation Plan

### Phase 1: QtWebEngine/Blink Modifications

1. **Add CLI flag parsing** in Blink's switches
2. **Add hook in `StyleResolver::ResolveStyle()`** to intercept computed styles
3. **Implement transformation algorithm** (start with linear interpolation)
4. **Handle special cases**: gradients, pseudo-elements, state changes

### Phase 2: qutebrowser Integration

1. **Add config options** in `configdata.yml`
2. **Add flag generation** in `qtargs.py`
3. **Build and test** with custom QtWebEngine

### Alternative Considered: Extend Dark Mode Infrastructure

Chromium's dark mode has existing infrastructure for color transformations. We could add new `DarkModeInversionAlgorithm` values, but this approach is too limited:
- Algorithms are predefined, not pluggable
- No access to read computed styles before transformation
- Less control over per-property behavior

**Decision**: Implement a proper ComputedStyle hook for full flexibility.

## Files Referenced

### qutebrowser (Python side - to modify)

| File | Purpose |
|------|---------|
| `qutebrowser/config/configdata.yml` | Add `shader.background`, `shader.text` options |
| `qutebrowser/config/qtargs.py` | Generate `--element-shader-settings` flag |
| `qutebrowser/browser/webengine/darkmode.py` | Reference for how dark mode flags work |

### QtWebEngine/Chromium (Blink side - to modify)

| File | Purpose |
|------|---------|
| `src/3rdparty/chromium/third_party/blink/renderer/core/css/resolver/style_resolver.cc` | Hook point for style interception |
| `src/3rdparty/chromium/third_party/blink/renderer/core/css/computed_style.h` | ComputedStyle object to read/modify |
| `src/3rdparty/chromium/third_party/blink/public/common/switches.cc` | CLI flag definition |

## Resolved Questions

| Question | Answer |
|----------|--------|
| What properties to modify? | `background-color`, `color`, gradients, pseudo-elements, hover states |
| Per-element or zone-based? | Per-element (via ComputedStyle hook) |
| How to configure? | CLI flags from Python config (`c.shader.background`, `c.shader.text`) |
| Download Chromium separately? | No - QtWebEngine bundles Chromium |

## Open Questions

1. What's the acceptable performance overhead per element?
2. How do we handle the Qt/Chromium version upgrade cycle?
3. Which specific transformation algorithm to start with? (Linear interpolation as first attempt)
