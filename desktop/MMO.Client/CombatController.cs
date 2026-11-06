using System.IO;
using System.Text.Json;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using System.Windows.Threading;
using Mmo.Client.Core;

namespace Mmo.Client.Desktop;

public partial class MainWindow
{
    private readonly DispatcherTimer _combatTimer = new() { Interval = TimeSpan.FromMilliseconds(80) };
    private readonly Dictionary<string, (Button Button, TextBlock State, TextBlock Cost)> _skillControls = [];
    private WorldView? _combatView;
    private DateTime _combatSnapshotAt;
    private bool _skillSending;

    private void InitializeCombat()
    {
        _skillControls["strike"] = (StrikeButton, StrikeState, StrikeCost);
        _skillControls["whirlwind"] = (WhirlwindButton, WhirlwindState, WhirlwindCost);
        _skillControls["charge"] = (ChargeButton, ChargeState, ChargeCost);
        _skillControls["mend"] = (MendButton, MendState, MendCost);
        World.TargetSelected += _ => UpdateCombatHud();
        World.EventObserved += ObserveCombatEvent;
        _combatTimer.Tick += (_, _) => UpdateCombatHud();
        _combatTimer.Start();
    }

    private Role MergeCombatSnapshot(Role incoming)
    {
        if (incoming.Combat is not null && _role?.Combat is { } previous &&
            incoming.RoleId == _role.RoleId && incoming.OwnerEpoch == _role.OwnerEpoch && incoming.SessionEpoch == _role.SessionEpoch &&
            incoming.Combat.Revision < previous.Revision)
            return incoming with { Combat = previous };
        _combatSnapshotAt = DateTime.UtcNow;
        return incoming;
    }

    private double SnapshotAgeMs => Math.Max(0, (DateTime.UtcNow - _combatSnapshotAt).TotalMilliseconds);
    private double Remaining(string id) => Math.Max(0, (_role?.Combat?.Cooldowns.GetValueOrDefault(id) ?? 0) - SnapshotAgeMs);
    private SkillDefinition? Definition(string id) => _combatView?.Skills?.FirstOrDefault(skill => skill.Id == id);
    private Mob? SelectedMob => _combatView?.Mobs?.FirstOrDefault(mob => mob.Id == World.SelectedTargetId);
    private double TargetDistance => _role is not null && SelectedMob is { } mob ? Math.Sqrt(Math.Pow(mob.X - _role.X, 2) + Math.Pow(mob.Y - _role.Y, 2)) : double.PositiveInfinity;

    private void UpdateConfigHud(WorldView? view)
    {
        bool available = !string.IsNullOrWhiteSpace(view?.ConfigVersion);
        ConfigVersionLabel.Text = view is null ? "规则 —" : available
            ? $"规则 {view.ConfigVersion} · #{view.ConfigGeneration}"
            : "规则版本：服务端未提供";
        ConfigVersionLabel.ToolTip = available
            ? $"当前场景战斗规则：{view!.ConfigVersion}\n发布代次：{view.ConfigGeneration}\n应用于场景 Tick {view.ConfigAppliedTick}\n数值更新随服务器状态自动刷新，无需重新登录。"
            : "服务器提供规则版本后在这里显示；旧版服务器仍可正常使用已有功能。";
    }

    private string? SkillUnavailable(string id)
    {
        if (!_loggedIn || _connection?.IsConnected != true) return "请先登录角色。";
        if (_busy || _skillSending) return "正在处理操作，请稍候。";
        if (_role?.Combat is not { } combat || Definition(id) is not { } skill) return "服务器尚未提供战斗功能，请更新服务端。";
        if (SnapshotAgeMs > 2500) return "正在等待最新战斗状态。";
        if (combat.Dead) return "角色已经倒下，请等待复活。";
        if (Remaining(id) > 60) return "技能尚在冷却。";
        if (combat.Mp < skill.MpCost) return "法力不足，等待自动恢复。";
        if (skill.Target == "enemy")
        {
            if (SelectedMob is not { } target || !target.Alive) return "请先点击活着的怪物，或按 Tab 选择目标。";
            if (TargetDistance > skill.Range + .08) return $"目标太远，请靠近至 {skill.Range:0.#} 米内。";
        }
        if (id == "mend" && combat.Hp >= combat.MaxHp) return "当前生命已满。";
        return null;
    }

    private void UpdateCombatHud()
    {
        if (HpLabel is null) return;
        var combat = _role?.Combat;
        bool active = _connection?.IsConnected == true && _loggedIn;
        bool supported = combat is not null && (_combatView?.Skills?.Count ?? 0) > 0;
        HpLabel.Text = combat is null ? "—" : $"{combat.Hp} / {combat.MaxHp}";
        MpLabel.Text = combat is null ? "—" : $"{combat.Mp} / {combat.MaxMp}";
        HpBar.Maximum = Math.Max(1, combat?.MaxHp ?? 1); HpBar.Value = combat?.Hp ?? 0;
        MpBar.Maximum = Math.Max(1, combat?.MaxMp ?? 1); MpBar.Value = combat?.Mp ?? 0;
        CombatStatusLabel.Text = !active ? "登录角色后开始试炼" : !supported ? "旧版服务：更新服务端后启用战斗" : combat!.Dead ? "已倒下 · 等待复活" : SnapshotAgeMs > 2500 ? "等待服务器同步…" : "生命与技能状态由服务器同步";
        CombatStatusLabel.Foreground = new SolidColorBrush((Color)ColorConverter.ConvertFromString(combat?.Dead == true ? "#E7A3AE" : "#8AA4B8"));
        bool dead = active && combat?.Dead == true;
        ReviveButton.Visibility = dead ? Visibility.Visible : Visibility.Collapsed;
        double reviveRemaining = Math.Max(0, (combat?.ReviveRemainingMs ?? 0) - SnapshotAgeMs);
        ReviveButton.Content = reviveRemaining > 0 ? $"复活倒计时 {reviveRemaining / 1000:0.0}s" : "返回营地复活";
        ReviveButton.IsEnabled = dead && reviveRemaining <= 0 && !_busy && !_skillSending && SnapshotAgeMs < 2500;
        if (dead)
        {
            StopMotion();
        }
        UpButton.IsEnabled = DownButton.IsEnabled = LeftButton.IsEnabled = RightButton.IsEnabled = active && !_busy && !dead;
        foreach (var item in _skillControls)
        {
            string id = item.Key;
            var skill = Definition(id);
            double remaining = Remaining(id);
            var control = item.Value;
            control.Button.IsEnabled = SkillUnavailable(id) is null;
            control.State.Text = !active ? "待连接" : !supported ? "待升级" : combat!.Dead ? "倒下" : remaining > 60 ? $"{remaining / 1000:0.0}s" : _skillSending ? "施放中" : "就绪";
            if (skill is not null)
            {
                control.Cost.Text = $"{(skill.Target == "self" ? "自身" : "目标")} {skill.Range:0.#}m · {skill.MpCost} 法力";
                string config = string.IsNullOrWhiteSpace(_combatView?.ConfigVersion) ? "" : $"\n规则 {_combatView.ConfigVersion} · #{_combatView.ConfigGeneration}";
                control.Button.ToolTip = $"{skill.Name}：{skill.Description}\n伤害 {skill.Damage} · 治疗 {skill.Heal}\n范围 {skill.Range:0.#} 米 · 消耗 {skill.MpCost} 法力 · 冷却 {skill.CooldownMs / 1000:0.#} 秒{config}\n{SkillUnavailable(id) ?? "可以施放"}";
            }
            else control.Button.ToolTip = "等待服务器提供技能信息";
        }
        SkillHint.Text = active && !supported
            ? "当前服务器还没有战斗协议。请更新 gateway、backend 与 scene 镜像后重连。"
            : "点击怪物选中 · Tab 切换目标 · 1–4 施放技能 · WASD 移动 · 服务器判定命中与消耗";
        UpdateTargetHud();
    }

    private void UpdateTargetHud()
    {
        if (SelectedMob is not { } mob)
        {
            TargetNameLabel.Text = "点击怪物或按 Tab 选中"; TargetDistanceLabel.Text = "—";
            TargetHpLabel.Text = "—"; TargetHpBar.Value = 0; return;
        }
        TargetNameLabel.Text = mob.Name + (mob.MaxHp >= 200 ? " · 精英" : "");
        TargetDistanceLabel.Text = double.IsFinite(TargetDistance) ? $"{TargetDistance:0.0}m" : "—";
        TargetHpBar.Maximum = Math.Max(1, mob.MaxHp); TargetHpBar.Value = Math.Max(0, mob.Hp);
        TargetHpLabel.Text = mob.Alive ? $"生命 {mob.Hp} / {mob.MaxHp}" : $"已击败 · {Math.Ceiling(mob.RespawnRemainingMs / 1000)} 秒后刷新";
    }

    private void ObserveCombatEvent(CombatEvent item)
    {
        if (_role is null) return;
        if (item.Type == "death" && item.TargetId == _role.RoleId) Log("你被击倒了。等待复活倒计时，复活后重新投入战斗。");
        else if (item.Type == "death" && item.SourceId == _role.RoleId) Log("击败目标：" + (_combatView?.Mobs?.FirstOrDefault(mob => mob.Id == item.TargetId)?.Name ?? item.TargetId));
        else if (item.Type == "damage" && item.TargetId == _role.RoleId) Log($"受到 {item.Value} 点伤害。");
        else if (item.Type == "heal" && item.TargetId == _role.RoleId) Log($"恢复 {item.Value} 点生命。");
    }

    private async void Skill_Click(object sender, RoutedEventArgs e)
    {
        string id = (string)((Button)sender).Tag;
        World.Focus();
        await CastSkillAsync(id);
    }

    private async Task<bool> CastSkillAsync(string id)
    {
        string? unavailable = SkillUnavailable(id);
        if (unavailable is not null) { Log(unavailable); return false; }
        var conn = _connection!;
        var skill = Definition(id)!;
        string target = skill.Target == "enemy" ? SelectedMob!.Id : "";
        _skillSending = true; UpdateCombatHud();
        try
        {
            // 动作编号与移动输入共用单调计数，超时后不会偷偷生成新编号重复施法。
            var result = await conn.RequestAsync("cast", new Dictionary<string, object?>
            {
                ["seq"] = conn.NextMoveSequence(), ["skill_id"] = id, ["target_id"] = target
            });
            if (!ReferenceEquals(conn, _connection)) return false;
            if (result.Data.TryGetProperty("role", out var role)) ApplyRole(Role.FromJson(role));
            string version = result.Data.TryGetProperty("config_version", out var configVersion) ? configVersion.GetString() ?? "" : "";
            Log("施放：" + skill.Name + (target.Length == 0 ? "" : " → " + target) +
                (string.IsNullOrWhiteSpace(version) ? "" : " · 结算规则 " + version));
            return true;
        }
        catch (Exception ex)
        {
            if (ReferenceEquals(conn, _connection)) Log(ex is TimeoutException ? "施法结果尚未确认，请等待服务器状态；客户端没有自动重复攻击。" : "施法失败：" + Explain(ex));
            return false;
        }
        finally { _skillSending = false; UpdateCombatHud(); }
    }

    private async void Revive_Click(object sender, RoutedEventArgs e) => await ReviveAsync();
    private async Task<bool> ReviveAsync()
    {
        var conn = _connection;
        if (conn?.IsConnected != true || _role?.Combat?.Dead != true || _skillSending || _busy) return false;
        _skillSending = true; StopMotion(); UpdateCombatHud();
        try
        {
            var result = await conn.RequestAsync("revive", new Dictionary<string, object?> { ["seq"] = conn.NextMoveSequence() });
            if (!ReferenceEquals(conn, _connection)) return false;
            if (result.Data.TryGetProperty("role", out var role)) ApplyRole(Role.FromJson(role));
            await RefreshAsync(); Log("已在营地复活。"); World.Focus(); return true;
        }
        catch (Exception ex) { Log("复活失败：" + Explain(ex)); return false; }
        finally { _skillSending = false; UpdateControls(); }
    }

    private void ClearCombat()
    {
        _combatView = null; _combatSnapshotAt = default; _skillSending = false;
        UpdateConfigHud(null);
        World.SelectTarget(null); UpdateCombatHud();
    }

    private async Task CheckCombatUiAsync(List<string> results)
    {
        if (_role?.Combat is null || (_combatView?.Skills?.Count ?? 0) == 0) return;
        _busy = false; UpdateControls();
        if (!string.IsNullOrWhiteSpace(_combatView?.ConfigVersion))
        {
            if (ConfigVersionLabel.Text != $"规则 {_combatView.ConfigVersion} · #{_combatView.ConfigGeneration}" ||
                ConfigVersionLabel.ToolTip is not string versionTip || !versionTip.Contains($"Tick {_combatView.ConfigAppliedTick}"))
                throw new InvalidDataException("场景规则版本未同步到界面。");
            foreach (var skill in _combatView.Skills!)
            {
                if (!_skillControls.TryGetValue(skill.Id, out var control)) continue;
                if (control.Button.ToolTip is not string tip || !tip.Contains($"伤害 {skill.Damage}") ||
                    !tip.Contains($"治疗 {skill.Heal}") || !tip.Contains($"冷却 {skill.CooldownMs / 1000:0.#} 秒") ||
                    !tip.Contains($"消耗 {skill.MpCost} 法力") || !tip.Contains($"范围 {skill.Range:0.#} 米") || !tip.Contains(_combatView.ConfigVersion))
                    throw new InvalidDataException("技能提示没有使用服务器当前规则数值。");
            }
            results.Add("规则版本、发布代次与服务器技能数值提示");
        }
        var target = (_combatView!.Mobs ?? []).Where(mob => mob.Alive)
            .OrderBy(mob => Math.Pow(mob.X - _role.X, 2) + Math.Pow(mob.Y - _role.Y, 2)).FirstOrDefault()
            ?? throw new InvalidDataException("战斗界面检查缺少活着的怪物。");
        World.SelectTarget(target.Id);
        if (SelectedMob?.Id != target.Id || TargetHpLabel.Text == "—") throw new InvalidDataException("目标选择面板未同步。");
        results.Add("怪物选择、目标血条和距离面板");
        if (!await CastSkillAsync("charge")) throw new InvalidDataException("突进界面施法失败。");
        await Task.Delay(250); await RefreshAsync();
        if (TargetDistance > 3.2) throw new InvalidDataException("突进没有进入近战范围。");
        results.Add("突进技能、服务器位移和冷却显示");
        await Task.Delay(1800); await RefreshAsync();
        if (_role!.Combat!.Hp >= _role.Combat.MaxHp) throw new InvalidDataException("怪物没有造成可见伤害。");
        if (!await CastSkillAsync("mend")) throw new InvalidDataException("治疗界面施法失败。");
        results.Add("受击血条与战意恢复");
        if (!await CastSkillAsync("strike")) throw new InvalidDataException("斩击界面施法失败。");
        if (!await CastSkillAsync("whirlwind")) throw new InvalidDataException("旋风斩界面施法失败。");
        await Task.Delay(750); await RefreshAsync();
        if (SelectedMob?.Alive == true && !await CastSkillAsync("strike")) throw new InvalidDataException("追击斩击失败。");
        await Task.Delay(250); await RefreshAsync();
        results.Add("斩击、范围技能、事件反馈与怪物击败");
        long hpBeforeReconnect = _role!.Combat!.Hp;
        double mendBeforeReconnect = Remaining("mend");
        await DisconnectAsync(); await ConnectAndLoginAsync();
        if (_role!.Combat!.Hp != hpBeforeReconnect || mendBeforeReconnect > 1000 && Remaining("mend") <= 0)
            throw new InvalidDataException("战斗状态被重连刷新。");
        results.Add("战斗生命与技能冷却在重连后保留");
        var elite = (_combatView!.Mobs ?? []).FirstOrDefault(mob => mob.Alive && mob.MaxHp >= 200)
            ?? throw new InvalidDataException("死亡复活界面检查缺少精英怪。");
        double approachX = elite.X - _role.X, approachY = elite.Y - _role.Y;
        double length = Math.Sqrt(approachX * approachX + approachY * approachY);
        if (length > 12)
        {
            _buttonX = approachX / length; _buttonY = approachY / length;
            await Task.Delay(1200); StopMotion(); await Task.Delay(300); await RefreshAsync();
        }
        await Task.Delay((int)Math.Min(8000, Remaining("charge") + 180));
        World.SelectTarget(elite.Id);
        if (!await CastSkillAsync("charge")) throw new InvalidDataException("精英怪战斗触发失败。");
        var deathDeadline = DateTime.UtcNow.AddSeconds(35);
        while (_role!.Combat!.Dead != true && DateTime.UtcNow < deathDeadline)
        {
            await Task.Delay(300); await RefreshAsync();
        }
        if (_role!.Combat!.Dead != true || ReviveButton.Visibility != Visibility.Visible || UpButton.IsEnabled)
            throw new InvalidDataException("死亡界面、移动禁用或复活按钮未同步。");
        results.Add("角色死亡遮罩、血条与移动禁用");
        if (_uiCheckPath is not null) Capture(Path.ChangeExtension(_uiCheckPath, ".dead.png"));
        await Task.Delay((int)Math.Min(6000, Math.Max(0, _role.Combat.ReviveRemainingMs - SnapshotAgeMs) + 220));
        if (!await ReviveAsync() || _role?.Combat is not { Dead: false } revived || revived.Hp != revived.MaxHp || !UpButton.IsEnabled)
            throw new InvalidDataException("营地复活界面检查失败。");
        double revivedX = _role.X;
        _buttonX = 1; await Task.Delay(450); StopMotion(); await Task.Delay(250); await RefreshAsync();
        if (_role.X <= revivedX) throw new InvalidDataException("复活后移动没有恢复。");
        results.Add("服务器复活计时与营地重生");
        _busy = false; UpdateControls();
    }

    private async Task CheckLiveConfigUiAsync(string reportPath, string expectedVersion, List<string> results)
    {
        // 仅由已有回环地址保护的 UI 验收调用；外部驱动看到 ready 后才发布测试规则。
        var connection = _connection ?? throw new InvalidDataException("热更界面检查缺少活动连接。");
        var role = _role ?? throw new InvalidDataException("热更界面检查缺少角色。");
        var view = _combatView ?? throw new InvalidDataException("热更界面检查缺少场景视图。");
        if (view.ConfigVersion == expectedVersion) throw new InvalidDataException("热更界面检查必须从不同版本开始。");
        Directory.CreateDirectory(Path.GetDirectoryName(Path.GetFullPath(reportPath))!);
        File.WriteAllText(reportPath + ".ready", JsonSerializer.Serialize(new
        {
            role_id = role.RoleId, session_epoch = role.SessionEpoch, scene_id = view.SceneId,
            config_version = view.ConfigVersion, config_generation = view.ConfigGeneration,
            expected_version = expectedVersion
        }));
        var deadline = DateTime.UtcNow.AddSeconds(30);
        while (_combatView?.ConfigVersion != expectedVersion && DateTime.UtcNow < deadline)
        {
            if (!ReferenceEquals(connection, _connection) || !_loggedIn || !connection.IsConnected)
                throw new InvalidDataException("配置更新期间连接发生变化。");
            await Task.Delay(100);
        }
        if (_combatView?.ConfigVersion != expectedVersion) throw new TimeoutException("等待新规则推送超时。");
        if (!ReferenceEquals(connection, _connection) || _role?.RoleId != role.RoleId || _role.SessionEpoch != role.SessionEpoch)
            throw new InvalidDataException("热更期间角色身份或会话发生变化。");
        if (_combatView.ConfigGeneration <= view.ConfigGeneration ||
            ConfigVersionLabel.Text != $"规则 {expectedVersion} · #{_combatView.ConfigGeneration}")
            throw new InvalidDataException("推送后的规则版本与发布代次未刷新到界面。");
        if (Definition("strike")?.Damage != 36 || StrikeButton.ToolTip is not string strikeTip || !strikeTip.Contains("伤害 36") ||
            Definition("mend")?.CooldownMs != 5000 || MendButton.ToolTip is not string mendTip || !mendTip.Contains("冷却 5 秒"))
            throw new InvalidDataException("测试规则中的斩击伤害或治疗冷却未热更新到技能提示。");
        results.Add("同一会话接收热更推送，规则版本、斩击伤害和治疗冷却实时刷新");
    }
}
