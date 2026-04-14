using System.IO;
using System.Net.Http;
using System.Text;
using System.Text.Json;
using System.Windows;
using System.Windows.Media.Imaging;
using System.Windows.Threading;
using QRCoder;

namespace StallCamServer;

// ─── Konfiguration (hier anpassen) ────────────────────────────────────
file static class Config
{
    public const string RelaySecret  = "Ji8e83HUhr24hgU$§Pou";      // ← muss mit ESP32 übereinstimmen
    public const string WebUser      = "Ludwig";           // ← Browser-Login Benutzername
    public const string WebPass      = "!blacky14/02$";     // ← Browser-Login Passwort
    public const string BrokerUrl = "https://stallcam-broker.onrender.com"; // ← Render.com URL, z.B. "https://stallcam-broker.onrender.com"
    public const string BrokerSecret = "broker-geheim123"; // ← muss mit Broker übereinstimmen
    public const int    LocalPort    = 8080;              // ← Port des lokalen Web-Servers
}

// ─── Hauptfenster ─────────────────────────────────────────────────────
public partial class MainWindow : Window
{
    private readonly RelayServer         _relay;
    private readonly BoreTunnel          _bore;
    private readonly CancellationTokenSource _cts = new();
    private readonly DispatcherTimer     _uiTimer;
    private readonly HttpClient          _http    = new();

    private int  _frameCount;
    private int  _fps;

    public MainWindow()
    {
        InitializeComponent();

        _relay = new RelayServer(Config.RelaySecret, Config.WebUser, Config.WebPass);
        _bore  = new BoreTunnel();

        // Relay-Events → UI
        _relay.FrameReceived     += OnFrameReceived;
        _relay.StatusUpdated     += OnStatusUpdated;
        _relay.VideoListReceived += OnVideoListReceived;
        _relay.LogLine           += AppendLog;

        // Bore-Events
        _bore.LogLine       += AppendLog;
        _bore.UrlDiscovered += OnBoreUrlDiscovered;

        // UI-Refresh-Timer (alle 500ms)
        _uiTimer = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(500) };
        _uiTimer.Tick += UiTimer_Tick;
        _uiTimer.Start();

        // Server + bore starten
        _ = Task.Run(() => _relay.RunAsync(Config.LocalPort, _cts.Token));
        bool boreOk = _bore.Start(Config.LocalPort);
        if (!boreOk)
        {
            TxtBoreUrl.Text    = "cloudflared.exe nicht gefunden! Siehe Log.";
            TxtBoreBadge.Text  = "tunnel: fehlt";
        }

        AppendLog($"[app] Server gestartet – lokal: http://localhost:{Config.LocalPort}");
        AppendLog($"[app] Browser-Login: {Config.WebUser} / {Config.WebPass}");
    }

    // ─────────────────────────────────────────────────────────────────
    // bore-URL verfügbar → Broker registrieren + QR-Code zeigen
    // ─────────────────────────────────────────────────────────────────
    private async void OnBoreUrlDiscovered(string url)
    {
        // cloudflared gibt bereits die vollständige https://-URL zurück
        Dispatcher.Invoke(() =>
        {
            TxtBoreUrl.Text   = url;
            TxtBoreBadge.Text = $"tunnel: aktiv";
            UpdateQrCode(url);
            AppendLog($"[tunnel] Öffentliche URL: {url}");
        });

        // Beim Broker registrieren – host:port aus URL extrahieren
        // cloudflared URL: https://abc123.trycloudflare.com (Port 443 implizit)
        var hostForBroker = url.Replace("https://", "").Replace("http://", "").TrimEnd('/');
        await RegisterWithBrokerAsync(hostForBroker);
    }

    private async Task RegisterWithBrokerAsync(string boreUrl)
    {
        if (string.IsNullOrEmpty(Config.BrokerUrl)) return;
        try
        {
            var payload = JsonSerializer.Serialize(new
            {
                secret = Config.BrokerSecret,
                url    = boreUrl
            });
            var res = await _http.PostAsync(
                Config.BrokerUrl.TrimEnd('/') + "/register",
                new StringContent(payload, Encoding.UTF8, "application/json"),
                _cts.Token);
            AppendLog(res.IsSuccessStatusCode
                ? "[broker] Registrierung erfolgreich"
                : $"[broker] Fehler: {res.StatusCode}");

            // Alle 5 Minuten neu registrieren (hält Broker & bore am Leben)
            _ = Task.Run(async () =>
            {
                while (!_cts.Token.IsCancellationRequested)
                {
                    await Task.Delay(TimeSpan.FromMinutes(5), _cts.Token);
                    if (_bore.Url is { Length: > 0 } u)
                        await RegisterWithBrokerAsync(u.Replace("https://", "").Replace("http://", "").TrimEnd('/'));
                }
            }, _cts.Token);
        }
        catch (Exception ex) { AppendLog($"[broker] Verbindungsfehler: {ex.Message}"); }
    }

    // ─────────────────────────────────────────────────────────────────
    // Kamera-Frame → WPF-Bild aktualisieren
    // ─────────────────────────────────────────────────────────────────
    private void OnFrameReceived(byte[] data)
    {
        _frameCount++;
        Dispatcher.InvokeAsync(() =>
        {
            try
            {
                using var ms  = new MemoryStream(data);
                var bmp       = new BitmapImage();
                bmp.BeginInit();
                bmp.StreamSource  = ms;
                bmp.CacheOption   = BitmapCacheOption.OnLoad;
                bmp.EndInit();
                bmp.Freeze();
                StreamImage.Source  = bmp;
                TxtOffline.Visibility = Visibility.Collapsed;
            }
            catch { /* ungültiger Frame – ignorieren */ }
        }, DispatcherPriority.Render);
    }

    // ─────────────────────────────────────────────────────────────────
    // Status-Update vom ESP32
    // ─────────────────────────────────────────────────────────────────
    private void OnStatusUpdated(StatusData s)
    {
        Dispatcher.Invoke(() =>
        {
            TxtEsp32Badge.Text      = "● ESP32: online";
            TxtEsp32Badge.Foreground = System.Windows.Media.Brushes.LightGreen;
            TxtOffline.Visibility   = Visibility.Collapsed;

            TxtVideos.Text   = s.VideoCount.ToString();
            TxtSdUsed.Text   = FormatBytes(s.SdUsed);
            TxtSdTotal.Text  = FormatBytes(s.SdTotal);
            TxtInterval.Text = $"{s.Interval} min";
            TxtDuration.Text = $"{s.Duration} sek";

            // SD-Füllbalken
            double pct = s.SdTotal > 0 ? (double)s.SdUsed / s.SdTotal : 0;
            var total   = ((System.Windows.Controls.Border)SdBar.Parent).ActualWidth;
            SdBar.Width = total * pct;
            SdBar.Background = pct > 0.9
                ? System.Windows.Media.Brushes.Tomato
                : pct > 0.7
                    ? System.Windows.Media.Brushes.Orange
                    : System.Windows.Media.Brushes.LimeGreen;

            // Einstellungen synchronisieren (nur wenn Slider nicht gerade bedient wird)
            SliderInterval.Value = s.Interval;
            SliderDuration.Value = s.Duration;

            // Auflösung-Combobox
            foreach (System.Windows.Controls.ComboBoxItem item in CboResolution.Items)
                if (item.Tag?.ToString() == s.CamRes.ToString())
                    item.IsSelected = true;

            ChkNight.IsChecked  = s.NightMode;
            ChkIrAuto.IsChecked = s.IrAuto;
        });
    }

    // ─────────────────────────────────────────────────────────────────
    // Video-Liste vom ESP32
    // ─────────────────────────────────────────────────────────────────
    private void OnVideoListReceived(string json)
    {
        try
        {
            using var doc  = JsonDocument.Parse(json);
            var files      = doc.RootElement.GetProperty("files");
            var sb         = new StringBuilder();
            foreach (var f in files.EnumerateArray())
            {
                var name = f.GetProperty("name").GetString() ?? "";
                var size = f.GetProperty("size").GetInt64();
                sb.AppendLine($"{name}  ({FormatBytes(size)})");
            }
            Dispatcher.Invoke(() =>
                TxtVideoList.Text = sb.Length > 0 ? sb.ToString() : "Keine Videos vorhanden");
        }
        catch (Exception ex)
        {
            Dispatcher.Invoke(() => TxtVideoList.Text = $"Fehler: {ex.Message}");
        }
    }

    // ─────────────────────────────────────────────────────────────────
    // 500ms-Timer: ESP32-Status-Badge + FPS
    // ─────────────────────────────────────────────────────────────────
    private DateTime _lastFpsCheck = DateTime.Now;
    private void UiTimer_Tick(object? sender, EventArgs e)
    {
        // ESP32-Badge
        if (!_relay.IsEsp32Connected)
        {
            TxtEsp32Badge.Text       = "● ESP32: offline";
            TxtEsp32Badge.Foreground = System.Windows.Media.Brushes.Tomato;
            TxtOffline.Visibility    = Visibility.Visible;
        }

        // Zuschauer-Anzahl
        TxtViewerBadge.Text = $"Zuschauer: {_relay.ViewerCount}";

        // FPS berechnen
        var now   = DateTime.Now;
        var dt    = (now - _lastFpsCheck).TotalSeconds;
        _fps      = (int)(_frameCount / Math.Max(dt, 0.001));
        _frameCount    = 0;
        _lastFpsCheck  = now;
        if (_relay.IsEsp32Connected)
            TxtLiveBadge.Text = $"● LIVE  {_fps} fps";
    }

    // ─────────────────────────────────────────────────────────────────
    // QR-Code generieren
    // ─────────────────────────────────────────────────────────────────
    private void UpdateQrCode(string url)
    {
        try
        {
            using var gen  = new QRCodeGenerator();
            var data       = gen.CreateQrCode(url, QRCodeGenerator.ECCLevel.Q);
            using var code = new BitmapByteQRCode(data);
            var bmpBytes   = code.GetGraphic(8, "#3FB950", "#0D1117");

            using var ms   = new MemoryStream(bmpBytes);
            var img        = new BitmapImage();
            img.BeginInit();
            img.StreamSource  = ms;
            img.CacheOption   = BitmapCacheOption.OnLoad;
            img.EndInit();
            img.Freeze();
            QrImage.Source = img;
        }
        catch (Exception ex) { AppendLog($"[qr] Fehler: {ex.Message}"); }
    }

    // ─────────────────────────────────────────────────────────────────
    // UI-Events
    // ─────────────────────────────────────────────────────────────────
    private void SliderInterval_ValueChanged(object s, System.Windows.RoutedPropertyChangedEventArgs<double> e)
        => RunInterval.Text = ((int)e.NewValue).ToString();

    private void SliderDuration_ValueChanged(object s, System.Windows.RoutedPropertyChangedEventArgs<double> e)
        => RunDuration.Text = ((int)e.NewValue).ToString();

    private async void BtnSave_Click(object s, RoutedEventArgs e)
    {
        var res = (System.Windows.Controls.ComboBoxItem)CboResolution.SelectedItem;
        var cmd = JsonSerializer.Serialize(new
        {
            cmd       = "save_settings",
            interval  = (int)SliderInterval.Value,
            duration  = (int)SliderDuration.Value,
            camRes    = int.Parse(res?.Tag?.ToString() ?? "5"),
            nightMode = ChkNight.IsChecked == true,
            irAuto    = ChkIrAuto.IsChecked == true,
        });
        await _relay.SendCommandAsync(cmd);
        AppendLog("[gui] Einstellungen gespeichert");
    }

    private async void BtnRecord_Click(object s, RoutedEventArgs e)
    {
        await _relay.SendCommandAsync("""{"cmd":"record"}""");
        AppendLog("[gui] Aufnahme ausgelöst");
    }

    private async void BtnLoadVideos_Click(object s, RoutedEventArgs e)
    {
        await _relay.SendCommandAsync("""{"cmd":"get_videos"}""");
    }

    private async void BtnDeleteAll_Click(object s, RoutedEventArgs e)
    {
        if (MessageBox.Show("Alle Videos auf der SD-Karte löschen?",
            "StallCam", MessageBoxButton.YesNo, MessageBoxImage.Warning) == MessageBoxResult.Yes)
        {
            await _relay.SendCommandAsync("""{"cmd":"delete_all"}""");
            AppendLog("[gui] Alle Videos gelöscht");
        }
    }

    private void BtnCopyUrl_Click(object s, RoutedEventArgs e)
    {
        var url = TxtBoreUrl.Text;
        if (!string.IsNullOrEmpty(url))
        {
            Clipboard.SetText(url);
            AppendLog($"[app] URL kopiert: {url}");
        }
    }

    private void Window_Closing(object s, System.ComponentModel.CancelEventArgs e)
    {
        _cts.Cancel();
        _bore.Dispose();
    }

    // ─────────────────────────────────────────────────────────────────
    // Log
    // ─────────────────────────────────────────────────────────────────
    private void AppendLog(string line)
    {
        Dispatcher.InvokeAsync(() =>
        {
            var ts = DateTime.Now.ToString("HH:mm:ss");
            TxtLog.Text = (TxtLog.Text + $"\n{ts}  {line}").TrimStart('\n');
            // Maximale Log-Länge begrenzen
            var lines = TxtLog.Text.Split('\n');
            if (lines.Length > 80)
                TxtLog.Text = string.Join('\n', lines[^80..]);
            LogScroller.ScrollToBottom();
        }, DispatcherPriority.Background);
    }

    // ─────────────────────────────────────────────────────────────────
    // Hilfsmethode: Bytes formatieren
    // ─────────────────────────────────────────────────────────────────
    private static string FormatBytes(long b) =>
        b > 1_000_000_000 ? $"{b / 1e9:F1} GB" :
        b > 1_000_000     ? $"{b / 1e6:F0} MB" :
        b > 1_000         ? $"{b / 1e3:F0} KB" : $"{b} B";
}
