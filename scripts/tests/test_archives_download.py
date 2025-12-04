#!/usr/bin/env python3
"""
Verification test script for SEC Archives endpoint functionality.
Tests that the refactored document handler can successfully fetch documents from www.sec.gov/Archives.
"""
import sys
import asyncio
import logging
from pathlib import Path

# Add parent directory to path for imports
sys.path.insert(0, str(Path(__file__).parent.parent))

from sec.sec_api import SECDataFetcher
from sec.document_handler import FilingDocumentHandler
from sec.http_client import SecHttpClient

# Configure logging to see what's happening
logging.basicConfig(level=logging.INFO, format='%(asctime)s - %(levelname)s - %(message)s')

async def test_user_agent_normalization():
    """Test that user agent normalization works correctly."""
    print("=" * 50)
    print("TEST 1: User Agent Normalization")
    print("=" * 50)
    
    # Test the normalization function directly
    client = SecHttpClient("Sentinel Trading Terminal pattty847@gmail.com")
    print(f"Original: 'Sentinel Trading Terminal pattty847@gmail.com'")
    print(f"Normalized: '{client.user_agent}'")
    
    # Should normalize to format with version and parentheses
    expected_format = "Sentinel Trading Terminal/1.0 (pattty847@gmail.com)"
    if client.user_agent == expected_format:
        print("[PASS] User agent normalization working correctly")
    else:
        print("[FAIL] User agent normalization failed")
        print(f"Expected: {expected_format}")
        print(f"Got: {client.user_agent}")
    
    await client.close()

async def test_basic_filing_fetch():
    """Test basic filing metadata fetch (should work with existing logic)."""
    print("\n" + "=" * 50)
    print("TEST 2: Basic Filing Fetch (Control Test)")
    print("=" * 50)
    
    try:
        fetcher = SECDataFetcher()
        
        # Test basic filing fetch (uses data.sec.gov, should work)
        print("Fetching AAPL 10-K filings...")
        filings = await fetcher.get_filings_by_form("AAPL", "10-K", days_back=365)
        
        if filings and len(filings) > 0:
            print(f"[PASS] Successfully fetched {len(filings)} filings")
            recent_filing = filings[0]
            print(f"Most recent 10-K: {recent_filing.get('filing_date')} - {recent_filing.get('accession_no')}")
            return recent_filing
        else:
            print("[FAIL] No filings returned")
            return None
            
    except Exception as e:
        print(f"[FAIL] Basic filing fetch failed: {e}")
        return None
    finally:
        if 'fetcher' in locals():
            await fetcher.close()

async def test_document_list_fetch(filing_info):
    """Test fetching document list from Archives (uses www.sec.gov)."""
    print("\n" + "=" * 50)
    print("TEST 3: Document List Fetch (Archives Endpoint)")
    print("=" * 50)
    
    if not filing_info:
        print("[SKIP] No filing info available")
        return None
    
    try:
        fetcher = SECDataFetcher()
        
        accession_no = filing_info.get('accession_no')
        print(f"Fetching document list for {accession_no}...")
        
        # This should use the new archive-safe method
        docs = await fetcher.document_handler.get_filing_documents_list(accession_no, "AAPL")
        
        if docs and len(docs) > 0:
            print(f"[PASS] Successfully fetched {len(docs)} documents from Archives")
            print("First few documents:")
            for i, doc in enumerate(docs[:3]):
                print(f"  {i+1}. {doc.get('name')} ({doc.get('type')}) - {doc.get('size')} bytes")
            return docs
        else:
            print("[FAIL] No documents returned from Archives endpoint")
            return None
            
    except Exception as e:
        print(f"[FAIL] Document list fetch failed: {e}")
        import traceback
        traceback.print_exc()
        return None
    finally:
        if 'fetcher' in locals():
            await fetcher.close()

async def test_document_content_fetch(filing_info, docs):
    """Test fetching actual document content (HTML/XML) from Archives."""
    print("\n" + "=" * 50)
    print("TEST 4: Document Content Fetch (Archives Endpoint)")
    print("=" * 50)
    
    if not filing_info or not docs:
        print("[SKIP] No filing or document info available")
        return False
    
    try:
        fetcher = SECDataFetcher()
        
        accession_no = filing_info.get('accession_no')
        
        # Find a primary document to download
        primary_doc = filing_info.get('primary_document')
        if not primary_doc and docs:
            # Find an HTML document from the list
            for doc in docs:
                if doc.get('name', '').lower().endswith(('.htm', '.html')):
                    primary_doc = doc.get('name')
                    break
        
        if not primary_doc:
            print("[FAIL] No suitable document found to download")
            return False
            
        print(f"Downloading document: {primary_doc}")
        print(f"From filing: {accession_no}")
        
        # This should use the new archive-safe method
        content = await fetcher.document_handler.download_form_document(accession_no, primary_doc, "AAPL")
        
        if content and len(content) > 0:
            print(f"[PASS] Successfully downloaded document content")
            print(f"Content length: {len(content)} characters")
            print(f"Content preview (first 200 chars):")
            print(f"'{content[:200]}...'")
            
            # Verify it's actual HTML/XML content
            if any(tag in content.lower() for tag in ['<html', '<xml', '<!doctype', '<head', '<body']):
                print("[PASS] Content appears to be valid HTML/XML")
                return True
            else:
                print("[WARN] Content doesn't appear to be HTML/XML")
                return False
        else:
            print("[FAIL] No content returned or empty content")
            return False
            
    except Exception as e:
        print(f"[FAIL] Document content fetch failed: {e}")
        import traceback
        traceback.print_exc()
        return False
    finally:
        if 'fetcher' in locals():
            await fetcher.close()

async def main():
    """Run all verification tests."""
    print("SEC Archives Download Verification Tests")
    print("Testing refactored document handler functionality")
    print()
    
    # Test 1: User Agent Normalization
    await test_user_agent_normalization()
    
    # Test 2: Basic filing fetch (control test)
    filing_info = await test_basic_filing_fetch()
    
    # Test 3: Document list from Archives
    docs = await test_document_list_fetch(filing_info)
    
    # Test 4: Document content from Archives
    content_success = await test_document_content_fetch(filing_info, docs)
    
    print("\n" + "=" * 50)
    print("TEST SUMMARY")
    print("=" * 50)
    
    if filing_info:
        print("[PASS] Basic API connectivity")
    else:
        print("[FAIL] Basic API connectivity")
    
    if docs:
        print("[PASS] Archives document listing")
    else:
        print("[FAIL] Archives document listing")
    
    if content_success:
        print("[PASS] Archives document download")
    else:
        print("[FAIL] Archives document download")
    
    if docs and content_success:
        print("\n[SUCCESS] All critical tests passed! Archives endpoint is working!")
    else:
        print("\n[WARNING] Some tests failed. Check logs for details.")

if __name__ == "__main__":
    asyncio.run(main())