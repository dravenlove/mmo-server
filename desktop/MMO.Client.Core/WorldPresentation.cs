namespace Mmo.Client.Core;

public readonly record struct WorldPosition(double X, double Y);

// 仅生成画面坐标，不修改权威快照、碰撞、技能距离、血蓝或动作序号。
public sealed class WorldPresentation
{
    private sealed class Track(double x, double y, bool alive)
    {
        public double FromX = x, FromY = y, TargetX = x, TargetY = y, Started, Duration;
        public bool Alive = alive;

        public WorldPosition Sample(double now)
        {
            double amount = Duration <= 0 ? 1 : Math.Clamp((now - Started) / Duration, 0, 1);
            return new(FromX + (TargetX - FromX) * amount, FromY + (TargetY - FromY) * amount);
        }

        public void Update(double x, double y, bool alive, double now, double duration, bool snap)
        {
            snap |= Alive != alive || Math.Pow(x - TargetX, 2) + Math.Pow(y - TargetY, 2) > 36;
            Alive = alive;
            if (snap)
            {
                FromX = TargetX = x; FromY = TargetY = y; Duration = 0; Started = now;
            }
            else if (x != TargetX || y != TargetY)
            {
                var current = Sample(now);
                FromX = current.X; FromY = current.Y; TargetX = x; TargetY = y;
                Started = now; Duration = duration;
            }
            // 重复视图和只改血量的视图不能不断重新开始插值。
        }
    }

    private readonly Dictionary<string, Track> _players = new(StringComparer.Ordinal);
    private readonly Dictionary<string, Track> _mobs = new(StringComparer.Ordinal);
    private string _scene = "", _self = "";
    private long _owner, _session, _eventCursor;
    private ulong _tick;
    private double _snapshotAt, _duration = .2;
    private bool _initialized;

    public void Clear()
    {
        _players.Clear(); _mobs.Clear(); _initialized = false; _eventCursor = 0; _duration = .2;
    }

    public void Update(WorldView view, double now)
    {
        if (!double.IsFinite(now)) throw new ArgumentOutOfRangeException(nameof(now));
        bool reset = !_initialized || view.SceneId != _scene || view.Self.RoleId != _self ||
            view.Self.OwnerEpoch != _owner || view.Self.SessionEpoch != _session || view.Tick < _tick;
        double interval = now - _snapshotAt;
        bool staleGap = _initialized && (interval < 0 || interval > .8);
        if (reset)
        {
            Clear();
            _eventCursor = view.Events is { Count: > 0 } ? view.Events.Max(item => item.Id) : 0;
        }
        else if (view.Tick > _tick && interval >= .04 && !staleGap)
            _duration = .85 * _duration + .15 * Math.Clamp(interval, .16, .3);

        var snapPlayers = new HashSet<string>(StringComparer.Ordinal);
        var snapMobs = new HashSet<string>(StringComparer.Ordinal);
        if (!reset)
        {
            foreach (var item in view.Events ?? [])
            {
                if (item.Id <= _eventCursor) continue;
                if (item.Type == "cast" && item.SkillId == "charge") snapPlayers.Add(item.SourceId);
                if (item.Type == "revive") snapPlayers.Add(item.TargetId);
                if (item.Type is "respawn" or "reset") snapMobs.Add(item.TargetId);
            }
            if (view.Events is { Count: > 0 }) _eventCursor = Math.Max(_eventCursor, view.Events.Max(item => item.Id));
        }
        var seenPlayers = new HashSet<string>(StringComparer.Ordinal) { view.Self.RoleId };
        foreach (var role in view.Players)
        {
            if (role.RoleId == view.Self.RoleId) continue;
            seenPlayers.Add(role.RoleId);
            Set(_players, role.RoleId, role.X, role.Y, role.Combat?.Dead != true, now, reset || staleGap || snapPlayers.Contains(role.RoleId));
        }
        Set(_players, view.Self.RoleId, view.Self.X, view.Self.Y, view.Self.Combat?.Dead != true, now,
            reset || staleGap || snapPlayers.Contains(view.Self.RoleId));
        foreach (var id in _players.Keys.Where(id => !seenPlayers.Contains(id)).ToArray()) _players.Remove(id);

        var seenMobs = new HashSet<string>(StringComparer.Ordinal);
        foreach (var mob in view.Mobs ?? [])
        {
            seenMobs.Add(mob.Id);
            Set(_mobs, mob.Id, mob.X, mob.Y, mob.Alive, now, reset || staleGap || snapMobs.Contains(mob.Id));
        }
        foreach (var id in _mobs.Keys.Where(id => !seenMobs.Contains(id)).ToArray()) _mobs.Remove(id);
        _scene = view.SceneId; _self = view.Self.RoleId; _owner = view.Self.OwnerEpoch;
        _session = view.Self.SessionEpoch; _tick = view.Tick; _snapshotAt = now; _initialized = true;
    }

    private void Set(Dictionary<string, Track> tracks, string id, double x, double y, bool alive, double now, bool snap)
    {
        if (!tracks.TryGetValue(id, out var track)) tracks.Add(id, new Track(x, y, alive));
        else track.Update(x, y, alive, now, _duration, snap);
    }

    public WorldPosition Player(Role role, double now) =>
        _players.TryGetValue(role.RoleId, out var track) ? track.Sample(now) : new(role.X, role.Y);

    public WorldPosition Monster(Mob mob, double now) =>
        _mobs.TryGetValue(mob.Id, out var track) ? track.Sample(now) : new(mob.X, mob.Y);
}
