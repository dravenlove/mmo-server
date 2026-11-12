#include "scene.hpp"
#include <algorithm>
#include <cmath>

namespace mmo {
Json Scene::SkillDefinition::json() const {
    Json out{{"skill_id",id},{"name",name},{"type",kind},{"definition_version",version},
        {"damage",damage},{"mp_cost",mp_cost},{"cooldown_ms",static_cast<int>(std::round(cooldown*1000))},{"range",range}};
    if (kind=="projectile") {out["speed"]=speed;out["radius"]=radius;}
    else {out["length"]=length;out["thickness"]=thickness;out["duration_ms"]=static_cast<int>(std::round(duration*1000));}
    return out;
}

Scene::SkillDefinition Scene::ability_definition(const Entity& entity,const std::string& id) const {
    if (config_.skill_lab_enabled) {
        auto it=entity.skill_definitions.find(id);
        if (it==entity.skill_definitions.end()) throw Error(400,"skill_not_initialized");
        return it->second;
    }
    SkillDefinition def;def.id=id;
    if (id=="arc_bolt") {def.name="奥术弹";def.kind="projectile";}
    else if (id=="stone_wall") {def.name="石墙";def.kind="wall";def.damage=0;def.mp_cost=20;def.cooldown=8;def.range=8;}
    else throw Error(400,"unknown ability skill");
    return def;
}

Json Scene::initialize_skill(Entity& entity,const Json& request) {
    auto reject=[](const char* reason){return Json{{"accepted",false},{"reason",reason}};};
    if (!config_.skill_lab_enabled) return reject("skill_lab_disabled");
    try {
        if (request.value("world_epoch",std::string())!=world_epoch_) return reject("world_changed");
        if (!request.contains("skill_id") || !request["skill_id"].is_string()) return reject("invalid_skill_id");
        SkillDefinition def;def.id=request["skill_id"].get<std::string>();
        if (def.id.empty() || def.id.size()>32 || def.id.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.-")!=std::string::npos) return reject("invalid_skill_id");
        const auto& value=request.at("definition");
        if (!value.is_object() || value.dump().size()>2048) return reject("invalid_definition");
        def.kind=value.at("type").get<std::string>();
        if (def.kind!="projectile" && def.kind!="wall") return reject("unknown_behavior");
        std::set<std::string> fields{"type","name","damage","mp_cost","cooldown_ms","range"};
        if (def.kind=="projectile") {fields.insert("speed");fields.insert("radius");}
        else {fields.insert("length");fields.insert("thickness");fields.insert("duration_ms");}
        for (auto it=value.begin();it!=value.end();++it) if (!fields.count(it.key())) return reject("unknown_parameter");
        auto numeric=[&](const char* key,double low,double high,bool integer=false){
            if (!value.contains(key) || !value[key].is_number()) throw Error(400,key);
            double n=value[key].get<double>();
            if (!std::isfinite(n) || n<low || n>high || (integer && std::floor(n)!=n)) throw Error(400,key);
            return n;
        };
        def.name=value.value("name",def.id);
        if (def.name.empty() || def.name.size()>64) return reject("invalid_definition");
        def.damage=static_cast<int>(numeric("damage",0,1000,true));
        def.mp_cost=static_cast<int>(numeric("mp_cost",0,100,true));
        def.cooldown=numeric("cooldown_ms",50,60000,true)/1000;
        def.range=numeric("range",.5,def.kind=="wall"?20:50);
        if (def.kind=="projectile") {def.speed=numeric("speed",1,100);def.radius=numeric("radius",.05,2);}
        else {
            if (def.damage!=0) return reject("invalid_parameter");
            def.length=numeric("length",.5,12);def.thickness=numeric("thickness",.1,3);
            def.duration=numeric("duration_ms",100,20000,true)/1000;
        }
        auto previous=entity.skill_definitions.find(def.id);
        if (previous!=entity.skill_definitions.end()) {
            auto old=previous->second.json(),next=def.json();old.erase("definition_version");next.erase("definition_version");
            if (old==next) return {{"accepted",true},{"reason",""},{"definition",previous->second.json()},{"unchanged",true}};
            if (request.value("expected_definition_version",std::uint64_t(0))!=previous->second.version) return reject("definition_conflict");
            if (entity.skill_cooldowns[def.id]>1e-7) return reject("cooldown_active");
            for (const auto& item:projectiles_) if (item.second.owner_id==entity.role.role_id && item.second.skill_id==def.id) return reject("skill_in_use");
            for (const auto& item:walls_) if (item.second.owner_id==entity.role.role_id && item.second.skill_id==def.id) return reject("skill_in_use");
        } else {
            if (request.value("expected_definition_version",std::uint64_t(0))!=0) return reject("definition_conflict");
            if (entity.skill_definitions.size()>=16) return reject("skill_limit");
        }
        def.version=++next_skill_definition_id_;
        entity.skill_definitions[def.id]=def;entity.skill_cooldowns[def.id]=0;
        return {{"accepted",true},{"reason",""},{"definition",def.json()},{"unchanged",false}};
    } catch (const Json::exception&) {return reject("invalid_definition");}
      catch (const Error& e) {return {{"accepted",false},{"reason","invalid_parameter"},{"field",e.what()}};}
}
} // namespace mmo
