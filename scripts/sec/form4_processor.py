import logging
import xml.etree.ElementTree as ET
from collections import defaultdict
from datetime import datetime, timezone
from typing import List, Dict, Optional, TYPE_CHECKING, Callable, Awaitable, Any

try:
    import pandas as pd
except ImportError:  # pragma: no cover - optional dependency in lightweight environments
    pd = None

# Import FilingDocumentHandler for dependency injection
from .document_handler import FilingDocumentHandler

# Use TYPE_CHECKING block to avoid circular imports at runtime
# SECDataFetcher is needed for fetching filing metadata (get_filings_by_form)
if TYPE_CHECKING:
    from .sec_api import SECDataFetcher # type: ignore

class Form4Processor:
    """
    Handles the specialized processing, parsing, and analysis of SEC Form 4 filings (Insider Transactions).

    This class encapsulates all logic related to Form 4 data. Its core responsibilities include:
    - Parsing the XML structure of Form 4 filings to extract transaction details
      for both non-derivative and derivative securities.
    - Mapping transaction codes (e.g., 'P', 'S') to human-readable descriptions (e.g., 'Purchase', 'Sale').
    - Classifying transactions as acquisitions or dispositions based on codes.
    - Orchestrating the retrieval of recent Form 4 filing metadata (via an injected function).
    - Downloading the corresponding Form 4 XML documents (using the injected `FilingDocumentHandler`).
    - Processing multiple recent filings to compile a list of transactions formatted for display or analysis.
    - Performing basic quantitative analysis on the compiled transactions (e.g., buy/sell counts, net value).

    It depends on an injected `FilingDocumentHandler` for XML downloads and a function
    (usually from `SECDataFetcher`) to retrieve the list of recent Form 4 filing accession numbers.
    """

    # Transaction codes for Form 4 filings
    TRANSACTION_CODE_MAP = {
        'P': 'Purchase',
        'S': 'Sale',
        'A': 'Award',
        'D': 'Disposition (Gift/Other)',
        'F': 'Tax Withholding',
        'I': 'Discretionary Transaction',
        'M': 'Option Exercise',
        'C': 'Conversion',
        'W': 'Warrant Exercise',
        'G': 'Gift',
        'J': 'Other Acquisition/Disposition',
        'U': 'Tender of Shares',
        'X': 'Option Expiration',
        'Z': 'Trust Transaction',
    }
    # Define codes considered as 'Acquisition'/'Disposition'
    ACQUISITION_CODES = ['P', 'A', 'M', 'C', 'W', 'G', 'J', 'I']
    DISPOSITION_CODES = ['S', 'D', 'F', 'X', 'U', 'Z']
    SIGNAL_CLASS_MAP = {
        'P': 'open_market_buy',
        'S': 'open_market_sell',
        'F': 'tax_sale',
        'M': 'option_exercise',
        'A': 'award_or_grant',
        'G': 'gift',
        'D': 'gift',
        'C': 'derivative_conversion',
        'W': 'derivative_conversion',
    }
    ECONOMIC_INTENT_MAP = {
        'open_market_buy': 'bullish',
        'open_market_sell': 'bearish',
        'tax_sale': 'neutral',
        'option_exercise': 'neutral',
        'award_or_grant': 'compensation',
        'gift': 'neutral',
        'derivative_conversion': 'neutral',
        'planned_sale_10b5_1': 'bearish',
        'other': 'neutral',
    }

    def __init__(self, 
                 document_handler: FilingDocumentHandler, 
                 fetch_filings_func: Callable[..., Awaitable[List[Dict]]]):
        """
        Initializes the Form 4 Processor.

        Args:
            document_handler (FilingDocumentHandler): An instance of the
                `FilingDocumentHandler` used specifically for downloading the
                XML content of Form 4 filings.
            fetch_filings_func (Callable[..., Awaitable[List[Dict]]]): An awaitable
                function that retrieves the metadata for recent Form 4 filings
                (e.g., accession number, filing date) for a given ticker.
                This is typically bound to `SECDataFetcher.fetch_insider_filings`.
        """
        self.document_handler = document_handler
        self.fetch_filings_metadata = fetch_filings_func # e.g., SECDataFetcher.fetch_insider_filings

    def parse_form4_xml(self, xml_content: str) -> List[Dict]:
        """
        Parses the XML content of a single SEC Form 4 filing into structured transaction data.

        Uses `xml.etree.ElementTree` to navigate the standard Form 4 XML structure.
        Extracts details for both `nonDerivativeTransaction` and `derivativeTransaction` elements.
        Handles potential missing fields gracefully and attempts basic type conversion (e.g., float for shares/price).
        Adds derived fields like 'transaction_type', 'is_acquisition', 'is_disposition', and calculated 'value'.

        Args:
            xml_content (str): A string containing the complete XML content of a Form 4 filing.

        Returns:
            List[Dict]: A list of dictionaries, where each dictionary represents a single
                transaction (either non-derivative or derivative) parsed from the form.
                Returns an empty list if the XML is malformed or cannot be parsed.
                Keys in the dictionary include 'ticker', 'owner_name', 'transaction_date',
                'transaction_code', 'transaction_type', 'shares', 'price_per_share',
                'value', 'is_derivative', 'is_acquisition', 'is_disposition', etc.
        """
        transactions = []
        try:
            root = ET.fromstring(xml_content)

            # Extract common info
            issuer_cik = root.findtext('.//issuer/issuerCik', default='N/A')
            issuer_name = root.findtext('.//issuer/issuerName', default='N/A')
            issuer_symbol = root.findtext('.//issuer/issuerTradingSymbol', default='N/A')

            owner_cik = root.findtext('.//reportingOwner/reportingOwnerId/rptOwnerCik', default='N/A')
            owner_name = root.findtext('.//reportingOwner/reportingOwnerId/rptOwnerName', default='N/A')

            # Extract owner relationship
            relationship_node = root.find('.//reportingOwner/reportingOwnerRelationship')
            owner_positions = []
            officer_title = None
            if relationship_node is not None:
                if relationship_node.findtext('isDirector', default='0').strip() in ['1', 'true']:
                    owner_positions.append('Director')
                if relationship_node.findtext('isOfficer', default='0').strip() in ['1', 'true']:
                    owner_positions.append('Officer')
                    officer_title = relationship_node.findtext('officerTitle', default='').strip()
                if relationship_node.findtext('isTenPercentOwner', default='0').strip() in ['1', 'true']:
                    owner_positions.append('10% Owner')
                if relationship_node.findtext('isOther', default='0').strip() in ['1', 'true']:
                    owner_positions.append('Other')
            
            # Format the position string
            owner_position_str = ', '.join(owner_positions)
            if officer_title and 'Officer' in owner_positions:
                 # Replace 'Officer' with 'Officer (Title)' if title exists
                 owner_position_str = owner_position_str.replace('Officer', f'Officer ({officer_title})', 1)
            elif not owner_position_str:
                 owner_position_str = 'N/A' # Default if no flags are set

            # Process Non-Derivative Transactions
            for tx in root.findall('.//nonDerivativeTransaction'):
                try:
                    security_title = tx.findtext('./securityTitle/value', default='N/A')
                    tx_date = tx.findtext('./transactionDate/value', default='N/A')
                    tx_code = tx.findtext('./transactionCoding/transactionCode', default='N/A')

                    shares_str = tx.findtext('./transactionAmounts/transactionShares/value', default='0')
                    price_str = tx.findtext('./transactionAmounts/transactionPricePerShare/value', default='0')
                    acq_disp_code = tx.findtext('./transactionAmounts/transactionAcquiredDisposedCode/value', default='N/A')

                    shares_owned_after_str = tx.findtext('./postTransactionAmounts/sharesOwnedFollowingTransaction/value', default='N/A')
                    direct_indirect = tx.findtext('./ownershipNature/directOrIndirectOwnership/value', default='N/A')

                    shares = float(shares_str) if shares_str and shares_str.replace('.', '', 1).isdigit() else 0.0
                    price = float(price_str) if price_str and price_str.replace('.', '', 1).isdigit() else 0.0
                    shares_owned_after = float(shares_owned_after_str) if shares_owned_after_str and shares_owned_after_str.replace('.', '', 1).isdigit() else None

                    transaction_type = self.TRANSACTION_CODE_MAP.get(tx_code, 'Unknown')
                    is_acquisition = tx_code in self.ACQUISITION_CODES
                    is_disposition = tx_code in self.DISPOSITION_CODES

                    transaction = {
                        'ticker': issuer_symbol,
                        'issuer_cik': issuer_cik,
                        'issuer_name': issuer_name,
                        'owner_cik': owner_cik,
                        'owner_name': owner_name,
                        'transaction_date': tx_date,
                        'security_title': security_title,
                        'transaction_code': tx_code,
                        'transaction_type': transaction_type,
                        'acq_disp_code': acq_disp_code,
                        'is_acquisition': is_acquisition,
                        'is_disposition': is_disposition,
                        'shares': shares,
                        'price_per_share': price,
                        'value': shares * price if shares is not None and price is not None else 0.0,
                        'shares_owned_after': shares_owned_after,
                        'direct_indirect': direct_indirect,
                        'is_derivative': False,
                        'owner_position': owner_position_str
                    }
                    transactions.append(transaction)
                except Exception as e:
                    logging.warning(f"Error parsing non-derivative tx: {e} - XML: {ET.tostring(tx, encoding='unicode')[:200]}")

            # Process Derivative Transactions
            for tx in root.findall('.//derivativeTransaction'):
                try:
                    security_title = tx.findtext('./securityTitle/value', default='N/A')
                    tx_date = tx.findtext('./transactionDate/value', default='N/A')
                    tx_code = tx.findtext('./transactionCoding/transactionCode', default='N/A')
                    conv_exercise_price_str = tx.findtext('./conversionOrExercisePrice/value', default='0')
                    shares_str = tx.findtext('./transactionAmounts/transactionShares/value', default='0')
                    acq_disp_code = tx.findtext('./transactionAmounts/transactionAcquiredDisposedCode/value', default='N/A')
                    exercise_date = tx.findtext('./exerciseDate/value', default='N/A')
                    expiration_date = tx.findtext('./expirationDate/value', default='N/A')
                    underlying_title = tx.findtext('./underlyingSecurity/underlyingSecurityTitle/value', default='N/A')
                    underlying_shares_str = tx.findtext('./underlyingSecurity/underlyingSecurityShares/value', default='0')
                    shares_owned_after_str = tx.findtext('./postTransactionAmounts/sharesOwnedFollowingTransaction/value', default='N/A')
                    direct_indirect = tx.findtext('./ownershipNature/directOrIndirectOwnership/value', default='N/A')

                    shares = float(shares_str) if shares_str and shares_str.replace('.', '', 1).isdigit() else 0.0
                    conv_exercise_price = float(conv_exercise_price_str) if conv_exercise_price_str and conv_exercise_price_str.replace('.', '', 1).isdigit() else 0.0
                    underlying_shares = float(underlying_shares_str) if underlying_shares_str and underlying_shares_str.replace('.', '', 1).isdigit() else 0.0
                    shares_owned_after = float(shares_owned_after_str) if shares_owned_after_str and shares_owned_after_str.replace('.', '', 1).isdigit() else None

                    transaction_type = self.TRANSACTION_CODE_MAP.get(tx_code, 'Unknown')
                    is_acquisition = tx_code in self.ACQUISITION_CODES
                    is_disposition = tx_code in self.DISPOSITION_CODES

                    transaction = {
                        'ticker': issuer_symbol,
                        'issuer_cik': issuer_cik,
                        'issuer_name': issuer_name,
                        'owner_cik': owner_cik,
                        'owner_name': owner_name,
                        'transaction_date': tx_date,
                        'security_title': security_title,
                        'transaction_code': tx_code,
                        'transaction_type': transaction_type,
                        'acq_disp_code': acq_disp_code,
                        'is_acquisition': is_acquisition,
                        'is_disposition': is_disposition,
                        'shares': shares,
                        'conversion_exercise_price': conv_exercise_price,
                        'exercise_date': exercise_date,
                        'expiration_date': expiration_date,
                        'underlying_title': underlying_title,
                        'underlying_shares': underlying_shares,
                        'shares_owned_after': shares_owned_after,
                        'direct_indirect': direct_indirect,
                        'is_derivative': True,
                        'owner_position': owner_position_str
                    }
                    transactions.append(transaction)
                except Exception as e:
                    logging.warning(f"Error parsing derivative tx: {e} - XML: {ET.tostring(tx, encoding='unicode')[:200]}")

            return transactions

        except ET.ParseError as e:
            logging.error(f"XML Parse Error in Form 4: {e} - Content length: {len(xml_content)}")
            return []
        except Exception as e:
            logging.error(f"Unexpected error parsing Form 4 XML: {e}", exc_info=True)
            return []

    async def process_form4_filing(self, accession_no: str, ticker: str = None) -> List[Dict]:
        """
        Downloads and parses a single Form 4 XML filing identified by its accession number.

        This method orchestrates the two main steps for processing one filing:
        1. Calls `document_handler.download_form_xml` to get the XML content.
        2. Calls `parse_form4_xml` to parse the downloaded content.

        Args:
            accession_no (str): The accession number of the Form 4 filing to process.
            ticker (str, optional): The stock ticker symbol of the *issuer*. This is passed
                to the `download_form_xml` method as a hint for constructing the correct
                download URL. Defaults to None.

        Returns:
            List[Dict]: A list of transaction dictionaries parsed from the filing.
                Returns an empty list if the XML download or parsing fails.
        """
        # Use document_handler to download the XML
        xml_content = await self.document_handler.download_form_xml(accession_no, ticker=ticker)

        if not xml_content:
            logging.warning(f"Could not download Form 4 XML for {accession_no} (ticker: {ticker})")
            return []

        # Parse the XML
        return self.parse_form4_xml(xml_content)

    def _classify_signal(self, transaction: Dict[str, Any]) -> str:
        tx_code = str(transaction.get('transaction_code') or '').upper()
        price = transaction.get('price_per_share')
        is_derivative = bool(transaction.get('is_derivative'))
        security_title = str(transaction.get('security_title') or '').lower()

        if '10b5-1' in security_title:
            return 'planned_sale_10b5_1'

        if tx_code == 'P' and not is_derivative and price not in (None, 0, 0.0):
            return 'open_market_buy'
        if tx_code == 'S' and not is_derivative and price not in (None, 0, 0.0):
            return 'open_market_sell'
        if tx_code == 'F':
            return 'tax_sale'
        if tx_code == 'M':
            return 'option_exercise'
        if tx_code in ('A',):
            return 'award_or_grant'
        if tx_code in ('G', 'D'):
            return 'gift'
        if tx_code in ('C', 'W'):
            return 'derivative_conversion'
        return self.SIGNAL_CLASS_MAP.get(tx_code, 'other')

    def _economic_intent(self, signal_class: str) -> str:
        return self.ECONOMIC_INTENT_MAP.get(signal_class, 'neutral')

    def _event_identity(self, transaction: Dict[str, Any]) -> str:
        price = transaction.get('price_per_share')
        if price is None:
            price = transaction.get('conversion_exercise_price')
        return "|".join([
            str(transaction.get('issuer_cik') or ''),
            str(transaction.get('owner_cik') or ''),
            str(transaction.get('transaction_date') or ''),
            str(transaction.get('transaction_code') or ''),
            str(transaction.get('shares') or 0),
            str(price or 0),
        ])

    def _base_event_identity(self, transaction: Dict[str, Any]) -> str:
        return "|".join([
            str(transaction.get('issuer_cik') or ''),
            str(transaction.get('owner_cik') or ''),
            str(transaction.get('transaction_date') or ''),
            str(transaction.get('transaction_code') or ''),
        ])

    def _role_weight(self, role: str) -> float:
        role_l = (role or '').lower()
        if 'chief executive' in role_l or 'ceo' in role_l:
            return 1.0
        if 'chief financial' in role_l or 'cfo' in role_l:
            return 0.9
        if 'president' in role_l or 'chief operating' in role_l or 'coo' in role_l:
            return 0.85
        if 'director' in role_l:
            return 0.7
        if '10% owner' in role:
            return 0.65
        if 'officer' in role_l:
            return 0.75
        return 0.5

    def _safe_date(self, value: Any) -> Optional[datetime]:
        if not value:
            return None
        try:
            return datetime.strptime(str(value)[:10], '%Y-%m-%d')
        except ValueError:
            return None

    def _normalize_signal_event(self, transaction: Dict[str, Any], filing_meta: Dict[str, Any], ticker: str) -> Dict[str, Any]:
        signal_class = self._classify_signal(transaction)
        price = transaction.get('price_per_share')
        if price is None:
            price = transaction.get('conversion_exercise_price')
        gross_value = transaction.get('value')
        if gross_value in (None, 0, 0.0) and price not in (None, 0, 0.0):
            try:
                gross_value = float(transaction.get('shares') or 0) * float(price)
            except (TypeError, ValueError):
                gross_value = 0.0

        form_name = str(filing_meta.get('form') or '4')
        is_amendment = form_name.endswith('/A')
        filing_date = filing_meta.get('filing_date')
        transaction_date = transaction.get('transaction_date') or filing_date
        anchor_timestamp = filing_date or transaction_date

        event = {
            'ticker': ticker.upper(),
            'issuer_cik': transaction.get('issuer_cik'),
            'issuer_name': transaction.get('issuer_name'),
            'owner_cik': transaction.get('owner_cik'),
            'owner_name': transaction.get('owner_name'),
            'owner_role': transaction.get('owner_position'),
            'transaction_date': transaction_date,
            'filing_date': filing_date,
            'event_anchor_type': 'filing_date',
            'event_anchor_timestamp': anchor_timestamp,
            'transaction_code': transaction.get('transaction_code'),
            'transaction_type': transaction.get('transaction_type'),
            'shares': transaction.get('shares'),
            'price_per_share': transaction.get('price_per_share'),
            'gross_value': float(gross_value or 0.0),
            'value': float(gross_value or 0.0),
            'is_derivative': bool(transaction.get('is_derivative')),
            'is_acquisition': bool(transaction.get('is_acquisition')),
            'is_disposition': bool(transaction.get('is_disposition')),
            'signal_class': signal_class,
            'economic_intent': self._economic_intent(signal_class),
            'accession_no': filing_meta.get('accession_no'),
            'form_url': filing_meta.get('url'),
            'primary_document': filing_meta.get('primary_document'),
            'primary_document_description': filing_meta.get('primary_document_description'),
            'form': form_name,
            'is_amendment': is_amendment,
            'amends_accession': None,
            'event_identity': '',
            'event_identity_base': '',
            'direct_indirect': transaction.get('direct_indirect'),
            'security_title': transaction.get('security_title'),
        }
        event['event_identity'] = self._event_identity(event)
        event['event_identity_base'] = self._base_event_identity(event)
        return event

    def _dedupe_and_apply_amendments(self, events: List[Dict[str, Any]]) -> List[Dict[str, Any]]:
        ordered = sorted(
            events,
            key=lambda event: (
                event.get('filing_date') or '',
                event.get('accession_no') or '',
                event.get('transaction_date') or '',
            )
        )
        effective: List[Dict[str, Any]] = []
        seen_identity = set()

        for event in ordered:
            identity = event.get('event_identity')
            if identity in seen_identity and not event.get('is_amendment'):
                continue

            if event.get('is_amendment'):
                base_identity = event.get('event_identity_base')
                effective = [existing for existing in effective if existing.get('event_identity_base') != base_identity]
                seen_identity = {existing.get('event_identity') for existing in effective}

            if identity in seen_identity:
                continue

            effective.append(event)
            seen_identity.add(identity)

        return effective

    def _score_aggregate(self, aggregate: Dict[str, Any]) -> None:
        net_open_market_value = float(aggregate.get('net_open_market_value') or 0.0)
        unique_insiders = int(aggregate.get('unique_insiders') or 0)
        avg_role_weight = float(aggregate.get('avg_role_weight') or 0.0)
        clustered_buy_count = int(aggregate.get('clustered_buy_count') or 0)
        derivative_count = int(aggregate.get('derivative_event_count') or 0)
        tax_sale_count = int(aggregate.get('tax_sale_count') or 0)
        total_event_count = max(1, int(aggregate.get('total_event_count') or 1))
        open_market_activity = int(aggregate.get('open_market_buy_count') or 0) + int(aggregate.get('open_market_sell_count') or 0)

        reasons: List[str] = []
        score = 0.0

        if net_open_market_value > 0:
            positive = min(1.0, net_open_market_value / 1_000_000.0)
            score += positive * 0.45
            reasons.append(f"net_open_market_value={net_open_market_value:.0f}")
        elif net_open_market_value < 0:
            negative = min(1.0, abs(net_open_market_value) / 1_000_000.0)
            score -= negative * 0.45
            reasons.append(f"net_open_market_value={net_open_market_value:.0f}")

        if unique_insiders > 0 and open_market_activity > 0:
            insider_bonus = min(1.0, unique_insiders / 4.0) * 0.2
            score += insider_bonus if net_open_market_value >= 0 else -insider_bonus
            reasons.append(f"unique_insiders={unique_insiders}")

        if avg_role_weight > 0 and open_market_activity > 0:
            role_bonus = avg_role_weight * 0.15
            score += role_bonus if net_open_market_value >= 0 else -role_bonus
            reasons.append(f"role_weight={avg_role_weight:.2f}")

        if clustered_buy_count > 1:
            cluster_bonus = min(0.15, clustered_buy_count * 0.05)
            score += cluster_bonus
            reasons.append(f"clustered_buys={clustered_buy_count}")

        derivative_ratio = derivative_count / total_event_count
        if derivative_ratio > 0.5:
            score -= 0.1
            reasons.append('derivative_heavy')

        if tax_sale_count == total_event_count:
            score = min(score - 0.15, -0.05)
            reasons.append('tax_only')

        aggregate['signal_strength_score'] = round(max(-1.0, min(1.0, score)), 3)
        aggregate['signal_strength_reason'] = reasons

    def _build_daily_aggregates(self, ticker: str, events: List[Dict[str, Any]]) -> List[Dict[str, Any]]:
        events_by_anchor: Dict[str, List[Dict[str, Any]]] = defaultdict(list)
        events_by_owner: Dict[str, List[datetime]] = defaultdict(list)

        for event in events:
            if event.get('signal_class') == 'open_market_buy':
                key = f"{event.get('owner_cik') or event.get('owner_name')}"
                event_date = self._safe_date(event.get('transaction_date'))
                if event_date:
                    events_by_owner[key].append(event_date)

        for event in events:
            anchor = str(event.get('event_anchor_timestamp') or '')[:10]
            if anchor:
                events_by_anchor[anchor].append(event)

        aggregates: List[Dict[str, Any]] = []
        for anchor_date, day_events in sorted(events_by_anchor.items()):
            open_buy = [event for event in day_events if event.get('signal_class') == 'open_market_buy']
            open_sell = [event for event in day_events if event.get('signal_class') == 'open_market_sell']
            tax_sales = [event for event in day_events if event.get('signal_class') == 'tax_sale']
            option_events = [event for event in day_events if event.get('signal_class') == 'option_exercise']
            gifts = [event for event in day_events if event.get('signal_class') == 'gift']
            non_economic = [event for event in day_events if event.get('economic_intent') in ('neutral', 'compensation')]

            unique_filing_links = []
            filing_seen = set()
            for event in day_events:
                link = event.get('form_url')
                if link and link not in filing_seen:
                    unique_filing_links.append(link)
                    filing_seen.add(link)

            event_date = self._safe_date(anchor_date)
            clustered_buys = 0
            if event_date:
                for event in open_buy:
                    owner_key = f"{event.get('owner_cik') or event.get('owner_name')}"
                    owner_dates = events_by_owner.get(owner_key, [])
                    clustered_buys += sum(1 for candidate in owner_dates if abs((candidate - event_date).days) <= 3)

            key_events = sorted(
                day_events,
                key=lambda event: (
                    abs(float(event.get('gross_value') or 0.0)),
                    self._role_weight(str(event.get('owner_role') or '')),
                ),
                reverse=True,
            )[:3]

            aggregate = {
                'ticker': ticker,
                'event_anchor_type': 'filing_date',
                'event_anchor_timestamp': anchor_date,
                'transaction_date': anchor_date,
                'filing_date': anchor_date,
                'total_event_count': len(day_events),
                'open_market_buy_count': len(open_buy),
                'open_market_sell_count': len(open_sell),
                'tax_sale_count': len(tax_sales),
                'option_exercise_count': len(option_events),
                'gift_count': len(gifts),
                'non_economic_event_count': len(non_economic),
                'derivative_event_count': sum(1 for event in day_events if event.get('is_derivative')),
                'unique_insiders': len({event.get('owner_cik') or event.get('owner_name') for event in day_events}),
                'net_open_market_value': sum(float(event.get('gross_value') or 0.0) for event in open_buy)
                    - sum(float(event.get('gross_value') or 0.0) for event in open_sell),
                'net_value': sum(float(event.get('gross_value') or 0.0) for event in open_buy)
                    - sum(float(event.get('gross_value') or 0.0) for event in open_sell),
                'net_shares': sum(float(event.get('shares') or 0.0) for event in open_buy)
                    - sum(float(event.get('shares') or 0.0) for event in open_sell),
                'avg_role_weight': (
                    sum(self._role_weight(str(event.get('owner_role') or '')) for event in day_events) / len(day_events)
                    if day_events else 0.0
                ),
                'clustered_buy_count': clustered_buys,
                'signal_strength_score': 0.0,
                'signal_strength_reason': [],
                'key_events': [
                    {
                        'owner_name': event.get('owner_name'),
                        'role': event.get('owner_role'),
                        'signal_class': event.get('signal_class'),
                        'gross_value': event.get('gross_value'),
                        'importance_score': round(
                            abs(float(event.get('gross_value') or 0.0)) / 1_000_000.0
                            + self._role_weight(str(event.get('owner_role') or '')),
                            3,
                        ),
                        'reason': [
                            f"signal_class={event.get('signal_class')}",
                            f"gross_value={float(event.get('gross_value') or 0.0):.0f}",
                        ],
                        'filing_url': event.get('form_url'),
                    }
                    for event in key_events
                ],
                'filing_links': unique_filing_links[:3],
            }
            self._score_aggregate(aggregate)
            aggregates.append(aggregate)

        return aggregates

    def _build_llm_digest(self, ticker: str, events: List[Dict[str, Any]], aggregates: List[Dict[str, Any]], anchor_type: str) -> Dict[str, Any]:
        open_buy_events = [event for event in events if event.get('signal_class') == 'open_market_buy']
        open_sell_events = [event for event in events if event.get('signal_class') == 'open_market_sell']
        total_filings = len({event.get('accession_no') for event in events if event.get('accession_no')})
        unique_insiders = len({event.get('owner_cik') or event.get('owner_name') for event in events})
        total_open_buy_value = sum(float(event.get('gross_value') or 0.0) for event in open_buy_events)
        total_open_sell_value = sum(float(event.get('gross_value') or 0.0) for event in open_sell_events)
        buy_sell_ratio = round(total_open_buy_value / total_open_sell_value, 3) if total_open_sell_value > 0 else None

        ranked_events = sorted(
            events,
            key=lambda event: (
                abs(float(event.get('gross_value') or 0.0)),
                self._role_weight(str(event.get('owner_role') or '')),
            ),
            reverse=True,
        )[:5]

        anomalies: List[str] = []
        if sum(1 for aggregate in aggregates if aggregate.get('open_market_buy_count', 0) > 0) >= 2:
            anomalies.append('clustered_buying')
        if len({event.get('owner_name') for event in open_buy_events + open_sell_events}) >= 3:
            anomalies.append('repeated_insider_activity')
        if ranked_events and abs(float(ranked_events[0].get('gross_value') or 0.0)) >= 1_000_000.0:
            anomalies.append('unusually_large_trade')

        caveats: List[str] = []
        if events and sum(1 for event in events if event.get('is_derivative')) / max(1, len(events)) > 0.5:
            caveats.append('derivative_heavy_period')
        if events and all(event.get('signal_class') == 'tax_sale' for event in events):
            caveats.append('mostly_tax_sales')
        if not open_buy_events and not open_sell_events:
            caveats.append('limited_open_market_activity')

        return {
            'summary': {
                'ticker': ticker,
                'total_filings': total_filings,
                'net_value': round(total_open_buy_value - total_open_sell_value, 2),
                'unique_insiders': unique_insiders,
                'buy_sell_ratio': buy_sell_ratio,
                'as_of': datetime.now(timezone.utc).strftime('%Y-%m-%dT%H:%M:%SZ'),
                'anchor_type': anchor_type,
            },
            'key_events': [
                {
                    'owner_name': event.get('owner_name'),
                    'role': event.get('owner_role'),
                    'signal_class': event.get('signal_class'),
                    'gross_value': round(float(event.get('gross_value') or 0.0), 2),
                    'importance_score': round(
                        abs(float(event.get('gross_value') or 0.0)) / 1_000_000.0
                        + self._role_weight(str(event.get('owner_role') or '')),
                        3,
                    ),
                    'reason': [
                        f"signal_class={event.get('signal_class')}",
                        f"transaction_type={event.get('transaction_type')}",
                    ],
                    'filing_url': event.get('form_url'),
                }
                for event in ranked_events
            ],
            'anomalies': anomalies,
            'caveats': caveats,
        }

    async def get_insider_signal_payload(self, ticker: str, days_back: int = 180,
                                         use_cache: bool = True, filing_limit: int = 40,
                                         anchor_type: str = 'filing_date') -> Dict[str, Any]:
        ticker = ticker.upper()
        filings_meta = await self.fetch_filings_metadata(ticker, days_back=days_back, use_cache=use_cache)
        if filing_limit > 0:
            filings_meta = filings_meta[:filing_limit]

        normalized_events: List[Dict[str, Any]] = []
        for filing_meta in filings_meta:
            accession_no = filing_meta.get('accession_no')
            if not accession_no:
                continue
            parsed_transactions = await self.process_form4_filing(accession_no, ticker=ticker)
            for transaction in parsed_transactions:
                normalized_events.append(self._normalize_signal_event(transaction, filing_meta, ticker))

        effective_events = self._dedupe_and_apply_amendments(normalized_events)
        daily_aggregates = self._build_daily_aggregates(ticker, effective_events)
        llm_digest = self._build_llm_digest(ticker, effective_events, daily_aggregates, anchor_type)

        return {
            'symbol': ticker,
            'window': {
                'days_back': days_back,
                'filing_limit': filing_limit,
            },
            'as_of': datetime.now(timezone.utc).strftime('%Y-%m-%dT%H:%M:%SZ'),
            'events': effective_events,
            'daily_aggregates': daily_aggregates,
            'llm_digest': llm_digest,
        }

    async def get_recent_insider_transactions(self, ticker: str, days_back: int = 90,
                                         use_cache: bool = True, filing_limit: int = 10) -> List[Dict]:
        """
        Fetches metadata, downloads, parses, and formats recent Form 4 transactions.

        This is a high-level method designed to retrieve a list of recent insider
        transactions suitable for direct use (e.g., displaying in a UI table).

        Workflow:
        1. Calls the injected `fetch_filings_metadata` function to get a list of recent
           Form 4 filings (accession numbers, dates, etc.) for the `ticker`.
        2. Iterates through the fetched metadata (up to `filing_limit`).
        3. For each filing, calls `process_form4_filing` to download and parse its XML.
        4. Formats the relevant fields from the parsed transactions into a simplified
           dictionary structure commonly needed for display.
        5. Appends these formatted dictionaries to a final list.

        Args:
            ticker (str): The stock ticker symbol of the issuer.
            days_back (int, optional): How many days back to look for Form 4 filings.
                Passed to the `fetch_filings_metadata` function. Defaults to 90.
            use_cache (bool, optional): Whether the `fetch_filings_metadata` function
                should attempt to use cached filing metadata. Defaults to True.
            filing_limit (int, optional): The maximum number of the most recent filings
                to download and parse. This helps limit processing time and API usage.
                Defaults to 10.

        Returns:
            List[Dict]: A list of dictionaries, each representing a formatted insider
                transaction ready for display. Keys typically include 'filer' (owner name),
                'date', 'type' (e.g., Purchase, Sale), 'shares', 'price', 'value',
                'form_url', 'primary_document'. Returns an empty list if no filings
                are found or no transactions can be parsed.
        """
        ticker = ticker.upper()

        # Get Form 4 filing metadata using the injected function
        filings_meta = await self.fetch_filings_metadata(ticker, days_back=days_back, use_cache=use_cache)

        if not filings_meta:
            logging.info(f"No recent Form 4 filing metadata found for {ticker}")
            return []

        all_ui_transactions = []
        logging.info(f"Processing up to {filing_limit} most recent Form 4 filings for {ticker}...")

        processed_count = 0
        for filing_meta in filings_meta:
            if processed_count >= filing_limit:
                 logging.info(f"Reached processing limit of {filing_limit} filings for {ticker}.")
                 break

            accession_no = filing_meta.get('accession_no')
            filing_url = filing_meta.get('url', 'N/A') # Get URL from metadata if available
            primary_doc_name = filing_meta.get('primary_document') # Get primary doc name

            if not accession_no:
                logging.warning(f"Skipping filing for {ticker} due to missing accession number in metadata: {filing_meta}")
                continue

            # Process the XML to get detailed transactions
            parsed_transactions = await self.process_form4_filing(accession_no, ticker=ticker)

            if not parsed_transactions:
                logging.debug(f"No transactions parsed for {ticker}, accession: {accession_no}")
                continue # Move to the next filing

            processed_count += 1

            # Format each parsed transaction for the UI
            for tx in parsed_transactions:
                ui_transaction = {
                    'filer': tx.get('owner_name', 'N/A'),
                    'position': tx.get('owner_position', 'N/A'),
                    'date': tx.get('transaction_date', 'N/A'),
                    'type': tx.get('transaction_type', 'Unknown'),
                    'shares': tx.get('shares'),
                    'price': tx.get('price_per_share') if not tx.get('is_derivative') else tx.get('conversion_exercise_price'),
                    'value': tx.get('value') if not tx.get('is_derivative') else None, # Value calculation for derivatives is complex
                    'form_url': filing_url, # Use URL from metadata
                    'primary_document': primary_doc_name # Use filename from metadata
                }

                # Recalculate value for non-derivatives if needed
                if not tx.get('is_derivative') and ui_transaction['value'] is None:
                     shares = ui_transaction.get('shares')
                     price = ui_transaction.get('price')
                     if shares is not None and price is not None:
                          try: ui_transaction['value'] = float(shares) * float(price)
                          except (ValueError, TypeError): ui_transaction['value'] = 0.0
                     else: ui_transaction['value'] = 0.0

                all_ui_transactions.append(ui_transaction)

        logging.info(f"Completed processing {processed_count} filings for {ticker}. Found {len(all_ui_transactions)} transactions.")
        return all_ui_transactions

    async def analyze_insider_transactions(self, ticker: str, days_back: int = 90, use_cache: bool = True) -> Dict:
        """
        Performs a basic quantitative analysis of recent insider transactions for a ticker.

        Downloads and parses recent Form 4 filings (similar to
        `get_recent_insider_transactions` but retrieves the full parsed data),
        converts the data into a pandas DataFrame, and calculates summary statistics.

        Workflow:
        1. Fetches recent Form 4 filing metadata for the `ticker`.
        2. Processes each filing using `process_form4_filing` to get detailed transactions.
        3. Concatenates all parsed transactions into a single list.
        4. Converts the list into a pandas DataFrame.
        5. Calculates metrics like:
           - Total number of transactions.
           - Number of buy vs. sell transactions (based on transaction codes).
           - Total value of buy vs. sell transactions.
           - Net transaction value (Total Buy Value - Total Sell Value).
           - Number of unique owners involved.
           - List of unique owners involved.
           - Optionally includes the raw DataFrame.

        Args:
            ticker (str): The stock ticker symbol of the issuer.
            days_back (int, optional): How many days back to look for Form 4 filings.
                Defaults to 90.
            use_cache (bool, optional): Whether to use cached filing metadata when fetching
                the list of filings to process. Defaults to True.

        Returns:
            Dict: A dictionary containing the analysis results. Keys include
                'ticker', 'analysis_period_days', 'total_transactions', 'buy_count',
                'sell_count', 'total_buy_value', 'total_sell_value', 'net_value',
                'involved_owners_count', 'involved_owners_list'. Includes an 'error'
                key if fetching or analysis fails. May include 'dataframe' if successful.
        """
        ticker = ticker.upper()

        # Get *parsed* transactions first (not UI formatted)
        # Need a method that fetches filings and processes them without UI formatting
        # Let's adapt process_form4_filing to run over multiple filings

        logging.info(f"Analyzing insider transactions for {ticker} ({days_back} days back)...")
        filings_meta = await self.fetch_filings_metadata(ticker, days_back=days_back, use_cache=use_cache)
        if not filings_meta:
             return {'ticker': ticker, 'error': "No filing metadata found."} 

        all_parsed_transactions = []
        # No limit for analysis, process all filings in the period
        for filing_meta in filings_meta:
             accession_no = filing_meta.get('accession_no')
             if not accession_no:
                 continue
             parsed = await self.process_form4_filing(accession_no, ticker=ticker)
             all_parsed_transactions.extend(parsed)

        if not all_parsed_transactions:
            return {
                'ticker': ticker,
                'error': "No transactions found in filings.",
                'total_transactions': 0
             }

        if pd is None:
            return {
                'ticker': ticker,
                'error': "pandas is required for transaction analysis.",
                'total_transactions_parsed': len(all_parsed_transactions),
            }

        df = pd.DataFrame(all_parsed_transactions)

        try:
            # Ensure required columns exist and handle potential NaNs
            df['is_acquisition'] = df['is_acquisition'].fillna(False)
            df['is_disposition'] = df['is_disposition'].fillna(False)
            df['value'] = pd.to_numeric(df['value'], errors='coerce').fillna(0)
            df['shares'] = pd.to_numeric(df['shares'], errors='coerce').fillna(0)
            df['transaction_date'] = pd.to_datetime(df['transaction_date'], errors='coerce')

            # Filter out rows where conversion failed
            df = df.dropna(subset=['transaction_date'])

            # Separate buys/sells based on the boolean flags
            # Consider only non-derivative transactions for simple value analysis
            non_deriv_df = df[~df['is_derivative'].fillna(False)]
            buys = non_deriv_df[non_deriv_df['is_acquisition'] == True]
            sells = non_deriv_df[non_deriv_df['is_disposition'] == True]

            # Summary statistics
            result = {
                'ticker': ticker,
                'analysis_period_days': days_back,
                'total_filings_processed': len(filings_meta),
                'total_transactions_parsed': len(df),
                'buy_transaction_count': len(buys),
                'sell_transaction_count': len(sells),
                'total_buy_value': buys['value'].sum(),
                'total_sell_value': sells['value'].sum(),
                'net_value': buys['value'].sum() - sells['value'].sum(),
                'unique_filers': df['owner_name'].nunique(),
                'involved_filers': df['owner_name'].unique().tolist(),
                'analysis_start_date': df['transaction_date'].min().strftime('%Y-%m-%d') if not df.empty else None,
                'analysis_end_date': df['transaction_date'].max().strftime('%Y-%m-%d') if not df.empty else None,
                # Optional: More detailed stats
                # 'transactions_by_type': df['transaction_type'].value_counts().to_dict(),
                # 'top_buyers_by_value': buys.groupby('owner_name')['value'].sum().nlargest(5).to_dict(),
                # 'top_sellers_by_value': sells.groupby('owner_name')['value'].sum().nlargest(5).to_dict(),
            }

            logging.info(f"Analysis complete for {ticker}. Net value: {result['net_value']:.2f}")
            return result

        except Exception as e:
            logging.error(f"Error analyzing insider transactions for {ticker}: {e}", exc_info=True)
            return {
                'ticker': ticker,
                'error': f"Analysis error: {str(e)}",
                'total_transactions_parsed': len(df)
            } 
