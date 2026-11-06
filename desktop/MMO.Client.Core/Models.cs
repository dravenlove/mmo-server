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
    long OwnerEpoch, long SessionEpoch, double X, double Y, long Coins, CombatState? Combat = null)
{
    public static Role FromJson(JsonElement data)
    {
        var role = new Role(data.GetProperty("role_id").GetString() ?? "",
            data.GetProperty("name").GetString() ?? "",
            data.GetProperty("scene_id").GetString() ?? "",
            data.GetProperty("owner_epoch").GetInt64(),
            data.GetProperty("session_epoch").GetInt64(),
            data.GetProperty("x").GetDouble(), data.GetProperty("y").GetDouble(),
            data.GetProperty("coins").GetInt64(),
            data.TryGetProperty("combat", out var combat) && combat.ValueKind == JsonValueKind.Object ? CombatState.FromJson(combat) : null);
        if (role.RoleId.Length == 0 || !double.IsFinite(role.X) || !double.IsFinite(role.Y))
            throw new InvalidDataException("服务端角色数据无效。");
        return role;
    }
}

public sealed record CombatState(long Hp, long MaxHp, long Mp, long MaxMp, bool Dead,
    double ReviveRemainingMs, long Revision, long LastActionSeq, long ActionSession,
    IReadOnlyDictionary<string, double> Cooldowns)
{
    public static CombatState FromJson(JsonElement data)
    {
        var cooldowns = new Dictionary<string, double>(StringComparer.Ordinal);
        if (data.TryGetProperty("cooldowns", out var values) && values.ValueKind == JsonValueKind.Object)
            foreach (var value in values.EnumerateObject()) cooldowns[value.Name] = Math.Max(0, value.Value.GetDouble());
        long ReadLong(string key, long fallback = 0) => data.TryGetProperty(key, out var element) ? element.GetInt64() : fallback;
        var result = new CombatState(ReadLong("hp"), ReadLong("max_hp"), ReadLong("mp"), ReadLong("max_mp"),
            data.TryGetProperty("dead", out var dead) && dead.GetBoolean(),
            data.TryGetProperty("revive_remaining_ms", out var remaining) ? Math.Max(0, remaining.GetDouble()) : 0,
            ReadLong("revision"), ReadLong("last_action_seq"), ReadLong("action_session"), cooldowns);
        if (result.MaxHp <= 0 || result.Hp < 0 || result.Hp > result.MaxHp || result.MaxMp < 0 || result.Mp < 0 || result.Mp > result.MaxMp ||
            !double.IsFinite(result.ReviveRemainingMs) || cooldowns.Values.Any(v => !double.IsFinite(v)))
            throw new InvalidDataException("服务端战斗状态无效。");
        return result;
    }
}

public sealed record Mob(string Id, string Name, string Kind, double X, double Y, long Hp,
    long MaxHp, bool Alive, double RespawnRemainingMs, string TargetId)
{
    public static Mob FromJson(JsonElement data)
    {
        var mob = new Mob(data.GetProperty("id").GetString() ?? "", data.GetProperty("name").GetString() ?? "",
            data.GetProperty("kind").GetString() ?? "", data.GetProperty("x").GetDouble(), data.GetProperty("y").GetDouble(),
            data.GetProperty("hp").GetInt64(), data.GetProperty("max_hp").GetInt64(), data.GetProperty("alive").GetBoolean(),
            data.TryGetProperty("respawn_remaining_ms", out var remaining) ? remaining.GetDouble() : 0,
            data.TryGetProperty("target_id", out var target) ? target.GetString() ?? "" : "");
        if (mob.Id.Length == 0 || !double.IsFinite(mob.X) || !double.IsFinite(mob.Y) ||
            mob.MaxHp <= 0 || mob.Hp < 0 || mob.Hp > mob.MaxHp || !double.IsFinite(mob.RespawnRemainingMs) || mob.RespawnRemainingMs < 0)
            throw new InvalidDataException("服务端怪物数据无效。");
        return mob;
    }
}

public sealed record SkillDefinition(string Id, string Name, string Description, double Range,
    double CooldownMs, long MpCost, long Damage, long Heal, string Target)
{
    public static SkillDefinition FromJson(JsonElement data)
    {
        var skill = new SkillDefinition(data.GetProperty("id").GetString() ?? "", data.GetProperty("name").GetString() ?? "",
            data.TryGetProperty("description", out var description) ? description.GetString() ?? "" : "",
            data.GetProperty("range").GetDouble(), data.GetProperty("cooldown_ms").GetDouble(), data.GetProperty("mp_cost").GetInt64(),
            data.TryGetProperty("damage", out var damage) ? damage.GetInt64() : 0,
            data.TryGetProperty("heal", out var heal) ? heal.GetInt64() : 0,
            data.GetProperty("target").GetString() ?? "enemy");
        if (skill.Id.Length == 0 || !double.IsFinite(skill.Range) || skill.Range < 0 ||
            !double.IsFinite(skill.CooldownMs) || skill.CooldownMs < 0 || skill.MpCost < 0 || skill.Damage < 0 || skill.Heal < 0)
            throw new InvalidDataException("服务端技能数据无效。");
        return skill;
    }
}

public sealed record CombatEvent(long Id, string Type, string SourceId, string TargetId, string SkillId,
    long Value, double X, double Y, ulong Tick)
{
    public static CombatEvent FromJson(JsonElement data)
    {
        var item = new CombatEvent(data.GetProperty("id").GetInt64(), data.GetProperty("type").GetString() ?? "",
            data.TryGetProperty("source_id", out var source) ? source.GetString() ?? "" : "",
            data.TryGetProperty("target_id", out var target) ? target.GetString() ?? "" : "",
            data.TryGetProperty("skill_id", out var skill) ? skill.GetString() ?? "" : "",
            data.TryGetProperty("value", out var value) ? value.GetInt64() : 0,
            data.GetProperty("x").GetDouble(), data.GetProperty("y").GetDouble(), data.GetProperty("tick").GetUInt64());
        if (item.Id < 0 || !double.IsFinite(item.X) || !double.IsFinite(item.Y)) throw new InvalidDataException("服务端战斗事件无效。");
        return item;
    }
}

public sealed record WorldView(Role Self, IReadOnlyList<Role> Players, ulong Tick, string SceneId,
    IReadOnlyList<Mob>? Mobs = null, IReadOnlyList<SkillDefinition>? Skills = null, IReadOnlyList<CombatEvent>? Events = null,
    bool PlayersTruncated = false, int VisiblePlayerCount = 0,
    string ConfigVersion = "", ulong ConfigGeneration = 0, ulong ConfigAppliedTick = 0)
{
    public static WorldView FromJson(JsonElement data) => new(
        Role.FromJson(data.GetProperty("self")),
        data.GetProperty("players").EnumerateArray().Select(Role.FromJson).ToArray(),
        data.GetProperty("tick").GetUInt64(),
        data.GetProperty("scene_id").GetString() ?? "",
        data.TryGetProperty("mobs", out var mobs) ? mobs.EnumerateArray().Select(Mob.FromJson).ToArray() : [],
        data.TryGetProperty("skills", out var skills) ? skills.EnumerateArray().Select(SkillDefinition.FromJson).ToArray() : [],
        data.TryGetProperty("events", out var events) ? events.EnumerateArray().Select(CombatEvent.FromJson).ToArray() : [],
        data.TryGetProperty("players_truncated", out var truncated) && truncated.GetBoolean(),
        data.TryGetProperty("visible_player_count", out var visible) ? visible.GetInt32() : data.GetProperty("players").GetArrayLength(),
        data.TryGetProperty("config_version", out var version) ? version.GetString() ?? "" : "",
        data.TryGetProperty("config_generation", out var generation) ? generation.GetUInt64() : 0,
        data.TryGetProperty("config_applied_tick", out var appliedTick) ? appliedTick.GetUInt64() : 0);
}

public sealed record SavedCredential(string Token, string RoleId)
{
    public override string ToString() => $"角色 {RoleId} 的本地加密凭据";
}
