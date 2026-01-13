<!-- 1fa252c0-3377-4f09-a03d-3161e8e783bd ad038003-af38-455c-a14e-225782d40a2c -->
# Operation Spiderweb: Supply Chain & SQLite Migration

## Phase 1: The Foundation (SQLite Migration)

We will implement a new SQL-based cache manager to store processed financial data and the new supply chain graph.

### 1. Create `scripts/sec/sql_cache_manager.py`

- **Dependencies**: `aiosqlite` (for async support) or standard `sqlite3`.
- **Class**: `SqlCacheManager`
- **Schema Implementation**:
- `financial_history`: Stores extracted time-series data (Revenue, Net Income, etc.).
- `industrial_graph`: Stores extracted relationships (Supplier, Customer, Competitor).
- **Methods**:
- `initialize_db()`: Create tables if not exist.
- `save_financial_history(ticker, data)`: Insert/Upsert processed financial metrics.
- `save_relationship(source, target, type, weight, context)`: Insert graph edges.
- `get_financial_history(ticker)`: Retrieve time-series.
- `get_relationships(ticker)`: Retrieve graph edges.

## Phase 2: The Hunter (HTML Fetching)

We will enhance the document handler to reliably fetch and clean the raw HTML of 10-K filings.

### 2. Update `scripts/sec/document_handler.py`

- **Enhance**: `fetch_filing_document` or add `fetch_primary_html`.
- **Functionality**:
- Ensure explicit targeting of `.htm` / `.html` files for 10-Ks.
- Add basic HTML cleaning (tag stripping) helper if not present in the parser.

## Phase 3: The Spider (Text Parsing)

We will build the parsing engine to extract "Business" and "Risk Factors" sections and mine them for relationships.

### 3. Create `scripts/sec/supply_chain_parser.py`

- **Class**: `SupplyChainParser`
- **Workflow**:
- **Sectioning**: Regex to identify and extract "Item 1. Business" and "Item 1A. Risk Factors".
- **Entity Extraction (Level 1)**:
- Regex patterns for keywords: "customer", "supplier", "accounted for %", "revenue", "competitor", "litigation".
- Extract proper nouns appearing near these keywords.
- **Graph Construction**: Format extracted data into nodes and weighted edges.
- **Integration**:
- Add `get_supply_chain(ticker)` method to `SECDataFetcher` (delegating to this parser).

## Execution Order

1.  Setup `SqlCacheManager` and schemas.
2.  Update `FilingDocumentHandler` to ensure we can get the raw HTML.
3.  Build `SupplyChainParser` with regex logic.
4.  Wire it all together in `SECDataFetcher`.
5.  Create and run integration tests (`scripts/tests/test_spiderweb.py`).

### To-dos

- [ ] Create scripts/sec/sql_cache_manager.py with tables financial_history and industrial_graph
- [ ] Update scripts/sec/document_handler.py to support explicit HTML fetching
- [ ] Create scripts/sec/supply_chain_parser.py with Sectioning and Regex extraction logic
- [ ] Create integration test scripts/tests/test_spiderweb.py to verify full flow (Fetch -> Parse -> DB -> Query)