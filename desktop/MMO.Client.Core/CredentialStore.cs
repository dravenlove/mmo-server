using System.Security.Cryptography;
using System.Text;
using System.Text.Json;

namespace Mmo.Client.Core;

public sealed class CredentialStore
{
    private const int MaxCredentialFileBytes = 64 * 1024;
    private readonly string _root;
    public string RootDirectory => _root;

    public CredentialStore(string root)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(root);
        _root = Path.GetFullPath(root);
    }

    private sealed record Envelope(int Version, string Identity, string Token, string RoleId);

    private (string Identity, string File, string Pending, string Lock) GetPaths(ConnectionOptions options, string name)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(name);
        if (name != name.Trim()) throw new ArgumentException("角色名首尾不能包含空白。", nameof(name));
        var identity = Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(
            (options.UseTls ? "tls://" : "tcp://") + options.NormalizedAddress + "\n" + name)));
        var path = Path.Combine(_root, identity.ToLowerInvariant());
        return (identity, path + ".credential", path + ".pending", path + ".lock");
    }

    public bool TryLoad(ConnectionOptions options, string name, out SavedCredential credential)
    {
        var paths = GetPaths(options, name);
        Directory.CreateDirectory(_root);
        using var mutex = Acquire(paths.Lock);
        credential = null!;
        var source = File.Exists(paths.File) ? paths.File : File.Exists(paths.Pending) ? paths.Pending : null;
        if (source is null) return false;
        credential = Read(source, paths.Identity);
        if (source == paths.Pending)
        {
            // 注册响应已加密写出但进程在提交前退出时，先恢复凭据，绝不重新注册。
            File.Move(paths.Pending, paths.File, overwrite: false);
        }
        return true;
    }

    public CredentialReservation Reserve(ConnectionOptions options, string name)
    {
        var paths = GetPaths(options, name);
        Directory.CreateDirectory(_root);
        FileStream? mutex = null;
        FileStream? pending = null;
        try
        {
            mutex = Acquire(paths.Lock);
            if (File.Exists(paths.File))
                throw new IOException("此角色已有本地凭据，请重新尝试登录。");
            if (File.Exists(paths.Pending))
                throw new IOException("发现该角色未完成的注册记录。请保留凭据目录并先尝试恢复，避免丢失账号。");
            pending = new FileStream(paths.Pending, FileMode.CreateNew, FileAccess.ReadWrite,
                FileShare.None, 4096, FileOptions.WriteThrough);
            // 在网络注册前实际写入并刷新，先发现目录权限、磁盘空间等问题。
            pending.Write(new byte[4096]);
            pending.Flush(flushToDisk: true);
            return new CredentialReservation(paths.Identity, paths.File, paths.Pending, mutex, pending);
        }
        catch
        {
            if (pending is not null)
            {
                pending.Dispose();
                try { File.Delete(paths.Pending); } catch (IOException) { }
            }
            mutex?.Dispose();
            throw;
        }
    }

    private static FileStream Acquire(string path)
    {
        try { return new FileStream(path, FileMode.OpenOrCreate, FileAccess.ReadWrite, FileShare.None); }
        catch (IOException ex) { throw new IOException("该角色的凭据正在使用，请关闭另一客户端后重试。", ex); }
    }

    private static SavedCredential Read(string path, string identity)
    {
        byte[]? plain = null;
        try
        {
            var info = new FileInfo(path);
            if (info.Length is <= 0 or > MaxCredentialFileBytes)
                throw new InvalidDataException("凭据文件长度无效。");
            var encrypted = File.ReadAllBytes(path);
            plain = WindowsDpapi.Unprotect(encrypted, Encoding.UTF8.GetBytes("MMO.Client.Credential.v1:" + identity));
            var envelope = JsonSerializer.Deserialize<Envelope>(plain)
                ?? throw new InvalidDataException("凭据内容为空。");
            if (envelope.Version != 1 || envelope.Identity != identity ||
                string.IsNullOrWhiteSpace(envelope.Token) || string.IsNullOrWhiteSpace(envelope.RoleId))
                throw new InvalidDataException("凭据内容或目标服务器不匹配。");
            return new SavedCredential(envelope.Token, envelope.RoleId);
        }
        catch (Exception ex) when (ex is System.ComponentModel.Win32Exception or JsonException or InvalidDataException)
        {
            throw new InvalidDataException("本地凭据无效或无法解密。请保留文件，并使用创建凭据的 Windows 账号；不会自动重新注册同名角色。", ex);
        }
        finally { if (plain is not null) CryptographicOperations.ZeroMemory(plain); }
    }

    public sealed class CredentialReservation : IDisposable
    {
        private readonly string _identity;
        private readonly string _pendingPath;
        private FileStream? _mutex;
        private FileStream? _pending;
        private bool _saved;
        private bool _saveAttempted;
        public string FilePath { get; }
        public string? RecoveryPath => _saveAttempted && !_saved ? _pendingPath : null;

        internal CredentialReservation(string identity, string file, string pendingPath,
            FileStream mutex, FileStream pending)
        {
            _identity = identity;
            FilePath = file;
            _pendingPath = pendingPath;
            _mutex = mutex;
            _pending = pending;
        }

        public void Save(string token, string roleId)
        {
            ObjectDisposedException.ThrowIf(_mutex is null, this);
            if (_saved) throw new InvalidOperationException("该凭据已经保存。");
            if (string.IsNullOrWhiteSpace(token) || token.Length > 4096 ||
                string.IsNullOrWhiteSpace(roleId) || roleId.Length > 1024)
                throw new InvalidDataException("服务端返回的登录凭据无效。");
            _saveAttempted = true;
            byte[]? plaintext = null;
            try
            {
                plaintext = JsonSerializer.SerializeToUtf8Bytes(new Envelope(1, _identity, token, roleId));
                var encrypted = WindowsDpapi.Protect(plaintext,
                    Encoding.UTF8.GetBytes("MMO.Client.Credential.v1:" + _identity));
                _pending!.Position = 0;
                _pending.Write(encrypted);
                _pending.SetLength(encrypted.Length);
                _pending.Flush(flushToDisk: true);
                _pending.Dispose();
                _pending = null;
                // 同一目录内原子提交，禁止覆盖其他进程可能已保存的凭据。
                File.Move(_pendingPath, FilePath, overwrite: false);
                _saved = true;
            }
            catch (Exception ex) when (ex is IOException or UnauthorizedAccessException or System.ComponentModel.Win32Exception)
            {
                throw new IOException("账号已注册，但凭据保存失败。请保留本地 .pending 恢复文件并重试登录，不要删除后重新注册。", ex);
            }
            finally { if (plaintext is not null) CryptographicOperations.ZeroMemory(plaintext); }
        }

        public void Dispose()
        {
            _pending?.Dispose();
            _pending = null;
            if (!_saved && !_saveAttempted)
                try { File.Delete(_pendingPath); } catch (IOException) { }
            _mutex?.Dispose();
            _mutex = null;
        }
    }
}
