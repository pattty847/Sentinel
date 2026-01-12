import aiosqlite
import logging
import os
from typing import List, Dict, Optional, Union, Any

class SqlCacheManager:
    """
    Manages caching of SEC data and supply chain relationships using SQLite.
    
    This class handles persistent storage for:
    - Financial History (Revenue, Net Income, etc.)
    - Industrial Graph (Supply chain relationships, competitors, etc.)
    """
    
    DB_PATH = "data/sentinel.db"
    
    def __init__(self, db_path: str = DB_PATH):
        """
        Initialize the SQL Cache Manager.
        
        Args:
            db_path (str): Path to the SQLite database file.
        """
        self.db_path = db_path
        self._ensure_db_directory()
    
    def _ensure_db_directory(self):
        """Ensures the directory for the database exists."""
        os.makedirs(os.path.dirname(self.db_path), exist_ok=True)

    async def initialize_db(self):
        """
        Creates the necessary tables if they do not exist.
        """
        logging.info(f"Initializing database at {self.db_path}")
        async with aiosqlite.connect(self.db_path) as db:
            # Table A: financial_history (The Matrix)
            await db.execute("""
                CREATE TABLE IF NOT EXISTS financial_history (
                    id INTEGER PRIMARY KEY AUTOINCREMENT,
                    ticker TEXT NOT NULL,
                    metric TEXT NOT NULL,
                    period_end_date TEXT,
                    value REAL,
                    form_type TEXT,
                    period_type TEXT,
                    UNIQUE(ticker, metric, period_end_date)
                );
            """)
            
            # Table B: industrial_graph (The Spiderweb)
            await db.execute("""
                CREATE TABLE IF NOT EXISTS industrial_graph (
                    id INTEGER PRIMARY KEY AUTOINCREMENT,
                    source_ticker TEXT NOT NULL,
                    target_entity TEXT NOT NULL,
                    relationship_type TEXT,
                    context TEXT,
                    weight REAL,
                    confidence_score REAL,
                    source_doc_date TEXT,
                    UNIQUE(source_ticker, target_entity, relationship_type, source_doc_date)
                );
            """)
            
            await db.commit()
            logging.info("Database tables initialized.")

    async def save_financial_history(self, ticker: str, data: Dict[str, Any]) -> None:
        """
        Saves extracted financial history to the database.
        
        Args:
            ticker (str): The ticker symbol.
            data (Dict): Dictionary containing metric data (e.g., 'revenue': {'quarterly': [...]}).
                         Expected structure matches FinancialDataProcessor output.
        """
        async with aiosqlite.connect(self.db_path) as db:
            for metric, history in data.items():
                # Skip metadata keys
                if metric in ['ticker', 'entityName', 'cik', 'source_form', 'period_end'] or not isinstance(history, dict):
                    continue
                
                # Process quarterly data
                for entry in history.get('quarterly', []):
                    await self._upsert_financial_record(db, ticker, metric, entry, 'quarterly')
                
                # Process annual data
                for entry in history.get('annual', []):
                    await self._upsert_financial_record(db, ticker, metric, entry, 'annual')
            
            await db.commit()
            logging.info(f"Saved financial history for {ticker} to SQL.")

    async def _upsert_financial_record(self, db, ticker: str, metric: str, entry: Dict, period_type: str):
        """Helper to upsert a single financial record."""
        await db.execute("""
            INSERT INTO financial_history (ticker, metric, period_end_date, value, form_type, period_type)
            VALUES (?, ?, ?, ?, ?, ?)
            ON CONFLICT(ticker, metric, period_end_date) DO UPDATE SET
                value=excluded.value,
                form_type=excluded.form_type,
                period_type=excluded.period_type
        """, (
            ticker.upper(),
            metric,
            entry.get('date'),
            entry.get('value'),
            entry.get('form'),
            period_type
        ))

    async def save_relationship(self, source_ticker: str, target_entity: str, relationship_type: str, 
                              weight: float = 0.0, context: str = "", confidence: float = 1.0, 
                              doc_date: str = None):
        """
        Saves a supply chain relationship edge.
        
        Args:
            source_ticker (str): Ticker of the filing company.
            target_entity (str): Name of the partner/competitor.
            relationship_type (str): 'supplier', 'customer', 'competitor', 'litigation', etc.
            weight (float): Intensity/importance (0.0-1.0 or revenue %).
            context (str): Excerpt or reason for the link.
            confidence (float): Confidence score of the extraction.
            doc_date (str): Date of the source document.
        """
        async with aiosqlite.connect(self.db_path) as db:
            await db.execute("""
                INSERT INTO industrial_graph (
                    source_ticker, target_entity, relationship_type, 
                    context, weight, confidence_score, source_doc_date
                )
                VALUES (?, ?, ?, ?, ?, ?, ?)
                ON CONFLICT(source_ticker, target_entity, relationship_type, source_doc_date) DO UPDATE SET
                    context=excluded.context,
                    weight=excluded.weight,
                    confidence_score=excluded.confidence_score
            """, (
                source_ticker.upper(),
                target_entity,
                relationship_type,
                context,
                weight,
                confidence,
                doc_date
            ))
            await db.commit()

    async def get_latest_filing_date(self, ticker: str, form_type: str = None) -> Optional[str]:
        """
        Gets the date of the most recent filing stored in the database for a ticker.
        Useful for checking if we have up-to-date data before fetching.
        
        Args:
            ticker (str): Ticker symbol.
            form_type (Optional[str]): Filter by form type (e.g. '10-Q').
            
        Returns:
            Optional[str]: Date string (YYYY-MM-DD) or None if no data.
        """
        query = "SELECT MAX(period_end_date) as last_date FROM financial_history WHERE ticker = ?"
        params = [ticker.upper()]
        
        if form_type:
            query += " AND form_type = ?"
            params.append(form_type)
            
        async with aiosqlite.connect(self.db_path) as db:
            db.row_factory = aiosqlite.Row
            async with db.execute(query, params) as cursor:
                row = await cursor.fetchone()
                return row['last_date'] if row else None

    async def get_financial_history(self, ticker: str, metric: Optional[str] = None) -> List[Dict]:
        """
        Retrieves financial history for a ticker.
        
        Args:
            ticker (str): Ticker symbol.
            metric (Optional[str]): Filter by specific metric (e.g., 'revenue').
        
        Returns:
            List[Dict]: List of records.
        """
        query = "SELECT * FROM financial_history WHERE ticker = ?"
        params = [ticker.upper()]
        
        if metric:
            query += " AND metric = ?"
            params.append(metric)
            
        query += " ORDER BY period_end_date DESC"
        
        async with aiosqlite.connect(self.db_path) as db:
            db.row_factory = aiosqlite.Row
            async with db.execute(query, params) as cursor:
                rows = await cursor.fetchall()
                return [dict(row) for row in rows]


    async def get_relationships(self, ticker: str, relationship_type: Optional[str] = None) -> List[Dict]:
        """
        Retrieves graph edges for a ticker (where it is the source).
        
        Args:
            ticker (str): Source ticker.
            relationship_type (Optional[str]): Filter by type ('supplier', etc.).
            
        Returns:
            List[Dict]: List of relationships.
        """
        query = "SELECT * FROM industrial_graph WHERE source_ticker = ?"
        params = [ticker.upper()]
        
        if relationship_type:
            query += " AND relationship_type = ?"
            params.append(relationship_type)
            
        async with aiosqlite.connect(self.db_path) as db:
            db.row_factory = aiosqlite.Row
            async with db.execute(query, params) as cursor:
                rows = await cursor.fetchall()
                return [dict(row) for row in rows]

