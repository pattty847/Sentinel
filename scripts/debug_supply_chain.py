import asyncio
import re
from sec.sec_api import SECDataFetcher
from sec.supply_chain_parser import SupplyChainParser


def snippet(label: str, text: str, start: int = 0, length: int = 800) -> None:
    print(f"\n--- {label} (offset {start}, len {length}) ---")
    end = min(len(text), start + length)
    print(text[start:end])


def keyword_windows(text: str, keyword: str, window: int = 160, max_hits: int = 3) -> None:
    print(f"\n>>> keyword '{keyword}'")
    lowered = text.lower()
    hits = [m.start() for m in re.finditer(re.escape(keyword.lower()), lowered)]
    for idx, pos in enumerate(hits[:max_hits]):
        s = max(0, pos - window // 2)
        e = min(len(text), pos + window // 2)
        print(f"[hit {idx+1} @ {pos}] ...{text[s:e]}...")
    if not hits:
        print("  (no hits)")


async def main() -> None:
    fetcher = SECDataFetcher()
    filings = await fetcher.fetch_annual_reports("AAPL", days_back=730, use_cache=True)
    print("filings count", len(filings))
    if not filings:
        return

    latest = filings[0]
    raw = await fetcher.document_handler.fetch_primary_html(latest["accession_no"], "CRUS")
    print("raw len", len(raw) if raw else None)

    parser = SupplyChainParser()
    clean = parser.clean_html(raw or "")

    # Debug header hits before extraction
    item1_hits = len(list(parser.ITEM_1_PATTERN.finditer(clean)))
    item1a_hits = len(list(parser.ITEM_1A_PATTERN.finditer(clean)))
    item_end_hits = len(list(parser.ITEM_END_RISK_PATTERN.finditer(clean)))
    print(f"item1 hits: {item1_hits}, item1A hits: {item1a_hits}, itemEnd hits: {item_end_hits}")

    sections = parser.extract_sections(clean)
    business = sections["business"]
    risks = sections["risk_factors"]
    print("business len", len(business), "risk len", len(risks))

    # Show a couple slices to see real prose (TOC tends to live at the top)
    snippet("business start", business, 0, 1200)
    snippet("business mid", business, len(business) // 2, 800)
    snippet("risk start", risks, 0, 1200)
    snippet("risk mid", risks, len(risks) // 2, 800)

    # Keyword context to see phrasing that may matter for regexes
    for kw in ["customer", "customers", "supplier", "suppliers", "depend", "rely", "competitor", "competition"]:
        keyword_windows(business + " " + risks, kw)

    rel = parser.extract_relationships(sections)
    print("\nrelationships", len(rel))
    for r in rel[:10]:
        print(r)

    await fetcher.close()


if __name__ == "__main__":
    asyncio.run(main())