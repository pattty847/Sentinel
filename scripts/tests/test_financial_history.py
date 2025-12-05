#!/usr/bin/env python3
"""
Test script for financial history extraction functionality.
Tests that the refactored financial_processor can extract time-series data
for historical bar charts in the UI.
"""
import sys
import asyncio
import logging
import json
from pathlib import Path

# Add parent directory to path for imports
sys.path.insert(0, str(Path(__file__).parent.parent))

from sec.sec_api import SECDataFetcher
from sec.financial_processor import FinancialDataProcessor

# Configure logging to see what's happening
logging.basicConfig(level=logging.INFO, format='%(asctime)s - %(levelname)s - %(message)s')

async def test_financial_history_extraction():
    """Test that financial history extraction works correctly for AAPL."""
    print("=" * 50)
    print("TEST: Financial History Extraction")
    print("=" * 50)
    
    try:
        fetcher = SECDataFetcher()
        
        # Test fetching financial summary with history
        print("Fetching financial summary with history for AAPL...")
        summary = await fetcher.get_financial_summary("AAPL", use_cache=True)
        
        if not summary:
            print("[FAIL] No summary data returned")
            return False
        
        print(f"[PASS] Successfully fetched financial summary for {summary.get('ticker')}")
        print(f"Entity: {summary.get('entityName')}")
        print(f"CIK: {summary.get('cik')}")
        print(f"Period End: {summary.get('period_end')}")
        print(f"Source Form: {summary.get('source_form')}")
        
        # Test revenue history specifically
        revenue = summary.get('revenue')
        if not revenue:
            print("[FAIL] No revenue data found")
            return False
        
        if not isinstance(revenue, dict):
            print(f"[FAIL] Revenue is not a dictionary. Got: {type(revenue)}")
            return False
        
        quarterly = revenue.get('quarterly', [])
        annual = revenue.get('annual', [])
        
        print(f"\nRevenue History:")
        print(f"  Quarterly entries: {len(quarterly)}")
        print(f"  Annual entries: {len(annual)}")
        
        # Verify we have at least 4 quarterly entries
        if len(quarterly) < 4:
            print(f"[FAIL] Expected at least 4 quarterly revenue entries, got {len(quarterly)}")
            return False
        
        print("[PASS] Found at least 4 quarterly revenue entries")
        
        # Verify structure of entries
        first_quarterly = quarterly[0]
        required_fields = ['period', 'date', 'value', 'form']
        for field in required_fields:
            if field not in first_quarterly:
                print(f"[FAIL] Missing required field '{field}' in quarterly entry")
                return False
        
        print("[PASS] Quarterly entries have correct structure")
        
        # Verify sorting (newest first)
        if len(quarterly) >= 2:
            date1 = quarterly[0].get('date', '0000-00-00')
            date2 = quarterly[1].get('date', '0000-00-00')
            if date1 < date2:
                print(f"[FAIL] Entries not sorted correctly. First: {date1}, Second: {date2}")
                return False
        
        print("[PASS] Entries are sorted correctly (newest first)")
        
        # Display sample entries
        print("\nSample Quarterly Revenue Entries (first 4):")
        for i, entry in enumerate(quarterly[:4]):
            print(f"  {i+1}. {entry.get('period')} ({entry.get('date')}): ${entry.get('value'):,} - {entry.get('form')}")
        
        if annual:
            print("\nSample Annual Revenue Entries (first 2):")
            for i, entry in enumerate(annual[:2]):
                print(f"  {i+1}. {entry.get('period')} ({entry.get('date')}): ${entry.get('value'):,} - {entry.get('form')}")
        
        # Test other metrics
        print("\nTesting other metrics...")
        metrics_to_test = ['net_income', 'assets', 'equity']
        for metric in metrics_to_test:
            metric_data = summary.get(metric)
            if metric_data and isinstance(metric_data, dict):
                q_count = len(metric_data.get('quarterly', []))
                a_count = len(metric_data.get('annual', []))
                print(f"  {metric}: {q_count} quarterly, {a_count} annual entries")
            else:
                print(f"  {metric}: No data")
        
        await fetcher.close()
        return True
        
    except Exception as e:
        print(f"[FAIL] Test failed with exception: {e}")
        import traceback
        traceback.print_exc()
        return False

async def test_mocked_fact_response():
    """Test with a mocked fact response to verify parsing logic."""
    print("\n" + "=" * 50)
    print("TEST: Mocked Fact Response")
    print("=" * 50)
    
    # Create a mock facts response
    mock_facts = {
        "cik": 320193,
        "entityName": "Test Company",
        "facts": {
            "us-gaap": {
                "RevenueFromContractWithCustomerExcludingAssessedTax": {
                    "label": "Revenue",
                    "units": {
                        "USD": [
                            {
                                "end": "2024-09-30",
                                "val": 1000000000,
                                "start": "2024-07-01",
                                "fy": 2024,
                                "fp": "Q3",
                                "form": "10-Q",
                                "frame": "CY2024Q3"
                            },
                            {
                                "end": "2024-06-30",
                                "val": 950000000,
                                "start": "2024-04-01",
                                "fy": 2024,
                                "fp": "Q2",
                                "form": "10-Q",
                                "frame": "CY2024Q2"
                            },
                            {
                                "end": "2024-06-30",
                                "val": 3000000000,
                                "fy": 2024,
                                "fp": "Q3",
                                "form": "10-Q",
                                "frame": "CY2024Q3",
                                # Missing start date simulates a YTD/cumulative fact that should be rejected
                            },
                            {
                                "end": "2024-03-31",
                                "val": 900000000,
                                "start": "2024-01-01",
                                "fy": 2024,
                                "fp": "Q1",
                                "form": "10-Q",
                                "frame": "CY2024Q1"
                            },
                            {
                                "end": "2023-12-31",
                                "val": 850000000,
                                "start": "2023-10-01",
                                "fy": 2023,
                                "fp": "Q4",
                                "form": "10-Q",
                                "frame": "CY2023Q4"
                            },
                            {
                                "end": "2023-12-31",
                                "val": 3200000000,
                                "start": "2023-01-01",
                                "fy": 2023,
                                "fp": "FY",
                                "form": "10-K",
                                "frame": "CY2023"
                            }
                        ]
                    }
                }
            }
        }
    }
    
    # Create a mock fetch function
    async def mock_fetch(ticker: str, use_cache: bool = True):
        return mock_facts
    
    # Create processor with mock function
    processor = FinancialDataProcessor(mock_fetch)
    
    # Test _get_fact_history directly
    history = processor._get_fact_history(
        mock_facts,
        "us-gaap",
        "RevenueFromContractWithCustomerExcludingAssessedTax",
        metric_key="revenue",
    )
    
    if not history:
        print("[FAIL] No history returned from mocked data")
        return False
    
    quarterly = history.get('quarterly', [])
    annual = history.get('annual', [])
    
    print(f"Quarterly entries: {len(quarterly)}")
    print(f"Annual entries: {len(annual)}")
    
    if len(quarterly) != 4:
        print(f"[FAIL] Expected 4 quarterly entries, got {len(quarterly)}")
        return False
    
    if len(annual) != 1:
        print(f"[FAIL] Expected 1 annual entry, got {len(annual)}")
        return False

    # Ensure YTD-like entry without duration was rejected
    june_entries = [entry for entry in quarterly if entry.get('date') == '2024-06-30']
    if len(june_entries) != 1:
        print(f"[FAIL] Expected 1 quarterly entry for 2024-06-30 after filtering, got {len(june_entries)}")
        return False

    if june_entries[0].get('value') != 950000000:
        print(
            f"[FAIL] Expected quarterly value 950000000 for 2024-06-30, got {june_entries[0].get('value')}"
        )
        return False
    
    # Verify first quarterly entry is newest
    if quarterly[0].get('date') != '2024-09-30':
        print(f"[FAIL] First quarterly entry should be 2024-09-30, got {quarterly[0].get('date')}")
        return False
    
    # Verify period formatting
    if quarterly[0].get('period') != 'Q3 2024':
        print(f"[FAIL] Period should be 'Q3 2024', got {quarterly[0].get('period')}")
        return False
    
    if annual[0].get('period') != '2023':
        print(f"[FAIL] Annual period should be '2023', got {annual[0].get('period')}")
        return False
    
    print("[PASS] Mocked fact response test passed")
    print("\nSample entries:")
    for entry in quarterly:
        print(f"  {entry.get('period')} ({entry.get('date')}): ${entry.get('value'):,}")
    for entry in annual:
        print(f"  {entry.get('period')} ({entry.get('date')}): ${entry.get('value'):,}")
    
    return True

async def main():
    """Run all tests."""
    print("Financial History Extraction Tests")
    print("Testing refactored financial_processor functionality")
    print()
    
    # Test 1: Real API call with AAPL
    test1_passed = await test_financial_history_extraction()
    
    # Test 2: Mocked response
    test2_passed = await test_mocked_fact_response()
    
    print("\n" + "=" * 50)
    print("TEST SUMMARY")
    print("=" * 50)
    
    if test1_passed:
        print("[PASS] Real API financial history extraction")
    else:
        print("[FAIL] Real API financial history extraction")
    
    if test2_passed:
        print("[PASS] Mocked fact response parsing")
    else:
        print("[FAIL] Mocked fact response parsing")
    
    if test1_passed and test2_passed:
        print("\n[SUCCESS] All tests passed!")
    else:
        print("\n[WARNING] Some tests failed. Check logs for details.")

if __name__ == "__main__":
    asyncio.run(main())

