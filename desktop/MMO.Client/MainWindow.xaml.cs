using System.Diagnostics;
using System.IO;
using System.Net.Sockets;
using System.Security.Authentication;
using System.Text.Json;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using System.Windows.Threading;
using Mmo.Client.Core;

namespace Mmo.Client.Desktop;

public partial class MainWindow : Window
{
    private readonly string _dataDirectory;
    private readonly CredentialStore _credentials;
    private readonly DispatcherTimer _motionTimer = new() { Interval = TimeSpan.FromMilliseconds(100) };
    private readonly DispatcherTimer _pingTimer = new() { Interval = TimeSpan.FromSeconds(3) };
    private readonly HashSet<Key> _keys = [];
    private GameConnection? _connection;
    private Role? _role;
    private bool _busy, _loggedIn, _motionSending, _pingSending, _closing, _probeSucceeded;
    private double _buttonX, _buttonY;
    private bool _lastMoving;
    private string? _pendingTransferId, _pendingTarget;
    private readonly string? _screenshotPath, _uiCheckPath, _probeReportPath;

    public MainWindow(string[] args)
    {
        string? Arg(string key)
        {
            int i = Array.IndexOf(args, key);
            return i >= 0 && i + 1 < args.Length ? args[i + 1] : null;
        }
        _dataDirectory = Arg("--data-dir") ?? Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "MmoServerClient");
        _credentials = new CredentialStore(Path.Combine(_dataDirectory, "credentials"));
        _screenshotPath = Arg("--screenshot");
        _uiCheckPath = Arg("--ui-check-report");
        _probeReportPath = Arg("--probe-report");
        InitializeComponent();
        var settings = ClientSettings.Load(_dataDirectory);
        AddressBox.Text = Arg("--address") ?? settings.Address;
        ProfileBox.Text = Arg("--profile") ?? settings.ProfileName;
        TlsBox.IsChecked = args.Contains("--tls") || (!args.Contains("--plain") && settings.UseTls);
        AddressBox.TextChanged += (_, _) => { _probeSucceeded = false; UpdateControls(); };
        _motionTimer.Tick += async (_, _) => await SendMotionAsync();
        _pingTimer.Tick += async (_, _) => await PingAsync();
        _motionTimer.Start();
        _pingTimer.Start();
        UpdateControls();
        Log("已就绪。测试连接只发送 ping，不创建账号。角色凭据在本机加密保存。");
        Loaded += async (_, _) =>
        {
            if (_probeReportPath is not null) await RunProbeCheckAsync(_probeReportPath);
            else if (_uiCheckPath is not null) await RunUiCheckAsync(_uiCheckPath);
            else if (_screenshotPath is not null)
            {
                await Task.Delay(250);
                Capture(_screenshotPath);
                Close();
            }
        };
    }

    private ConnectionOptions Options() => new(AddressBox.Text.Trim(), TlsBox.IsChecked == true);
    private void SaveSettings() => new ClientSettings { Address = AddressBox.Text.Trim(), ProfileName = ProfileBox.Text.Trim(), UseTls = TlsBox.IsChecked == true }.Save(_dataDirectory);

    private void Log(string text)
    {
        if (_closing) return;
        LogBox.AppendText($"[{DateTime.Now:HH:mm:ss}] {text}\n");
        if (LogBox.Text.Length > 24000) LogBox.Text = LogBox.Text[^18000..];
        LogBox.ScrollToEnd();
    }

    private static string Explain(Exception exception)
    {
        if (exception is OperationCanceledException || exception is TimeoutException) return "连接或请求超时，请检查地址、端口和安全组。";
        if (exception is AuthenticationException) return "TLS 验证失败，请检查证书是否可信、域名是否匹配，以及服务器是否已启用 TLS。";
        if (exception is SocketException socket)
            return socket.SocketErrorCode switch
            {
                SocketError.TimedOut => "连接超时，请检查网关地址和 TCP 端口访问规则。",
                SocketError.ConnectionRefused => "连接被拒绝，请检查网关是否运行及端口是否正确。",
                SocketError.HostNotFound => "域名无法解析，请检查网关地址。",
                _ => "网络连接失败：" + socket.Message
            };
        return exception.Message;
    }

    private async Task RunActionAsync(Func<Task> action)
    {
        if (_busy) return;
        _busy = true; StopMotion(); UpdateControls();
        try { await action(); }
        catch (Exception ex) { Log("操作失败：" + Explain(ex)); }
        finally { _busy = false; UpdateControls(); }
    }

    private void UpdateControls()
    {
        if (ConnectButton is null) return;
        bool connected = _connection?.IsConnected == true;
        AddressBox.IsEnabled = ProfileBox.IsEnabled = TlsBox.IsEnabled = !connected && !_busy;
        ProbeButton.IsEnabled = !connected && !_busy;
        ConnectButton.IsEnabled = !connected && !_busy;
        DisconnectButton.IsEnabled = connected && !_busy;
        bool active = connected && _loggedIn && !_busy;
        foreach (var button in new[] { ClaimButton, RefreshButton, TransferButton, UpButton, DownButton, LeftButton, RightButton }) button.IsEnabled = active;
        TargetSceneBox.IsEnabled = active && _pendingTransferId is null;
        if (_loggedIn && connected)
        {
            StatusLabel.Text = "●  已登录";
            StatusLabel.Foreground = new SolidColorBrush(Color.FromRgb(74, 222, 172));
            StatusPlate.Background = new SolidColorBrush(Color.FromRgb(18, 55, 48));
        }
        else
        {
            StatusLabel.Text = _busy ? "●  正在处理" : _probeSucceeded ? "●  可连接 · 未登录" : "●  尚未连接";
            StatusLabel.Foreground = new SolidColorBrush(_probeSucceeded && !_busy ? Color.FromRgb(100, 168, 255) : Color.FromRgb(155, 174, 201));
            StatusPlate.Background = new SolidColorBrush(Color.FromRgb(29, 43, 66));
        }
    }

    private void Tls_Changed(object sender, RoutedEventArgs e)
    {
        if (ConnectionHint is null) return;
        _probeSucceeded = false;
        ConnectionHint.Text = TlsBox.IsChecked == true
            ? "TLS 会验证证书及域名。连接地址应与证书匹配；测试连接不会创建账号。"
            : "当前为明文 TCP 联调。公网启用 TLS 可保护登录凭据；测试连接只发送 ping。";
    }

    private async void Probe_Click(object sender, RoutedEventArgs e) => await RunActionAsync(ProbeAsync);
    private async Task ProbeAsync()
    {
        SaveSettings();
        var options = Options();
        Log("正在测试 " + options.Address + (options.UseTls ? "（TLS）" : "（TCP）") + " …");
        await using var probe = new GameConnection();
        var watch = Stopwatch.StartNew();
        await probe.ConnectAsync(options, CancellationToken.None);
        long connectMs = watch.ElapsedMilliseconds;
        watch.Restart();
        await probe.RequestAsync("ping");
        LatencyLabel.Text = watch.ElapsedMilliseconds + " ms";
        _probeSucceeded = true;
        Log($"连接测试成功：连接 {connectMs} ms，游戏协议 ping {watch.ElapsedMilliseconds} ms。尚未登录。");
    }

    private async void Connect_Click(object sender, RoutedEventArgs e) => await RunActionAsync(ConnectAndLoginAsync);
    private async Task ConnectAndLoginAsync()
    {
        string name = ProfileBox.Text.Trim();
        if (name.EnumerateRunes().Count() is < 1 or > 24 || name.Any(char.IsControl)) throw new InvalidOperationException("角色名需要 1～24 个字符，且不能包含控制字符。");
        var options = Options();
        SaveSettings();
        if (_connection is not null) await _connection.DisposeAsync();
        var conn = new GameConnection();
        _connection = conn;
        conn.MessageReceived += message => Dispatcher.BeginInvoke(() =>
        {
            if (!ReferenceEquals(_connection, conn) || _closing) return;
            try
            {
                if (message.Op == "state" && message.Ok) ApplyView(WorldView.FromJson(message.Data));
                else if (!message.Ok) Log("服务器：" + message.Error);
            }
            catch (Exception ex) when (ex is JsonException or InvalidOperationException or KeyNotFoundException or InvalidDataException or FormatException or OverflowException)
            {
                Log("服务器状态格式无效，已停止连接：" + Explain(ex));
                _ = DisconnectAsync();
            }
        });
        conn.Disconnected += reason => Dispatcher.BeginInvoke(() =>
        {
            if (!ReferenceEquals(_connection, conn) || _closing) return;
            _loggedIn = false; StopMotion(); ClearLiveState(); UpdateControls();
            Log("连接已断开：" + reason + "。可使用相同角色档案重新登录。");
        });
        try
        {
            Log("连接 " + options.Address + " …");
            await conn.ConnectAsync(options, CancellationToken.None);
            SavedCredential saved;
            if (!_credentials.TryLoad(options, name, out saved!))
            {
                using var reservation = _credentials.Reserve(options, name);
                var registered = await conn.RequestAsync("register", new Dictionary<string, object?> { ["name"] = name });
                string token = registered.Data.GetProperty("token").GetString() ?? throw new InvalidDataException("注册响应缺少凭据。");
                string roleId = registered.Data.GetProperty("role_id").GetString() ?? throw new InvalidDataException("注册响应缺少角色编号。");
                reservation.Save(token, roleId);
                saved = new SavedCredential(token, roleId);
                Log("角色已注册，登录凭据已加密保存到本机。");
            }
            else Log("已读取本机角色档案，正在登录。");
            var logged = await conn.RequestAsync("login", new Dictionary<string, object?> { ["token"] = saved.Token });
            _loggedIn = true;
            _pendingTransferId = _pendingTarget = null;
            TransferButton.Content = "切换场景";
            TransferHint.Text = "切换过程中会暂时停止移动。";
            ApplyRole(Role.FromJson(logged.Data));
            TargetSceneBox.SelectedIndex = _role!.SceneId == "scene-1" ? 1 : 0;
            await RefreshAsync();
            var latency = Stopwatch.StartNew();
            await conn.RequestAsync("ping");
            LatencyLabel.Text = latency.ElapsedMilliseconds + " ms";
            Log("登录成功。点击场景后可使用 WASD 或方向键移动。");
            World.Focus();
        }
        catch
        {
            _loggedIn = false;
            if (ReferenceEquals(_connection, conn)) _connection = null;
            await conn.DisposeAsync();
            throw;
        }
    }

    private void ApplyRole(Role role)
    {
        _role = role;
        RoleNameLabel.Text = role.Name;
        SceneLabel.Text = "场景  " + role.SceneId;
        PositionLabel.Text = $"坐标  {role.X:0.00}, {role.Y:0.00}";
        CoinsLabel.Text = role.Coins.ToString();
    }

    private void ApplyView(WorldView view)
    {
        ApplyRole(view.Self);
        World.Update(view);
        TickLabel.Text = $"{view.SceneId}  ·  Tick {view.Tick}  ·  可见 {view.Players.Count} 人";
    }

    private async void Refresh_Click(object sender, RoutedEventArgs e) => await RunActionAsync(RefreshAsync);
    private async Task RefreshAsync()
    {
        var conn = _connection ?? throw new InvalidOperationException("请先连接角色。");
        var reply = await conn.RequestAsync("view");
        ApplyView(WorldView.FromJson(reply.Data));
    }

    private async Task<bool> ClaimAsync()
    {
        var conn = _connection ?? throw new InvalidOperationException("请先连接角色。");
        var reply = await conn.RequestAsync("claim");
        long coins = reply.Data.GetProperty("coins").GetInt64();
        bool granted = reply.Data.GetProperty("granted").GetBoolean();
        CoinsLabel.Text = coins.ToString();
        Log(granted ? $"奖励领取成功，当前金币 {coins}。" : $"该角色已领取过奖励，当前金币 {coins}，未重复增加。");
        await RefreshAsync();
        return granted;
    }

    private async void Claim_Click(object sender, RoutedEventArgs e) => await RunActionAsync(async () => { await ClaimAsync(); });
    private async void Transfer_Click(object sender, RoutedEventArgs e) => await RunActionAsync(TransferAsync);
    private async Task TransferAsync()
    {
        var conn = _connection ?? throw new InvalidOperationException("请先连接角色。");
        string target = _pendingTarget ?? ((ComboBoxItem)TargetSceneBox.SelectedItem).Tag.ToString()!;
        _pendingTarget = target;
        _pendingTransferId ??= "desktop-" + Guid.NewGuid().ToString("N");
        TransferButton.Content = "重试同一次切换";
        TransferHint.Text = "重试复用同一编号；新切换请先断开重连。";
        Log("正在切换至 " + target + " …");
        var reply = await conn.RequestAsync("transfer", new Dictionary<string, object?> { ["target_scene_id"] = target, ["request_id"] = _pendingTransferId });
        ApplyRole(Role.FromJson(reply.Data));
        _pendingTransferId = _pendingTarget = null;
        TargetSceneBox.SelectedIndex = _role!.SceneId == "scene-1" ? 1 : 0;
        TransferButton.Content = "切换场景";
        TransferHint.Text = "切换成功。可继续移动或返回原场景。";
        await RefreshAsync();
        Log("已进入 " + target + "。");
    }

    private async void Disconnect_Click(object sender, RoutedEventArgs e) => await RunActionAsync(DisconnectAsync);
    private async Task DisconnectAsync()
    {
        StopMotion();
        var conn = _connection;
        _connection = null; _loggedIn = false; ClearLiveState();
        if (conn is not null) await conn.DisposeAsync();
        Log("已断开。本机角色凭据仍保留。");
        UpdateControls();
    }

    private void StopMotion()
    {
        _keys.Clear(); _buttonX = _buttonY = 0;
    }

    private void ClearLiveState()
    {
        _role = null; World.Update(null);
        RoleNameLabel.Text = "尚未登录"; SceneLabel.Text = "场景  —";
        PositionLabel.Text = "坐标  —"; CoinsLabel.Text = LatencyLabel.Text = "—";
        TickLabel.Text = "等待服务器状态";
    }

    private async Task SendMotionAsync()
    {
        var conn = _connection;
        if (_motionSending || conn?.IsConnected != true || !_loggedIn) return;
        double x = _buttonX, y = _buttonY;
        if (_keys.Contains(Key.A) || _keys.Contains(Key.Left)) x -= 1;
        if (_keys.Contains(Key.D) || _keys.Contains(Key.Right)) x += 1;
        if (_keys.Contains(Key.W) || _keys.Contains(Key.Up)) y += 1;
        if (_keys.Contains(Key.S) || _keys.Contains(Key.Down)) y -= 1;
        x = Math.Clamp(x, -1, 1); y = Math.Clamp(y, -1, 1);
        if (_busy) x = y = 0;
        bool moving = x != 0 || y != 0;
        if (!moving && !_lastMoving) return;
        _lastMoving = moving; _motionSending = true;
        try
        {
            await conn.RequestAsync("move", new Dictionary<string, object?> { ["seq"] = conn.NextMoveSequence(), ["dx"] = x, ["dy"] = y });
        }
        catch (Exception ex) { StopMotion(); _lastMoving = false; Log("移动失败：" + Explain(ex)); }
        finally { _motionSending = false; }
    }

    private async Task PingAsync()
    {
        var conn = _connection;
        if (_pingSending || conn?.IsConnected != true || !_loggedIn || _busy) return;
        _pingSending = true;
        try
        {
            var watch = Stopwatch.StartNew();
            await conn.RequestAsync("ping");
            if (ReferenceEquals(_connection, conn)) LatencyLabel.Text = watch.ElapsedMilliseconds + " ms";
        }
        catch (Exception ex) { Log("延迟测试失败：" + Explain(ex)); }
        finally { _pingSending = false; }
    }

    private void Move_Down(object sender, MouseButtonEventArgs e)
    {
        if (!_loggedIn || _busy) return;
        var button = (Button)sender;
        var direction = ((string)button.Tag).Split(',');
        _buttonX = double.Parse(direction[0]); _buttonY = double.Parse(direction[1]);
        button.CaptureMouse(); e.Handled = true;
    }
    private void Move_Up(object sender, MouseButtonEventArgs e) { StopMotion(); ((Button)sender).ReleaseMouseCapture(); e.Handled = true; }
    private void Move_LostCapture(object sender, MouseEventArgs e) => StopMotion();
    private void World_MouseDown(object sender, MouseButtonEventArgs e) => World.Focus();
    private static bool MovementKey(Key key) => key is Key.W or Key.A or Key.S or Key.D or Key.Up or Key.Left or Key.Down or Key.Right;
    private void Window_KeyDown(object sender, KeyEventArgs e)
    {
        if (!_loggedIn || _busy || !World.IsKeyboardFocusWithin) return;
        if (MovementKey(e.Key)) { _keys.Add(e.Key); e.Handled = true; }
    }
    private void Window_KeyUp(object sender, KeyEventArgs e) { if (MovementKey(e.Key)) { _keys.Remove(e.Key); e.Handled = true; } }
    private void Window_Deactivated(object? sender, EventArgs e) => StopMotion();

    private void Window_Closing(object? sender, System.ComponentModel.CancelEventArgs e)
    {
        _closing = true; _motionTimer.Stop(); _pingTimer.Stop(); StopMotion();
        try { SaveSettings(); } catch { }
        var conn = _connection; _connection = null;
        if (conn is not null) _ = conn.DisposeAsync();
    }

    private void Capture(string path)
    {
        UpdateLayout();
        var surface = (FrameworkElement)Content;
        int width = (int)Math.Ceiling(surface.ActualWidth), height = (int)Math.Ceiling(surface.ActualHeight);
        var visual = new DrawingVisual();
        using (var dc = visual.RenderOpen())
        {
            dc.DrawRectangle(Background, null, new Rect(0, 0, width, height));
            dc.DrawRectangle(new VisualBrush(surface), null, new Rect(0, 0, width, height));
        }
        var bitmap = new RenderTargetBitmap(width, height, 96, 96, PixelFormats.Pbgra32);
        bitmap.Render(visual);
        var encoder = new PngBitmapEncoder(); encoder.Frames.Add(BitmapFrame.Create(bitmap));
        Directory.CreateDirectory(Path.GetDirectoryName(Path.GetFullPath(path))!);
        using var file = File.Create(path); encoder.Save(file);
    }

    private async Task RunUiCheckAsync(string path)
    {
        var results = new List<string>();
        try
        {
            if (!AddressBox.Text.StartsWith("127.0.0.1:", StringComparison.Ordinal)) throw new InvalidOperationException("界面自动验收只允许回环测试服务。");
            _busy = true; UpdateControls();
            await ProbeAsync(); results.Add("界面连接测试与协议 ping");
            await ConnectAndLoginAsync();
            string id = _role!.RoleId; results.Add("注册、加密凭据与登录界面");
            double startX = _role.X;
            _busy = false; _buttonX = 1;
            await Task.Delay(800); StopMotion(); await Task.Delay(450); await RefreshAsync();
            if (_role!.X <= startX) throw new InvalidOperationException("界面持续移动未改变坐标。");
            results.Add("方向输入与场景推送更新坐标");
            _busy = true;
            bool first = await ClaimAsync(), second = await ClaimAsync();
            if (!first || second || _role!.Coins != 100) throw new InvalidOperationException("界面奖励幂等检查失败。");
            results.Add("奖励领取与重复领取界面");
            TargetSceneBox.SelectedIndex = 1; await TransferAsync();
            if (_role!.SceneId != "scene-2") throw new InvalidOperationException("界面切换场景检查失败。");
            results.Add("切换场景与角色面板更新");
            await DisconnectAsync(); await ConnectAndLoginAsync();
            if (_role!.RoleId != id || _role.SceneId != "scene-2" || _role.Coins != 100) throw new InvalidOperationException("界面重连身份或状态不匹配。");
            results.Add("原凭据重连及身份、金币和场景保持");
            _busy = false; UpdateControls();
            await Task.Delay(350); Capture(Path.ChangeExtension(path, ".png"));
            File.WriteAllText(path, JsonSerializer.Serialize(new { ok = true, checks = results, credentials_not_logged = true }, new JsonSerializerOptions { WriteIndented = true }));
        }
        catch (Exception ex)
        {
            Directory.CreateDirectory(Path.GetDirectoryName(Path.GetFullPath(path))!);
            File.WriteAllText(path, JsonSerializer.Serialize(new { ok = false, checks = results, error = Explain(ex) }, new JsonSerializerOptions { WriteIndented = true }));
        }
        finally { Close(); }
    }

    private async Task RunProbeCheckAsync(string path)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(Path.GetFullPath(path))!);
        try
        {
            _busy = true; UpdateControls();
            await ProbeAsync();
            _busy = false; UpdateControls();
            await Task.Delay(150);
            Capture(Path.ChangeExtension(path, ".png"));
            File.WriteAllText(path, JsonSerializer.Serialize(new { ok = true, target = AddressBox.Text, tls = TlsBox.IsChecked == true, protocol_ping = LatencyLabel.Text, account_created = false }, new JsonSerializerOptions { WriteIndented = true }));
        }
        catch (Exception ex)
        {
            File.WriteAllText(path, JsonSerializer.Serialize(new { ok = false, target = AddressBox.Text, error = Explain(ex), account_created = false }, new JsonSerializerOptions { WriteIndented = true }));
        }
        finally { Close(); }
    }
}
