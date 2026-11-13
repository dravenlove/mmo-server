-- 可信服务端技能编排。输入为C++校验过的定义与场景上下文，不接受客户端Lua。
-- Lua只提交生成意图；MP/CD/动作序号、数值、碰撞和最终结算仍由C++统一提交。
return {
    plan = function(ctx)
        if ctx.definition.type == "projectile" then
            host.emit("projectile", ctx.skill_id)
        elseif ctx.definition.type == "wall" then
            host.emit("wall", ctx.skill_id)
        else
            host.deny("unsupported_behavior")
        end
    end
}
