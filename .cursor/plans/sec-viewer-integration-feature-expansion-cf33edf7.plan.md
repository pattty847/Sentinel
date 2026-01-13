<!-- cf33edf7-74b5-49e9-a4e4-eeb98420308f 978d1788-857f-4707-ac80-00fce3b73818 -->
# SEC Viewer Integration & Feature Expansion Plan

## Current State

- SEC backend (`scripts/sec/`) is built and functional
- `SecApiClient` calls scripts but still uses inline Python for initialization
- `SecFilingDock` displays filings, transactions, and financial summaries
- Symbol propagation is already correct (signal-only, no duplicate calls)
- Financial processor extracts latest values but lacks time-series capability

## Phase 1: Merge-Readiness Fixes

### 1.1 Remove Inline Python Initialization

**Files:** [`libs/gui/widgets/SecApiClient.cpp`](libs/gui/widgets/SecApiClient.cpp), [`libs/gui/widgets/SecApiClient.hpp`](libs/gui/widgets/SecApiClient.hpp)

**Changes:**

- Remove `initializePython()` method that uses inline Python command
- Replace readiness check with simple script existence validation
- Remove `m_pythonReady` flag and `executePythonCommand()` method
- Update `isReady()` to check if scripts directory exists
- Remove `m_initTimer` if no longer needed

**Rationale:** Scripts already handle their own initialization. The readiness probe adds complexity without value.

### 1.2 Symbol Propagation (Already Fixed)

**Status:** ✓ Complete - `propagateSymbolChange()` already only emits signal, docks connected via signals/slots.

### 1.3 CMake GLOB Cleanup (Optional)

**File:** [`libs/gui/CMakeLists.txt`](libs/gui/CMakeLists.txt)

**Decision:** Defer to separate cleanup PR. Not a functional blocker.

## Phase 2: SEC Viewer Dock UX Enhancements

### 2.1 Button State Management & Progress Indicators

**Files:** [`libs/gui/widgets/SecFilingDock.cpp`](libs/gui/widgets/SecFilingDock.cpp), [`libs/gui/widgets/SecFilingDock.hpp`](libs/gui/widgets/SecFilingDock.hpp)

**Changes:**

- Add `setButtonsEnabled(bool enabled)` helper method
- Disable buttons when operations start (`fetchFilings`, `fetchInsiderTransactions`, `fetchFinancialSummary`)
- Re-enable buttons on completion or error
- Add `QProgressBar* m_progressBar` in status area
- Show/hide progress bar during operations

### 2.2 Enhanced UI Display

**File:** [`libs/gui/widgets/SecFilingDock.cpp`](libs/gui/widgets/SecFilingDock.cpp)

**Changes:**

- **Financials display:** Use `QTextEdit::setHtml()` instead of `setPlainText()`
- Format as HTML table with dark theme styling
- Format numbers with commas and proper units
- **Filings table:** Add URL column or make description clickable
- Store URL in `QStandardItem` user data
- Connect `doubleClicked()` signal to open URL in browser via `QDesktopServices::openUrl()`
- **Transactions table:** Format numbers with commas and currency symbols
- **Status label:** Improve styling with better colors and padding

### 2.3 Process Management & Error Handling

**Files:** [`libs/gui/widgets/SecApiClient.cpp`](libs/gui/widgets/SecApiClient.cpp), [`libs/gui/widgets/SecApiClient.hpp`](libs/gui/widgets/SecApiClient.hpp)

**Changes:**

- Add `m_isOperationInProgress` flag to prevent concurrent requests
- Add `QTimer* m_timeoutTimer` for request timeouts (30-60 seconds)
- Parse JSON error responses: Check for `{"error": "..."}` before data markers
- Extract error messages from script stderr output
- Add timeout handler: `onTimeout()` to kill process and emit error
- Better error messages: Include operation type, ticker, and specific error

## Phase 3: XBRL Time-Series & Financial Statements

### 3.1 Extend Financial Processor for Time-Series

**Files:** [`scripts/sec/financial_processor.py`](scripts/sec/financial_processor.py), [`scripts/sec/sec_api.py`](scripts/sec/sec_api.py)

**New Methods:**

- `get_financial_time_series(ticker, metric, periods='quarterly', years=3)` in `FinancialDataProcessor`
- Extract historical values by period from Company Facts API
- Support quarterly and annual views
- Return structured data: `[{period: "2024-Q1", value: 123.45, end_date: "2024-03-31"}, ...]`
- Add to `SECDataFetcher`: `get_financial_time_series(ticker, metric, periods, years)`

**Supported Metrics:**

- Revenue, Net Income, EPS
- Assets, Liabilities, Equity
- Operating/Investing/Financing Cash Flow
- Shares Outstanding (from DEI taxonomy)

### 3.2 Full Financial Statements Extraction

**New File:** `scripts/sec/statement_processor.py`

**Purpose:**

- Extract full Income Statement, Balance Sheet, Cash Flow Statement
- Parse XBRL tags from Company Facts API
- Map standard XBRL concepts to line items
- Return structured statement data with period labels

**New Script:** `scripts/sec/sec_fetch_statements.py`

- Fetch full financial statements (Income, Balance Sheet, Cash Flow)
- Return JSON with structured line items and historical periods
- Support quarterly and annual views

**Integration:**

- Add `fetchFinancialStatements(ticker, statement_type, periods)` to `SecApiClient`
- Add `statementsReady(QJsonObject statements)` signal
- Add statements display tab/section in `SecFilingDock`

### 3.3 Financial Ratios Calculation

**File:** [`scripts/sec/financial_processor.py`](scripts/sec/financial_processor.py)

**Enhancement:** Implement `_calculate_ratios()` method

**Ratios to Calculate:**

- **Profitability:** ROE (Net Income / Equity), ROA (Net Income / Assets), Net Margin (Net Income / Revenue), EBITDA Margin (if EBITDA available)
- **Liquidity:** Current Ratio (if current assets/liabilities available)
- **Leverage:** Debt-to-Equity (if debt available)
- **Efficiency:** Asset Turnover (Revenue / Assets)

**Note:** Some ratios require additional XBRL tags (e.g., EBITDA, Current Assets). Add tags to `KEY_FINANCIAL_SUMMARY_METRICS` as needed.

**Integration:**

- Add ratios to financial summary output
- Display ratios in `SecFilingDock` financials section

## Phase 4: External Price Data Integration

### 4.1 Price Data Provider

**New File:** `scripts/market/yahoo_finance.py` (or `polygon.py` for free tier)

**Purpose:**

- Fetch delayed price data (Yahoo Finance) or free-tier real-time (Polygon)
- Get current stock price, market cap, P/E, P/S ratios
- Historical price data for charting

**Integration:**

- Add `fetchPriceData(ticker)` to `SecApiClient` (or separate `MarketDataClient`)
- Display price data in `SecFilingDock` header or separate section
- Use price data to calculate real-time ratios (P/E, P/S) when combined with SEC financials

**Note:** Price data requires external API. Yahoo Finance is free but delayed. Polygon free tier has rate limits. Document limitations.

## Phase 5: UI Enhancements for New Features

### 5.1 Time-Series Charts

**File:** [`libs/gui/widgets/SecFilingDock.cpp`](libs/gui/widgets/SecFilingDock.cpp)

**New Components:**

- Add tab/section for "Financial Charts"
- Integrate Qt Charts (`QChart`, `QLineSeries`, `QBarSeries`)
- Create chart widgets for:
- Revenue/Net Income over time (line chart)
- Margins over time (line chart)
- Balance sheet items (bar chart)
- Add period toggle (Quarterly/Annual) controls

### 5.2 Financial Statements Display

**File:** [`libs/gui/widgets/SecFilingDock.cpp`](libs/gui/widgets/SecFilingDock.cpp)

**New Components:**

- Add tabs or expandable sections for Income Statement, Balance Sheet, Cash Flow
- Display statements as formatted HTML tables
- Show multiple periods side-by-side for comparison
- Add export functionality (CSV/JSON)

## Implementation Priority

1. **Phase 1.1** - Remove inline Python (blocking merge)
2. **Phase 2** - UX enhancements (improves user experience immediately)
3. **Phase 3.1** - Time-series extraction (enables charts)
4. **Phase 3.2** - Full statements (comprehensive data)
5. **Phase 3.3** - Ratios (derived metrics)
6. **Phase 4** - Price data (external dependency)
7. **Phase 5** - UI for new features (visualization)

## Out of Scope (Premium/External)

- **Analyst Estimates:** Requires premium data providers (Bloomberg, Refinitiv, FactSet)
- **Earnings Calendar:** Premium data only
- **Real-time Market Data:** Requires paid API subscriptions
- **Segment/Geographic Breakdowns:** Requires parsing 10-K/10-Q segment disclosures (complex, defer)

## Testing Checklist

- [ ] Script existence check works without inline Python
- [ ] Buttons disable/enable correctly during operations
- [ ] Progress indicators show/hide appropriately
- [ ] Financials display as formatted HTML
- [ ] Filing URLs are clickable/openable
- [ ] Error messages are user-friendly
- [ ] Timeout handling works (test with network disabled)
- [ ] Time-series data extracts correctly for multiple periods
- [ ] Financial statements display correctly
- [ ] Ratios calculate correctly
- [ ] Price data integrates correctly (if implemented)
- [ ] Charts render correctly with time-series data

## Notes

- All SEC data is free and public
- XBRL parsing requires understanding GAAP taxonomy but is achievable
- Price data requires external API (Yahoo Finance free, Polygon free tier)
- Ratios are computed from SEC data (free)
- Premium features (estimates, earnings calendar) explicitly deferred

### To-dos

- [ ] Remove inline Python initialization from SecApiClient, replace with script existence check
- [ ] Add button state management and progress indicators to SecFilingDock
- [ ] Enhance UI display: HTML formatting for financials, clickable filing URLs, formatted numbers
- [ ] Add process management: timeouts, operation queuing, better error handling
- [ ] Extend FinancialDataProcessor to extract historical time-series data from XBRL
- [ ] Create StatementProcessor and script to extract full Income/Balance/Cash Flow statements
- [ ] Implement financial ratios calculation (ROE, ROA, margins, etc.) in FinancialDataProcessor
- [ ] Integrate external price data provider (Yahoo Finance or Polygon free tier)
- [ ] Add Qt Charts integration for time-series visualization in SecFilingDock
- [ ] Add UI components for displaying full financial statements with period comparison