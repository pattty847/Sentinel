#!/usr/bin/env python3
"""
SEC Service - FastAPI bridge for Sentinel Qt application.
Wraps SECDataFetcher and exposes REST API endpoints.
"""

import sys
import os
import asyncio
from pathlib import Path

# Add parent directory to path to import sec module
sys.path.insert(0, str(Path(__file__).parent.parent))

from fastapi import FastAPI, HTTPException
from fastapi.middleware.cors import CORSMiddleware
from pydantic import BaseModel
from typing import List, Optional, Dict, Any
import uvicorn

from sec.sec_api import SECDataFetcher

app = FastAPI(title="Sentinel SEC Service")

# CORS middleware for Qt app
app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],
    allow_credentials=True,
    allow_methods=["*"],
    allow_headers=["*"],
)

# Initialize SEC fetcher
fetcher = SECDataFetcher()

@app.get("/")
async def root():
    return {"status": "running", "service": "SEC Data Service"}

@app.get("/api/filings")
async def get_filings(ticker: str, form_type: Optional[str] = None):
    """Get filings for a ticker, optionally filtered by form type."""
    try:
        filings = await fetcher.get_filings_by_form(ticker, form_type)
        return {"ticker": ticker, "form_type": form_type, "filings": filings}
    except Exception as e:
        raise HTTPException(status_code=500, detail=str(e))

@app.get("/api/insider-transactions")
async def get_insider_transactions(ticker: str):
    """Get insider transactions (Form 4) for a ticker."""
    try:
        transactions = await fetcher.fetch_insider_filings(ticker)
        return {"ticker": ticker, "transactions": transactions}
    except Exception as e:
        raise HTTPException(status_code=500, detail=str(e))

@app.get("/api/financial-summary")
async def get_financial_summary(ticker: str):
    """Get financial summary for a ticker."""
    try:
        summary = await fetcher.get_financial_summary(ticker)
        return {"ticker": ticker, "summary": summary}
    except Exception as e:
        raise HTTPException(status_code=500, detail=str(e))

if __name__ == "__main__":
    port = int(os.environ.get("SEC_SERVICE_PORT", 8765))
    uvicorn.run(app, host="0.0.0.0", port=port)

