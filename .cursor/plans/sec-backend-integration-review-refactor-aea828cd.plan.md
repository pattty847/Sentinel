<!-- aea828cd-99d2-4f77-b9aa-16c33bd5acf9 44a58d71-2229-42d3-8b21-292dd84e81e3 -->
# SEC Backend Integration Review & Refactor

## Current Issues Identified

1. **SecApiClient uses inline Python code** (`SecApiClient.cpp` lines 47-56, 70-79, 93-102): Building multi-line Python strings with string interpolation is error-prone and hard to maintain. The existing test scripts (`scripts/sec_fetch_*.py`) already work perfectly but aren't being used.

2. **No button state management**: Buttons can be spammed during operations (`SecFilingDock.cpp` lines 105-136). No visual feedback or loading indicators.

3. **Basic UI display**: Financials shown as plain text (`SecFilingDock.cpp` line 186-196). No clickable URLs in filings table. No rich formatting.

4. **Process management issues**: Creates new process per request, kills previous process if running (could lose data), no timeout handling.

5. **Error handling**: Basic error messages, no JSON error parsing from scripts, process errors not well handled.

## Solution Plan

### Phase 1: Refactor SecApiClient to Use Test Scripts

**File: `libs/gui/widgets/SecApiClient.cpp`**

- Replace inline Python code with calls to `scripts/sec_fetch_filings.py`, `scripts/sec_fetch_transactions.py`, `scripts/sec_fetch_financials.py`
- Use `QProcess::start()` with script path and ticker/formType as arguments
- Remove `initializePython()` method (not needed - scripts handle their own initialization)
- Update `onPythonFinished()` to parse JSON error responses: `{"error": "..."}` format
- Add timeout handling (30-60 seconds per operation)
- Track operation state to prevent concurrent requests

**File: `libs/gui/widgets/SecApiClient.hpp`**

- Remove `m_pythonReady` flag and `initializePython()` method
- Add `m_isOperationInProgress` flag
- Add `QTimer* m_timeoutTimer` for request timeouts
- Update `isReady()` to check if operation is in progress

### Phase 2: Add Button State Management

**File: `libs/gui/widgets/SecFilingDock.cpp`**

- Add `setButtonsEnabled(bool enabled)` helper method
- Disable all buttons when operation starts (`fetchFilings()`, `fetchInsiderTransactions()`, `fetchFinancialSummary()`)
- Re-enable buttons on completion (`onFilingsReady()`, `onTransactionsReady()`, `onFinancialsReady()`) or error (`onApiError()`)
- Add `QProgressBar* m_progressBar` or loading indicator in status area
- Show progress bar during operations, hide on completion

**File: `libs/gui/widgets/SecFilingDock.hpp`**

- Add `QProgressBar* m_progressBar` member
- Add `setButtonsEnabled(bool enabled)` private method

### Phase 3: Improve UI Display

**File: `libs/gui/widgets/SecFilingDock.cpp`**

- **Financials display** (`displayFinancials()`): Use `QTextEdit::setHtml()` instead of `setPlainText()`
- Format as HTML table with proper styling
- Use Sentinel dark theme colors (#1e1e1e background, #fff text)
- Format numbers with commas, add units styling
- **Filings table**: Add URL column or make description clickable
- Use `QStandardItem` with `setData(url, Qt::UserRole)` for URL storage
- Connect `m_filingsTable->doubleClicked()` signal to open URL in browser
- Or add "Open" button column
- **Transactions table**: Format numbers with commas, add currency symbols
- **Status label**: Improve styling with better colors and padding

**File: `libs/gui/widgets/SecFilingDock.cpp` - `buildUi()`**

- Set `m_financialsDisplay->setAcceptRichText(true)` for HTML support
- Add double-click handler for filings table to open URLs

### Phase 4: Enhanced Error Handling

**File: `libs/gui/widgets/SecApiClient.cpp`**

- Parse JSON error responses: Check for `{"error": "..."}` in output before looking for data markers
- Extract error messages from script stderr output
- Add timeout signal/slot: `onTimeout()` to kill process and emit error
- Better error messages: Include operation type, ticker, and specific error

**File: `libs/gui/widgets/SecFilingDock.cpp`**

- Update `onApiError()` to show user-friendly messages
- Optionally use `QMessageBox` for critical errors (but keep status label for non-critical)

### Phase 5: Process Management Improvements

**File: `libs/gui/widgets/SecApiClient.cpp`**

- Queue operations: If operation in progress, queue next request or reject with message
- Better cleanup: Ensure process is properly terminated before starting new one
- Add `QProcess::readyReadStandardOutput()` connection to parse output incrementally (for large responses)
- Handle process crashes gracefully

## Implementation Details

### Script Path Resolution

- Use `QDir::current().absoluteFilePath("scripts/sec_fetch_*.py")` 
- Verify script exists before execution
- Handle Windows vs Unix path differences

### JSON Parsing

- Scripts output: `FILINGS_DATA:{...}` or `{"error": "..."}`
- Parse both formats correctly
- Handle malformed JSON gracefully

### UI Thread Safety

- All operations already on GUI thread (QProcess signals are queued)
- No additional threading needed

## Testing Checklist

- [ ] Buttons disabled during operations
- [ ] Buttons re-enabled on completion/error
- [ ] Progress indicator shows during operations
- [ ] Financials display as formatted HTML
- [ ] Filing URLs are clickable/openable
- [ ] Error messages are user-friendly
- [ ] Timeout handling works (test with network disabled)
- [ ] Multiple rapid clicks don't cause issues
- [ ] Scripts are called correctly with proper arguments
- [ ] JSON parsing handles both success and error cases

## Phase 6: Financial Visualization Blueprint

### Visualization Requirements

Based on financial dashboard examples, the system should support:

1. **Time-Series Charts:**

- Valuation ratios over time (P/E, P/S) - quarterly and annual views
- Performance metrics (Revenue, Net Income, Margins) - quarterly and annual views
- Revenue to profit conversion waterfall charts
- Growth trends over multiple periods

2. **Categorical Breakdowns:**

- Revenue by business segment (donut/pie charts)
- Revenue by geography (donut/pie charts)
- Ownership structure (free float vs. closely held)

3. **Key Metrics Display:**

- Market capitalization, Enterprise Value
- Debt, Cash & Equivalents, Minority Interest
- P/E ratio, P/S ratio, EPS
- Employee count, founding date, CEO info

4. **Historical Data Depth:**

- Multiple years of quarterly data (minimum 2-3 years)
- Annual data for longer-term trends
- Ability to toggle between quarterly/annual views

### Current SEC API Capabilities

**What we can get now:**

- Company Facts API (`/api/xbrl/companyfacts/`) provides XBRL-tagged financial data
- Submissions API provides filing metadata and dates
- Form 10-K/10-Q filings contain full financial statements
- Form 4 provides insider transaction data

**What `sec_api.py` currently extracts:**

- Basic financial summary metrics (via `FinancialDataProcessor`)
- Recent insider transactions (via `Form4Processor`)
- Filing lists with metadata (via `SECDataFetcher`)

### Limitations & Gaps

1. **Historical Data Extraction:**

- Current `get_financial_summary()` only gets latest values
- No time-series extraction from Company Facts API
- Company Facts API has historical data but needs proper parsing by period

2. **Granular Financial Statement Data:**

- Need Revenue, COGS, Gross Profit, Operating Expenses, Operating Income, Net Income
- Need Balance Sheet items: Cash, Debt, Equity, Assets, Liabilities
- Need Cash Flow items: Operating/Investing/Financing activities
- Current API doesn't extract full statement line items

3. **Segment & Geographic Data:**

- Revenue breakdown by business segment requires parsing 10-K/10-Q segment disclosures
- Geographic revenue breakdown similarly requires parsing specific filing sections
- Not currently extracted - would need new processor module

4. **Ownership Data:**

- Free float and closely held shares not directly available from SEC API
- Would need to parse proxy statements (DEF 14A) or calculate from Form 4 data
- May require external data source integration

5. **Market Data (NOT from SEC):**

- Market capitalization, current stock price require real-time market data API
- P/E, P/S ratios require current price (not historical)
- SEC only provides historical reported financials, not current market data

6. **Analyst Estimates (NOT from SEC):**

- Revenue and earnings estimates are NOT available from SEC filings
- SEC contains historical reported data only
- Would require integration with financial data providers (Bloomberg, Refinitiv, FactSet, etc.)

### Architecture Extensions Needed

**File: `sec/financial_processor.py` - Extend `FinancialDataProcessor`**

- Add `get_financial_time_series(ticker, metric, periods='quarterly')` method
- Parse Company Facts API JSON to extract historical values by period
- Support filtering by fiscal period (quarterly vs. annual)
- Return structured data: `[{period: "2024-Q1", value: 123.45}, ...]`

**File: `sec/statement_processor.py` - New module**

- Extract full Income Statement, Balance Sheet, Cash Flow Statement
- Parse XBRL tags from Company Facts API or filing documents
- Map standard XBRL concepts to line items (us-gaap:Revenues, us-gaap:CostOfGoodsAndServicesSold, etc.)
- Return structured statement data with period labels

**File: `sec/segment_processor.py` - New module**

- Parse segment disclosure tables from 10-K/10-Q filings
- Extract revenue by business segment (e.g., Automotive, Energy Generation)
- Extract revenue by geographic region
- Handle varying table formats across companies

**File: `sec/ownership_processor.py` - New module**

- Parse DEF 14A proxy statements for ownership data
- Or calculate from Form 4 filings (insider holdings)
- Extract free float vs. closely held shares
- May need external data source for complete accuracy

**File: `scripts/sec_fetch_statements.py` - New script**

- Fetch full financial statements (Income, Balance Sheet, Cash Flow)
- Return JSON with structured line items and historical periods
- Support quarterly and annual views

**File: `scripts/sec_fetch_segments.py` - New script**

- Fetch segment and geographic revenue breakdowns
- Return JSON with breakdown percentages and absolute values

**Qt Integration: `libs/gui/widgets/SecFilingDock.cpp`**

- Add new tab/section for "Financial Charts" or "Visualizations"
- Integrate Qt Charts (QChart, QLineSeries, QBarSeries, QPieSeries)
- Create chart widgets for time-series, donut charts, waterfall charts
- Add period toggle (Quarterly/Annual) controls
- Connect to new data fetching methods

### Data Source Strategy

**Tier 1: SEC API Only (Historical Financials)**

- Income Statement, Balance Sheet, Cash Flow (from Company Facts API)
- Segment/Geographic data (from 10-K/10-Q parsing)
- Historical ratios (calculated from historical financials)

**Tier 2: Hybrid (SEC + Market Data) - FUTURE/DREAM**

- Current market cap, P/E, P/S (requires market data API integration)
- Real-time stock price for ratio calculations
- **Note:** Market data APIs are expensive (price feeds, order books). Keep in blueprint for future if monetization occurs, but not blocking for initial implementation.

**Tier 3: External Providers (Estimates) - FUTURE/DREAM**

- Analyst estimates require third-party API (Bloomberg, Refinitiv, etc.)
- Also expensive - mark as "future enhancement" if monetization occurs
- Document as architectural limitation

### Implementation Priority

**Phase 6A: Basic Time-Series (High Priority)**

- Extend `FinancialDataProcessor` to extract historical values
- Add `get_financial_time_series()` method
- Create basic line charts for key metrics (Revenue, Net Income, Margins)
- Use existing Company Facts API data structure

**Phase 6B: Full Statements (Medium Priority)**

- Create `StatementProcessor` for Income/Balance/Cash Flow
- Extract all major line items with historical periods
- Enable waterfall charts and detailed analysis

**Phase 6C: Segment Data (Medium Priority)**

- Create `SegmentProcessor` for business/geographic breakdowns
- Enable donut charts for revenue segmentation
- Handle varying disclosure formats

**Phase 6D: Market Data Integration (Low Priority)**

- Integrate with market data API for current prices
- Calculate real-time ratios (P/E, P/S)
- Mark market cap and current ratios

**Phase 6E: Estimates (Future)**

- Document requirement for external provider
- Design API interface for future integration
- Not blocking for initial visualization capability

### Documentation Updates

**File: `docs/features/sec_viewer/VISUALIZATION_BLUEPRINT.md` - New file**

- Document visualization requirements
- Map data sources to visualizations
- List current limitations and future enhancements
- Provide examples of data structures needed

**File: `sec/README.md` - Update**

- Add section on visualization data extraction
- Document new processor modules
- Note limitations (estimates, market data)