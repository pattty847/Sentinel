import re
import logging
import html
from typing import Dict, List, Tuple, Optional

class SupplyChainParser:
    """
    Parses raw 10-K HTML text to extract supply chain relationships.
    
    This class acts as "The Spider", crawling through the 'Business' and 'Risk Factors'
    sections of 10-K filings to identify customers, suppliers, and competitors.
    """
    
    # IMPROVED REGEX: More permissive
    # Allows for "Item 1" ... (up to 20 chars of noise) ... "Business"
    # This catches "Item 1. \n\n Business" or "Item 1. (Page 4) Business"
    ITEM_1_PATTERN = re.compile(r'Item\s+1\.?\s.{0,20}?Business', re.IGNORECASE | re.DOTALL)
    
    # Item 1A. Risk Factors
    ITEM_1A_PATTERN = re.compile(r'Item\s+1A\.?\s.{0,20}?Risk\s+Factors', re.IGNORECASE | re.DOTALL)
    
    # Item 1B. Unresolved... OR Item 2. Properties (End of Risk Factors)
    # Apple sometimes skips 1B, so we look for 1B OR 2
    ITEM_END_RISK_PATTERN = re.compile(r'Item\s+(?:1B|2)\.?\s', re.IGNORECASE)

    def __init__(self):
        pass

    def clean_html(self, raw_html: str) -> str:
        """
        Robustly strips HTML tags and normalizes whitespace.
        """
        # 1. Decode HTML entities (&nbsp; -> space, &amp; -> &, etc.)
        text = html.unescape(raw_html)
        
        # 2. Remove script/style tags completely
        text = re.sub(r'<(script|style).*?>.*?</\1>', ' ', text, flags=re.DOTALL | re.IGNORECASE)
        
        # 3. Remove HTML tags (replace with space to prevent words merging)
        text = re.sub(r'<[^>]+>', ' ', text)
        
        # 4. Collapse all whitespace (newlines, tabs, non-breaking spaces) into a single space
        # \s matches [ \t\n\r\f\v], and unescape handles \xa0
        text = re.sub(r'\s+', ' ', text).strip()
        
        return text

    def extract_sections(self, text: str) -> Dict[str, str]:
        """
        Extracts 'Business' and 'Risk Factors' using the Longest Block Heuristic.
        """
        sections = {
            'business': "",
            'risk_factors': ""
        }
        
        # DEBUG: Log the first 200 chars to see if cleaning worked
        logging.debug(f"Cleaned Text Sample: {text[:200]}...")
        
        # 1. Find ALL occurrences of the section headers
        item1_matches = list(self.ITEM_1_PATTERN.finditer(text))
        item1a_matches = list(self.ITEM_1A_PATTERN.finditer(text))
        item_end_matches = list(self.ITEM_END_RISK_PATTERN.finditer(text))
        
        logging.info(f"Found {len(item1_matches)} 'Item 1' hits, {len(item1a_matches)} 'Item 1A' hits.")

        # 2. Extract Business Section (Item 1 -> Item 1A)
        best_business_len = 0
        
        for i1 in item1_matches:
            for i1a in item1a_matches:
                # The section must end AFTER it starts
                if i1a.start() > i1.end():
                    block_len = i1a.start() - i1.end()
                    # HEURISTIC: A real Business section is usually > 1000 chars
                    # We ignore tiny blocks (likely TOC entries)
                    if block_len > best_business_len:
                        best_business_len = block_len
                        sections['business'] = text[i1.end():i1a.start()].strip()

        # 3. Extract Risk Factors Section (Item 1A -> Item 1B or Item 2)
        best_risk_len = 0
        
        for i1a in item1a_matches:
            for i_end in item_end_matches:
                if i_end.start() > i1a.end():
                    block_len = i_end.start() - i1a.end()
                    if block_len > best_risk_len:
                        best_risk_len = block_len
                        sections['risk_factors'] = text[i1a.end():i_end.start()].strip()
        
        logging.info(f"Parsed sections | Business: {len(sections['business'])} chars | Risks: {len(sections['risk_factors'])} chars")
        
        return sections

    def extract_relationships(self, sections: Dict[str, str]) -> List[Dict]:
        """
        Mines the text sections for entity relationships.
        """
        relationships = []

        def _split_entities(raw: str) -> List[str]:
            """Heuristic splitter for comma / and separated entity lists."""
            parts = re.split(r',|\sand\s', raw)
            cleaned = []
            for p in parts:
                cand = p.strip().strip('.')
                if 2 < len(cand) < 60 and cand[0].isupper():
                    cleaned.append(cand)
            return cleaned
        
        for section_name, text in sections.items():
            if not text or len(text) < 100:
                continue
                
            # --- LEVEL 1: KEYWORD + PROPER NOUN EXTRACTION (Heuristic) ---
            
            # Pattern A: "Customer X accounted for Y% of revenue"
            # Matches: "Walmart accounted for 15% of our revenue"
            # Matches: "Alphabet Inc. represented 10% of sales"
            customer_revenue_pattern = re.compile(
                r'([A-Z][a-zA-Z0-9\s\.\,]+?)\s+(?:accounted|represented|comprised)\s+for\s+([0-9]+(?:\.[0-9]+)?)\s*%\s+of\s+(?:our\s+)?(?:total\s+)?(?:net\s+)?(?:sales|revenue)',
                re.IGNORECASE
            )
            
            for match in customer_revenue_pattern.finditer(text):
                entity_name = match.group(1).strip()
                pct_str = match.group(2)
                
                # Cleaning: Remove "Approximately" or leading words if grabbed
                if "approximately" in entity_name.lower():
                    entity_name = entity_name.split("approximately")[-1].strip()
                
                # Filter out false positives (too long)
                if len(entity_name) > 50: continue
                    
                relationships.append({
                    'target_entity': entity_name,
                    'relationship_type': 'customer',
                    'weight': float(pct_str) / 100.0,
                    'context': match.group(0),
                    'confidence_score': 0.85,
                    'section': section_name
                })

            # Pattern B: "Supplier [Name]" - Broadened for Apple
            # Apple often says: "substantially all of our manufacturing is performed by outsourcing partners..."
            # This is hard for Regex. We look for explicit "Relies on" or "Dependent on"
            supplier_pattern = re.compile(
                r'(?:rely|depend)\s+(?:substantially|heavily|solely)?\s+on\s+([A-Z][a-zA-Z0-9\s\.,]+?)\s+(?:for|to|as)',
                re.IGNORECASE
            )
            
            for match in supplier_pattern.finditer(text):
                entity_name = match.group(1).strip()
                
                # Noise filters
                if len(entity_name) > 40 or len(entity_name) < 3: continue
                bad_terms = ["third parties", "a limited number", "our suppliers", "various"]
                if any(bt in entity_name.lower() for bt in bad_terms): continue

                relationships.append({
                    'target_entity': entity_name,
                    'relationship_type': 'supplier',
                    'weight': 0.0,
                    'context': match.group(0),
                    'confidence_score': 0.60,
                    'section': section_name
                })

            # Pattern C: Competitors
            # "Primary competitors include Google, Microsoft, and Meta."
            competitor_pattern = re.compile(
                r'(?:competitors|competition)\s+(?:include|are|consists\s+of)\s+([A-Z][a-zA-Z0-9\s\,\.]+)',
                re.IGNORECASE
            )
            
            for match in competitor_pattern.finditer(text):
                list_str = match.group(1)
                # Heuristic split
                candidates = re.split(r',|\sand\s', list_str)
                
                for cand in candidates:
                    cand = cand.strip().strip('.')
                    if len(cand) > 2 and len(cand) < 40 and cand[0].isupper():
                        relationships.append({
                            'target_entity': cand,
                            'relationship_type': 'competitor',
                            'weight': 0.0,
                            'context': match.group(0),
                            'confidence_score': 0.70,
                            'section': section_name
                        })

            # Pattern D: List-style mentions for customers or suppliers
            # e.g., "Key suppliers include Foxconn, Pegatron and TSMC."
            include_pattern = re.compile(
                r'(customers?|distribution partners|resellers|suppliers?|manufacturing partners|contract manufacturers|competitors)\s+(?:include|includes|including|consist of|are|were)\s+([A-Z][A-Za-z0-9&\\-\\.\\,\\s]{5,180})',
                re.IGNORECASE
            )

            for match in include_pattern.finditer(text):
                role_raw = match.group(1).lower()
                entities_raw = match.group(2)
                role = None
                if 'customer' in role_raw or 'reseller' in role_raw or 'distribution partner' in role_raw:
                    role = 'customer'
                elif 'supplier' in role_raw or 'manufacturing partner' in role_raw or 'contract manufacturer' in role_raw:
                    role = 'supplier'
                elif 'competitor' in role_raw:
                    role = 'competitor'
                if not role:
                    continue

                for ent in _split_entities(entities_raw):
                    relationships.append({
                        'target_entity': ent,
                        'relationship_type': role,
                        'weight': 0.0,
                        'context': match.group(0),
                        'confidence_score': 0.55 if role != 'competitor' else 0.65,
                        'section': section_name
                    })
                        
        return relationships