import logging
import re
from datetime import datetime, timedelta
from typing import Dict, Optional, List, Callable, Awaitable

class FinancialDataProcessor:
    """Handles the processing and summarization of financial data extracted from SEC Company Facts (XBRL API).

    This class focuses on consuming the structured data available through the SEC's
    `/api/xbrl/companyfacts/` endpoint. Its primary roles are:
    - Defining a mapping (`KEY_FINANCIAL_SUMMARY_METRICS`) between desired summary
      metric names (e.g., 'revenue', 'net_income') and their corresponding XBRL tags
      within specific taxonomies (usually 'us-gaap' or 'dei').
    - Providing a method (`_get_fact_history`) to navigate the raw company facts JSON
      and extract historical time-series data for a given XBRL concept, organized by quarterly
      and annual periods, considering units (prioritizing USD or shares) and reporting period end dates.
    - Orchestrating the retrieval of raw company facts (via an injected function) and using
      the defined mapping and extraction logic to produce a flattened dictionary
      (`get_financial_summary`) containing key financial metrics suitable for display or further analysis.
    - Includes a placeholder for potential future financial ratio calculations.

    It relies on an injected function (typically from `SECDataFetcher`) to obtain the raw
    company facts JSON data for a given ticker.
    """

    # Define key metrics mapping for the financial summary required by the UI
    # Keys are snake_case matching the target flat dictionary output.
    # Values are the corresponding us-gaap or dei XBRL tags.
    KEY_FINANCIAL_SUMMARY_METRICS = {
        # Income Statement
        "revenue": ("us-gaap", "RevenueFromContractWithCustomerExcludingAssessedTax"),
        "net_income": ("us-gaap", "NetIncomeLoss"),
        "eps": ("us-gaap", "EarningsPerShareBasic"), # Using Basic EPS for UI
        # Balance Sheet
        "assets": ("us-gaap", "Assets"),
        "liabilities": ("us-gaap", "Liabilities"),
        "equity": ("us-gaap", "StockholdersEquity"),
        # Cash Flow
        "operating_cash_flow": ("us-gaap", "NetCashProvidedByUsedInOperatingActivities"),
        "investing_cash_flow": ("us-gaap", "NetCashProvidedByUsedInInvestingActivities"),
        "financing_cash_flow": ("us-gaap", "NetCashProvidedByUsedInFinancingActivities"),
        # Other potentially useful
        # "shares_outstanding": ("dei", "EntityCommonStockSharesOutstanding") # Example DEI tag
    }

    def __init__(self, fetch_facts_func: Callable[[str, bool], Awaitable[Optional[Dict]]]):
        """
        Initializes the Financial Data Processor.

        Args:
            fetch_facts_func (Callable[[str, bool], Awaitable[Optional[Dict]]]): An awaitable
                function that takes a ticker (str) and a use_cache flag (bool) and returns
                the raw company facts JSON data as a dictionary (Optional[Dict]).
                This is typically bound to `SECDataFetcher.get_company_facts`.
        """
        self.fetch_company_facts = fetch_facts_func

    def _format_period(self, entry: Dict) -> str:
        """
        Formats a period string from frame or fp+fy fields.
        
        Args:
            entry (Dict): A fact entry containing 'frame', 'fp', and 'fy' fields.
            
        Returns:
            str: Formatted period string like "Q3 2024" or "2024".
        """
        frame = entry.get('frame', '')
        fp = entry.get('fp', '')
        fy = entry.get('fy')
        
        # Prioritize frame field if available
        if frame:
            # Check for quarterly pattern: CY2023Q3 or CY2023Q3I
            quarterly_match = re.match(r'CY(\d{4})Q(\d)', frame)
            if quarterly_match:
                year = quarterly_match.group(1)
                quarter = quarterly_match.group(2)
                return f"Q{quarter} {year}"
            
            # Check for annual pattern: CY2023
            annual_match = re.match(r'CY(\d{4})$', frame)
            if annual_match:
                return annual_match.group(1)
        
        # Fall back to fp + fy
        if fp and fy:
            if fp.startswith('Q'):
                return f"{fp} {fy}"
            elif fp == 'FY':
                return str(fy)
        
        # Last resort: just return the year
        if fy:
            return str(fy)
        
        return "Unknown"

    def _is_quarterly(self, entry: Dict) -> bool:
        """
        Determines if an entry represents quarterly data based on frame or fp field.
        
        Args:
            entry (Dict): A fact entry containing 'frame' or 'fp' fields.
            
        Returns:
            bool: True if quarterly, False if annual or unknown.
        """
        frame = entry.get('frame', '')
        fp = entry.get('fp', '')
        
        # Check frame first (standardized by SEC)
        if frame:
            # Quarterly: CY2023Q3, CY2023Q3I, etc.
            if re.match(r'CY\d{4}Q\d', frame):
                return True
            # Annual: CY2023 (no Q)
            if re.match(r'CY\d{4}$', frame):
                return False
        
        # Fall back to fp field
        if fp:
            return fp.startswith('Q')
        
        return False

    def _calculate_duration_days(self, entry: Dict) -> Optional[int]:
        """
        Calculates the duration in days for an entry.
        
        Args:
            entry (Dict): A fact entry containing 'start' and 'end' date fields.
            
        Returns:
            Optional[int]: Duration in days, or None if start date is not available.
        """
        start_date = entry.get('start')
        end_date = entry.get('end')
        
        if not start_date or not end_date:
            return None
        
        try:
            start = datetime.strptime(start_date, '%Y-%m-%d')
            end = datetime.strptime(end_date, '%Y-%m-%d')
            duration = (end - start).days
            return duration
        except (ValueError, TypeError) as e:
            logging.debug(f"Could not parse dates for duration calculation: {e}")
            return None

    def _metric_requires_duration(self, metric_key: Optional[str]) -> bool:
        """
        Determines whether a metric concept represents a duration-based fact.

        Duration-based facts (income statement, cash flow, EPS, etc.) should always
        include a start/end date window. Instant metrics (balance sheet items) do not
        require a duration and should not be rejected when the start date is absent.

        Args:
            metric_key (Optional[str]): Metric identifier such as 'revenue' or 'assets'.

        Returns:
            bool: True if the metric should have a duration, False otherwise.
        """
        instant_metrics = {"assets", "liabilities", "equity", "cash_equivalents"}
        if metric_key is None:
            return True

        return metric_key not in instant_metrics

    def _filter_by_duration(self, entries: List[Dict], form_type: str, is_quarterly: bool, requires_duration: bool) -> List[Dict]:
        """
        Filters entries by duration to exclude YTD/cumulative values.
        
        For 10-Q forms:
        - Quarterly entries should be 85-95 days (3-month period)
        - Rejects ~270-day YTD entries
        
        For 10-K forms:
        - Annual entries should be 360-370 days (full year)
        - Or 85-95 days if it's Q4 (sometimes Q4 is in 10-K)
        
        Args:
            entries (List[Dict]): List of entries to filter
            form_type (str): Form type ('10-Q', '10-K', etc.)
            is_quarterly (bool): Whether these are quarterly entries
            requires_duration (bool): Whether entries lacking duration should be rejected

        Returns:
            List[Dict]: Filtered entries that match duration criteria
        """
        filtered = []
        
        for entry in entries:
            form = entry.get('form', '')
            duration = self._calculate_duration_days(entry)
            
            # If no duration info available, enforce requirement for duration-based metrics
            if duration is None:
                if requires_duration:
                    logging.debug(
                        f"Rejected {form} entry missing duration for metric requiring a period window"
                    )
                    continue

                filtered.append(entry)
                continue
            
            # Filter 10-Q quarterly entries: must be 85-95 days
            if form_type == '10-Q' and is_quarterly:
                if 85 <= duration <= 95:
                    filtered.append(entry)
                else:
                    logging.debug(f"Rejected {form} entry with duration {duration} days (expected 85-95 for quarterly)")
            
            # Filter 10-K annual entries: 360-370 days OR 85-95 days (Q4)
            elif form_type == '10-K' and not is_quarterly:
                if 360 <= duration <= 370:
                    filtered.append(entry)
                elif 85 <= duration <= 95:
                    # Q4 might be in 10-K, check if fp is Q4
                    fp = entry.get('fp', '')
                    if fp == 'Q4':
                        filtered.append(entry)
                    else:
                        logging.debug(f"Rejected {form} entry with duration {duration} days (not Q4)")
                else:
                    logging.debug(f"Rejected {form} entry with duration {duration} days (expected 360-370 or 85-95 for Q4)")
            
            # For other forms or cases, include the entry
            else:
                filtered.append(entry)
        
        return filtered

    def _deduplicate_entries(self, entries: List[Dict]) -> List[Dict]:
        """
        Deduplicates entries that have the same (period, end_date).
        
        Strategy:
        1. Prefer entries with start date (can verify duration)
        2. For entries without start date, prefer smaller values (quarterly vs YTD)
        3. Prefer latest filed date as tiebreaker
        
        Args:
            entries (List[Dict]): List of entries that may have duplicates
            
        Returns:
            List[Dict]: Deduplicated entries
        """
        # Group by (period, end_date)
        grouped = {}
        for entry in entries:
            key = (entry.get('period'), entry.get('date'))
            if key not in grouped:
                grouped[key] = []
            grouped[key].append(entry)
        
        deduplicated = []
        for key, group in grouped.items():
            if len(group) == 1:
                deduplicated.append(group[0])
            else:
                # Multiple entries with same period/date
                # Strategy: prefer entries with start date, then smaller values (quarterly vs YTD)
                entries_with_start = [e for e in group if e.get('start')]
                entries_without_start = [e for e in group if not e.get('start')]
                
                def _get_filed_timestamp(entry: Dict) -> float:
                    """Helper to safely parse filed date to timestamp."""
                    filed = entry.get('filed')
                    if not filed:
                        return 0.0
                    try:
                        return datetime.strptime(filed, '%Y-%m-%d').timestamp()
                    except (ValueError, TypeError):
                        return 0.0
                
                if entries_with_start:
                    # Prefer entries with start date (we can verify duration)
                    # Among those, prefer smaller values (quarterly vs cumulative), then latest filed
                    best_entry = min(entries_with_start, key=lambda e: (
                        e.get('value', float('inf')),  # Smaller value (quarterly vs YTD)
                        -_get_filed_timestamp(e)  # Latest filed date
                    ))
                elif entries_without_start:
                    # No start dates available - prefer smaller values (likely quarterly)
                    # Sort by value ascending, then by latest filed date
                    best_entry = min(entries_without_start, key=lambda e: (
                        e.get('value', float('inf')),  # Smaller value first
                        -_get_filed_timestamp(e)  # Then latest filed date
                    ))
                else:
                    # Fallback: just use latest filed date
                    best_entry = max(group, key=lambda e: _get_filed_timestamp(e))
                
                deduplicated.append(best_entry)
                if len(group) > 1:
                    logging.debug(f"Deduplicated {len(group)} entries for {key}, kept entry with value {best_entry.get('value')} filed {best_entry.get('filed')}")
        
        return deduplicated

    def _get_fact_history(self, facts_data: Dict, taxonomy: str, concept_tag: str, metric_key: Optional[str] = None) -> Optional[Dict]:
        """
        Extracts historical time-series data for a specific XBRL concept from raw facts data.

        Navigates the nested structure of the company facts JSON (`facts_data`) based on the
        provided `taxonomy` (e.g., 'us-gaap') and `concept_tag` (e.g., 'Assets').
        It identifies the relevant units (preferring 'USD' or 'shares') and then processes
        all data points, categorizing them into quarterly and annual periods based on the
        'frame' field (if available) or 'fp' (Fiscal Period) field.

        **YTD Filtering:** This method filters out Year-To-Date (YTD) cumulative values that
        can appear in 10-Q filings alongside quarterly values. It uses duration filtering
        (85-95 days for quarterly, 360-370 days for annual) when 'start' dates are available,
        and deduplication logic (preferring smaller values) when they are not.

        Args:
            facts_data (Dict): The raw JSON dictionary obtained from the SEC Company Facts API
                (usually via the `fetch_company_facts` function).
            taxonomy (str): The XBRL taxonomy where the concept is defined (e.g., 'us-gaap', 'dei').
            concept_tag (str): The specific XBRL concept tag to extract data for
                (e.g., 'RevenueFromContractWithCustomerExcludingAssessedTax').
            metric_key (Optional[str]): Metric identifier used to decide if duration
                should be enforced (e.g., 'revenue').

        Returns:
            Optional[Dict]: A dictionary with 'quarterly' and 'annual' keys, each containing
                a list of entries sorted by 'end' date descending (newest first). Each entry
                contains 'period', 'date', 'value', and 'form' fields. Returns None if the
                concept, unit, or valid data points cannot be found.
        """
        try:
            concept_data = facts_data.get('facts', {}).get(taxonomy, {}).get(concept_tag)
            if not concept_data:
                # logging.debug(f"Concept '{concept_tag}' not found in {taxonomy}.")
                return None

            units = concept_data.get('units')
            if not units:
                # logging.debug(f"No units for concept '{concept_tag}'.")
                return None

            # Collect data from all units (USD, shares, etc.)
            all_entries = []
            for unit_name, unit_data in units.items():
                if not isinstance(unit_data, list):
                    continue
                
                for entry in unit_data:
                    if not entry or 'val' not in entry or 'end' not in entry:
                        continue
                    
                    # Create formatted entry (include start for duration calculation)
                    formatted_entry = {
                        "period": self._format_period(entry),
                        "date": entry.get('end'),
                        "end": entry.get('end'),
                        "value": entry.get('val'),
                        "form": entry.get('form', 'N/A'),
                        "unit": unit_name,
                        "fy": entry.get('fy'),
                        "fp": entry.get('fp'),
                        "frame": entry.get('frame'),
                        "filed": entry.get('filed'),
                        "start": entry.get('start')  # Include for duration filtering
                    }
                    all_entries.append(formatted_entry)

            if not all_entries:
                # logging.debug(f"No valid data for concept '{concept_tag}'.")
                return None

            # Prefer USD or shares units if available, otherwise use all
            preferred_units = ['USD', 'shares']
            preferred_entries = [e for e in all_entries if e.get('unit') in preferred_units]
            if preferred_entries:
                all_entries = preferred_entries
            
            # Further filter: if we have USD, prefer USD; if we have shares, prefer shares
            if any(e.get('unit') == 'USD' for e in all_entries):
                all_entries = [e for e in all_entries if e.get('unit') == 'USD']
            elif any(e.get('unit') == 'shares' for e in all_entries):
                all_entries = [e for e in all_entries if e.get('unit') == 'shares']

            # Categorize into quarterly and annual
            quarterly = []
            annual = []

            requires_duration = self._metric_requires_duration(metric_key)
            
            for entry in all_entries:
                if self._is_quarterly(entry):
                    quarterly.append(entry)
                else:
                    annual.append(entry)

            # Filter by duration to exclude YTD/cumulative values
            # Group by form type for proper filtering
            quarterly_by_form = {}
            annual_by_form = {}
            
            for entry in quarterly:
                form = entry.get('form', '')
                if form not in quarterly_by_form:
                    quarterly_by_form[form] = []
                quarterly_by_form[form].append(entry)
            
            for entry in annual:
                form = entry.get('form', '')
                if form not in annual_by_form:
                    annual_by_form[form] = []
                annual_by_form[form].append(entry)
            
            # Apply duration filtering
            quarterly_filtered = []
            for form, entries in quarterly_by_form.items():
                filtered = self._filter_by_duration(entries, form, is_quarterly=True, requires_duration=requires_duration)
                quarterly_filtered.extend(filtered)

            annual_filtered = []
            for form, entries in annual_by_form.items():
                filtered = self._filter_by_duration(entries, form, is_quarterly=False, requires_duration=requires_duration)
                annual_filtered.extend(filtered)
            
            # Deduplicate entries with same (period, end_date)
            quarterly = self._deduplicate_entries(quarterly_filtered)
            annual = self._deduplicate_entries(annual_filtered)

            # Sort by date descending (newest first)
            quarterly.sort(key=lambda x: x.get('date', '0000-00-00'), reverse=True)
            annual.sort(key=lambda x: x.get('date', '0000-00-00'), reverse=True)

            # Remove extra fields from final output (keep only period, date, value, form)
            quarterly_clean = [
                {
                    "period": e["period"],
                    "date": e["date"],
                    "value": e["value"],
                    "form": e["form"]
                }
                for e in quarterly
            ]
            
            annual_clean = [
                {
                    "period": e["period"],
                    "date": e["date"],
                    "value": e["value"],
                    "form": e["form"]
                }
                for e in annual
            ]

            return {
                "quarterly": quarterly_clean,
                "annual": annual_clean
            }

        except Exception as e:
            logging.error(f"Error processing concept '{concept_tag}' in {taxonomy}: {e}", exc_info=True)
            return None

    async def get_financial_summary(self, ticker: str, use_cache: bool = True) -> Optional[Dict]:
        """
        Generates a summary dictionary of key financial metrics with historical time-series data for a given ticker.

        This is the main public method of the processor. It orchestrates the process:
        1. Calls the injected `fetch_company_facts` function to get the raw XBRL data.
        2. Iterates through the `KEY_FINANCIAL_SUMMARY_METRICS` mapping.
        3. For each metric, calls `_get_fact_history` to extract historical time-series data.
        4. Populates a dictionary with the extracted history (quarterly and annual), along with metadata like
           ticker, entity name, CIK, and the estimated source form and period end date
           (based on the latest end date found among key income statement metrics).

        Args:
            ticker (str): The stock ticker symbol for which to generate the summary.
            use_cache (bool, optional): Passed to the `fetch_company_facts` function to indicate
                whether cached data should be used if available and fresh. Defaults to True.

        Returns:
            Optional[Dict]: A dictionary containing the requested financial summary with historical data.
                Keys include 'ticker', 'entityName', 'cik', 'source_form', 'period_end',
                and the keys defined in `KEY_FINANCIAL_SUMMARY_METRICS` (e.g., 'revenue',
                'net_income', 'assets'). Each metric value is a dictionary with 'quarterly' and
                'annual' keys, each containing a list of entries sorted by date descending.
                Returns None if the initial company facts data cannot be retrieved or if none
                of the requested metrics are found.
        """
        company_facts = await self.fetch_company_facts(ticker, use_cache=use_cache)
        if not company_facts:
            logging.warning(f"Could not retrieve company facts for {ticker}. Cannot generate financial summary.")
            return None

        summary_data = {
            "ticker": ticker.upper(),
            "entityName": company_facts.get('entityName', "N/A"),
            "cik": company_facts.get('cik', "N/A"),
            "source_form": None,
            "period_end": None,
        }

        # Initialize all required metric keys to None
        for key in self.KEY_FINANCIAL_SUMMARY_METRICS.keys():
            summary_data[key] = None

        latest_period_info = {"end_date": "0000-00-00", "form": None}
        has_data = False

        # Iterate through the required metrics defined in the mapping
        for metric_key, (taxonomy, concept_tag) in self.KEY_FINANCIAL_SUMMARY_METRICS.items():
            fact_history = self._get_fact_history(company_facts, taxonomy, concept_tag)

            if fact_history:
                has_data = True
                # Store the full history structure (quarterly and annual)
                summary_data[metric_key] = {
                    "quarterly": fact_history.get("quarterly", []),
                    "annual": fact_history.get("annual", [])
                }

                # Try to determine the most recent period end date and form
                quarterly = fact_history.get("quarterly", [])
                annual = fact_history.get("annual", [])
                
                # Check both quarterly and annual, keeping the most recent
                for entry_list in [quarterly, annual]:
                    if entry_list:
                        current_end_date = entry_list[0].get("date", "0000-00-00")
                        if current_end_date > latest_period_info["end_date"]:
                            latest_period_info["end_date"] = current_end_date
                            latest_period_info["form"] = entry_list[0].get("form")
            else:
                 logging.debug(f"Metric '{metric_key}' (Tag: {concept_tag}, Tax: {taxonomy}) not found/no data for {ticker}.")
                 summary_data[metric_key] = None

        summary_data["period_end"] = latest_period_info["end_date"] if latest_period_info["end_date"] != "0000-00-00" else None
        summary_data["source_form"] = latest_period_info["form"]

        if not has_data:
             logging.warning(f"No financial metrics found for {ticker} based on defined tags.")
             return None

        logging.info(f"Generated financial summary for {ticker} ending {summary_data['period_end']} from form {summary_data['source_form']}.")
        return summary_data

    # Placeholder for future ratio calculations
    def _calculate_ratios(self, summary_metrics: Dict) -> Dict:
        """
        Placeholder method for calculating financial ratios from the summary data.

        This method is not currently implemented or used. It would take the dictionary
        produced by `get_financial_summary` and compute common financial ratios
        (e.g., P/E, Debt-to-Equity) if needed.

        Args:
            summary_metrics (Dict): The dictionary containing the flattened summary metrics.

        Returns:
            Dict: A dictionary containing calculated ratios (currently empty).
        """
        logging.warning("_calculate_ratios is not fully implemented.")
        # TODO: Implement ratio calculations based on extracted summary_metrics if needed.
        return {} 