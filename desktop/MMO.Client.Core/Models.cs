using System.Net;
using System.Text.Json;
using System.Text.Json.Serialization;

namespace Mmo.Client.Core;

public sealed record ConnectionOptions(string Address, bool UseTls = false)
{
    public (string Host, int Port) GetEndpoint()
    {
        var address = Address?.Trim() ?? "";
        if (address.Length == 0 || address.Contains("://", StringComparison.Ordinal))
            throw new ArgumentException("请填写网关地址，例如 127.0.0.1:7000；协议使用 TLS 选项选择。");

        // 裸 IPv6 地址使用默认端口；显式端口须采用 [IPv6]:端口。
        if (IPAddress.TryParse(address, out var literal))
            return (literal.ToString(), 7000);
        if (!Uri.TryCreate("tcp://" + address, UriKind.Absolute, out var uri) ||
            uri.Host.Length == 0 || uri.UserInfo.Length != 0 ||
            uri.AbsolutePath != "/" && uri.AbsolutePath.Length != 0 ||
            uri.Query.Length != 0 || uri.Fragment.Length != 0)
            throw new ArgumentException("网关地址格式无效，请使用 主机名:端口 或 IP:端口。");
        var port = uri.Port < 0 ? 7000 : uri.Port;
        if (port is < 1 or > 65535)
            throw new ArgumentException("网关端口必须在 1 到 65535 之间。");
        return (uri.IdnHost.Trim('[', ']').ToLowerInvariant(), port);
    }

    public string NormalizedAddress
    {
        get
        {
            var (host, port) = GetEndpoint();
            return host.Contains(':') ? $"[{host}]:{port}" : $"{host}:{port}";
        }
    }
}

public sealed class ServerMessage
{
    [JsonPropertyName("id")] public string Id { get; init; } = "";
    [JsonPropertyName("op")] public string Op { get; init; } = "";
    [JsonPropertyName("ok")] public bool Ok { get; init; }
    [JsonPropertyName("data")] public JsonElement Data { get; init; }
    [JsonPropertyName("error")] public string Error { get; init; } = "";

    // 注册响应中包含凭据，禁止默认对象格式化意外输出 Data。
    public override string ToString() => $"{Op}: {(Ok ? "成功" : "失败")}";
}

public sealed class GameRequestException : Exception
{
    public string Operation { get; }
    public GameRequestException(string operation, string message)
        : base(string.IsNullOrWhiteSpace(message) ? "服务端拒绝了该操作。" : message)
        => Operation = operation;
}

public sealed record Role(string RoleId, string Name, string SceneId,
    long OwnerEpoch, long SessionEpoch, double X, double Y, long Coins)
{
    public static Role FromJson(JsonElement data)
    {
        var role = new Role(data.GetProperty("role_id").GetString() ?? "",
            data.GetProperty("name").GetString() ?? "",
            data.GetProperty("scene_id").GetString() ?? "",
            data.GetProperty("owner_epoch").GetInt64(),
            data.GetProperty("session_epoch").GetInt64(),
            data.GetProperty("x").GetDouble(), data.GetProperty("y").GetDouble(),
            data.GetProperty("coins").GetInt64());
        if (role.RoleId.Length == 0 || !double.IsFinite(role.X) || !double.IsFinite(role.Y))
            throw new InvalidDataException("服务端角色数据无效。");
        return role;
    }
}

public sealed record WorldView(Role Self, IReadOnlyList<Role> Players, ulong Tick, string SceneId)
{
    public static WorldView FromJson(JsonElement data) => new(
        Role.FromJson(data.GetProperty("self")),
        data.GetProperty("players").EnumerateArray().Select(Role.FromJson).ToArray(),
        data.GetProperty("tick").GetUInt64(),
        data.GetProperty("scene_id").GetString() ?? "");
}

public sealed record SavedCredential(string Token, string RoleId)
{
    public override string ToString() => $"角色 {RoleId} 的本地加密凭据";
}
