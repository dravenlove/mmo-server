using System.Diagnostics;
using System.Net;
using System.Net.Security;
using System.Net.Sockets;
using System.Security.Authentication;
using System.Security.Cryptography;
using System.Security.Cryptography.X509Certificates;
using System.Text;
using System.Text.Json;
using Mmo.Client.Core;

// 这些检查只连接脚本新建的回环 fixture，不读取用户实际部署配置。
internal static class Program
{
    private static readonly List<string> Passed = [];
    private static readonly List<string> Secrets = [];

    private static async Task<int> Main(string[] args)
    {
        var arguments = new Dictionary<string, string>();
        for (var i = 0; i + 1 < args.Length; i += 2)
            arguments[args[i]] = args[i + 1];
        var address = arguments["--addr"];
        var directory = arguments["--work"];
        var report = arguments["--report"];
        var host = address[..address.LastIndexOf(':')];
        if (host != "127.0.0.1")
            throw new ArgumentException("自动检查仅允许回环地址。");
        var errors = new List<string>();
        try
        {
            await CheckRealServer(address, directory);
            await CheckPendingDisconnect();
            await CheckRemoteDisconnect();
            await CheckFragmentedUtf8();
            await CheckOversizedFrame();
            await CheckUntrustedTls();
        }
        catch (Exception exception)
        {
            var safe = exception.ToString();
            foreach (var secret in Secrets)
                safe = safe.Replace(secret, "[redacted]", StringComparison.Ordinal);
            errors.Add(safe);
            Console.Error.WriteLine(safe);
        }
        var result = new
        {
            success = errors.Count == 0,
            checks = Passed,
            errors,
            address,
            finished_utc = DateTimeOffset.UtcNow,
            external_services_contacted = false
        };
        await File.WriteAllTextAsync(report, JsonSerializer.Serialize(result,
            new JsonSerializerOptions { WriteIndented = true }));
        return errors.Count == 0 ? 0 : 1;
    }

    private static async Task CheckRealServer(string address, string directory)
    {
        var options = new ConnectionOptions(address);
        var name = "desktop-" + Guid.NewGuid().ToString("N")[..12];
        var storePath = Path.Combine(directory, "credentials");
        var store = new CredentialStore(storePath);

        using (store.Reserve(options, name))
        {
            var rejected = false;
            try
            {
                using var second = store.Reserve(options, name);
            }
            catch (IOException) { rejected = true; }
            Require(rejected, "同一凭据路径的并发注册预留应被拒绝。");
        }
        Require(!store.TryLoad(options, name, out _), "取消预留不应留下可登录凭据。");
        Pass("exclusive credential reservation and cancelled reservation cleanup");

        await using var first = new GameConnection();
        await first.ConnectAsync(options);
        var probe = await first.RequestAsync("ping");
        Require(probe.Ok && probe.Data.GetProperty("unix_ms").GetInt64() > 0,
            "TCP 探测必须收到真正的游戏 ping 响应。");
        Pass("unauthenticated protocol ping");

        var stateReceived = new TaskCompletionSource<ServerMessage>(TaskCreationOptions.RunContinuationsAsynchronously);
        var sensitiveBroadcast = 0;
        first.MessageReceived += message =>
        {
            if (message.Op == "register" || (message.Data.ValueKind == JsonValueKind.Object && message.Data.TryGetProperty("token", out _)))
                Interlocked.Exchange(ref sensitiveBroadcast, 1);
            if (message.Op == "state" && message.Ok)
                stateReceived.TrySetResult(message);
        };
        string token;
        string roleId;
        using (var reservation = store.Reserve(options, name))
        {
            var registration = await first.RequestAsync("register", Fields(("name", name)));
            token = registration.Data.GetProperty("token").GetString()!;
            roleId = registration.Data.GetProperty("role_id").GetString()!;
            Secrets.Add(token);
            reservation.Save(token, roleId);
        }
        Require(store.TryLoad(options, name, out var saved) && saved.Token == token && saved.RoleId == roleId,
            "保存后应能恢复同一个角色凭据。");
        Require(Directory.GetFiles(storePath, "*", SearchOption.AllDirectories)
            .All(file => !Encoding.UTF8.GetString(File.ReadAllBytes(file)).Contains(token, StringComparison.Ordinal)),
            "凭据文件不能保存明文 token。");
        Require(!store.TryLoad(new ConnectionOptions(address, true), name, out _),
            "不同传输模式不能共享凭据命名空间。");
        Require(sensitiveBroadcast == 0, "注册凭据不能通过广播事件泄露到 UI 日志。");
        Pass("encrypted credential persistence and endpoint isolation");

        var corruptPath = Path.Combine(directory, "corrupt-credentials");
        var corruptStore = new CredentialStore(corruptPath);
        using (var reserved = corruptStore.Reserve(options, name))
            reserved.Save(token, roleId);
        var credentialFile = Directory.GetFiles(corruptPath, "*", SearchOption.AllDirectories)
            .Single(file => new FileInfo(file).Length > 0);
        await File.WriteAllTextAsync(credentialFile, "corrupted credential fixture");
        var corruptRejected = false;
        try { corruptStore.TryLoad(options, name, out _); }
        catch (InvalidDataException) { corruptRejected = true; }
        Require(corruptRejected, "损坏凭据不能静默当成新角色注册。");
        Pass("corrupted credentials fail closed");

        var login = Role.FromJson((await first.RequestAsync("login", Fields(("token", token)))).Data);
        Require(login.RoleId == roleId && login.SceneId == "scene-1", "新角色应登录到 scene-1。");
        var initial = WorldView.FromJson((await first.RequestAsync("view")).Data);
        Require(initial.Self.RoleId == roleId && initial.Players.Any(role => role.RoleId == roleId),
            "视图应包含当前角色。");
        var pushed = await stateReceived.Task.WaitAsync(TimeSpan.FromSeconds(3));
        Require(WorldView.FromJson(pushed.Data).Self.RoleId == roleId, "异步推送应保留正确角色。");
        Pass("login, explicit view and asynchronous world push");

        var pings = await Task.WhenAll(Enumerable.Range(0, 5).Select(_ => first.RequestAsync("ping")));
        Require(pings.All(reply => reply.Ok) && pings.Select(reply => reply.Id).Distinct().Count() == 5,
            "并发请求必须按 id 分发响应。");
        Pass("concurrent request correlation while world pushes arrive");

        for (var i = 0; i < 8; i++)
        {
            await first.RequestAsync("move", Fields(("seq", first.NextMoveSequence()), ("dx", 1d), ("dy", 0d)));
            await Task.Delay(90);
        }
        var moved = WorldView.FromJson((await first.RequestAsync("view")).Data);
        Require(moved.Self.X > initial.Self.X + 1, "连续方向输入应推进权威场景位置。");
        await Task.Delay(500);
        var stopped = WorldView.FromJson((await first.RequestAsync("view")).Data);
        await Task.Delay(350);
        var still = WorldView.FromJson((await first.RequestAsync("view")).Data);
        Require(Math.Abs(stopped.Self.X - still.Self.X) < 0.15, "输入超时后角色应自动停止。");
        Pass("continuous authoritative movement and input timeout stop");

        var claim1 = await first.RequestAsync("claim");
        var claim2 = await first.RequestAsync("claim");
        Require(claim1.Data.GetProperty("granted").GetBoolean() &&
                !claim2.Data.GetProperty("granted").GetBoolean() &&
                claim2.Data.GetProperty("coins").GetInt64() == 100,
            "首次领取与重复领取必须保持 100 金币。");
        Pass("reward claim idempotence");

        var requestId = "desktop-check-" + Guid.NewGuid().ToString("N");
        var transfer = Fields(("target_scene_id", "scene-2"), ("request_id", requestId));
        var transferred = Role.FromJson((await first.RequestAsync("transfer", transfer)).Data);
        var retried = Role.FromJson((await first.RequestAsync("transfer", transfer)).Data);
        Require(transferred.SceneId == "scene-2" && retried.SceneId == "scene-2" &&
                transferred.OwnerEpoch == retried.OwnerEpoch && transferred.OwnerEpoch > login.OwnerEpoch,
            "同 request_id 的迁移重试不能重复推进归属代次。");
        Pass("scene transfer and stable request id retry");

        var duplicateRejected = false;
        try { await first.RequestAsync("register", Fields(("name", name))); }
        catch (GameRequestException) { duplicateRejected = true; }
        Require(duplicateRejected && (await first.RequestAsync("ping")).Ok,
            "业务错误不能破坏后续合法请求。");
        Pass("business error propagation preserves connection");

        await first.DisconnectAsync();
        await using var reconnect = new GameConnection();
        await reconnect.ConnectAsync(options);
        Require(store.TryLoad(options, name, out var restored), "重新登录前应仍能读取凭据。");
        var rejoined = Role.FromJson((await reconnect.RequestAsync("login", Fields(("token", restored.Token)))).Data);
        var finalClaim = await reconnect.RequestAsync("claim");
        Require(rejoined.RoleId == roleId && rejoined.SceneId == "scene-2" &&
                finalClaim.Data.GetProperty("coins").GetInt64() == 100 &&
                !finalClaim.Data.GetProperty("granted").GetBoolean(),
            "重连应恢复同一角色、场景和奖励领取状态。");
        Require(reconnect.NextMoveSequence() == 1, "新连接的移动序号必须从 1 开始。");
        Pass("reconnect with persisted identity, scene and reward state");
    }

    private static async Task CheckPendingDisconnect()
    {
        using var listener = Listen();
        var received = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var server = Task.Run(async () =>
        {
            using var client = await listener.AcceptTcpClientAsync();
            using var reader = new StreamReader(client.GetStream(), Encoding.UTF8, leaveOpen: true);
            await reader.ReadLineAsync();
            received.TrySetResult();
            using var deadline = new CancellationTokenSource(TimeSpan.FromSeconds(5));
            try
            {
                var read = await client.GetStream().ReadAsync(new byte[1], deadline.Token);
                Require(read == 0, "断开 fixture 不应收到额外请求。");
            }
            catch (Exception error) when (error is IOException or OperationCanceledException) { }
        });
        await using var connection = new GameConnection();
        await connection.ConnectAsync(Endpoint(listener));
        var pending = connection.RequestAsync("ping");
        await received.Task.WaitAsync(TimeSpan.FromSeconds(2));
        var timer = Stopwatch.StartNew();
        await connection.DisconnectAsync().WaitAsync(TimeSpan.FromSeconds(2));
        await MustFailSoon(pending, "主动断开后 pending 请求必须快速失败。");
        Require(timer.Elapsed < TimeSpan.FromSeconds(2), "断开不能等待完整请求超时。");
        await server;
        Pass("disconnect immediately fails outstanding requests");
    }

    private static async Task CheckRemoteDisconnect()
    {
        using var listener = Listen();
        var server = Task.Run(async () =>
        {
            using var client = await listener.AcceptTcpClientAsync();
            using var reader = new StreamReader(client.GetStream(), Encoding.UTF8, leaveOpen: true);
            await reader.ReadLineAsync();
        });
        await using var connection = new GameConnection();
        await connection.ConnectAsync(Endpoint(listener));
        await MustFailSoon(connection.RequestAsync("ping"), "远端断开后 pending 请求必须快速失败。");
        Require(!connection.IsConnected, "远端断开应更新连接状态。");
        await server;
        Pass("remote EOF immediately fails outstanding requests");
    }

    private static async Task CheckFragmentedUtf8()
    {
        using var listener = Listen();
        var server = Task.Run(async () =>
        {
            using var client = await listener.AcceptTcpClientAsync();
            await using var stream = client.GetStream();
            using var reader = new StreamReader(stream, Encoding.UTF8, leaveOpen: true);
            using var document = JsonDocument.Parse((await reader.ReadLineAsync())!);
            var id = document.RootElement.GetProperty("id").GetString();
            var bytes = Encoding.UTF8.GetBytes(JsonSerializer.Serialize(new
            {
                id, op = "ping", ok = true, data = new { message = "场景🌟" }
            }, new JsonSerializerOptions { Encoder = System.Text.Encodings.Web.JavaScriptEncoder.UnsafeRelaxedJsonEscaping }) + "\n");
            for (var i = 0; i < bytes.Length; i += 2)
            {
                await stream.WriteAsync(bytes.AsMemory(i, Math.Min(2, bytes.Length - i)));
                await Task.Delay(1);
            }
        });
        await using var connection = new GameConnection();
        await connection.ConnectAsync(Endpoint(listener));
        var reply = await connection.RequestAsync("ping");
        Require(reply.Data.GetProperty("message").GetString() == "场景🌟", "拆包后 UTF-8 字符不应损坏。");
        await server;
        Pass("fragmented UTF-8 JSON line decoding");
    }

    private static async Task CheckOversizedFrame()
    {
        using var listener = Listen();
        var server = Task.Run(async () =>
        {
            using var client = await listener.AcceptTcpClientAsync();
            await using var stream = client.GetStream();
            using var reader = new StreamReader(stream, Encoding.UTF8, leaveOpen: true);
            await reader.ReadLineAsync();
            var payload = Encoding.UTF8.GetBytes("{\"op\":\"state\",\"ok\":true,\"data\":{\"padding\":\"" + new string('a', 70_000) + "\"}}\n");
            try { await stream.WriteAsync(payload); }
            catch (IOException) { }
            // 服务端保持连接，确保检查不会把服务端主动关闭误判为超长帧拒绝。
            using var deadline = new CancellationTokenSource(TimeSpan.FromSeconds(4));
            try
            {
                var read = await stream.ReadAsync(new byte[1], deadline.Token);
                Require(read == 0, "超长帧 fixture 不应收到额外请求。");
            }
            catch (Exception error) when (error is IOException or OperationCanceledException) { }
        });
        await using var connection = new GameConnection();
        await connection.ConnectAsync(Endpoint(listener));
        await MustFailSoon(connection.RequestAsync("ping"), "超过 64KiB 的输入帧应关闭连接并终止请求。");
        Require(!connection.IsConnected, "超长帧不能保留可写连接。");
        await server;
        Pass("oversized JSON line rejection");
    }

    private static async Task CheckUntrustedTls()
    {
        using var listener = Listen();
        using var rsa = RSA.Create(2048);
        var request = new CertificateRequest("CN=localhost", rsa, HashAlgorithmName.SHA256, RSASignaturePadding.Pkcs1);
        var san = new SubjectAlternativeNameBuilder();
        san.AddDnsName("localhost");
        san.AddIpAddress(IPAddress.Loopback);
        request.CertificateExtensions.Add(san.Build());
        request.CertificateExtensions.Add(new X509BasicConstraintsExtension(false, false, 0, true));
        var usages = new OidCollection { new("1.3.6.1.5.5.7.3.1") };
        request.CertificateExtensions.Add(new X509EnhancedKeyUsageExtension(usages, false));
        using var generated = request.CreateSelfSigned(DateTimeOffset.UtcNow.AddMinutes(-1), DateTimeOffset.UtcNow.AddMinutes(10));
        // Schannel 服务端需要可访问的私钥容器，重新导入 PFX 到当前用户的临时密钥集。
        using var certificate = X509CertificateLoader.LoadPkcs12(generated.Export(X509ContentType.Pfx), null,
            X509KeyStorageFlags.UserKeySet | X509KeyStorageFlags.Exportable);
        var controlHandshake = false;
        string? serverError = null;
        var server = Task.Run(async () =>
        {
            for (var i = 0; i < 2; i++)
            {
                using var client = await listener.AcceptTcpClientAsync();
                using var ssl = new SslStream(client.GetStream());
                try
                {
                    using var cancel = new CancellationTokenSource(TimeSpan.FromSeconds(6));
                    await ssl.AuthenticateAsServerAsync(new SslServerAuthenticationOptions
                    {
                        ServerCertificate = certificate,
                        EnabledSslProtocols = SslProtocols.Tls12 | SslProtocols.Tls13
                    }, cancel.Token);
                    if (i == 0) controlHandshake = true;
                }
                catch (Exception error) when (error is AuthenticationException or IOException or OperationCanceledException)
                {
                    serverError = error.ToString();
                    if (i == 0) return;
                }
            }
        });
        // 仅测试用的对照握手，证明临时 TLS 服务本身正常；正式 Core 不接受自定义跳过验证。
        using (var control = new TcpClient())
        {
            await control.ConnectAsync(IPAddress.Loopback, ((IPEndPoint)listener.LocalEndpoint).Port);
            using var ssl = new SslStream(control.GetStream(), false, (_, _, _, _) => true);
            try { await ssl.AuthenticateAsClientAsync("localhost").WaitAsync(TimeSpan.FromSeconds(5)); }
            catch (Exception exception)
            {
                await server;
                throw new InvalidOperationException("TLS 对照服务握手失败：" + serverError, exception);
            }
        }
        await using var connection = new GameConnection();
        var refused = false;
        try { await connection.ConnectAsync(Endpoint(listener) with { UseTls = true }); }
        catch (Exception exception) when (exception is AuthenticationException or IOException or InvalidOperationException)
        { refused = true; }
        Require(refused && !connection.IsConnected, "TLS 不能接受未受信任的自签证书。");
        await server;
        Require(controlHandshake, "TLS 对照服务必须成功握手，避免把坏 fixture 误认为证书拒绝。");
        Pass("TLS rejects an untrusted server certificate");
    }

    private static Dictionary<string, object?> Fields(params (string Key, object? Value)[] pairs) =>
        pairs.ToDictionary(pair => pair.Key, pair => pair.Value);

    private static TcpListener Listen()
    {
        var listener = new TcpListener(IPAddress.Loopback, 0);
        listener.Start();
        return listener;
    }

    private static ConnectionOptions Endpoint(TcpListener listener) =>
        new("127.0.0.1:" + ((IPEndPoint)listener.LocalEndpoint).Port);

    private static async Task MustFailSoon(Task<ServerMessage> task, string message)
    {
        if (await Task.WhenAny(task, Task.Delay(2000)) != task)
            throw new InvalidOperationException(message);
        var failed = false;
        try { await task; }
        catch { failed = true; }
        Require(failed, message);
    }

    private static void Require(bool condition, string message)
    {
        if (!condition) throw new InvalidOperationException(message);
    }

    private static void Pass(string name)
    {
        Passed.Add(name);
        Console.WriteLine("PASS " + name);
    }
}
