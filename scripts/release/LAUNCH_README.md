# Sentinel — Run the app

Run the **server** first, then the **client**. Use the client to connect and stream a heatmap.

---

## Quick start

1. **Run the server**  
   In a terminal, from this folder:
   - **macOS release bundle:** `./SentinelServer.app/Contents/MacOS/sentinel-server` (or `./run.sh` which starts server + GUI)
   - **Mac/Linux flat binary:** `./sentinel-server`
   - **Windows:** `sentinel-server.exe`

2. **Run the client**  
   In a second terminal, from this folder unless you used `./run.sh`:
   - **macOS release bundle:** `./Sentinel.app/Contents/MacOS/sentinel-gui` or `./run.sh`
   - **Mac/Linux build tree:** `./sentinel-gui` next to `build/<preset>/apps/sentinel-gui/<Release|Debug>/`
   - **Windows:** `sentinel-gui.exe`

3. **Stream a heatmap**  
   In the client: press the **magnifying glass** or enter a Coinbase symbol (e.g. `BTC-USD`) and stream your heatmap.

---

## First-time setup

- **Certs:** The client connects to the server over TLS. If you see a certificate error, generate a self-signed cert once:
  - **Mac/Linux:** `bash certs/gen-certs.sh`
  - **Windows:** Use OpenSSL to create `certs/sentinel-server.crt` (and key) with SAN for `localhost` / `127.0.0.1`, or see the repo’s `certs/` docs.

- **Config:** Defaults are in `config/`. To override, copy `config/server_config.yaml` → `config/.server_config.yaml` and `config/client_config.yaml` → `config/.client_config.yaml` and edit as needed.

---

## Layout

- **Server:** `sentinel-server` / `sentinel-server.exe`, or **`SentinelServer.app`** (macOS release) — run first  
- **Client:** **`Sentinel.app`** (macOS) or **`sentinel-gui`** / `sentinel-gui.exe` — GUI client  
- `scripts/` — Python backends (SEC, screener, candle fetch); installs via `uv sync` inside this directory when packaging  
- `config/` — server and client config
- `certs/` — TLS certs (generate with `certs/gen-certs.sh` if missing)
