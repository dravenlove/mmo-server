using System.Globalization;
using System.Windows;
using System.Windows.Input;
using System.Windows.Media;
using System.Windows.Threading;
using Mmo.Client.Core;

namespace Mmo.Client.Desktop;

public sealed class WorldCanvas : FrameworkElement
{
    private sealed record CombatVisualItem(CombatEvent Event, DateTime Born);
    private readonly List<CombatVisualItem> _effects = [];
    private readonly DispatcherTimer _animation = new() { Interval = TimeSpan.FromMilliseconds(50) };
    private WorldView? _view;
    private long _eventCursor;
    private string _eventScene = "";
    private ulong _previousTick;
    private bool _haveEventBaseline;
    private double _cameraX, _cameraY, _scale;
    private double _facingX = 1, _facingY;
    public string? SelectedTargetId { get; private set; }
    public event Action<string?>? TargetSelected;
    public event Action<CombatEvent>? EventObserved;

    public WorldCanvas()
    {
        ClipToBounds = true;
        Cursor = Cursors.Cross;
        _animation.Tick += (_, _) =>
        {
            _effects.RemoveAll(effect => (DateTime.UtcNow - effect.Born).TotalSeconds > 1.2);
            if (_view is not null || _effects.Count > 0) InvalidateVisual();
        };
        Loaded += (_, _) => _animation.Start();
        Unloaded += (_, _) => _animation.Stop();
    }

    public void Update(WorldView? view)
    {
        if (view is null)
        {
            _view = null; _effects.Clear(); _haveEventBaseline = false; _eventCursor = 0;
            SelectTarget(null); InvalidateVisual(); return;
        }
        bool changedScene = _eventScene != view.SceneId || view.Tick < _previousTick;
        if (changedScene)
        {
            _haveEventBaseline = false; _eventCursor = 0; _effects.Clear(); SelectTarget(null);
        }
        if (_view is not null && _view.SceneId == view.SceneId)
        {
            double dx = view.Self.X - _view.Self.X, dy = view.Self.Y - _view.Self.Y;
            if (Math.Abs(dx) + Math.Abs(dy) > .02)
            {
                double length = Math.Sqrt(dx * dx + dy * dy);
                _facingX = dx / length; _facingY = dy / length;
            }
        }
        _view = view; _eventScene = view.SceneId; _previousTick = view.Tick;
        var events = view.Events ?? [];
        if (!_haveEventBaseline)
        {
            // 进场只建立事件游标，历史命中不能重新播放成当前伤害。
            _eventCursor = events.Count == 0 ? 0 : events.Max(item => item.Id);
            _haveEventBaseline = true;
        }
        else
        {
            foreach (var item in events.Where(item => item.Id > _eventCursor).OrderBy(item => item.Id))
            {
                _effects.Add(new CombatVisualItem(item, DateTime.UtcNow));
                _eventCursor = item.Id;
                EventObserved?.Invoke(item);
            }
            if (_effects.Count > 64) _effects.RemoveRange(0, _effects.Count - 64);
        }
        if (SelectedTargetId is not null && !(view.Mobs ?? []).Any(mob => mob.Id == SelectedTargetId)) SelectTarget(null);
        InvalidateVisual();
    }

    public void SelectTarget(string? id)
    {
        if (SelectedTargetId == id) return;
        SelectedTargetId = id; TargetSelected?.Invoke(id); InvalidateVisual();
    }

    public void CycleTarget()
    {
        if (_view is null) return;
        var mobs = (_view.Mobs ?? []).Where(mob => mob.Alive)
            .OrderBy(mob => DistanceSquared(mob.X, mob.Y, _view.Self.X, _view.Self.Y)).ToArray();
        if (mobs.Length == 0) { SelectTarget(null); return; }
        int current = Array.FindIndex(mobs, mob => mob.Id == SelectedTargetId);
        SelectTarget(mobs[(current + 1) % mobs.Length].Id);
    }

    protected override void OnMouseDown(MouseButtonEventArgs e)
    {
        base.OnMouseDown(e); Focus();
        if (e.ChangedButton != MouseButton.Left || _view is null) return;
        var click = e.GetPosition(this);
        var candidate = (_view.Mobs ?? []).Select(mob => (Mob: mob, Point: Map(mob.X, mob.Y)))
            .OrderBy(item => (item.Point - click).LengthSquared).FirstOrDefault();
        SelectTarget(candidate.Mob is not null && (candidate.Point - click).Length <= 26 ? candidate.Mob.Id : null);
    }

    protected override void OnRender(DrawingContext dc)
    {
        base.OnRender(dc);
        var bounds = new Rect(0, 0, ActualWidth, ActualHeight);
        dc.DrawRoundedRectangle(Brush("#0B1523"), null, bounds, 8, 8);
        if (ActualWidth < 60 || ActualHeight < 60) return;
        _cameraX = Math.Clamp(_view?.Self.X ?? 0, -65, 65);
        _cameraY = Math.Clamp(_view?.Self.Y ?? 0, -65, 65);
        _scale = Math.Min((ActualWidth - 34) / 72, (ActualHeight - 34) / 58);
        var grid = new Pen(Brush("#172B3B"), 1);
        for (int n = -100; n <= 100; n += 5)
        {
            var line = n % 20 == 0 ? new Pen(Brush("#213D4B"), 1) : grid;
            dc.DrawLine(line, Map(n, -100), Map(n, 100));
            dc.DrawLine(line, Map(-100, n), Map(100, n));
        }
        DrawArenaDecoration(dc);
        Text(dc, _view is null ? "战斗试炼场" : _view.SceneId == "scene-1" ? "第一试炼 · 狼群营地" : "第二试炼 · 守卫前哨", new Point(16, 12), "#8EACB6", 13);
        Text(dc, "世界范围 -100 ～ 100  /  服务器权威判定", new Point(16, ActualHeight - 24), "#5D7D8F", 10);
        if (_view is null)
        {
            Text(dc, "连接角色，开始试炼", new Point(Math.Max(20, ActualWidth / 2 - 88), ActualHeight / 2 - 12), "#97ADBF", 17);
            return;
        }
        var self = _view.Self;
        var selected = (_view.Mobs ?? []).FirstOrDefault(mob => mob.Id == SelectedTargetId);
        if (selected is not null)
        {
            var line = new Pen(Brush("#7B6D46"), 1) { DashStyle = DashStyles.Dash };
            dc.DrawLine(line, Map(self.X, self.Y), Map(selected.X, selected.Y));
            dc.DrawEllipse(null, new Pen(Brush("#314D5E"), 1), Map(self.X, self.Y), 3.2 * _scale, 3.2 * _scale);
        }
        foreach (var mob in (_view.Mobs ?? []).OrderByDescending(mob => mob.Y)) DrawMob(dc, mob);
        int labelIndex = 0;
        foreach (var role in _view.Players.Where(role => role.RoleId != self.RoleId))
            DrawPlayer(dc, role, Map(role.X, role.Y), false, 12 + (labelIndex++ % 3) * 16);
        DrawPlayer(dc, self, Map(self.X, self.Y), true);
        foreach (var effect in _effects) DrawEffect(dc, effect);
        if (self.Combat?.Dead == true)
        {
            dc.DrawRectangle(new SolidColorBrush(Color.FromArgb(110, 12, 10, 20)), null, bounds);
            Text(dc, "你已倒下", new Point(ActualWidth / 2 - 52, ActualHeight / 2 - 28), "#FFB6AF", 26);
            Text(dc, "等待右侧复活计时结束后重新加入战斗", new Point(ActualWidth / 2 - 137, ActualHeight / 2 + 12), "#D1BCC2", 13);
        }
    }

    private Point Map(double x, double y) => new(ActualWidth / 2 + (x - _cameraX) * _scale, ActualHeight / 2 - (y - _cameraY) * _scale);
    private static double DistanceSquared(double x, double y, double a, double b) => (x - a) * (x - a) + (y - b) * (y - b);
    private static SolidColorBrush Brush(string color) => new((Color)ColorConverter.ConvertFromString(color));

    private void DrawArenaDecoration(DrawingContext dc)
    {
        // 简单矢量地表，不依赖贴图和显卡特定资源。
        var center = Map(0, 0);
        dc.DrawEllipse(Brush("#102532"), new Pen(Brush("#244452"), 2), center, 6 * _scale, 6 * _scale);
        dc.DrawEllipse(null, new Pen(Brush("#264551"), 1), center, 4.8 * _scale, 4.8 * _scale);
        Text(dc, "营地", center + new Vector(-14, 4.5 * _scale), "#557C8B", 10);
        foreach (var position in new[] { (-15d, 13d), (22d, -12d), (-19d, -15d), (30d, 20d) })
        {
            var p = Map(position.Item1, position.Item2);
            dc.DrawEllipse(Brush("#102E2D"), null, p, 9, 6);
            dc.DrawEllipse(Brush("#173A33"), null, p + new Vector(3, -4), 5, 9);
        }
    }

    private void DrawMob(DrawingContext dc, Mob mob)
    {
        Point p = Map(mob.X, mob.Y);
        bool selected = mob.Id == SelectedTargetId;
        if (!mob.Alive)
        {
            dc.DrawEllipse(Brush("#302D31"), null, p + new Vector(0, 5), 14, 6);
            dc.DrawLine(new Pen(Brush("#756268"), 2), p + new Vector(-5, -3), p + new Vector(5, 7));
            dc.DrawLine(new Pen(Brush("#756268"), 2), p + new Vector(5, -3), p + new Vector(-5, 7));
            Text(dc, $"{mob.Name} · 刷新 {Math.Ceiling(mob.RespawnRemainingMs / 1000)}s", p + new Vector(-42, 17), "#8C7D86", 10);
            return;
        }
        if (selected)
        {
            double pulse = 20 + Math.Sin(DateTime.UtcNow.Ticks / (double)TimeSpan.TicksPerSecond * 5) * 2;
            dc.DrawEllipse(null, new Pen(Brush("#F4CF78"), 2), p + new Vector(0, 4), pulse, pulse * .58);
            dc.DrawLine(new Pen(Brush("#F4CF78"), 2), p + new Vector(-5, -30), p + new Vector(0, -24));
            dc.DrawLine(new Pen(Brush("#F4CF78"), 2), p + new Vector(5, -30), p + new Vector(0, -24));
        }
        dc.DrawEllipse(Brush("#060C12"), null, p + new Vector(0, 9), 14, 5);
        bool elite = mob.Kind.Contains("elite", StringComparison.OrdinalIgnoreCase) || mob.MaxHp >= 200;
        if (elite)
        {
            Polygon(dc, "#8E6558", new Pen(Brush("#D9AA79"), 1.5), p + new Vector(-10, -9), p + new Vector(9, -9), p + new Vector(13, 10), p + new Vector(-12, 10));
            dc.DrawEllipse(Brush("#BDA38C"), null, p + new Vector(0, -15), 7, 8);
            dc.DrawLine(new Pen(Brush("#9AABBE"), 3), p + new Vector(17, -11), p + new Vector(17, 12));
            Polygon(dc, "#506478", new Pen(Brush("#A8BACA"), 1), p + new Vector(-17, -5), p + new Vector(-8, -3), p + new Vector(-10, 10), p + new Vector(-17, 5));
        }
        else
        {
            Polygon(dc, "#8E8791", new Pen(Brush("#C8C0C8"), 1), p + new Vector(-13, 6), p + new Vector(-9, -5), p + new Vector(4, -8), p + new Vector(12, -2), p + new Vector(14, 6), p + new Vector(5, 9));
            Polygon(dc, "#BBB0BB", null, p + new Vector(-6, -5), p + new Vector(-4, -17), p + new Vector(1, -7), p + new Vector(6, -17), p + new Vector(10, -3));
            dc.DrawLine(new Pen(Brush("#C04F54"), 2), p + new Vector(2, -6), p + new Vector(7, -6));
            dc.DrawLine(new Pen(Brush("#AAA0AC"), 2), p + new Vector(-10, 5), p + new Vector(-12, 12));
            dc.DrawLine(new Pen(Brush("#AAA0AC"), 2), p + new Vector(9, 5), p + new Vector(11, 12));
        }
        Text(dc, mob.Name + (elite ? " ◆" : ""), p + new Vector(-32, -48), selected ? "#FFE1A0" : "#D8B9BB", 11);
        Bar(dc, p + new Vector(-25, -31), 50, 5, mob.Hp, mob.MaxHp, elite ? "#C99651" : "#BA5E65");
    }

    private void DrawPlayer(DrawingContext dc, Role role, Point point, bool self, double labelOffset = -35)
    {
        bool dead = role.Combat?.Dead == true;
        string color = dead ? "#6C7888" : self ? "#4ADEAC" : "#64A8FF";
        dc.DrawEllipse(Brush("#060C12"), null, point + new Vector(0, 10), 13, 5);
        if (dead)
        {
            dc.DrawEllipse(Brush("#41515C"), null, point + new Vector(0, 5), 12, 5);
        }
        else
        {
            Polygon(dc, color, new Pen(Brush("#C5F5E7"), 1), point + new Vector(-8, -5), point + new Vector(7, -5), point + new Vector(10, 10), point + new Vector(-10, 10));
            dc.DrawEllipse(Brush("#D8C7B8"), new Pen(Brush("#455D6E"), 2), point + new Vector(0, -13), 6, 7);
            Vector direction = self ? new Vector(_facingX, -_facingY) : new Vector(1, -.2);
            dc.DrawLine(new Pen(Brush("#D4E5F1"), 2.5), point + direction * 8, point + direction * 24);
            dc.DrawEllipse(null, new Pen(Brush(color), 1), point + new Vector(0, 6), 17, 9);
        }
        Text(dc, role.Name + (self ? " · 自己" : ""), point + new Vector(12, labelOffset), color, 11);
        if (role.Combat is { } combat) Bar(dc, point + new Vector(-22, -26), 44, 4, combat.Hp, combat.MaxHp, color);
    }

    private void DrawEffect(DrawingContext dc, CombatVisualItem effect)
    {
        double age = (DateTime.UtcNow - effect.Born).TotalSeconds;
        double progress = Math.Clamp(age / 1.1, 0, 1);
        var item = effect.Event; var point = Map(item.X, item.Y);
        dc.PushOpacity(1 - progress);
        if (item.Type == "cast")
        {
            string color = item.SkillId switch { "mend" => "#64E0AB", "whirlwind" => "#B9A2FF", "charge" => "#70BCFF", _ => "#FFD087" };
            double radius = item.SkillId == "whirlwind" ? 5 * _scale * (.3 + progress) : 10 + progress * 24;
            dc.DrawEllipse(null, new Pen(Brush(color), 3 * (1 - progress) + 1), point, radius, radius * .75);
            if (item.SkillId == "strike")
                dc.DrawLine(new Pen(Brush(color), 3), point + new Vector(-20, 14 - progress * 32), point + new Vector(20, -14 + progress * 32));
        }
        else
        {
            string text = item.Type switch
            {
                "damage" => "-" + item.Value,
                "heal" => "+" + item.Value,
                "death" => item.TargetId == _view?.Self.RoleId ? "倒下" : "击败",
                "revive" => "复活",
                "respawn" => "刷新",
                _ => ""
            };
            string color = item.Type == "heal" || item.Type == "revive" ? "#7DF4BB" : item.TargetId == _view?.Self.RoleId ? "#FF8D94" : "#FFD688";
            if (text.Length != 0) Text(dc, text, point + new Vector(-12, -36 - progress * 40), color, item.Type == "damage" ? 19 : 15);
        }
        dc.Pop();
    }

    private static void Bar(DrawingContext dc, Point point, double width, double height, long value, long max, string color)
    {
        dc.DrawRoundedRectangle(Brush("#18202C"), new Pen(Brush("#0A1019"), 1), new Rect(point.X, point.Y, width, height), 2, 2);
        if (max > 0 && value > 0) dc.DrawRoundedRectangle(Brush(color), null, new Rect(point.X, point.Y, width * Math.Clamp(value / (double)max, 0, 1), height), 2, 2);
    }

    private static void Polygon(DrawingContext dc, string color, Pen? pen, params Point[] points)
    {
        var geometry = new StreamGeometry();
        using (var context = geometry.Open())
        {
            context.BeginFigure(points[0], true, true);
            context.PolyLineTo(points.Skip(1).ToArray(), true, false);
        }
        dc.DrawGeometry(Brush(color), pen, geometry);
    }

    private void Text(DrawingContext dc, string text, Point point, string color, double size)
    {
        var formatted = new FormattedText(text, CultureInfo.CurrentCulture, FlowDirection.LeftToRight,
            new Typeface("Microsoft YaHei UI"), size, Brush(color), VisualTreeHelper.GetDpi(this).PixelsPerDip);
        dc.DrawText(formatted, point);
    }
}
