# Sentinel macOS bundle — Quick start

## Run this bundle

Double-click **`run.sh`** in Finder (“Open With → Terminal”), or:

```bash
cd /path/to/this-folder
chmod +x run.sh   # only if your zip stripped execute bits
./run.sh
```

The launcher sets the folder as the working directory (`config/`, `scripts/`, `certs/` are read from **here**, not from a source checkout).

---

## Expected first launch

1. **`run.sh`** starts **SentinelServer** (`SentinelServer.app`) in the background, then opens **Sentinel** (`Sentinel.app`).
2. You may see a **Gatekeeper / unsigned developer** prompt (ad-hoc signed bundle). Grant “Open Anyway” **System Settings → Privacy & Security** if needed (normal for unsigned downloadable builds unless notarized separately).
3. If **port 8080** is already in use locally, quit the other Sentinel server or resolve the conflict.
4. **`MANIFEST.txt`** describes what was bundled and **which git revision** produced it.

---

## Coinbase data (demo)

Live **Coinbase Advance / Exchange REST and public websocket** feeds use **symbols like `BTC-USD`**. Default server config ships with **`BTC-USD`**.

**Public market data requires no Coinbase API key.** User-specific or private channels remain behind exchange credentials elsewhere in the ecosystem.

---

## TLS (GUI ↔ sentinel-server on port **8080**)

The client connects over **TLS** to **`127.0.0.1:8080`**. Files **`certs/sentinel-server.crt`** and **`certs/sentinel-server.key`** must be one **matching** keypair. **`run.sh`** checks this with OpenSSL and runs **`bash certs/gen-certs.sh`** to replace a bad pair automatically.

If the stream server never accepts connections (**Connection refused**), open **`logs/sentinel-server.log`** — a line like **`SentinelStreamServer start failed`** with an OpenSSL hint usually means the cert/key were out of sync (common after copying halves from different machines).

---

## Where logs go

| Output | Location |
|--------|-----------|
| **Server** stdout/stderr | **`logs/sentinel-server.log`** (same folder hierarchy as **`config/`**). **`./run.sh` truncates this file at the start of each run**; copy it aside first if you need history. |

| **GUI / Qt** | If you launched from Terminal, traces may appear **in that Terminal window**. There is **no guaranteed on-disk stdout** for graphical macOS launches from Finder (“Open”). Use Console.app filtering for `sentinel-gui`, or **`./run.sh` from Terminal** to capture messages. |

---

## Known limitations

- **Unsigned / ad-hoc** bundles may trigger macOS security prompts until explicitly allowed.
- **Python-backed** features (`scripts/` — SEC overlays, candles, some screener paths) rely on **`uv`** and **`uv sync`** in `scripts/`. Missing `.venv`/deps → those panels report errors rather than crashing the GUI.
- **`copetech-edgar`**: If the overlay depends on EDG tooling, releases bundle it as a wheel in **`third_party/wheels/`**, built from a tagged CopeTech-Edgar release (see `docs/RELEASE_CHECKLIST.md`) — no adjacent git checkout is needed.
- **Smoke / CI parity**: Maintainers validate against extracted zips **`smoke_macos.sh --clean-room`**; end users only need `./run.sh` + **README_RELEASE.md**.
