#include "scene.hpp"
#include <algorithm>
#include <cmath>
#include <limits>

namespace mmo {
namespace {
constexpr double player_radius = .45, npc_radius = .6;
constexpr const char* rules = "projectile-wall-v1";

double number(const Json& j, const char* name, double maximum) {
    if (!j.contains(name) || !j[name].is_number()) throw Error(400, std::string("invalid ") + name);
    const double v = j[name].get<double>();
    if (!std::isfinite(v) || std::abs(v) > maximum) throw Error(400, std::string("invalid ") + name);
    return v;
}

// 连续线段与扩张矩形相交，防止快速弹道或单 Tick 移动穿墙。
double box_hit(double x, double y, double dx, double dy, double cx, double cy,
               double nx, double ny, double radius, double half_length=3, double half_thickness=.3) {
    const double p[] = {(x-cx)*nx + (y-cy)*ny, -(x-cx)*ny + (y-cy)*nx};
    const double d[] = {dx*nx + dy*ny, -dx*ny + dy*nx};
    const double h[] = {half_thickness+radius, half_length+radius};
    double low = 0, high = 1;
    for (int i=0; i<2; ++i) {
        if (std::abs(d[i]) < 1e-12) { if (std::abs(p[i]) > h[i]) return 2; }
        else {
            double a=(-h[i]-p[i])/d[i], b=(h[i]-p[i])/d[i];
            if (a>b) std::swap(a,b);
            low=std::max(low,a); high=std::min(high,b);
            if (low>high) return 2;
        }
    }
    return low;
}

double circle_hit(double x, double y, double dx, double dy, double cx, double cy, double radius) {
    const double px=x-cx, py=y-cy, c=px*px+py*py-radius*radius;
    if (c<=0) return 0;
    const double a=dx*dx+dy*dy, b=px*dx+py*dy, discriminant=b*b-a*c;
    if (a<1e-12 || discriminant<0) return 2;
    const double t=(-b-std::sqrt(discriminant))/a;
    return t>=0 && t<=1 ? t : 2;
}
}

Json Scene::Projectile::json() const {
    return {{"id",id},{"owner_id",owner_id},{"skill_id",skill_id},{"definition_version",definition_version},{"x",x},{"y",y},
        {"dx",dx},{"dy",dy},{"speed",speed},{"radius",radius},{"remaining_range",remaining}};
}
Json Scene::Wall::json() const {
    return {{"id",id},{"owner_id",owner_id},{"skill_id",skill_id},{"definition_version",definition_version},{"x",x},{"y",y},
        {"nx",nx},{"ny",ny},{"length",length},{"thickness",thickness},
        {"remaining_ms",static_cast<int>(std::ceil(std::max(0.,remaining)*1000-1e-7))}};
}

bool Scene::valid_object_owner(const std::string& id, std::int64_t owner, std::int64_t session) const {
    const auto it=entities_.find(id);
    return it!=entities_.end() && it->second.mode==Mode::Active && it->second.connected && it->second.lease_age<=3 &&
        !it->second.role.combat.dead && it->second.role.owner_epoch==owner && it->second.role.session_epoch==session;
}

void Scene::clear_abilities(const std::string& owner) {
    for (auto it=projectiles_.begin(); it!=projectiles_.end();) {
        if (it->second.owner_id!=owner) {++it;continue;}
        const auto& p=it->second;
        emit("projectile_expire",owner,"",p.skill_id,0,p.x,p.y,p.id);
        it=projectiles_.erase(it);
    }
    for (auto it=walls_.begin(); it!=walls_.end();) {
        if (it->second.owner_id!=owner) {++it;continue;}
        const auto& w=it->second;
        emit("wall_expire",owner,"",w.skill_id,0,w.x,w.y,w.id);
        it=walls_.erase(it);
    }
}

std::pair<double,double> Scene::wall_move(double x, double y, double nx, double ny, double radius) const {
    double first=1;
    for (const auto& item:walls_) {
        const auto& w=item.second;
        if (!valid_object_owner(w.owner_id,w.owner_epoch,w.session_epoch) || w.remaining<=1e-7) continue;
        // 重连可能出现在别人刚生成的墙内，允许朝墙外离开，不让角色困住。
        const double start=std::max(std::abs((x-w.x)*w.nx+(y-w.y)*w.ny)/(w.thickness/2+radius),
            std::abs(-(x-w.x)*w.ny+(y-w.y)*w.nx)/(w.length/2+radius));
        const double end=std::max(std::abs((nx-w.x)*w.nx+(ny-w.y)*w.ny)/(w.thickness/2+radius),
            std::abs(-(nx-w.x)*w.ny+(ny-w.y)*w.nx)/(w.length/2+radius));
        if (start<1 && end>start) continue;
        first=std::min(first,box_hit(x,y,nx-x,ny-y,w.x,w.y,w.nx,w.ny,radius,w.length/2,w.thickness/2));
    }
    if (first<1) first=std::max(0.,first-1e-6/std::max(1e-9,std::hypot(nx-x,ny-y)));
    return {x+(nx-x)*first,y+(ny-y)*first};
}

Json Scene::cast_ability(Entity& entity, const Json& request) {
    const auto& skill=request.at("skill_id");
    if (!skill.is_string()) throw Error(400,"invalid skill id");
    const auto id=skill.get<std::string>();
    if (config_.skill_lab_enabled && request.value("world_epoch",std::string())!=world_epoch_) return {{"accepted",false},{"reason","world_changed"}};
    if (config_.skill_lab_enabled && !entity.skill_definitions.count(id)) return {{"accepted",false},{"reason","skill_not_initialized"}};
    const auto definition=ability_definition(entity,id);
    const bool wall=definition.kind=="wall";
    if (config_.skill_lab_enabled && request.value("definition_version",std::uint64_t(0))!=definition.version) return {{"accepted",false},{"reason","definition_changed"}};
    if (!wall && (request.contains("x") || request.contains("y"))) throw Error(400,"projectile origin is server controlled");
    if (!request.contains("seq") || !request["seq"].is_number_integer() || request["seq"].get<double>()<1 ||
        request["seq"].get<double>()>9007199254740991.) throw Error(400,"invalid action seq");
    const auto seq=request["seq"].get<std::int64_t>();
    double dx=number(request,"dx",1), dy=number(request,"dy",1);
    const double length=std::hypot(dx,dy);
    if (length<1e-6) throw Error(400,"aim direction must be nonzero");
    dx/=length; dy/=length;
    const double x=wall?number(request,"x",100):entity.role.x;
    const double y=wall?number(request,"y",100):entity.role.y;
    if (request.contains("target_id") && (!request["target_id"].is_string() || request["target_id"]!=""))
        throw Error(400,"ability skills do not accept target_id");
    Json intent{{"skill_id",skill},{"dx",request["dx"]},{"dy",request["dy"]}};
    intent["definition_version"]=definition.version;
    if (wall) {intent["x"]=x;intent["y"]=y;}
    const auto key=intent.dump();
    auto reject=[&](const char* reason,int retry=0) -> Json {
        return {{"accepted",false},{"seq",seq},{"skill_id",skill},{"reason",reason},{"retry_after_ms",retry},
            {"role",entity.role.json()},{"tick",tick_},{"world_epoch",world_epoch_},{"ability_rules_version",config_.skill_lab_enabled?"parameterized-skills-v1":rules}};
    };
    auto& combat=entity.role.combat;
    if (seq<=combat.last_action_seq) {
        if (seq==combat.last_action_seq && entity.last_action_key==key && !entity.last_action_result.is_null()) return entity.last_action_result;
        return reject("stale_action_seq");
    }
    if (combat.dead) return reject("dead");
    auto& cooldown=config_.skill_lab_enabled?entity.skill_cooldowns.at(id):combat.cooldowns.at(id);
    if (cooldown>1e-7) return reject("cooldown",static_cast<int>(std::ceil(cooldown*1000-1e-7)));
    const int cost=definition.mp_cost;
    if (combat.mp+1e-7<cost) return reject("not_enough_mana");
    std::size_t own=0;
    if (wall) {
        if (std::hypot(x-entity.role.x,y-entity.role.y)>definition.range+1e-7) return reject("out_of_range");
        const double half_length=definition.length/2,half_thickness=definition.thickness/2;
        if (std::abs(x)+std::abs(dx)*half_thickness+std::abs(dy)*half_length>100 || std::abs(y)+std::abs(dy)*half_thickness+std::abs(dx)*half_length>100) return reject("out_of_bounds");
        for (const auto& item:walls_) if (item.second.owner_id==entity.role.role_id) ++own;
        if (walls_.size()>=8 || own>=2) return reject("object_limit");
        if (box_hit(0,0,0,0,x,y,dx,dy,.65,half_length,half_thickness)<=1) return reject("spawn_blocked");
        for (const auto& item:entities_) {
            const auto& other=item.second;
            if (other.mode==Mode::Active && other.connected && other.lease_age<=3 && !other.role.combat.dead &&
                box_hit(other.role.x,other.role.y,0,0,x,y,dx,dy,player_radius,half_length,half_thickness)<=1) return reject("occupied");
        }
        for (const auto& item:npcs_) if (item.second.hp>0 && box_hit(item.second.x,item.second.y,0,0,x,y,dx,dy,npc_radius,half_length,half_thickness)<=1) return reject("occupied");
        // 两个有向矩形的分离轴检测，拒绝相互重叠的墙。
        for (const auto& item:walls_) {
            const auto& w=item.second; bool separated=false;
            for (const auto axis: {std::pair<double,double>{dx,dy},{-dy,dx},{w.nx,w.ny},{-w.ny,w.nx}}) {
                const auto ax=axis.first, ay=axis.second;
                const double a=half_thickness*std::abs(ax*dx+ay*dy)+half_length*std::abs(-ax*dy+ay*dx);
                const double b=w.thickness/2*std::abs(ax*w.nx+ay*w.ny)+w.length/2*std::abs(-ax*w.ny+ay*w.nx);
                if (std::abs((x-w.x)*ax+(y-w.y)*ay)>a+b+1e-6) separated=true;
            }
            if (!separated) return reject("wall_overlap");
        }
    } else {
        for (const auto& item:projectiles_) if (item.second.owner_id==entity.role.role_id) ++own;
        if (projectiles_.size()>=32 || own>=4) return reject("object_limit");
    }
    if (config_.ability_planner) {
        // 计划阶段不允许直接接触组件。异常、拒绝或不完整批次均在扣费前退出。
        try {
            const Json context{{"skill_id", id}, {"definition", definition.json()},
                {"origin", {{"x", entity.role.x}, {"y", entity.role.y}}},
                {"aim", {{"dx", dx}, {"dy", dy}}}, {"target", {{"x", x}, {"y", y}}},
                {"owner", {{"role_id", entity.role.role_id}, {"owner_epoch", entity.role.owner_epoch}, {"session_epoch", entity.role.session_epoch}}},
                {"tick", tick_}, {"world_epoch", world_epoch_}};
            const Json plan = config_.ability_planner(context);
            if (!plan.is_object()) return reject("script_error");
            if (plan.value("denied", false)) return reject("script_denied");
            if (plan.size() != 1 || !plan.contains("effects") || !plan["effects"].is_array() || plan["effects"].size() != 1)
                return reject("script_error");
            const auto& effect = plan["effects"][0];
            if (!effect.is_object() || effect.size() != 2 || effect.value("type", std::string()) != definition.kind ||
                effect.value("skill_id", std::string()) != id) return reject("script_error");
        } catch (...) { return reject("script_error"); }
    }
    combat.mp-=cost; cooldown=definition.cooldown; combat.last_action_seq=seq; ++combat.revision;
    entity.lease_age=0;
    Json result{{"accepted",true},{"seq",seq},{"skill_id",id},{"role",entity.role.json()},{"tick",tick_},
        {"world_epoch",world_epoch_},{"ability_rules_version",config_.skill_lab_enabled?"parameterized-skills-v1":rules},{"config_version",combat_config_["version"]}};
    const auto object_id=(wall?"w-":"p-")+world_epoch_+"-"+std::to_string(++next_object_id_);
    if (wall) {
        Wall w; w.id=object_id; w.owner_id=entity.role.role_id; w.owner_epoch=entity.role.owner_epoch; w.session_epoch=entity.role.session_epoch;
        w.x=x;w.y=y;w.nx=dx;w.ny=dy;
        w.skill_id=id;w.definition_version=definition.version;w.length=definition.length;w.thickness=definition.thickness;w.remaining=definition.duration;
        walls_.emplace(w.id,w);result["wall"]=w.json();
        emit("wall_spawn",w.owner_id,"",id,0,x,y,w.id);
        events_.back()["wall"]=w.json();
    } else {
        Projectile p;p.id=object_id;p.owner_id=entity.role.role_id;p.owner_epoch=entity.role.owner_epoch;p.session_epoch=entity.role.session_epoch;
        p.x=x;p.y=y;p.dx=dx;p.dy=dy;
        p.skill_id=id;p.definition_version=definition.version;p.speed=definition.speed;p.radius=definition.radius;p.damage=definition.damage;p.remaining=definition.range;
        projectiles_.emplace(p.id,p);result["projectile"]=p.json();
        emit("projectile_spawn",p.owner_id,"",id,0,x,y,p.id);
        events_.back()["projectile"]=p.json();
    }
    entity.last_action_key=key;entity.last_action_result=result;
    return result;
}

void Scene::ability_view(Json& result, const Entity& entity) const {
    result["world_epoch"]=world_epoch_;result["ability_rules_version"]=config_.skill_lab_enabled?"parameterized-skills-v1":rules;result["pvp_enabled"]=config_.pvp_enabled;
    result["skills"]=Json::array({
        {{"id","arc_bolt"},{"name","奥术弹"},{"target","direction"},{"damage",28},{"mp_cost",10},{"cooldown_ms",1200},{"range",24},{"speed",18},{"radius",.25}},
        {{"id","stone_wall"},{"name","石墙"},{"target","point_direction"},{"damage",0},{"mp_cost",20},{"cooldown_ms",8000},{"range",8},{"length",6},{"thickness",.6},{"duration_ms",6000}}
    });
    if (config_.skill_lab_enabled) {
        result["features"]=Json::array({"parameterized_skills_v1"});result["skills"]=Json::array();result["skill_cooldowns"]=Json::object();
        for (const auto& item:entity.skill_definitions) result["skills"].push_back(item.second.json());
        for (const auto& item:entity.skill_cooldowns) result["skill_cooldowns"][item.first]=static_cast<int>(std::ceil(std::max(0.,item.second)*1000-1e-7));
    }
    result["walls"]=Json::array();result["projectiles"]=Json::array();
    auto visible=[&](double x,double y,double extra){return std::hypot(x-entity.role.x,y-entity.role.y)<=config_.aoi_radius+extra;};
    for (const auto& item:walls_) {
        const auto& w=item.second;
        if (valid_object_owner(w.owner_id,w.owner_epoch,w.session_epoch) && visible(w.x,w.y,std::hypot(w.length,w.thickness)/2)) result["walls"].push_back(w.json());
    }
    for (const auto& item:projectiles_) {
        const auto& p=item.second;
        if (valid_object_owner(p.owner_id,p.owner_epoch,p.session_epoch) && visible(p.x,p.y,p.radius)) result["projectiles"].push_back(p.json());
    }
    result["projectiles_truncated"]=false;result["events_truncated"]=false;
    result["event_cursor"]=next_event_id_;
    // 优先保留完整墙体；临时表现与事件可裁剪，HP/MP 始终依靠权威快照。
    while (result.dump(-1,' ',true).size()>24*1024 && !result["events"].empty()) {
        result["events"].erase(result["events"].begin());result["events_truncated"]=true;
    }
    while (result.dump(-1,' ',true).size()>24*1024 && !result["projectiles"].empty()) {
        result["projectiles"].erase(result["projectiles"].end()-1);result["projectiles_truncated"]=true;
    }
}

void Scene::step_abilities(double dt) {
    for (auto& item:entities_) if (item.second.mode==Mode::Active) for (auto& cooldown:item.second.skill_cooldowns) cooldown.second=std::max(0.,cooldown.second-dt);
    // 归属失效、死亡、断线、租约失效的对象不继续影响新会话。
    for (const auto& item:entities_) if (!valid_object_owner(item.first,item.second.role.owner_epoch,item.second.role.session_epoch)) clear_abilities(item.first);
    for (auto it=projectiles_.begin();it!=projectiles_.end();) {
        auto& p=it->second;
        if (!valid_object_owner(p.owner_id,p.owner_epoch,p.session_epoch)) {it=projectiles_.erase(it);continue;}
        const double travel=std::min(p.remaining,p.speed*dt), vx=p.dx*travel, vy=p.dy*travel;
        double first=2;std::string target,kind;
        // 墙体优先；同距离的目标按稳定 ID 顺序只结算第一个，不穿透。
        for (const auto& item:walls_) {
            const auto& w=item.second;
            if (!valid_object_owner(w.owner_id,w.owner_epoch,w.session_epoch)) continue;
            const double t=box_hit(p.x,p.y,vx,vy,w.x,w.y,w.nx,w.ny,p.radius,w.length/2,w.thickness/2);
            if (t<=1 && t<first) {first=t;target=w.id;kind="wall";}
        }
        for (const auto& item:npcs_) if (item.second.hp>0) {
            const double t=circle_hit(p.x,p.y,vx,vy,item.second.x,item.second.y,npc_radius+p.radius);
            if (t<=1 && t<first-1e-9) {first=t;target=item.first;kind="npc";}
        }
        if (config_.pvp_enabled) for (const auto& item:entities_) {
            const auto& e=item.second;
            if (item.first==p.owner_id || !valid_object_owner(item.first,e.role.owner_epoch,e.role.session_epoch)) continue;
            const double t=circle_hit(p.x,p.y,vx,vy,e.role.x,e.role.y,player_radius+p.radius);
            if (t<=1 && t<first-1e-9) {first=t;target=item.first;kind="player";}
        }
        if (first<=1) {
            p.x+=vx*first;p.y+=vy*first;
            if (kind=="npc") {
                Json hits=Json::array();damage_npc(entities_.at(p.owner_id),npcs_.at(target),p.skill_id,p.damage,hits);
            } else if (kind=="player") {
                auto& e=entities_.at(target);auto& c=e.role.combat;const int damage=std::min(c.hp,p.damage);
                c.hp-=damage;++c.revision;
                emit("damage",p.owner_id,target,p.skill_id,damage,e.role.x,e.role.y,p.id);
                if (!c.hp) {c.dead=true;c.revive_remaining=5;e.dx=e.dy=0;emit("death",p.owner_id,target,p.skill_id,0,e.role.x,e.role.y,p.id);}
            }
            emit(kind=="wall"?"projectile_blocked":"projectile_hit",p.owner_id,target,p.skill_id,0,p.x,p.y,p.id);
            it=projectiles_.erase(it);
        } else {
            p.x+=vx;p.y+=vy;p.remaining=std::max(0.,p.remaining-travel);
            if (p.remaining<=1e-7 || std::abs(p.x)>100 || std::abs(p.y)>100) {
                emit("projectile_expire",p.owner_id,"",p.skill_id,0,p.x,p.y,p.id);it=projectiles_.erase(it);
            } else ++it;
        }
    }
    for (auto it=walls_.begin();it!=walls_.end();) {
        auto& w=it->second;w.remaining=std::max(0.,w.remaining-dt);
        if (w.remaining<=1e-7 || !valid_object_owner(w.owner_id,w.owner_epoch,w.session_epoch)) {
            emit("wall_expire",w.owner_id,"",w.skill_id,0,w.x,w.y,w.id);it=walls_.erase(it);
        } else ++it;
    }
}
} // namespace mmo
