# Viewport model: auto price scale (owner-approved 2026-10-02)

Goal: predictability. Switching timeframe or symbol never moves the spot the user was looking at. Replaces the pending "fit after timeframe switch" machinery from lt-claude/zoom-autofit (delete it).

## Y (price): auto-scale toggle (TradingView model)
- ON by default. While ON the price range follows the visible candles (high/low + margin), refitting as the user pans left/right, zooms time, switches timeframe, and as candles load or update.
- While ON there is no vertical pan: chart drags move time only.
- Mouse wheel zooms time and keeps auto ON (zoom out and everything fits).
- Price-axis drag or price zoom turns auto OFF. While OFF, pan is free in both directions and the price range stays exactly where the user left it, including across timeframe switches.
- Double-click on the price axis turns auto ON.
- Manual tick: rows keep the user's tick; when candles do not fit in maxPriceSpan, show the max span centred on the current price.

## Symbol switch: keep the auto state
- Auto ON stays ON: the new symbol fits its candles.
- Auto OFF stays OFF: carry the zoom as a PERCENTAGE. The new range has the same relative span (span / price) and the new symbol's Now candle sits at the same screen height as the old one's.
- First load of a symbol with no previous view: default view (auto ON, last N bars, Now column at the default position).

## X (time)
- The Now column's screen position and the bar width (pixels per bar) stay put across timeframe and symbol switches.
- Double-click on the time axis resets to the default view.

## Agent API
- Expose the auto-scale state (read and set) and the fit actions; document in docs/AGENT_API.md.
