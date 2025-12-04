# sec/tag_mapping.py
# The "Rosetta Stone" of SEC XBRL tags

FINANCIAL_TAG_MAP = {
    "revenue": [
        ("us-gaap", "RevenueFromContractWithCustomerExcludingAssessedTax"),
        ("us-gaap", "SalesRevenueNet"),
        ("us-gaap", "Revenues"),
        ("us-gaap", "RevenuesNetOfInterestExpense")
    ],
    "net_income": [
        ("us-gaap", "NetIncomeLoss"),
        ("us-gaap", "NetIncomeLossAvailableToCommonStockholdersBasic"),
        ("us-gaap", "ProfitLoss")
    ],
    "operating_income": [
        ("us-gaap", "OperatingIncomeLoss")
    ],
    "eps": [
        ("us-gaap", "EarningsPerShareBasic"),
        ("us-gaap", "EarningsPerShareDiluted")
    ],
    # --- Balance Sheet ---
    "assets": [("us-gaap", "Assets")],
    "liabilities": [("us-gaap", "Liabilities")],
    "equity": [
        ("us-gaap", "StockholdersEquity"),
        ("us-gaap", "StockholdersEquityIncludingPortionAttributableToNoncontrollingInterest")
    ],
    "cash_equivalents": [
        ("us-gaap", "CashAndCashEquivalentsAtCarryingValue"),
        ("us-gaap", "Cash")
    ],
    # --- Cash Flow ---
    "operating_cash_flow": [("us-gaap", "NetCashProvidedByUsedInOperatingActivities")],
    "investing_cash_flow": [("us-gaap", "NetCashProvidedByUsedInInvestingActivities")],
    "financing_cash_flow": [("us-gaap", "NetCashProvidedByUsedInFinancingActivities")],
    "capex": [
        ("us-gaap", "PaymentsToAcquirePropertyPlantAndEquipment"),
        ("us-gaap", "PaymentsToAcquireProductiveAssets")
    ],
    # --- For EBITDA Calculation ---
    "interest_expense": [
        ("us-gaap", "InterestExpense"),
        ("us-gaap", "InterestExpenseDebt")
    ],
    "income_tax_expense": [
        ("us-gaap", "IncomeTaxExpenseBenefit")
    ],
    "depreciation_amortization": [
        ("us-gaap", "DepreciationDepletionAndAmortization"),
        ("us-gaap", "Depreciation"),
        ("us-gaap", "AmortizationOfIntangibleAssets")
    ]
}

