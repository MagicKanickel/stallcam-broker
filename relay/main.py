"""
StallCam URL-Broker
===================
Winziger Service (~30 Zeilen) der nur eine einzige Information speichert:
"Unter welcher bore-URL ist der C#-Server gerade erreichbar?"

Deploy auf Render.com (kostenlos, Free Tier).

Endpunkte:
  POST /register   <- C# Server meldet sich an  {"secret":"...", "url":"bore.pub:PORT"}
  GET  /relay-url  -> ESP32 fragt ab            {"host":"bore.pub", "port":PORT, "url":"..."}
  GET  /health     -> Health-Check (kein Auth)
"""

import os
import json
from aiohttp import web

BROKER_SECRET = os.environ.get("BROKER_SECRET", "broker-geheim123")

# Im RAM gespeichert – reicht aus (C# re-registriert sich alle 5 min)
relay = {"host": "", "port": 0, "url": ""}


async def handle_register(req: web.Request) -> web.Response:
    try:
        data = await req.json()
    except Exception:
        return web.Response(status=400, text="Kein gültiges JSON")

    if data.get("secret") != BROKER_SECRET:
        return web.Response(status=401, text="Falsches Secret")

    raw = data.get("url", "").strip()
    if not raw:
        return web.Response(status=400, text="Leere URL")

    # URL parsen: "bore.pub:PORT" oder cloudflare "xxx.trycloudflare.com"
    parts = raw.rsplit(":", 1)
    if len(parts) == 2 and parts[1].isdigit():
        host = parts[0]
        port = int(parts[1])
    else:
        host = raw          # cloudflare: kein Port → 443 (HTTPS)
        port = 443

    relay.update(host=host, port=port, url=raw)
    print(f"Relay registriert: {raw}", flush=True)
    return web.json_response({"ok": True})


async def handle_get_url(req: web.Request) -> web.Response:
    return web.json_response(relay)


async def handle_health(req: web.Request) -> web.Response:
    return web.Response(text="ok")


app = web.Application()
app.router.add_post("/register", handle_register)
app.router.add_get("/relay-url", handle_get_url)
app.router.add_get("/health",    handle_health)

if __name__ == "__main__":
    port = int(os.environ.get("PORT", "8080"))
    print(f"URL-Broker läuft auf Port {port}", flush=True)
    web.run_app(app, host="0.0.0.0", port=port)
