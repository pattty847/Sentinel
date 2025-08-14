# Sentinel Logging Guide (v2)

This guide explains how to use the simplified, high-performance logging system in Sentinel. The new system is built around four clear categories, controllable via a single environment variable.

## Quick Start: The Four Log Categories

The entire logging system is now organized into four top-level categories. You can control them using the `QT_LOGGING_RULES` environment variable.

| Category | Name | Purpose |
|---|---|---|
| `sentinel.app` | Application | Lifecycle, initialization, configuration, authentication. |
| `sentinel.data` | Data | WebSocket, network, cache, trades, order books. |
| `sentinel.render` | Rendering | All rendering, charts, GPU, coordinates, camera. |
| `sentinel.debug` | Debug | Detailed, high-frequency diagnostics (off by default). |

---

## How to Control Log Output

### 1. Production Mode (Cleanest)
This is the recommended setting for daily use. It disables all `debug` logs, showing only warnings and critical errors.

```bash
export QT_LOGGING_RULES="*.debug=false"
./build-mac-clang/apps/sentinel_gui/sentinel_gui
```
**Result**: Minimal output, perfect for a clean terminal experience.

### 2. Development Mode (Show Everything)
Enable all `sentinel` logs to get a complete picture of the application's activity.

```bash
export QT_LOGGING_RULES="sentinel.*.debug=true"
./build-mac-clang/apps/sentinel_gui/sentinel_gui
```
**Result**: Verbose output covering all application domains.

### 3. Focused Debugging (The Common Scenarios)
This is the most powerful feature. Combine rules to isolate specific problems.

#### Scenario: "My charts are rendering incorrectly."
Focus on rendering, but exclude the noisy data logs.
```bash
export QT_LOGGING_RULES="sentinel.render.debug=true;sentinel.data.debug=false"
```

#### Scenario: "Trades aren't showing up or the data seems wrong."
Focus on data processing and network, disabling rendering noise.
```bash
export QT_LOGGING_RULES="sentinel.data.debug=true;sentinel.render.debug=false"
```

#### Scenario: "The app is crashing on startup."
Focus on the application lifecycle.
```bash
export QT_LOGGING_RULES="sentinel.app.debug=true"
```

#### Scenario: "I need to see everything, but less of the render spam."
Enable all logs, but specifically disable the `render` category.
```bash
export QT_LOGGING_RULES="sentinel.*.debug=true;sentinel.render.debug=false"
```

---

## Advanced Usage

### Wildcards and Multiple Rules
The logging system is highly flexible. You can combine rules by separating them with a semicolon `;`.

- **Enable a specific category and its children**: `sentinel.data.debug=true`
- **Disable a specific category**: `sentinel.render.debug=false`
- **Enable everything except one category**: `sentinel.*.debug=true;sentinel.render.debug=false`

### Built-in Throttling
High-frequency events (like rendering or mouse movement) now have **built-in throttling** directly in the logging macros. This means you get useful, periodic updates instead of a flood of messages, without needing any special configuration.

### The `sentinel.debug` Category
The `sentinel.debug` category is reserved for extremely high-frequency, specialized diagnostics that are not useful for general development. It is disabled by default in all modes except when explicitly turned on.

**To enable it:**
```bash
export QT_LOGGING_RULES="sentinel.debug.debug=true"
```

---

## Best Practices

1.  **Start with Production Mode**: Run with `*.debug=false` to keep logs clean.
2.  **Isolate with Focused Rules**: When a bug appears, use a specific rule like `sentinel.data.debug=true` to narrow down the problem.
3.  **Avoid `sentinel.*.debug=true`**: Use it sparingly. Focused rules are more effective.
4.  **Share Your Rules**: If you find a useful rule for a specific bug, share it in the pull request or issue tracker.

This simplified system makes debugging faster and more efficient by giving you precise control over the log output.