"""Shared screener logic — field parsing, screener construction, DataFrame serialization."""
import math
from typing import Any

import tvscreener as tvs
from tvscreener import CryptoScreener, StockScreener
from tvscreener import CryptoField, StockField

# Fields always included regardless of user selection.
# Interval variants: tvscreener bakes interval into field name (e.g. RSI_240 = 4h RSI).
CRYPTO_BASE_FIELDS = [
    CryptoField.NAME,
    CryptoField.PRICE,
    CryptoField.CHANGE_PERCENT,
    CryptoField.VOLUME,
    CryptoField.RELATIVE_VOLUME,
    CryptoField.MARKET_CAP,
    CryptoField.CRYPTO_CATEGORIES,
    CryptoField.SECTOR,
    CryptoField.EXCHANGE,
]

STOCK_BASE_FIELDS = [
    StockField.NAME,
    StockField.PRICE,
    StockField.CHANGE_PERCENT,
    StockField.VOLUME,
    StockField.RELATIVE_VOLUME,
    StockField.MARKET_CAPITALIZATION,
    StockField.PRICE_TO_EARNINGS_RATIO_TTM,
    StockField.DIVIDENDS_YIELD_CURRENT,
    StockField.SECTOR,
    StockField.EXCHANGE,
]

# Map user-facing shorthand strings to actual tvscreener field attributes.
# Interval is embedded: "RSI_240" → 4h RSI, "RSI_1D" → 1D RSI, "RSI_1W" → 1W RSI.
_CRYPTO_FIELD_ALIASES: dict[str, str] = {
    # Price / volume
    "PRICE":            "PRICE",
    "CHANGE":           "CHANGE",
    "CHANGE_PERCENT":   "CHANGE_PERCENT",
    "CHANGE_4H":        "CHANGE_4H",
    "CHANGE_4H_PCT":    "CHANGE_4H_PERCENT",
    "CHANGE_1D":        "CHANGE_1W",        # no explicit 1D on crypto; 1W closest HTF
    "VOLUME":           "VOLUME",
    "VOLUME_24H_USD":   "VOLUME_24H_IN_USD",
    "RELATIVE_VOLUME":  "RELATIVE_VOLUME",
    "MARKET_CAP":       "MARKET_CAP",
    "CATEGORY":         "CRYPTO_CATEGORIES",
    "SECTOR":           "SECTOR",
    # RSI variants
    "RSI":              "RSI_240",           # default = 4h
    "RSI_1H":           "RSI_60",
    "RSI_4H":           "RSI_240",
    "RSI_1D":           "RSI_1M",            # 1M = 1 month? check — use 1D alias below
    "RSI_1W":           "RSI_1W",
    # MACD
    "MACD_4H":          "MACD_LEVEL_12_26_9_240",
    # EMA
    "EMA20":            "EMA20_240",
    "EMA50":            "EMA50_240",
    "EMA200":           "EMA200_240",
    # Volume weighted
    "VWAP":             "VOLUME_WEIGHTED_AVERAGE_PRICE",
}

_STOCK_FIELD_ALIASES: dict[str, str] = {
    "PRICE":            "PRICE",
    "CHANGE":           "CHANGE",
    "CHANGE_PERCENT":   "CHANGE_PERCENT",
    "VOLUME":           "VOLUME",
    "RELATIVE_VOLUME":  "RELATIVE_VOLUME",
    "MARKET_CAP":       "MARKET_CAPITALIZATION",
    "PE":               "PRICE_TO_EARNINGS_RATIO_TTM",
    "DIV_YIELD":        "DIVIDENDS_YIELD_CURRENT",
    "SECTOR":           "SECTOR",
    "RSI":              "RELATIVE_STRENGTH_INDEX_14",
    "RSI_4H":           "RELATIVE_STRENGTH_INDEX_14",
    "RSI_1D":           "RELATIVE_STRENGTH_INDEX_14",
    "EMA20":            "EXPONENTIAL_MOVING_AVERAGE_20",
    "EMA50":            "EXPONENTIAL_MOVING_AVERAGE_50",
    "EMA200":           "EXPONENTIAL_MOVING_AVERAGE_200",
    "MACD":             "MACD_LEVEL_12_26",
    "ADX":              "AVERAGE_DIRECTIONAL_INDEX_14",
    "ATR":              "AVERAGE_TRUE_RANGE_14",
}


def _resolve_field(name: str, asset: str):
    """Resolve a user field name string to a tvscreener field object. Returns None if unknown."""
    key = name.strip().upper()
    field_cls = CryptoField if asset == "crypto" else StockField
    alias_map = _CRYPTO_FIELD_ALIASES if asset == "crypto" else _STOCK_FIELD_ALIASES

    attr_name = alias_map.get(key, key)  # fall back to literal name if not in alias map
    try:
        return getattr(field_cls, attr_name)
    except AttributeError:
        return None  # unknown — skip gracefully


def build_screener(asset: str, extra_fields: list[str], min_volume: float, limit: int):
    """Construct and configure a tvscreener instance ready to call .get() on."""
    if asset == "crypto":
        screener = CryptoScreener()
        base = list(CRYPTO_BASE_FIELDS)
    else:
        screener = StockScreener()
        base = list(STOCK_BASE_FIELDS)

    resolved_extras = [f for name in extra_fields if (f := _resolve_field(name, asset)) is not None]
    screener.select(*base, *resolved_extras)

    if min_volume > 0:
        vol_field = CryptoField.VOLUME if asset == "crypto" else StockField.VOLUME
        screener.where(vol_field >= min_volume)

    return screener


def _sanitize(v: Any) -> Any:
    """Replace NaN/inf with None for JSON-safe serialization."""
    if isinstance(v, float) and (math.isnan(v) or math.isinf(v)):
        return None
    return v


def screener_to_rows(df) -> list[dict]:
    """Convert screener DataFrame to plain dicts, safe for JSON."""
    rows = []
    for symbol, row in df.iterrows():
        d = {"symbol": str(symbol)}
        for col, val in row.items():
            d[str(col)] = _sanitize(val)
        rows.append(d)
    return rows
