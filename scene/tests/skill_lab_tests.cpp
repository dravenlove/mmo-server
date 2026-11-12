#include "scene.hpp"
#include <cmath>
#include <iostream>
#include <stdexcept>
using mmo::Json;
void check(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
Json role(std::string id="a",double x=0,int session=1){return {{"role_id",id},{"name",id},{"scene_id","lab"},{"owner_epoch",1},{"session_epoch",session},{"x",x},{"y",0},{"coins",9}};}
Json ref(std::string id="a",int session=1){return {{"role_id",id},{"owner_epoch",1},{"session_epoch",session}};}
void enter(mmo::Scene& scene,const Json& r){scene.execute("/v1/enter",{{"role",r}});}
Json view(mmo::Scene& scene,std::string id="a",int session=1){return scene.execute("/v1/view",ref(id,session));}
Json definition(){return {{"type","projectile"},{"damage",17},{"mp_cost",3},{"cooldown_ms",300},{"range",40},{"speed",30},{"radius",.1}};}
Json init(mmo::Scene& scene,const std::string& id,const Json& def,std::string owner="a",int session=1){auto r=ref(owner,session);r["skill_id"]=id;r["definition"]=def;auto current=view(scene,owner,session);r["world_epoch"]=current["world_epoch"];for(const auto& d:current["skills"])if(d["skill_id"]==id)r["expected_definition_version"]=d["definition_version"];return scene.execute("/v1/skill_init",r);}
Json cast(mmo::Scene& scene,const std::string& id,std::uint64_t version,int seq=1,int session=1){auto r=ref("a",session);r.update({{"skill_id",id},{"definition_version",version},{"seq",seq},{"dx",1},{"dy",0},{"world_epoch",view(scene,"a",session)["world_epoch"]}});return r;}
int main(){try {
    mmo::Config cfg;cfg.scene_id="lab";cfg.abilities_enabled=cfg.skill_lab_enabled=true;
    mmo::Scene scene(cfg);enter(scene,role());enter(scene,role("b",6));
    check(view(scene)["skills"].empty(),"lab must start without invented skill IDs");
    check(view(scene)["features"][0]=="parameterized_skills_v1","lab feature missing");
    check(scene.execute("/v1/cast",cast(scene,"client_bolt",1))["reason"]=="skill_not_initialized","uninitialized skill was generated");
    auto accepted=init(scene,"client_bolt",definition());check(accepted["accepted"]==true,"client definition rejected");
    const auto version=accepted["definition"]["definition_version"].get<std::uint64_t>();
    check(init(scene,"client_bolt",definition())["definition"]["definition_version"]==version,"same init changed version");
    auto other=definition();other["damage"]=9;other["speed"]=10;
    check(init(scene,"client_bolt",other,"b")["accepted"]==true,"second owner definition rejected");
    check(view(scene,"b")["skills"][0]["damage"]==9 && view(scene)["skills"][0]["damage"]==17,"one owner overwrote another definition");
    auto wrong=cast(scene,"client_bolt",version+100);check(scene.execute("/v1/cast",wrong)["reason"]=="definition_changed","wrong definition version accepted");
    auto request=cast(scene,"client_bolt",version);auto staleWorld=request;staleWorld["world_epoch"]="retired-world";check(scene.execute("/v1/cast",staleWorld)["reason"]=="world_changed","stale world accepted");auto shot=scene.execute("/v1/cast",request);
    check(shot["accepted"]==true && shot["projectile"]["speed"]==30 && shot["projectile"]["radius"]==.1 && shot["projectile"]["remaining_range"]==40,"instance ignored client parameters");
    check(view(scene,"b")["projectiles"][0]["id"]==shot["projectile"]["id"],"second player sees a different instance");
    check(view(scene,"b")["self"]["combat"]["hp"]==180,"cast inflicted instant damage");
    check(view(scene)["self"]["combat"]["mp"]==97,"configured mana cost ignored");
    check(scene.execute("/v1/cast",request)==shot && view(scene)["projectiles"].size()==1,"retry duplicated instance");
    check(scene.execute("/v1/cast",cast(scene,"client_bolt",version,2))["reason"]=="cooldown","configured cooldown ignored");
    auto changed=definition();changed["damage"]=99;
    check(init(scene,"client_bolt",changed)["reason"]=="cooldown_active","reinitialization reset active cooldown");
    scene.step(.2);check(view(scene,"b")["self"]["combat"]["hp"]==163,"configured damage not settled by server");
    check(view(scene)["projectiles"].empty(),"projectile survived impact");
    scene.step(.1);auto updated=init(scene,"client_bolt",changed);
    check(updated["accepted"]==true && updated["definition"]["definition_version"]!=version,"idle definition could not be updated");
    auto staleInit=ref();staleInit.update({{"skill_id","client_bolt"},{"definition",definition()},{"world_epoch",view(scene)["world_epoch"]},{"expected_definition_version",version}});
    check(scene.execute("/v1/skill_init",staleInit)["reason"]=="definition_conflict","stale initialization overwrote newer parameters");
    check(scene.execute("/v1/cast",cast(scene,"client_bolt",version,2))["reason"]=="definition_changed","old definition cast after reinitialization");
    check(view(scene)["self"]["combat"]["cooldowns"].size()==6,"custom IDs contaminated persistent cooldown schema");
    auto invalid=definition();invalid["speed"]=101;check(init(scene,"bad",invalid)["accepted"]==false,"excessive speed accepted");
    invalid=definition();invalid["owner_id"]="b";check(init(scene,"bad",invalid)["reason"]=="unknown_parameter","unknown definition parameter ignored");
    invalid=definition();invalid["type"]="execute_script";check(init(scene,"bad",invalid)["reason"]=="unknown_behavior","unknown behavior accepted");
    for(int i=1;i<16;++i)check(init(scene,"filler_"+std::to_string(i),definition())["accepted"]==true,"valid catalog limit rejected");
    check(init(scene,"seventeenth",definition())["reason"]=="skill_limit","unbounded definition catalog");

    mmo::Scene reconnect(cfg);enter(reconnect,role());auto registered=init(reconnect,"custom",definition());
    auto v=registered["definition"]["definition_version"].get<std::uint64_t>();reconnect.execute("/v1/cast",cast(reconnect,"custom",v));enter(reconnect,role("a",0,2));
    check(init(reconnect,"custom",definition(),"a",2)["unchanged"]==true,"reconnect lost role-scoped definition");
    check(reconnect.execute("/v1/cast",cast(reconnect,"custom",v,1,2))["reason"]=="cooldown","reconnect reset custom cooldown");
    check(view(reconnect,"a",2)["projectiles"].empty(),"new session retained old objects");

    mmo::Scene walls(cfg);enter(walls,role());enter(walls,role("b",6));
    Json barrier{{"type","wall"},{"damage",0},{"mp_cost",0},{"cooldown_ms",50},{"range",12},{"length",2},{"thickness",1.4},{"duration_ms",2000}};
    auto wd=init(walls,"client_wall",barrier);auto placement=cast(walls,"client_wall",wd["definition"]["definition_version"]);placement["x"]=3;placement["y"]=0;
    auto placed=walls.execute("/v1/cast",placement);check(placed["accepted"]==true && placed["wall"]["length"]==2 && placed["wall"]["thickness"]==1.4 && placed["wall"]["remaining_ms"]==2000,"wall ignored initialized dimensions/lifetime");
    auto pd=init(walls,"blocked_bolt",definition());walls.execute("/v1/cast",cast(walls,"blocked_bolt",pd["definition"]["definition_version"],2));walls.step(.3);
    check(view(walls,"b")["self"]["combat"]["hp"]==180 && view(walls)["projectiles"].empty(),"custom wall failed to block projectile");
    for(int i=1;i<=8;++i){auto move=ref();move.update({{"seq",i},{"dx",1},{"dy",0}});walls.execute("/v1/move",move);walls.step(.05);}
    check(std::abs(view(walls)["self"]["x"].get<double>()-1.85)<.001,"movement used hardcoded wall thickness");
    walls.step(1);walls.step(.4);check(view(walls)["walls"].empty(),"wall ignored initialized expiration");
    mmo::Scene active(cfg);enter(active,role());auto slow=definition();slow["speed"]=1;slow["cooldown_ms"]=50;
    auto sd=init(active,"in_flight",slow);active.execute("/v1/cast",cast(active,"in_flight",sd["definition"]["definition_version"]));active.step(.1);
    auto faster=slow;faster["speed"]=2;check(init(active,"in_flight",faster)["reason"]=="skill_in_use","live instance definition changed");
    std::cout<<"PASS parameterized skill registration, ownership, versions, PvP, geometry and lifecycle\n";
    return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
