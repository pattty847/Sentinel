#!/usr/bin/env python3
"""
Persistent screener WebSocket server.
Wraps tvscreener and streams results to ScreenerDock in Sentinel.

Port: SENTINEL_SCREENER_PORT env var (default 17200)

Client → Server messages:
  { "type": "set_config", "asset": "crypto"|"stock",
    "fields": ["RSI|240", "MACD|1D", ...],
    "min_volume": 0.0, "limit": 50,
    "interval_sec": 120 }
  { "type": "refresh" }   -- immediate one-shot poll

Server → Client messages:
  { "type": "screener_update", "asset": "...", "timestamp": <unix>,
    "rows": [ { "symbol": "...", ... }, ... ] }
  { "type": "error", "message": "..." }
  { "type": "status", "message": "..." }
"""
import asyncio
import json
import logging
import os
import random
import sys
import time
from pathlib import Path

import aiohttp
from aiohttp import web

sys.path.insert(0, str(Path(__file__).parent.parent))  # resolve sibling packages when run directly

from screener.screener_core import build_screener, screener_to_rows

logging.basicConfig(level=logging.INFO, format="%(asctime)s [screener] %(message)s")
log = logging.getLogger("screener_server")

DEFAULT_PORT = int(os.environ.get("SENTINEL_SCREENER_PORT", "17200"))
MIN_INTERVAL_SEC = 30       # never poll faster than this regardless of config
MAX_INTERVAL_SEC = 300      # clamp upper end


class ScreenerSession:
    """Per-connection state."""

    def __init__(self, ws: web.WebSocketResponse):
        self.ws = ws
        self.asset = "crypto"
        self.fields: list[str] = []
        self.min_volume = 0.0
        self.limit = 50
        self.interval_sec = 120
        self._poll_task: asyncio.Task | None = None
        self._refresh_event = asyncio.Event()

    def update_config(self, msg: dict):
        self.asset = msg.get("asset", self.asset)
        self.fields = msg.get("fields", self.fields)
        self.min_volume = float(msg.get("min_volume", self.min_volume))
        self.limit = int(msg.get("limit", self.limit))
        raw_interval = float(msg.get("interval_sec", self.interval_sec))
        self.interval_sec = max(MIN_INTERVAL_SEC, min(MAX_INTERVAL_SEC, raw_interval))

    async def send(self, payload: dict):
        try:
            await self.ws.send_str(json.dumps(payload))
        except Exception:
            pass

    async def send_status(self, msg: str):
        await self.send({"type": "status", "message": msg})

    async def send_error(self, msg: str):
        await self.send({"type": "error", "message": msg})

    async def poll_once(self):
        """Run one screener fetch and push results."""
        try:
            await self.send_status(f"fetching {self.asset} screener...")
            loop = asyncio.get_event_loop()
            # tvscreener is synchronous — run in thread pool to avoid blocking
            screener = build_screener(self.asset, self.fields, self.min_volume, self.limit)
            df = await loop.run_in_executor(None, screener.get)
            rows = screener_to_rows(df)
            await self.send({
                "type": "screener_update",
                "asset": self.asset,
                "timestamp": int(time.time()),
                "row_count": len(rows),
                "rows": rows,
            })
            log.info("pushed %d rows (%s) to client", len(rows), self.asset)
        except Exception as e:
            log.warning("poll error: %s", e)
            await self.send_error(str(e))

    async def poll_loop(self):
        """Periodically poll tvscreener with jitter."""
        while True:
            await self.poll_once()
            # Random jitter ±15% of interval — avoids predictable TV fingerprint
            jitter = self.interval_sec * 0.15
            sleep_sec = self.interval_sec + random.uniform(-jitter, jitter)
            try:
                await asyncio.wait_for(self._refresh_event.wait(), timeout=sleep_sec)
                self._refresh_event.clear()  # manual refresh triggered
            except asyncio.TimeoutError:
                pass  # normal interval expiry

    def start_polling(self):
        if self._poll_task:
            self._poll_task.cancel()
        self._poll_task = asyncio.create_task(self.poll_loop())

    def stop_polling(self):
        if self._poll_task:
            self._poll_task.cancel()
            self._poll_task = None

    def trigger_refresh(self):
        self._refresh_event.set()


async def handle_ws(request: web.Request) -> web.WebSocketResponse:
    ws = web.WebSocketResponse()
    await ws.prepare(request)
    session = ScreenerSession(ws)

    log.info("client connected from %s", request.remote)
    await session.send_status("connected — send set_config to start")

    try:
        async for msg in ws:
            if msg.type == aiohttp.WSMsgType.TEXT:
                try:
                    data = json.loads(msg.data)
                except json.JSONDecodeError:
                    await session.send_error("invalid JSON")
                    continue

                msg_type = data.get("type")

                if msg_type == "set_config":
                    session.update_config(data)
                    log.info("config: asset=%s fields=%s interval=%ss",
                             session.asset, session.fields, session.interval_sec)
                    session.start_polling()  # restart loop with new config

                elif msg_type == "refresh":
                    if session._poll_task:
                        session.trigger_refresh()
                    else:
                        # No active poll loop yet — just do a one-shot fetch
                        await session.poll_once()

                else:
                    await session.send_error(f"unknown message type: {msg_type}")

            elif msg.type in (aiohttp.WSMsgType.ERROR, aiohttp.WSMsgType.CLOSE):
                break

    finally:
        session.stop_polling()
        log.info("client disconnected")

    return ws


async def handle_health(request: web.Request) -> web.Response:
    return web.Response(text="ok")


def make_app() -> web.Application:
    app = web.Application()
    app.router.add_get("/screener", handle_ws)
    app.router.add_get("/health", handle_health)
    return app


if __name__ == "__main__":
    port = DEFAULT_PORT
    log.info("starting screener server on ws://127.0.0.1:%d/screener", port)
    web.run_app(make_app(), host="127.0.0.1", port=port, print=None)
