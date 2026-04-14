using System.Diagnostics;
using System.IO;
using System.Text.RegularExpressions;

namespace StallCamServer;

/// <summary>
/// Startet cloudflared.exe als Kindprozess und parst die öffentliche HTTPS-URL.
/// cloudflared.exe muss im selben Verzeichnis wie StallCamServer.exe liegen.
/// Download: https://github.com/cloudflare/cloudflared/releases/latest/download/cloudflared-windows-amd64.exe
/// (umbenennen zu cloudflared.exe)
/// </summary>
public sealed class BoreTunnel : IDisposable
{
    private Process?    _process;
    private string      _url = "";
    private bool        _disposed;

    public string Url       => _url;
    public bool   IsRunning => _process is { HasExited: false };

    /// <summary>Wird aufgerufen sobald die öffentliche URL bekannt ist.</summary>
    public event Action<string>? UrlDiscovered;

    /// <summary>Wird für jede Ausgabezeile von cloudflared aufgerufen.</summary>
    public event Action<string>? LogLine;

    /// <summary>
    /// Startet cloudflared und tunnelt den lokalen <paramref name="localPort"/>.
    /// Gibt false zurück wenn cloudflared.exe nicht gefunden wurde.
    /// </summary>
    public bool Start(int localPort)
    {
        var exePath = Path.Combine(AppContext.BaseDirectory, "cloudflared.exe");
        if (!File.Exists(exePath))
        {
            LogLine?.Invoke($"[tunnel] cloudflared.exe nicht gefunden in: {AppContext.BaseDirectory}");
            LogLine?.Invoke("[tunnel] Download: https://github.com/cloudflare/cloudflared/releases/latest/download/cloudflared-windows-amd64.exe");
            LogLine?.Invoke("[tunnel] Datei umbenennen zu: cloudflared.exe");
            return false;
        }

        _process = new Process
        {
            StartInfo = new ProcessStartInfo
            {
                FileName               = exePath,
                Arguments              = $"tunnel --url http://localhost:{localPort}",
                RedirectStandardOutput = true,
                RedirectStandardError  = true,
                UseShellExecute        = false,
                CreateNoWindow         = true,
            },
            EnableRaisingEvents = true,
        };

        _process.OutputDataReceived += (_, e) => { if (e.Data != null) ParseLine(e.Data); };
        _process.ErrorDataReceived  += (_, e) => { if (e.Data != null) ParseLine(e.Data); };
        _process.Exited             += (_, _) => LogLine?.Invoke("[tunnel] Prozess beendet");

        try
        {
            _process.Start();
            _process.BeginOutputReadLine();
            _process.BeginErrorReadLine();
            LogLine?.Invoke($"[tunnel] cloudflared gestartet (Port {localPort})...");
            return true;
        }
        catch (Exception ex)
        {
            LogLine?.Invoke($"[tunnel] Startfehler: {ex.Message}");
            return false;
        }
    }

    // cloudflared gibt URLs aus wie:
    //   https://abc123.trycloudflare.com
    private static readonly Regex _urlRegex =
        new(@"https://[a-z0-9\-]+\.trycloudflare\.com", RegexOptions.Compiled);

    private void ParseLine(string line)
    {
        LogLine?.Invoke($"[tunnel] {line}");
        var m = _urlRegex.Match(line);
        if (m.Success)
        {
            _url = m.Value;
            UrlDiscovered?.Invoke(_url);
        }
    }

    public void Dispose()
    {
        if (_disposed) return;
        _disposed = true;
        try { _process?.Kill(); } catch { /* ignore */ }
        _process?.Dispose();
    }
}
