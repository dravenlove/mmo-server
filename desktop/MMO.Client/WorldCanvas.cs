using System.Globalization;
using System.Windows;
using System.Windows.Media;
using Mmo.Client.Core;

namespace Mmo.Client.Desktop;

public sealed class WorldCanvas : FrameworkElement
{
    private WorldView? _view;
    public void Update(WorldView? view) { _view = view; InvalidateVisual(); }

    protected override void OnRender(DrawingContext dc)
    {
        base.OnRender(dc);
        var bounds = new Rect(0, 0, ActualWidth, ActualHeight);
        dc.DrawRoundedRectangle(new SolidColorBrush(Color.FromRgb(9, 18, 32)), null, bounds, 8, 8);
        if (ActualWidth < 60 || ActualHeight < 60) return;
        double scale = Math.Min((ActualWidth - 50) / 200, (ActualHeight - 50) / 200);
        Point Map(double x, double y) => new(ActualWidth / 2 + x * scale, ActualHeight / 2 - y * scale);
        var grid = new Pen(new SolidColorBrush(Color.FromRgb(30, 46, 67)), 1);
        var axis = new Pen(new SolidColorBrush(Color.FromRgb(52, 73, 98)), 1);
        for (int n = -100; n <= 100; n += 20)
        {
            dc.DrawLine(n == 0 ? axis : grid, Map(n, -100), Map(n, 100));
            dc.DrawLine(n == 0 ? axis : grid, Map(-100, n), Map(100, n));
        }
        var a = Map(-100, 100); var b = Map(100, -100);
        dc.DrawRectangle(null, axis, new Rect(a, b));
        Text(dc, "(-100, 100)", a + new Vector(4, 4), Color.FromRgb(98, 123, 155), 10);
        Text(dc, "(100, -100)", b + new Vector(-82, -18), Color.FromRgb(98, 123, 155), 10);
        if (_view is null)
        {
            Text(dc, "连接角色后显示场景", new Point(Math.Max(15, ActualWidth / 2 - 75), ActualHeight / 2 - 12), Color.FromRgb(122, 147, 179), 14);
            return;
        }
        var self = _view.Self;
        var center = Map(self.X, self.Y);
        dc.DrawEllipse(new SolidColorBrush(Color.FromArgb(18, 74, 222, 172)),
            new Pen(new SolidColorBrush(Color.FromArgb(110, 74, 222, 172)), 1), center, 30 * scale, 30 * scale);
        int labelIndex = 0;
        foreach (var role in _view.Players.Where(p => p.RoleId != self.RoleId))
            DrawPlayer(dc, role, Map(role.X, role.Y), false, 10 + (labelIndex++ % 3) * 15);
        DrawPlayer(dc, self, center, true);
    }

    private void DrawPlayer(DrawingContext dc, Role role, Point point, bool self, double labelOffset = -22)
    {
        var color = self ? Color.FromRgb(74, 222, 172) : Color.FromRgb(100, 168, 255);
        dc.DrawEllipse(new SolidColorBrush(Color.FromArgb(32, color.R, color.G, color.B)), null, point, 15, 15);
        dc.DrawEllipse(new SolidColorBrush(color), new Pen(Brushes.White, 1.4), point, self ? 6 : 5, self ? 6 : 5);
        Text(dc, role.Name + (self ? " · 自己" : ""), point + new Vector(11, labelOffset), color, 12);
    }

    private void Text(DrawingContext dc, string text, Point point, Color color, double size)
    {
        var formatted = new FormattedText(text, CultureInfo.CurrentCulture, FlowDirection.LeftToRight,
            new Typeface("Microsoft YaHei UI"), size, new SolidColorBrush(color), VisualTreeHelper.GetDpi(this).PixelsPerDip);
        dc.DrawText(formatted, point);
    }
}
