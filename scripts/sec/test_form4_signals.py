#!/usr/bin/env python3
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent.parent))

from sec.form4_processor import Form4Processor


async def _unused_fetch(*args, **kwargs):
    return []


class DummyDocumentHandler:
    pass


class Form4SignalTests(unittest.TestCase):
    def setUp(self):
        self.processor = Form4Processor(DummyDocumentHandler(), _unused_fetch)

    def _make_tx(self, **overrides):
        tx = {
            'issuer_cik': '0000000001',
            'issuer_name': 'Acme Corp',
            'owner_cik': '0000001000',
            'owner_name': 'Jane Doe',
            'owner_position': 'Officer (CEO)',
            'transaction_date': '2026-03-01',
            'transaction_code': 'P',
            'transaction_type': 'Purchase',
            'shares': 1000.0,
            'price_per_share': 10.0,
            'value': 10000.0,
            'is_derivative': False,
            'is_acquisition': True,
            'is_disposition': False,
            'direct_indirect': 'D',
            'security_title': 'Common Stock',
        }
        tx.update(overrides)
        return tx

    def _make_meta(self, **overrides):
        meta = {
            'accession_no': '0000000000-26-000001',
            'filing_date': '2026-03-02',
            'form': '4',
            'url': 'https://example.com/filing',
            'primary_document': 'form4.xml',
            'primary_document_description': 'Form 4',
        }
        meta.update(overrides)
        return meta

    def test_classification_matrix(self):
        cases = [
            ('P', False, 10.0, 'open_market_buy'),
            ('S', False, 11.0, 'open_market_sell'),
            ('F', False, 0.0, 'tax_sale'),
            ('M', True, 0.0, 'option_exercise'),
            ('A', False, 0.0, 'award_or_grant'),
            ('G', False, 0.0, 'gift'),
            ('C', True, 0.0, 'derivative_conversion'),
        ]
        for code, is_derivative, price, expected in cases:
            tx = self._make_tx(
                transaction_code=code,
                is_derivative=is_derivative,
                price_per_share=price,
                transaction_type=code,
            )
            event = self.processor._normalize_signal_event(tx, self._make_meta(), 'ACME')
            self.assertEqual(event['signal_class'], expected)

    def test_amendment_replaces_prior_event(self):
        original = self.processor._normalize_signal_event(self._make_tx(value=10000.0), self._make_meta(), 'ACME')
        amended = self.processor._normalize_signal_event(
            self._make_tx(value=12500.0, shares=1250.0),
            self._make_meta(accession_no='0000000000-26-000002', form='4/A'),
            'ACME',
        )
        effective = self.processor._dedupe_and_apply_amendments([original, amended])
        self.assertEqual(len(effective), 1)
        self.assertTrue(effective[0]['is_amendment'])
        self.assertEqual(effective[0]['gross_value'], 12500.0)

    def test_same_day_events_collapse_into_single_aggregate(self):
        buy = self.processor._normalize_signal_event(self._make_tx(owner_cik='1', owner_name='A'), self._make_meta(), 'ACME')
        sell = self.processor._normalize_signal_event(
            self._make_tx(owner_cik='2', owner_name='B', transaction_code='S', transaction_type='Sale', is_acquisition=False, is_disposition=True, value=3000.0),
            self._make_meta(accession_no='0000000000-26-000003'),
            'ACME',
        )
        aggregates = self.processor._build_daily_aggregates('ACME', [buy, sell])
        self.assertEqual(len(aggregates), 1)
        aggregate = aggregates[0]
        self.assertEqual(aggregate['open_market_buy_count'], 1)
        self.assertEqual(aggregate['open_market_sell_count'], 1)
        self.assertEqual(aggregate['total_event_count'], 2)
        self.assertIn('signal_strength_score', aggregate)

    def test_tax_only_day_is_dampened(self):
        tax_sale = self.processor._normalize_signal_event(
            self._make_tx(transaction_code='F', transaction_type='Tax', value=0.0, price_per_share=0.0, is_acquisition=False, is_disposition=True),
            self._make_meta(),
            'ACME',
        )
        aggregates = self.processor._build_daily_aggregates('ACME', [tax_sale])
        self.assertEqual(len(aggregates), 1)
        self.assertLessEqual(aggregates[0]['signal_strength_score'], 0.0)
        self.assertIn('tax_only', aggregates[0]['signal_strength_reason'])

    def test_llm_digest_shape(self):
        buy = self.processor._normalize_signal_event(self._make_tx(), self._make_meta(), 'ACME')
        aggregates = self.processor._build_daily_aggregates('ACME', [buy])
        digest = self.processor._build_llm_digest('ACME', [buy], aggregates, 'filing_date')
        self.assertEqual(sorted(digest.keys()), ['anomalies', 'caveats', 'key_events', 'summary'])
        self.assertIn('total_filings', digest['summary'])
        self.assertTrue(isinstance(digest['key_events'], list))


if __name__ == '__main__':
    unittest.main()
