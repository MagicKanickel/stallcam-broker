using System.IO;
using System.Net.WebSockets;
using System.Text;
using System.Text.Json;
using Microsoft.AspNetCore.Builder;
using Microsoft.AspNetCore.Hosting;
using Microsoft.AspNetCore.Http;
using Microsoft.Extensions.Hosting;
using Microsoft.Extensions.Logging;

namespace StallCamServer;

// ─── Datenklassen ─────────────────────────────────────────────────────
public record StatusData(
    int    VideoCount,
    long   SdUsed,
    long   SdTotal,
    int    Interval,
    int    Duration,
    int    CamRes,
    bool   NightMode,
    bool   IrAuto,
    bool   ApMode
);

// ─── Relay-Server ─────────────────────────────────────────────────────
/// <summary>
/// Kestrel-Web-Server der:
///   - Den ESP32 per WebSocket auf /esp32 empfängt
///   - Browser per WebSocket auf /ws empfängt
///   - Das Web-Interface auf / ausliefert (HTTP Basic Auth)
///   - Frames vom ESP32 an alle Browser und das WPF-Fenster weiterleitet
/// </summary>
public sealed class RelayServer
{
    private readonly string _relaySecret;
    private readonly string _webUser;
    private readonly string _webPass;

    // ── Zustand ──────────────────────────────────────────────────────
    private WebSocket?          _esp32;
    private readonly List<WebSocket> _viewers    = new();
    private readonly object     _viewersLock     = new();
    private byte[]?             _latestFrame;
    private string              _statusJson      = "{}";
    private TaskCompletionSource<byte[]>? _fileTcs;
    private MemoryStream?                 _fileBuffer;

    // ── Öffentliche Events (für WPF-Fenster) ─────────────────────────
    public event Action<byte[]>?    FrameReceived;
    public event Action<StatusData>? StatusUpdated;
    public event Action<string>?    VideoListReceived;
    public event Action<string>?    LogLine;

    public bool IsEsp32Connected => _esp32?.State == WebSocketState.Open;
    public int  ViewerCount      => _viewers.Count;

    public RelayServer(string relaySecret, string webUser, string webPass)
    {
        _relaySecret = relaySecret;
        _webUser     = webUser;
        _webPass     = webPass;
    }

    // ─────────────────────────────────────────────────────────────────
    // Server starten
    // ─────────────────────────────────────────────────────────────────
    public async Task RunAsync(int port, CancellationToken ct)
    {
        var builder = WebApplication.CreateSlimBuilder(new WebApplicationOptions
        {
            EnvironmentName = "Production"
        });
        builder.WebHost.UseUrls($"http://0.0.0.0:{port}");
        builder.Logging.SetMinimumLevel(LogLevel.Warning);

        var app = builder.Build();
        app.UseWebSockets(new WebSocketOptions
        {
            KeepAliveInterval = TimeSpan.FromSeconds(25)
        });

        // HTTP Basic Auth für alle Routen außer /esp32 und /ws
        app.Use(async (ctx, next) =>
        {
            var path = ctx.Request.Path.Value ?? "/";
            if (path is "/esp32" or "/ws" or "/health")
            {
                await next(ctx);
                return;
            }
            if (!CheckHttpAuth(ctx))
            {
                ctx.Response.Headers.WWWAuthenticate = "Basic realm=\"StallCam\"";
                ctx.Response.StatusCode = 401;
                await ctx.Response.WriteAsync("Nicht autorisiert", ct);
                return;
            }
            await next(ctx);
        });

        app.MapGet("/", async ctx =>
        {
            ctx.Response.ContentType = "text/html; charset=utf-8";
            await ctx.Response.WriteAsync(BuildWebHtml(), ct);
        });

        app.Map("/esp32", async ctx =>
        {
            if (!ctx.WebSockets.IsWebSocketRequest)
            {
                ctx.Response.StatusCode = 400;
                return;
            }
            using var ws = await ctx.WebSockets.AcceptWebSocketAsync();
            await HandleEsp32Async(ws, ct);
        });

        app.Map("/ws", async ctx =>
        {
            if (!ctx.WebSockets.IsWebSocketRequest)
            {
                ctx.Response.StatusCode = 400;
                return;
            }
            using var ws = await ctx.WebSockets.AcceptWebSocketAsync();
            await HandleViewerAsync(ws, ct);
        });

        app.MapGet("/health", () => "ok");

        app.MapGet("/download", async ctx =>
        {
            if (!CheckHttpAuth(ctx))
            {
                ctx.Response.Headers.WWWAuthenticate = "Basic realm=\"StallCam\"";
                ctx.Response.StatusCode = 401;
                return;
            }
            var file = ctx.Request.Query["file"].ToString();
            if (string.IsNullOrEmpty(file) || _esp32 is not { } esp || esp.State != WebSocketState.Open)
            {
                ctx.Response.StatusCode = 503;
                await ctx.Response.WriteAsync("ESP32 nicht verbunden");
                return;
            }
            _fileTcs    = new TaskCompletionSource<byte[]>();
            _fileBuffer = new MemoryStream();
            await SendTextAsync(esp, JsonSerializer.Serialize(new { cmd = "download", file }), ct);
            try
            {
                var data = await _fileTcs.Task.WaitAsync(TimeSpan.FromSeconds(60));
                ctx.Response.ContentType = file.EndsWith(".avi") ? "video/x-msvideo" : "application/octet-stream";
                ctx.Response.Headers["Content-Disposition"] = $"attachment; filename=\"{file}\"";
                await ctx.Response.Body.WriteAsync(data);
            }
            catch (TimeoutException)
            {
                ctx.Response.StatusCode = 504;
                await ctx.Response.WriteAsync("Timeout");
            }
            finally
            {
                _fileTcs    = null;
                _fileBuffer = null;
            }
        });

        await app.StartAsync(ct);
        LogLine?.Invoke($"[server] Web-Server läuft auf Port {port}");
        await app.WaitForShutdownAsync(ct);
    }

    // ─────────────────────────────────────────────────────────────────
    // HTTP Basic Auth Check
    // ─────────────────────────────────────────────────────────────────
    private bool CheckHttpAuth(HttpContext ctx)
    {
        var auth = ctx.Request.Headers.Authorization.ToString();
        if (!auth.StartsWith("Basic ", StringComparison.OrdinalIgnoreCase))
            return false;
        try
        {
            var decoded = Encoding.UTF8.GetString(Convert.FromBase64String(auth[6..]));
            var sep     = decoded.IndexOf(':');
            if (sep < 0) return false;
            return decoded[..sep] == _webUser && decoded[(sep + 1)..] == _webPass;
        }
        catch { return false; }
    }

    // ─────────────────────────────────────────────────────────────────
    // ESP32 WebSocket
    // ─────────────────────────────────────────────────────────────────
    private async Task HandleEsp32Async(WebSocket ws, CancellationToken ct)
    {
        bool authenticated = false;
        LogLine?.Invoke("[esp32] Neue Verbindung");

        try
        {
            while (!ws.CloseStatus.HasValue && !ct.IsCancellationRequested)
            {
                var (data, msgType) = await ReceiveFullAsync(ws, ct);
                if (msgType == WebSocketMessageType.Close) break;

                if (msgType == WebSocketMessageType.Text)
                {
                    var text = Encoding.UTF8.GetString(data);
                    using var doc  = JsonDocument.Parse(text);
                    var root = doc.RootElement;

                    if (!authenticated)
                    {
                        var isAuth   = root.TryGetProperty("type",   out var t) && t.GetString() == "auth";
                        var okSecret = root.TryGetProperty("secret", out var s) && s.GetString() == _relaySecret;
                        if (isAuth && okSecret)
                        {
                            authenticated = true;
                            _esp32        = ws;
                            LogLine?.Invoke("[esp32] Authentifiziert!");
                            await SendTextAsync(ws, """{"cmd":"get_status"}""", ct);
                            lock (_viewersLock)
                                if (_viewers.Count > 0)
                                    _ = SendTextAsync(ws, """{"cmd":"stream_start"}""", CancellationToken.None);
                        }
                        else
                            LogLine?.Invoke("[esp32] Falsches Secret – abgelehnt");
                        continue;
                    }

                    // Authentifizierte Nachrichten
                    if (!root.TryGetProperty("type", out var typeEl)) continue;
                    var typeStr = typeEl.GetString();

                    if (typeStr == "ping") { /* Cloudflare keepalive – ignorieren */ }
                    else if (typeStr == "status")
                    {
                        _statusJson = text;
                        var status  = ParseStatus(root);
                        StatusUpdated?.Invoke(status);
                        await BroadcastTextAsync(text, ct);
                    }
                    else if (typeStr == "videos")
                    {
                        VideoListReceived?.Invoke(text);
                        await BroadcastTextAsync(text, ct);
                    }
                    else if (typeStr == "file_start")
                    {
                        _fileBuffer = new MemoryStream();
                    }
                    else if (typeStr == "file_end")
                    {
                        _fileTcs?.TrySetResult(_fileBuffer?.ToArray() ?? Array.Empty<byte>());
                    }
                }
                else if (msgType == WebSocketMessageType.Binary && authenticated)
                {
                    if (_fileBuffer != null)
                    {
                        await _fileBuffer.WriteAsync(data);
                    }
                    else
                    {
                        _latestFrame = data;
                        FrameReceived?.Invoke(data);
                        await BroadcastBinaryAsync(data, ct);
                    }
                }
            }
        }
        catch (OperationCanceledException) { }
        catch (WebSocketException ex) { LogLine?.Invoke($"[esp32] WS-Fehler: {ex.Message}"); }
        finally
        {
            if (_esp32 == ws) _esp32 = null;
            LogLine?.Invoke("[esp32] Verbindung getrennt");
            await BroadcastTextAsync("""{"type":"offline"}""", CancellationToken.None);
        }
    }

    // ─────────────────────────────────────────────────────────────────
    // Browser WebSocket
    // ─────────────────────────────────────────────────────────────────
    private async Task HandleViewerAsync(WebSocket ws, CancellationToken ct)
    {
        bool wasFirst;
        lock (_viewersLock)
        {
            _viewers.Add(ws);
            wasFirst = _viewers.Count == 1;
        }
        LogLine?.Invoke($"[browser] Verbunden ({_viewers.Count} aktiv)");

        try
        {
            // Sofort letzten Frame + Status schicken
            if (_latestFrame is { } frame)
                await ws.SendAsync(frame, WebSocketMessageType.Binary, true, ct);
            await SendTextAsync(ws, _statusJson, ct);

            // Streaming starten falls erster Viewer
            if (wasFirst && _esp32 is { } esp && esp.State == WebSocketState.Open)
                await SendTextAsync(esp, """{"cmd":"stream_start"}""", ct);
            else if (wasFirst && _esp32 == null)
                await SendTextAsync(ws, """{"type":"offline"}""", ct);

            // Befehle vom Browser empfangen und an ESP32 weiterleiten
            var buf = new byte[4096];
            while (!ws.CloseStatus.HasValue && !ct.IsCancellationRequested)
            {
                var result = await ws.ReceiveAsync(buf.AsMemory(), ct);
                if (result.MessageType == WebSocketMessageType.Close) break;
                if (result.MessageType == WebSocketMessageType.Text &&
                    _esp32 is { } e && e.State == WebSocketState.Open)
                {
                    var cmd = buf[..result.Count].ToArray();
                    await e.SendAsync(cmd, WebSocketMessageType.Text, true, ct);
                }
            }
        }
        catch (OperationCanceledException) { }
        catch (WebSocketException) { }
        finally
        {
            bool isEmpty;
            lock (_viewersLock)
            {
                _viewers.Remove(ws);
                isEmpty = _viewers.Count == 0;
            }
            LogLine?.Invoke($"[browser] Getrennt ({_viewers.Count} verbleiben)");
            if (isEmpty && _esp32 is { } esp && esp.State == WebSocketState.Open)
                await SendTextAsync(esp, """{"cmd":"stream_stop"}""", CancellationToken.None);
        }
    }

    // ─────────────────────────────────────────────────────────────────
    // Befehle von der WPF-GUI an ESP32 senden
    // ─────────────────────────────────────────────────────────────────
    public async Task SendCommandAsync(string cmdJson)
    {
        if (_esp32 is { } ws && ws.State == WebSocketState.Open)
            await SendTextAsync(ws, cmdJson, CancellationToken.None);
    }

    // ─────────────────────────────────────────────────────────────────
    // Hilfsmethoden
    // ─────────────────────────────────────────────────────────────────
    private static async Task<(byte[] data, WebSocketMessageType type)> ReceiveFullAsync(
        WebSocket ws, CancellationToken ct)
    {
        var buffer = new byte[65536];
        using var ms = new MemoryStream();
        ValueWebSocketReceiveResult result;
        do
        {
            result = await ws.ReceiveAsync(buffer.AsMemory(), ct);
            if (result.MessageType == WebSocketMessageType.Close)
                return (Array.Empty<byte>(), WebSocketMessageType.Close);
            ms.Write(buffer, 0, result.Count);
        }
        while (!result.EndOfMessage);
        return (ms.ToArray(), result.MessageType);
    }

    private static Task SendTextAsync(WebSocket ws, string text, CancellationToken ct)
        => ws.SendAsync(Encoding.UTF8.GetBytes(text), WebSocketMessageType.Text, true, ct);

    private Task BroadcastTextAsync(string text, CancellationToken ct)
    {
        var bytes = Encoding.UTF8.GetBytes(text);
        List<WebSocket> snapshot;
        lock (_viewersLock) snapshot = [.. _viewers];
        var tasks = snapshot
            .Where(w => w.State == WebSocketState.Open)
            .Select(w => w.SendAsync(bytes, WebSocketMessageType.Text, true, CancellationToken.None));
        return Task.WhenAll(tasks);
    }

    private Task BroadcastBinaryAsync(byte[] data, CancellationToken ct)
    {
        List<WebSocket> snapshot;
        lock (_viewersLock) snapshot = [.. _viewers];
        var tasks = snapshot
            .Where(w => w.State == WebSocketState.Open)
            .Select(w => w.SendAsync(data, WebSocketMessageType.Binary, true, CancellationToken.None));
        return Task.WhenAll(tasks);
    }

    private static StatusData ParseStatus(JsonElement root)
    {
        T Get<T>(string key, T def) where T : struct
        {
            if (!root.TryGetProperty(key, out var el)) return def;
            return typeof(T) == typeof(bool)
                ? (T)(object)el.GetBoolean()
                : typeof(T) == typeof(int) ? (T)(object)el.GetInt32()
                : typeof(T) == typeof(long) ? (T)(object)el.GetInt64()
                : def;
        }
        return new StatusData(
            VideoCount: Get("videoCount", 0),
            SdUsed:     Get<long>("sdUsed",  0L),
            SdTotal:    Get<long>("sdTotal", 0L),
            Interval:   Get("interval",  10),
            Duration:   Get("duration",  5),
            CamRes:     Get("camRes",    5),
            NightMode:  Get("nightMode", false),
            IrAuto:     Get("irAuto",    true),
            ApMode:     Get("apMode",    false)
        );
    }

    // ─────────────────────────────────────────────────────────────────
    // Web-UI HTML (identisch zur ESP32-UI, aber mit WebSocket-Stream)
    // ─────────────────────────────────────────────────────────────────
    private static string BuildWebHtml() => """
        <!DOCTYPE html>
        <html lang="de">
        <head>
        <meta charset="UTF-8"><meta name="viewport" content="width=device-width,initial-scale=1">
        <title>StallCam Remote</title>
        <style>
        @import url('https://fonts.googleapis.com/css2?family=Outfit:wght@300;400;600;700&family=Space+Mono&display=swap');
        *{box-sizing:border-box;margin:0;padding:0}
        :root{--bg:#0d1117;--card:#161b22;--border:#30363d;--green:#3fb950;--amber:#d29922;--red:#f85149;--blue:#58a6ff;--text:#e6edf3;--muted:#8b949e}
        body{background:var(--bg);color:var(--text);font-family:'Outfit',sans-serif;min-height:100vh;padding:20px}
        .hdr{display:flex;align-items:center;gap:12px;margin-bottom:24px;flex-wrap:wrap}
        .logo{font-size:1.6rem;font-weight:700;letter-spacing:-.04em}.logo span{color:var(--green)}
        .badge{font-family:'Space Mono',monospace;font-size:.7rem;padding:3px 8px;border-radius:4px;background:var(--card);border:1px solid var(--border);color:var(--muted)}
        .badge.on{border-color:var(--green);color:var(--green)}.badge.off{border-color:var(--red);color:var(--red)}
        .grid{display:grid;grid-template-columns:1fr 1fr;gap:16px;max-width:960px}
        @media(max-width:640px){.grid{grid-template-columns:1fr}}
        .card{background:var(--card);border:1px solid var(--border);border-radius:12px;overflow:hidden}
        .ch{padding:14px 16px;border-bottom:1px solid var(--border);font-weight:600;font-size:.9rem;display:flex;align-items:center;gap:8px}
        .cb{padding:16px}
        .sw{width:100%;aspect-ratio:4/3;background:#000;position:relative;display:flex;align-items:center;justify-content:center}
        canvas{width:100%;height:100%;object-fit:contain}
        .ld{position:absolute;top:10px;left:10px;background:var(--red);color:#fff;font-size:.65rem;padding:3px 8px;border-radius:999px;font-family:'Space Mono',monospace}
        .sr{display:flex;justify-content:space-between;align-items:center;padding:7px 0;border-bottom:1px solid var(--border);font-size:.84rem}
        .sr:last-child{border:none}.sl{color:var(--muted)}.sv{font-family:'Space Mono',monospace;color:var(--blue)}
        .row{margin-bottom:13px}.row label{display:block;font-size:.8rem;color:var(--muted);margin-bottom:4px}
        .row input[type=range]{width:100%;accent-color:var(--green)}
        .row select{width:100%;background:var(--bg);border:1px solid var(--border);color:var(--text);padding:8px;border-radius:6px;font-family:'Outfit',sans-serif;font-size:.85rem}
        .tr{display:flex;justify-content:space-between;align-items:center;padding:7px 0}
        .tg{position:relative;width:44px;height:24px}.tg input{opacity:0;width:0;height:0}
        .sl2{position:absolute;inset:0;background:var(--border);border-radius:12px;cursor:pointer;transition:.2s}
        .sl2::before{content:'';position:absolute;width:18px;height:18px;left:3px;top:3px;background:#fff;border-radius:50%;transition:.2s}
        input:checked+.sl2{background:var(--green)}input:checked+.sl2::before{transform:translateX(20px)}
        .btn{font-family:'Outfit',sans-serif;font-size:.84rem;font-weight:600;padding:9px 16px;border-radius:8px;border:none;cursor:pointer;transition:all .15s;display:inline-flex;align-items:center;gap:6px}
        .bp{background:var(--green);color:#000}.bd{background:var(--red);color:#fff}
        .bs{background:transparent;border:1px solid var(--border);color:var(--text)}
        .sdb{height:5px;background:var(--border);border-radius:3px;margin-top:8px;overflow:hidden}
        .sdf{height:100%;background:var(--green);border-radius:3px;transition:width .5s}
        .full{grid-column:1/-1}.gap{display:flex;flex-wrap:wrap;gap:8px}
        </style>
        </head>
        <body>
        <div class="hdr">
          <div class="logo">Stall<span>Cam</span> 🐴</div>
          <div class="badge" id="cam-badge">● Verbinde...</div>
          <div class="badge" id="sdbadge">SD: –</div>
        </div>
        <div class="grid">
          <div class="card full">
            <div class="ch">📹 Livestream</div>
            <div class="sw"><canvas id="cv"></canvas><div class="ld" id="ld">● LIVE</div></div>
          </div>
          <div class="card">
            <div class="ch">📊 Status</div>
            <div class="cb">
              <div class="sr"><span class="sl">Videos</span><span class="sv" id="svc">–</span></div>
              <div class="sr"><span class="sl">SD belegt</span><span class="sv" id="ssu">–</span></div>
              <div class="sr"><span class="sl">SD gesamt</span><span class="sv" id="sst">–</span></div>
              <div class="sr"><span class="sl">Intervall</span><span class="sv" id="sint">–</span></div>
              <div class="sr"><span class="sl">Dauer</span><span class="sv" id="sdur">–</span></div>
              <div class="sdb"><div class="sdf" id="sdf" style="width:0%"></div></div>
            </div>
          </div>
          <div class="card">
            <div class="ch">⚙️ Einstellungen</div>
            <div class="cb">
              <div class="row"><label>Intervall: <b id="iv">10 min</b></label><input type="range" id="isl" min="1" max="120" value="10" oninput="lbl('iv',this.value,' min')"></div>
              <div class="row"><label>Dauer: <b id="dv">5 sek</b></label><input type="range" id="dsl" min="2" max="120" value="5" oninput="lbl('dv',this.value,' sek')"></div>
              <div class="row"><label>Auflösung</label>
                <select id="rsel">
                  <option value="4">VGA (640×480)</option><option value="5" selected>SVGA (800×600)</option>
                  <option value="6">XGA (1024×768)</option><option value="7">SXGA (1280×1024)</option><option value="8">UXGA (1600×1200)</option>
                </select>
              </div>
              <div class="tr"><span style="font-size:.85rem">🌙 Nachtsicht</span><label class="tg"><input type="checkbox" id="nt" onchange="cmd(this.checked?'night_on':'night_off')"><span class="sl2"></span></label></div>
              <div class="tr"><span style="font-size:.85rem">💡 IR-LED auto</span><label class="tg"><input type="checkbox" id="ir" checked onchange="cmd(this.checked?'led_on':'led_off')"><span class="sl2"></span></label></div>
              <br><div class="gap">
                <button class="btn bp" onclick="save()">💾 Speichern</button>
                <button class="btn bs" onclick="cmd('record')">🎬 Aufnehmen</button>
              </div>
            </div>
          </div>
          <div class="card full">
            <div class="ch">🎞️ Videos
              <button class="btn bs" onclick="cmd('get_videos')" style="margin-left:8px;padding:5px 10px;font-size:.75rem">🔄</button>
              <button class="btn bd" onclick="delAll()" style="margin-left:auto;padding:5px 10px;font-size:.75rem">🗑️ Alle</button>
            </div>
            <div class="cb"><div id="vlist" style="color:var(--muted);font-size:.85rem">Klicke 🔄 zum Laden</div></div>
          </div>
        </div>
        <script>
        const ws=new WebSocket((location.protocol==='https:'?'wss:':'ws:')+'//'+ location.host+'/ws');
        ws.binaryType='arraybuffer';
        const cv=document.getElementById('cv'),ctx=cv.getContext('2d');
        ws.onopen=()=>badge('cam-badge','● Verbunden','on');
        ws.onclose=()=>{badge('cam-badge','● Offline','off');setTimeout(()=>location.reload(),3000)};
        ws.onmessage=e=>{
          if(e.data instanceof ArrayBuffer){drawFrame(e.data);}
          else{try{handle(JSON.parse(e.data));}catch(x){}}
        };
        function drawFrame(buf){
          const url=URL.createObjectURL(new Blob([buf],{type:'image/jpeg'}));
          const img=new Image();img.onload=()=>{
            if(cv.width!==img.width){cv.width=img.width;cv.height=img.height;}
            ctx.drawImage(img,0,0);URL.revokeObjectURL(url);
          };img.src=url;
        }
        function handle(d){
          if(d.type==='status'){
            badge('cam-badge','● Online','on');
            setText('svc',d.videoCount);setText('ssu',fgb(d.sdUsed));setText('sst',fgb(d.sdTotal));
            setText('sint',d.interval+' min');setText('sdur',d.duration+' sek');
            badge('sdbadge','SD: '+fgb(d.sdUsed)+'/'+fgb(d.sdTotal));
            document.getElementById('isl').value=d.interval;lbl('iv',d.interval,' min');
            document.getElementById('dsl').value=d.duration;lbl('dv',d.duration,' sek');
            if(d.camRes!=null)document.getElementById('rsel').value=d.camRes;
            if(d.nightMode!=null)document.getElementById('nt').checked=d.nightMode;
            if(d.irAuto!=null)document.getElementById('ir').checked=d.irAuto;
            const p=d.sdTotal>0?d.sdUsed/d.sdTotal*100:0;
            const b=document.getElementById('sdf');b.style.width=p+'%';
            b.className='sdf'+(p>90?' c':p>70?' w':'');
          } else if(d.type==='videos'){
            const el=document.getElementById('vlist');
            if(!d.files||!d.files.length){el.textContent='Keine Videos';return;}
            el.innerHTML=d.files.map(f=>`<div style="display:flex;justify-content:space-between;align-items:center;padding:5px 0;border-bottom:1px solid var(--border);font-size:.82rem"><span style="font-family:Space Mono,monospace;color:var(--muted);font-size:.72rem">${f.name}</span><a href="/download?file=${f.name}" style="color:var(--green);font-size:.72rem;text-decoration:none" download="${f.name}">⬇ ${fmb(f.size)}</a></div>`).join('');
          } else if(d.type==='offline'){
            badge('cam-badge','● Kamera offline','off');
          }
        }
        function cmd(c,extra){if(ws.readyState===1)ws.send(JSON.stringify({cmd:c,...(extra||{})}))}
        function save(){cmd('save_settings',{interval:+document.getElementById('isl').value,duration:+document.getElementById('dsl').value,camRes:+document.getElementById('rsel').value,nightMode:document.getElementById('nt').checked,irAuto:document.getElementById('ir').checked});}
        function delAll(){if(confirm('Alle Videos löschen?'))cmd('delete_all');}
        function lbl(id,v,u){document.getElementById(id).textContent=v+u}
        function setText(id,v){document.getElementById(id).textContent=v??'–'}
        function badge(id,t,cls){const e=document.getElementById(id);e.textContent=t;e.className='badge'+(cls?' '+cls:'');}
        function fgb(b){if(b>1e9)return(b/1e9).toFixed(1)+' GB';if(b>1e6)return(b/1e6).toFixed(0)+' MB';return b+' B'}
        function fmb(b){if(b>1e6)return(b/1e6).toFixed(1)+' MB';if(b>1e3)return(b/1e3).toFixed(0)+' KB';return b+' B'}
        </script>
        </body></html>
        """;
}
