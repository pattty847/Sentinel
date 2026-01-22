"""
SEC XBRL Tag Mapping
--------------------
This module defines the mapping between Sentinel's standardized financial metric keys
and the corresponding XBRL taxonomy tags (usually US-GAAP).

It acts as a fallback chain: if the first tag isn't found in a company's filing,
the processor will try the next one in the list.
"""

FINANCIAL_TAG_MAP = {
    # --- Income Statement ---
    "revenue": [
        ("us-gaap", "RevenueFromContractWithCustomerExcludingAssessedTax"), # Most common modern tag
        ("us-gaap", "SalesRevenueNet"),         # Older/Simpler tag
        ("us-gaap", "Revenues"),                # Generic
        ("us-gaap", "RevenuesNetOfInterestExpense") # Banks/Financials
    ],
    "net_income": [
        ("us-gaap", "NetIncomeLoss"),           # Standard
        ("us-gaap", "NetIncomeLossAvailableToCommonStockholdersBasic"), # Precise
        ("us-gaap", "ProfitLoss")               # International/Generic
    ],
    "operating_income": [
        ("us-gaap", "OperatingIncomeLoss")
    ],
    "eps": [
        ("us-gaap", "EarningsPerShareBasic"),
        ("us-gaap", "EarningsPerShareDiluted")
    ],

    # --- Balance Sheet (Snapshots) ---
    "assets": [
        ("us-gaap", "Assets")
    ],
    "liabilities": [
        ("us-gaap", "Liabilities")
    ],
    "equity": [
        ("us-gaap", "StockholdersEquity"),
        ("us-gaap", "StockholdersEquityIncludingPortionAttributableToNoncontrollingInterest")
    ],
    "cash_equivalents": [
        ("us-gaap", "CashAndCashEquivalentsAtCarryingValue"),
        ("us-gaap", "Cash")
    ],

    # --- Cash Flow Statement ---
    "operating_cash_flow": [
        ("us-gaap", "NetCashProvidedByUsedInOperatingActivities")
    ],
    "investing_cash_flow": [
        ("us-gaap", "NetCashProvidedByUsedInInvestingActivities")
    ],
    "financing_cash_flow": [
        ("us-gaap", "NetCashProvidedByUsedInFinancingActivities")
    ],

    # --- Components for Calculation (EBITDA / FCF) ---
    "capex": [
        ("us-gaap", "PaymentsToAcquirePropertyPlantAndEquipment"),
        ("us-gaap", "PaymentsToAcquireProductiveAssets")
    ],
    "interest_expense": [
        ("us-gaap", "InterestExpense"),
        ("us-gaap", "InterestExpenseDebt")
    ],
    "income_tax_expense": [
        ("us-gaap", "IncomeTaxExpenseBenefit")
    ],
    "depreciation_amortization": [
        ("us-gaap", "DepreciationDepletionAndAmortization"), # The combo tag
        ("us-gaap", "Depreciation"),                         # Standalone Dep
        ("us-gaap", "AmortizationOfIntangibleAssets")        # Standalone Amort
    ]
}