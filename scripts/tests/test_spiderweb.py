import asyncio
import logging
import sys
import os
from pathlib import Path

# Add parent directory to path for imports
sys.path.insert(0, str(Path(__file__).parent.parent))

from sec.sec_api import SECDataFetcher
from sec.sql_cache_manager import SqlCacheManager

# Configure logging
logging.basicConfig(
    level=logging.INFO,
    format='%(asctime)s - %(levelname)s - %(message)s'
)

async def test_operation_spiderweb():
    """
    Integration test for Operation Spiderweb.
    
    Verifies:
    1. Financial History SQL Storage (via get_financial_summary)
    2. Supply Chain Parsing & Storage (via get_supply_chain)
    3. SQL Retrieval
    """
    print("=" * 50)
    print("TEST: Operation Spiderweb (Supply Chain & SQL)")
    print("=" * 50)
    
    fetcher = SECDataFetcher()
    sql_manager = SqlCacheManager()
    
    TICKER = "AAPL" # Using Apple as it usually has rich relationships
    
    try:
        # --- PART 1: Financial History & SQL ---
        print(f"\n[1/3] Testing Financial Summary & SQL Persistence for {TICKER}...")
        summary = await fetcher.get_financial_summary(TICKER)
        
        if not summary:
            print("[FAIL] Could not fetch financial summary.")
            return
            
        # Verify SQL Write by reading it back
        history_rows = await sql_manager.get_financial_history(TICKER, metric="revenue")
        if history_rows:
            print(f"[PASS] Retrieved {len(history_rows)} revenue records from SQL.")
            print(f"       Latest: {history_rows[0]['period_end_date']} - ${history_rows[0]['value']:,.0f}")
        else:
            print("[FAIL] No history found in SQL after fetch.")
            
        # --- PART 2: Supply Chain Extraction ---
        print(f"\n[2/3] Testing Supply Chain Extraction for {TICKER}...")
        relationships = await fetcher.get_supply_chain(TICKER)
        
        if relationships:
            print(f"[PASS] Extracted {len(relationships)} relationships.")
            print("       Sample Links:")
            for rel in relationships[:5]:
                print(f"       - [{rel['relationship_type'].upper()}] {rel['target_entity']} (Conf: {rel['confidence_score']})")
        else:
            print("[WARN] No relationships found (Parser might need tuning or 10-K is clean).")
            
        # --- PART 3: SQL Graph Retrieval ---
        print(f"\n[3/3] Verifying Graph Storage in SQL...")
        db_edges = await sql_manager.get_relationships(TICKER)
        
        if db_edges:
            print(f"[PASS] Retrieved {len(db_edges)} edges from SQL database.")
            if len(db_edges) == len(relationships):
                 print("[PASS] Count matches extraction.")
        else:
            if relationships:
                print("[FAIL] Relationships were extracted but not found in DB.")
            else:
                print("[INFO] DB is empty because extraction yielded nothing.")

    except Exception as e:
        print(f"[FAIL] Exception during test: {e}")
        import traceback
        traceback.print_exc()
    finally:
        await fetcher.close()

if __name__ == "__main__":
    asyncio.run(test_operation_spiderweb())

