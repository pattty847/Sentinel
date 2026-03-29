# Sentinel — Run the app

Run the **server** first, then the **client**. Use the client to connect and stream a heatmap.

---

## Quick start

1. **Run the server**  
   In a terminal, from this folder:
   - **Mac/Linux:** `./sentinel-server`
   - **Windows:** `sentinel-server.exe`

2. **Run the client**  
   In a second terminal, from this folder:
   - **Mac/Linux:** `./sentinel_gui`
   - **Windows:** `sentinel_gui.exe`

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

- `sentinel-server` / `sentinel-server.exe` — data server (run first)
- `sentinel_gui` / `sentinel_gui.exe` — GUI client
- `config/` — server and client config
- `certs/` — TLS certs (generate with `certs/gen-certs.sh` if missing)
