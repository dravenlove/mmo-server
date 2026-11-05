using System.Buffers;
using System.Collections.Concurrent;
using System.Net.Security;
using System.Net.Sockets;
using System.Security.Authentication;
using System.Security.Cryptography.X509Certificates;
using System.Text.Json;

namespace Mmo.Client.Core;

public sealed class GameConnection : IAsyncDisposable
{
    public const int MaxLineBytes = 64 * 1024;
    private readonly SemaphoreSlim _lifecycle = new(1, 1);
    private Session? _session;
    private int _disposed;

    public event Action<ServerMessage>? MessageReceived;
    public event Action<string>? Disconnected;
    public bool IsConnected => _session is { Ended: 0 };

    private sealed class Pending(string operation)
    {
        public string Operation { get; } = operation;
        public TaskCompletionSource<ServerMessage> Completion { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);
    }

    private sealed class Session(TcpClient tcp, Stream stream)
    {
        public TcpClient Tcp { get; } = tcp;
        public Stream Stream { get; } = stream;
        public CancellationTokenSource Stop { get; } = new();
        public SemaphoreSlim Writer { get; } = new(1, 1);
        public ConcurrentDictionary<string, Pending> Pending { get; } = new();
        public Task Reader { get; set; } = Task.CompletedTask;
        public long Counter;
        public long Sequence;
        public int InFlight;
        public int Ended;
    }

    public async Task ConnectAsync(ConnectionOptions options, CancellationToken cancellationToken = default)
    {
        ObjectDisposedException.ThrowIf(Volatile.Read(ref _disposed) != 0, this);
        var (host, port) = options.GetEndpoint();
        await _lifecycle.WaitAsync(cancellationToken).ConfigureAwait(false);
        try
        {
            ObjectDisposedException.ThrowIf(Volatile.Read(ref _disposed) != 0, this);
            await DisconnectCurrentAsync().ConfigureAwait(false);
            using var timeout = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
            timeout.CancelAfter(TimeSpan.FromSeconds(5));
            var tcp = new TcpClient { NoDelay = true };
            Stream? stream = null;
            try
            {
                await tcp.ConnectAsync(host, port, timeout.Token).ConfigureAwait(false);
                stream = tcp.GetStream();
                if (options.UseTls)
                {
                    // 不设置跳过验证的回调，证书链和目标主机名均由系统验证。
                    var ssl = new SslStream(stream, leaveInnerStreamOpen: false);
                    stream = ssl;
                    await ssl.AuthenticateAsClientAsync(new SslClientAuthenticationOptions
                    {
                        TargetHost = host,
                        EnabledSslProtocols = SslProtocols.Tls12 | SslProtocols.Tls13,
                        CertificateRevocationCheckMode = X509RevocationMode.Online
                    }, timeout.Token).ConfigureAwait(false);
                }
                var session = new Session(tcp, stream);
                _session = session;
                session.Reader = ReadLoopAsync(session);
            }
            catch (OperationCanceledException) when (!cancellationToken.IsCancellationRequested)
            {
                stream?.Dispose();
                tcp.Dispose();
                throw new TimeoutException("连接或 TLS 握手超时，请检查公网端口、网络与服务器 TLS 配置。");
            }
            catch
            {
                stream?.Dispose();
                tcp.Dispose();
                throw;
            }
        }
        finally { _lifecycle.Release(); }
    }

    public long NextMoveSequence()
    {
        var session = GetSession();
        var sequence = Interlocked.Increment(ref session.Sequence);
        if (sequence <= 0) throw new InvalidOperationException("移动序号已用尽，请重新登录。");
        return sequence;
    }

    public async Task<ServerMessage> RequestAsync(string op,
        IDictionary<string, object?>? fields = null, CancellationToken cancellationToken = default)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(op);
        if (op.Length > 24) throw new ArgumentException("操作名称过长。", nameof(op));
        var session = GetSession();
        if (Interlocked.Increment(ref session.InFlight) > 64)
        {
            Interlocked.Decrement(ref session.InFlight);
            throw new InvalidOperationException("操作过于密集，请等待当前请求完成。");
        }
        string? id = null;
        try
        {
            var request = fields is null ? new Dictionary<string, object?>() : new(fields);
            if (request.ContainsKey("id") || request.ContainsKey("op"))
                throw new ArgumentException("附加字段不能覆盖 id 或 op。", nameof(fields));
            id = Interlocked.Increment(ref session.Counter).ToString(System.Globalization.CultureInfo.InvariantCulture);
            request.Add("id", id);
            request.Add("op", op);
            var json = JsonSerializer.SerializeToUtf8Bytes(request);
            if (json.Length + 1 > MaxLineBytes)
                throw new InvalidDataException("请求超过 64 KiB 协议限制。");
            var frame = new byte[json.Length + 1];
            json.CopyTo(frame, 0);
            frame[^1] = (byte)'\n';
            var pending = new Pending(op);
            if (!session.Pending.TryAdd(id, pending))
                throw new InvalidOperationException("请求标识冲突，请重新连接。");
            if (Volatile.Read(ref session.Ended) != 0)
                throw new IOException("连接已断开，请重新登录。");
            using var timeout = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
            timeout.CancelAfter(TimeSpan.FromSeconds(25));
            var writing = false;
            try
            {
                await session.Writer.WaitAsync(timeout.Token).ConfigureAwait(false);
                try
                {
                    writing = true;
                    await session.Stream.WriteAsync(frame, timeout.Token).ConfigureAwait(false);
                    await session.Stream.FlushAsync(timeout.Token).ConfigureAwait(false);
                    writing = false;
                }
                finally { session.Writer.Release(); }
                var response = await pending.Completion.Task.WaitAsync(timeout.Token).ConfigureAwait(false);
                if (!response.Ok) throw new GameRequestException(op, response.Error);
                if (op == "login") Interlocked.Exchange(ref session.Sequence, 0);
                return response;
            }
            catch (OperationCanceledException)
            {
                // 发送半帧后取消会破坏 JSONL 边界；等待响应超时则保留连接供同 ID 业务重试。
                if (writing) End(session, new IOException("发送中断，请重新连接。"));
                if (cancellationToken.IsCancellationRequested) throw;
                throw new TimeoutException("请求超时；场景切换重试请复用同一个 request_id。");
            }
            catch (Exception ex) when (ex is IOException or ObjectDisposedException or SocketException)
            {
                End(session, new IOException("连接已中断，请重新登录。", ex));
                throw new IOException("连接已中断，请重新登录。", ex);
            }
        }
        finally
        {
            if (id is not null) session.Pending.TryRemove(id, out _);
            Interlocked.Decrement(ref session.InFlight);
        }
    }

    private Session GetSession()
    {
        ObjectDisposedException.ThrowIf(Volatile.Read(ref _disposed) != 0, this);
        return _session is { Ended: 0 } session ? session :
            throw new InvalidOperationException("尚未连接到网关。");
    }

    private async Task ReadLoopAsync(Session session)
    {
        var buffer = ArrayPool<byte>.Shared.Rent(4096);
        var line = new ArrayBufferWriter<byte>();
        try
        {
            while (true)
            {
                var count = await session.Stream.ReadAsync(buffer.AsMemory(0, 4096), session.Stop.Token).ConfigureAwait(false);
                if (count == 0) throw new EndOfStreamException("服务器已断开连接，请重新登录。");
                for (var index = 0; index < count; index++)
                {
                    var value = buffer[index];
                    if (value != '\n')
                    {
                        if (line.WrittenCount >= MaxLineBytes)
                            throw new InvalidDataException("服务端消息超过 64 KiB，连接已关闭。");
                        line.GetSpan(1)[0] = value;
                        line.Advance(1);
                        continue;
                    }
                    if (line.WrittenCount == 0) throw new InvalidDataException("服务端发送了空消息。");
                    ServerMessage message;
                    try
                    {
                        message = JsonSerializer.Deserialize<ServerMessage>(line.WrittenSpan)
                            ?? throw new JsonException();
                    }
                    catch (JsonException)
                    {
                        throw new InvalidDataException("服务端消息不是有效的游戏 JSON 协议。");
                    }
                    line.Clear();
                    if (message.Id.Length != 0)
                    {
                        if (session.Pending.TryRemove(message.Id, out var pending))
                        {
                            if (message.Op != pending.Operation)
                            {
                                var error = new InvalidDataException("服务端响应的操作与请求不匹配。");
                                pending.Completion.TrySetException(error);
                                throw error;
                            }
                            pending.Completion.TrySetResult(message);
                        }
                    }
                    else
                    {
                        // 仅广播异步推送；带凭据的注册响应只能返回给对应的请求调用者。
                        if (message.Op is not ("state" or "session" or "error"))
                            throw new InvalidDataException("服务端推送类型无效。");
                        Raise(MessageReceived, message);
                    }
                }
            }
        }
        catch (OperationCanceledException) { End(session, new IOException("连接已断开。")); }
        catch (Exception ex) { End(session, ex); }
        finally { ArrayPool<byte>.Shared.Return(buffer, clearArray: true); }
    }

    private void End(Session session, Exception reason, bool expected = false)
    {
        if (Interlocked.Exchange(ref session.Ended, 1) != 0) return;
        Interlocked.CompareExchange(ref _session, null, session);
        session.Stop.Cancel();
        session.Stream.Dispose();
        session.Tcp.Dispose();
        foreach (var pair in session.Pending)
            if (session.Pending.TryRemove(pair.Key, out var pending))
                pending.Completion.TrySetException(new IOException("连接已断开，未完成的请求已终止。", reason));
        Raise(Disconnected, expected ? "已断开连接。" : reason.Message);
    }

    private static void Raise<T>(Action<T>? handlers, T value)
    {
        if (handlers is null) return;
        foreach (Action<T> handler in handlers.GetInvocationList())
        {
            try { handler(value); }
            catch { /* 界面回调异常不能阻塞协议收包线程。 */ }
        }
    }

    private async Task DisconnectCurrentAsync()
    {
        var session = _session;
        if (session is null) return;
        End(session, new IOException("连接已断开。"), expected: true);
        await session.Reader.ConfigureAwait(false);
        session.Stop.Dispose();
    }

    public async Task DisconnectAsync()
    {
        await _lifecycle.WaitAsync().ConfigureAwait(false);
        try { await DisconnectCurrentAsync().ConfigureAwait(false); }
        finally { _lifecycle.Release(); }
    }

    public async ValueTask DisposeAsync()
    {
        if (Interlocked.Exchange(ref _disposed, 1) != 0) return;
        await DisconnectAsync().ConfigureAwait(false);
    }
}
