import json
import os
import logging
import xml.etree.ElementTree as ET
import asyncio
import re
from pathlib import Path

from datetime import datetime, timedelta
from typing import List, Dict, Optional, Union, Tuple, Any, Callable, Awaitable
from .http_client import SecHttpClient
from .cache_manager import SecCacheManager
from .document_handler import FilingDocumentHandler
from .form4_processor import Form4Processor
from .financial_processor import FinancialDataProcessor
from .supply_chain_parser import SupplyChainParser
from .sql_cache_manager import SqlCacheManager

try:
    from dotenv import load_dotenv
except ImportError:
    load_dotenv = None

if load_dotenv:
    load_dotenv()
else:
    logging.debug("python-dotenv not installed; skipping .env loading")
class SECDataFetcher:
    """
    Acts as a facade/orchestrator for fetching and processing data from SEC EDGAR APIs.
    Initializes and delegates tasks to specialized handler/processor classes.
    """

    # Base endpoints remain here as they are fundamental
    SUBMISSIONS_ENDPOINT = "https://data.sec.gov/submissions/CIK{cik}.json"
    COMPANY_FACTS_ENDPOINT = "https://data.sec.gov/api/xbrl/companyfacts/CIK{cik}.json"
    COMPANY_CONCEPT_ENDPOINT = "https://data.sec.gov/api/xbrl/companyconcept/CIK{cik}/{taxonomy}/{concept}.json"

    # General constants like FORM_TYPES can stay here or move to a central constants file
    FORM_TYPES = {
        "4": "Statement of changes in beneficial ownership (insider transactions)",
        "3": "Initial statement of beneficial ownership",
        "5": "Annual statement of beneficial ownership",
        "8-K": "Current report",
        "10-K": "Annual report",
        "10-Q": "Quarterly report",
        "13F": "Institutional investment manager holdings report",
        "SC 13G": "Beneficial ownership report",
        "SC 13D": "Beneficial ownership report (active)",
        "DEF 14A": "Definitive proxy statement",
    }

    # Constants moved to specific processors:
    # - TRANSACTION_CODE_MAP, ACQUISITION_CODES, DISPOSITION_CODES -> Form4Processor
    # - KEY_FINANCIAL_SUMMARY_METRICS -> FinancialDataProcessor

    def __init__(self, user_agent: str = None, cache_dir: Optional[str] = None,
                 db_path: Optional[str] = None, rate_limit_sleep: float = 0.1):
        """
        Initialize the SECDataFetcher orchestrator.

        Args:
            user_agent (str, optional): User-Agent string for SEC API requests.
                             Format: "Name (first/last) your@email.com".
                             Defaults to None, attempts to read from 'SEC_API_USER_AGENT' env var.
            cache_dir (str, optional): Directory for storing cached data. Defaults to "data/edgar".
            rate_limit_sleep (float, optional): Seconds to wait between API requests for the HTTP client.
                                               Defaults to 0.1.
        """
        # Resolve base paths without relying on CWD
        base_dir = Path(__file__).resolve().parents[2]
        resolved_cache_dir = Path(cache_dir) if cache_dir else base_dir / "data" / "edgar"
        resolved_db_path = Path(db_path) if db_path else base_dir / "data" / "sentinel.db"

        # Initialize HttpClient, CacheManager, and other processors here
        resolved_user_agent = user_agent or os.environ.get('SEC_API_USER_AGENT')
        self.http_client = SecHttpClient(user_agent=resolved_user_agent, rate_limit_sleep=rate_limit_sleep)
        self.cache_manager = SecCacheManager(cache_dir=str(resolved_cache_dir))
        self.sql_manager = SqlCacheManager(db_path=str(resolved_db_path)) # Initialize SQL manager
        self.document_handler = FilingDocumentHandler(http_client=self.http_client, cik_lookup_func=self.get_cik_for_ticker)
        self.form4_processor = Form4Processor(
            document_handler=self.document_handler,
            fetch_filings_func=self.fetch_insider_filings # Pass the method directly
        )
        self.financial_processor = FinancialDataProcessor(
            fetch_facts_func=self.get_company_facts # Pass the method directly
        )
        self.supply_chain_parser = SupplyChainParser() # Initialize Parser


    # === CORE ORCHESTRATION METHODS ===

    async def get_cik_for_ticker(self, ticker: str) -> Optional[str]:
        """Retrieves the 10-digit CIK for a given stock ticker symbol."""
        ticker = ticker.upper()
        cik = await self.cache_manager.load_cik(ticker)
        if cik: return cik

        logging.info(f"CIK for {ticker} not in cache. Fetching map...")
        success = await self._fetch_and_cache_cik_map()
        if success:
            cik = await self.cache_manager.load_cik(ticker)
            if cik: return cik

        logging.error(f"Could not find or fetch CIK for {ticker}.")
        return None

    async def _fetch_and_cache_cik_map(self) -> bool:
        """Fetches the official Ticker-CIK map from SEC and caches it."""
        url = "https://www.sec.gov/files/company_tickers.json"
        logging.info(f"Fetching Ticker-CIK map from {url}")
        try:
            # Prepare headers for www.sec.gov
            temp_headers = self.http_client.default_headers.copy()
            temp_headers['Host'] = 'www.sec.gov'

            sec_map_data = await self.http_client.make_request(url, headers=temp_headers, is_json=True)
            if not sec_map_data or not isinstance(sec_map_data, dict):
                logging.error(f"Failed to fetch/parse Ticker-CIK map from {url}. Type: {type(sec_map_data)}")
                return False

            ticker_to_cik = {}
            for _index, company_info in sec_map_data.items():
                ticker_val = company_info.get('ticker')
                cik_int = company_info.get('cik_str')
                if ticker_val and cik_int:
                    cik_str = str(cik_int).zfill(10)
                    ticker_to_cik[ticker_val.upper()] = cik_str

            await self.cache_manager.save_cik_map(ticker_to_cik)
            return True

        except Exception as e:
            logging.error(f"Error during Ticker-CIK map fetch: {e}", exc_info=True)
            return False

    async def get_company_info(self, ticker: str, use_cache: bool = True) -> Optional[Dict]:
        """Fetches basic company information using the SEC submissions endpoint."""
        ticker = ticker.upper()
        if use_cache:
            cache_data = await self.cache_manager.load_data(ticker, 'company_info')
            if cache_data: return cache_data

        cik = await self.get_cik_for_ticker(ticker)
        if not cik: return None

        submissions_url = self.SUBMISSIONS_ENDPOINT.format(cik=cik)
        try:
            response = await self.http_client.make_request(submissions_url, is_json=True)
            if response is None or not isinstance(response, dict):
                logging.error(f"Failed info lookup for {ticker}. Type: {type(response)}")
                return None

            # Extract relevant company info (can be expanded)
            company_info = {
                'ticker': ticker,
                'cik': cik,
                'name': response.get('name'),
                'sic': response.get('sic'),
                'sic_description': response.get('sicDescription'),
                'address': response.get('addresses', {}).get('mailing'),
                'phone': response.get('phone'),
                'exchange': response.get('exchanges'), # Often a list
            }
            await self.cache_manager.save_data(ticker, 'company_info', company_info, cik=cik)
            return company_info
        except Exception as e:
            logging.error(f"Error fetching company info for {ticker}: {e}")
            return None

    async def get_company_submissions(self, ticker: str, use_cache: bool = True) -> Optional[Dict]:
        """ Fetches the complete submissions data for a company."""
        ticker = ticker.upper()
        if use_cache:
            cache_data = await self.cache_manager.load_data(ticker, 'submissions')
            if cache_data: return cache_data

        cik = await self.get_cik_for_ticker(ticker)
        if not cik: return None

        submissions_url = self.SUBMISSIONS_ENDPOINT.format(cik=cik)
        try:
            response = await self.http_client.make_request(submissions_url, is_json=True)
            if response is None or not isinstance(response, dict):
                 logging.error(f"Failed submissions lookup for {ticker}. Type: {type(response)}")
                 return None

            await self.cache_manager.save_data(ticker, 'submissions', response, cik=cik)
            return response
        except Exception as e:
             logging.error(f"Error fetching submissions for {ticker}: {e}")
             return None

    async def get_company_facts(self, ticker: str, use_cache: bool = True) -> Optional[Dict]:
        """[Orchestrator] Fetches company facts (XBRL data) needed by FinancialProcessor."""
        ticker = ticker.upper()
        if use_cache:
            cached_data = await self.cache_manager.load_data(ticker, 'facts')
            if cached_data: return cached_data

        cik = await self.get_cik_for_ticker(ticker)
        if not cik: return None

        facts_url = self.COMPANY_FACTS_ENDPOINT.format(cik=cik)
        logging.info(f"Fetching company facts from: {facts_url}")
        try:
            # Use default headers from http_client for data.sec.gov
            company_facts_data = await self.http_client.make_request(facts_url, is_json=True)
            if company_facts_data is None or not isinstance(company_facts_data, dict):
                 logging.error(f"Failed facts lookup for {ticker}. Type: {type(company_facts_data)}")
                 return None

            await self.cache_manager.save_data(ticker, 'facts', company_facts_data, cik=cik)
            return company_facts_data
        except Exception as e:
            logging.error(f"Error fetching facts for {ticker}: {e}")
            return None

    def _normalize_cik(self, cik_value: Any) -> Optional[str]:
        """Normalize CIK into a zero-padded 10-digit string."""
        if cik_value is None:
            return None
        cik_str = str(cik_value).strip()
        if not cik_str.isdigit():
            return None
        return cik_str.zfill(10)

    def _extract_recent_filings(self, submissions: Dict, cik_for_url: str, form_filter: Optional[str] = None) -> List[Dict]:
        """Extract filings list from submissions JSON, optionally filtering by form type."""
        recent_filings = submissions.get('filings', {}).get('recent', {})
        if not recent_filings:
            return []

        form_list = recent_filings.get('form', [])
        filing_date_list = recent_filings.get('filingDate', [])
        accession_number_list = recent_filings.get('accessionNumber', [])
        report_date_list = recent_filings.get('reportDate', [])
        primary_document_list = recent_filings.get('primaryDocument', [])
        primary_doc_desc_list = recent_filings.get('primaryDocDescription', [])

        min_len = min(len(form_list), len(filing_date_list), len(accession_number_list))
        filings = []
        for i in range(min_len):
            form = form_list[i]
            if form_filter and form != form_filter:
                continue
            filing_date = filing_date_list[i] if i < len(filing_date_list) else None
            accession_no = accession_number_list[i]
            report_date = report_date_list[i] if i < len(report_date_list) else None

            accession_no_cleaned = accession_no.replace('-', '')
            filing_url = f"https://www.sec.gov/Archives/edgar/data/{cik_for_url}/{accession_no_cleaned}/"

            primary_doc = primary_document_list[i] if i < len(primary_document_list) else None
            primary_doc_desc = primary_doc_desc_list[i] if i < len(primary_doc_desc_list) else None

            filings.append({
                'accession_no': accession_no,
                'filing_date': filing_date,
                'form': form,
                'report_date': report_date,
                'url': filing_url,
                'primary_document': primary_doc,
                'primary_document_description': primary_doc_desc
            })

        return filings

    async def get_filings_by_form(self, ticker: str, form_type: str, days_back: int = 90, use_cache: bool = True) -> List[Dict]:
        """ Fetches a list of filings of a specific form type within a given timeframe."""
        ticker = ticker.upper()
        if not form_type:
            logging.error("Form type is required for get_filings_by_form.")
            return []
        logging.debug(f"Getting {form_type} filings for {ticker} (days back: {days_back}, cache: {use_cache})")
        if use_cache:
            cache_data = await self.cache_manager.load_data(ticker, 'forms', form_type=form_type)
            if cache_data:
                cached_filings = None
                if isinstance(cache_data, dict) and isinstance(cache_data.get("filings"), list):
                    cached_filings = cache_data.get("filings")
                elif isinstance(cache_data, list):
                    logging.warning(f"Legacy cache format for {ticker} {form_type}; ignoring to avoid partial results.")
                    cached_filings = None

                if cached_filings is not None:
                    cutoff_date = (datetime.now() - timedelta(days=days_back)).strftime('%Y-%m-%d')
                    filtered_cache = [f for f in cached_filings if f.get('filing_date', '') >= cutoff_date]
                    logging.debug(f"Found {len(filtered_cache)} fresh {form_type} filings in cache for {ticker}.")
                    return filtered_cache

        submissions = await self.get_company_submissions(ticker, use_cache=use_cache)
        if not submissions:
            logging.warning(f"No submissions data found for {ticker}, cannot extract filings.")
            return []

        cik = self._normalize_cik(submissions.get('cik'))
        if not cik:
            cik_lookup = await self.get_cik_for_ticker(ticker)
            if not cik_lookup:
                logging.error(f"Cannot get CIK for {ticker} to process filings.")
                return []
            cik = cik_lookup

        cik_for_url = str(cik).lstrip('0')

        cutoff_date = (datetime.now() - timedelta(days=days_back)).strftime('%Y-%m-%d')
        try:
            filings_all = self._extract_recent_filings(submissions, cik_for_url, form_filter=form_type)
            if not filings_all:
                logging.info(f"No '{form_type}' filings found in submissions data for {ticker}.")
                return []

            await self.cache_manager.save_data(
                ticker,
                'forms',
                {
                    "as_of": datetime.now().strftime("%Y-%m-%d"),
                    "form_type": form_type,
                    "filings": filings_all
                },
                form_type=form_type
            )

            filtered = [f for f in filings_all if f.get('filing_date', '') >= cutoff_date]
            logging.info(f"Extracted {len(filtered)} {form_type} filings for {ticker} within {days_back} days.")
            return filtered
        except Exception as e:
             logging.error(f"Error processing filings for {ticker}: {e}", exc_info=True)
             return []

    async def get_recent_filings(self, ticker: str, days_back: int = 90, use_cache: bool = True) -> List[Dict]:
        """Fetches recent filings across all form types for a ticker."""
        ticker = ticker.upper()
        submissions = await self.get_company_submissions(ticker, use_cache=use_cache)
        if not submissions:
            logging.warning(f"No submissions data found for {ticker}, cannot extract filings.")
            return []

        cik = self._normalize_cik(submissions.get('cik'))
        if not cik:
            cik_lookup = await self.get_cik_for_ticker(ticker)
            if not cik_lookup:
                logging.error(f"Cannot get CIK for {ticker} to process filings.")
                return []
            cik = cik_lookup

        cik_for_url = str(cik).lstrip('0')
        filings_all = self._extract_recent_filings(submissions, cik_for_url, form_filter=None)
        if not filings_all:
            logging.info(f"No recent filings found in submissions data for {ticker}.")
            return []

        cutoff_date = (datetime.now() - timedelta(days=days_back)).strftime('%Y-%m-%d')
        filtered = [f for f in filings_all if f.get('filing_date', '') >= cutoff_date]
        logging.info(f"Extracted {len(filtered)} filings for {ticker} within {days_back} days.")
        return filtered

    # === CONVENIENCE FILING WRAPPERS ===
    async def fetch_insider_filings(self, ticker: str, days_back: int = 90, use_cache: bool = True) -> List[Dict]:
        """Convenience method to fetch Form 4 (insider trading) filings."""
        return await self.get_filings_by_form(ticker, "4", days_back, use_cache)

    async def fetch_annual_reports(self, ticker: str, days_back: int = 365*2, use_cache: bool = True) -> List[Dict]:
        """Convenience method to fetch Form 10-K (annual report) filings."""
        return await self.get_filings_by_form(ticker, "10-K", days_back, use_cache)

    async def fetch_quarterly_reports(self, ticker: str, days_back: int = 365, use_cache: bool = True) -> List[Dict]:
        """Convenience method to fetch Form 10-Q (quarterly report) filings."""
        return await self.get_filings_by_form(ticker, "10-Q", days_back, use_cache)

    async def fetch_current_reports(self, ticker: str, days_back: int = 90, use_cache: bool = True) -> List[Dict]:
        """Convenience method to fetch Form 8-K (current report) filings."""
        return await self.get_filings_by_form(ticker, "8-K", days_back, use_cache)

    # --- DELEGATED PROCESSOR METHODS --- 

    async def get_recent_insider_transactions(self, ticker: str, days_back: int = 90,
                                             use_cache: bool = True, filing_limit: int = 10) -> List[Dict]:
        """Fetches, parses, and formats recent Form 4 transactions for UI display (Delegated)."""
        return await self.form4_processor.get_recent_insider_transactions(
            ticker=ticker,
            days_back=days_back,
            use_cache=use_cache,
            filing_limit=filing_limit
        )

    async def analyze_insider_transactions(self, ticker: str, days_back: int = 90, use_cache: bool = True) -> Dict:
        """Performs basic analysis on recent insider transactions (Delegated)."""
        return await self.form4_processor.analyze_insider_transactions(
            ticker=ticker,
            days_back=days_back,
            use_cache=use_cache
        )

    async def get_financial_summary(self, ticker: str, use_cache: bool = True) -> Optional[Dict]:
        """Generates a flattened summary of key financial metrics for the UI (Delegated)."""
        summary = await self.financial_processor.get_financial_summary(ticker=ticker, use_cache=use_cache)
        
        if summary:
            # Persist to SQL for faster future access and querying
            try:
                await self.sql_manager.initialize_db() # Ensure DB is ready (idempotent)
                await self.sql_manager.save_financial_history(ticker, summary)
            except Exception as e:
                logging.warning(f"SQL cache unavailable or failed for {ticker}: {e}")
            
        return summary

    async def get_supply_chain(self, ticker: str) -> List[Dict]:
        """
        Extracts supply chain relationships for a company from its latest 10-K.
        
        Orchestrates:
        1. Fetch 10-K filings list.
        2. Get primary HTML of the latest 10-K.
        3. Clean and Parse HTML.
        4. Save extracted edges to SQL.
        5. Return relationships.
        
        Args:
            ticker (str): Ticker symbol.
            
        Returns:
            List[Dict]: List of relationship dictionaries.
        """
        # 1. Get latest 10-K
        filings = await self.fetch_annual_reports(ticker, days_back=365*2)
        if not filings:
            logging.warning(f"No recent 10-K found for {ticker} to extract supply chain.")
            return []
            
        latest_10k = filings[0]
        accession_no = latest_10k['accession_no']
        doc_date = latest_10k['filing_date']
        
        logging.info(f"Extracting supply chain for {ticker} from 10-K ({doc_date})...")
        
        # 2. Fetch Raw HTML
        raw_html = await self.document_handler.fetch_primary_html(accession_no, ticker)
        if not raw_html:
            logging.error(f"Failed to fetch HTML content for 10-K {accession_no}")
            return []
            
        # 3. Parse
        clean_text = self.supply_chain_parser.clean_html(raw_html)
        sections = self.supply_chain_parser.extract_sections(clean_text)
        relationships = self.supply_chain_parser.extract_relationships(sections)
        
        logging.info(f"Extracted {len(relationships)} relationships for {ticker}.")
        
        # 4. Save to SQL
        await self.sql_manager.initialize_db()
        for rel in relationships:
            await self.sql_manager.save_relationship(
                source_ticker=ticker,
                target_entity=rel['target_entity'],
                relationship_type=rel['relationship_type'],
                weight=rel['weight'],
                context=rel['context'],
                confidence=rel['confidence_score'],
                doc_date=doc_date
            )
            
        return relationships

    # === OTHER PUBLIC METHODS ===

    async def fetch_multiple_tickers(self, tickers: List[str], form_type: str = "4", days_back: int = 90, use_cache: bool = True) -> Dict[str, List]:
        """ Fetches filings for multiple tickers concurrently."""
        logging.info(f"Fetching {form_type} filings for {len(tickers)} tickers (days back: {days_back}, cache: {use_cache})...")
        # Use asyncio.gather to run fetches in parallel
        tasks = [self.get_filings_by_form(ticker, form_type, days_back, use_cache) for ticker in tickers]
        results_list = await asyncio.gather(*tasks, return_exceptions=True) # Capture exceptions

        results = {}
        for i, ticker in enumerate(tickers):
            if isinstance(results_list[i], Exception):
                logging.error(f"Error fetching {form_type} data for {ticker}: {results_list[i]}")
                results[ticker] = [] # Return empty list on error
            else:
                results[ticker] = results_list[i]
                logging.debug(f"Found {len(results[ticker])} {form_type} filings for {ticker}")

        logging.info(f"Finished fetching filings for {len(tickers)} tickers.")
        return results

    async def close(self):
        """Closes resources like the HTTP client session."""
        if hasattr(self, 'http_client') and self.http_client:
             await self.http_client.close()
        else:
             logging.warning("HttpClient not initialized, cannot close session.")
        # Close other resources if needed
