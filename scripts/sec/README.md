# SEC entry points

These scripts are the Python side of Sentinel's SEC overlays. `libs/gui/widgets/SecApiClient.cpp`
runs them (via `uv` in `scripts/`) and parses one prefixed JSON line from stdout.

| Script | Output prefix | Purpose |
|---|---|---|
| `sec_fetch_filings.py <ticker>` | `FILINGS_DATA:` | Recent filings |
| `sec_fetch_transactions.py <ticker>` | `TRANSACTIONS_DATA:` | Form 4 insider transactions |
| `sec_fetch_signals.py <ticker>` | `INSIDER_SIGNALS_DATA:` | Insider signal payload for chart overlays |
| `sec_fetch_financials.py <ticker>` | `FINANCIALS_DATA:` | Financial series |

Errors print `ERROR_DATA:` with a JSON `{"error": ...}` and exit non-zero.

All SEC logic (HTTP with SEC rate limits and User-Agent, caching, Form 4 parsing, point-in-time
financials) lives in the `copetech-edgar` package (import `copetech_sec`), developed in the
CopeTech-Edgar repository. Development installs it editable from the sibling checkout
(`scripts/pyproject.toml`); releases bundle a wheel built from a tagged release
(`scripts/release_macos.sh`, `COPETECH_EDGAR_REF`).
