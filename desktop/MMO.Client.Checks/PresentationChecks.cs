using Mmo.Client.Core;

internal static partial class Program
{
    private static WorldView MotionView(double x, ulong tick, string scene = "scene-1", long session = 1,
        CombatState? combat = null, IReadOnlyList<Mob>? mobs = null, IReadOnlyList<CombatEvent>? events = null)
    {
        var role = new Role("hero", "motion-check", scene, 1, session, x, 0, 0, combat);
        return new(role, [role], tick, scene, mobs, Events: events);
    }

    private static void CheckWorldPresentation()
    {
        var display = new WorldPresentation();
        display.Update(MotionView(0, 0), 0);
        int distinct = 0; double previous = 0, largestStep = 0;
        for (int frame = 1; frame <= 72; ++frame)
        {
            double now = frame / 60d;
            if (frame % 12 == 0) display.Update(MotionView(frame / 10d, (ulong)(frame / 3)), now);
            double x = display.Player(MotionView(0, 0).Self, now).X;
            if (Math.Abs(x - previous) > 1e-7) ++distinct;
            largestStep = Math.Max(largestStep, Math.Abs(x - previous)); previous = x;
        }
        Require(distinct >= 55 && largestStep < .2, $"5Hz快照未形成连续画面：变化帧{distinct}，最大步长{largestStep}");
        Pass("5Hz authoritative snapshots produce continuous 60Hz presentation without position jumps");

        display.Clear(); display.Update(MotionView(0, 1), 0);
        var authoritative = MotionView(1.2, 5); display.Update(authoritative, .2);
        var middle = display.Player(authoritative.Self, .3);
        Require(middle.X > .4 && middle.X < .8 && authoritative.Self.X == 1.2,
            "画面插值不能修改权威角色坐标。");
        Pass("presentation positions stay separate from authoritative gameplay state");

        display.Update(MotionView(1.2, 6), .3);
        Require(Math.Abs(display.Player(authoritative.Self, .35).X - .9) < 1e-6,
            "重复坐标更新重启了移动动画。");
        Pass("duplicate snapshots and metadata updates do not restart movement interpolation");

        Require(Math.Abs(display.Player(authoritative.Self, 4).X - 1.2) < 1e-7,
            "缺少新快照时不能无限预测移动。");
        display.Update(MotionView(4, 30), 4);
        Require(display.Player(authoritative.Self, 4).X == 4, "长时间断流后的权威位置应直接校正。");
        Pass("stopped and stalled streams settle at the last confirmed position without extrapolation");

        display.Update(MotionView(-20, 1, "scene-2"), 4.1);
        Require(display.Player(authoritative.Self, 4.1).X == -20, "跨场景不能扫过旧地图。");
        display.Update(MotionView(15, 2, "scene-2", 2), 4.2);
        Require(display.Player(authoritative.Self, 4.2).X == 15, "新登录会话应重建显示基线。");
        display.Update(MotionView(2, 0, "scene-2", 2), 4.3);
        Require(display.Player(authoritative.Self, 4.3).X == 2, "场景重启应清除旧插值。");
        Pass("scene transfer, new sessions and restarted ticks reset presentation immediately");

        var dead = new CombatState(0, 180, 100, 100, true, 5000, 10, 1, 2, new Dictionary<string, double>());
        display.Update(MotionView(3, 2, "scene-2", 2, dead), 4.4);
        Require(display.Player(authoritative.Self, 4.4).X == 3, "死亡状态不应延迟切换。");
        display.Update(MotionView(0, 3, "scene-2", 2, dead with { Hp = 180, Dead = false }), 4.5);
        Require(display.Player(authoritative.Self, 4.5).X == 0, "复活不能从尸体位置缓慢滑回出生点。");
        Pass("death and revival snap to authoritative positions");

        display.Clear(); display.Update(MotionView(0, 1), 0);
        var charge = new CombatEvent(1, "cast", "hero", "npc-1", "charge", 0, 2, 0, 5);
        display.Update(MotionView(2, 5, events: [charge]), .2);
        Require(display.Player(authoritative.Self, .2).X == 2, "突进需要即时显示权威位移。");
        display.Update(MotionView(3.2, 9, events: [charge]), .4);
        Require(display.Player(authoritative.Self, .4).X == 2, "旧突进事件不能重复打断后续插值。");
        Pass("new charge events snap once while retained old events cannot interrupt later movement");

        display.Clear();
        var wolf = new Mob("npc-1", "wolf", "wolf", 0, 0, 80, 80, true, 0, "");
        display.Update(MotionView(0, 1, mobs: [wolf]), 0);
        var moved = wolf with { X = 1.2 };
        display.Update(MotionView(0, 5, mobs: [moved]), .2);
        Require(display.Monster(moved, .3).X is > .4 and < .8, "怪物也应连续移动。");
        display.Update(MotionView(0, 9, mobs: []), .4);
        display.Update(MotionView(0, 13, mobs: [wolf with { X = 5 }]), .6);
        Require(display.Monster(wolf, .6).X == 5, "重新进入AOI的对象不能沿用已移除的旧轨迹。");
        display.Clear();
        Require(display.Player(authoritative.Self, 1).X == authoritative.Self.X, "断开后不能残留显示轨迹。");
        Pass("monsters interpolate, AOI removals clear tracks and disconnect clears all presentation");
    }
}
