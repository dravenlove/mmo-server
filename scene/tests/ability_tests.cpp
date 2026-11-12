#include "scene.hpp"
#include <cmath>
#include <iostream>
#include <stdexcept>
using mmo::Json;
void check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
Json role(std::string id="a",double x=0,double y=0,int session=1){return {{"role_id",id},{"name",id},{"scene_id","arena"},{"owner_epoch",1},{"session_epoch",session},{"x",x},{"y",y},{"coins",0}};}
Json ref(std::string id="a",int session=1){return {{"role_id",id},{"owner_epoch",1},{"session_epoch",session}};}
Json bolt(int seq=1,double dx=1,double dy=0){auto r=ref();r.update({{"seq",seq},{"skill_id","arc_bolt"},{"dx",dx},{"dy",dy}});return r;}
Json wall(int seq=1,double x=3,double y=0,double dx=1,double dy=0){auto r=ref();r.update({{"seq",seq},{"skill_id","stone_wall"},{"x",x},{"y",y},{"dx",dx},{"dy",dy}});return r;}
Json view(mmo::Scene& s,std::string id="a",int session=1){return s.execute("/v1/view",ref(id,session));}
void enter(mmo::Scene& s,const Json& r){s.execute("/v1/enter",{{"role",r}});}
void advance(mmo::Scene& s,double seconds,bool second=false){for(int i=0;i<static_cast<int>(std::round(seconds/.05));++i){s.step(.05);view(s);if(second)view(s,"b");}}
template<class F>void fails(F f){try{f();throw std::runtime_error("expected scene rejection");}catch(const mmo::Error&) {}}
int main(){try{
    mmo::Config cfg;cfg.scene_id="arena";cfg.abilities_enabled=true;
    mmo::Scene pvp(cfg);enter(pvp,role());enter(pvp,role("b",6));
    auto initial=view(pvp);
    check(initial["skills"].size()==2 && initial["ability_rules_version"]=="projectile-wall-v1" && initial["pvp_enabled"]==true,"ability catalog/version missing");
    check(initial["self"]["combat"]["cooldowns"].size()==6,"legacy cooldowns not preserved");
    fails([&]{pvp.execute("/v1/cast",bolt(1,0,0));});
    auto spoofed=bolt();spoofed["x"]=99;fails([&]{pvp.execute("/v1/cast",spoofed);});
    auto shot=pvp.execute("/v1/cast",bolt());
    check(shot["accepted"]==true && shot["projectile"]["speed"]==18,"projectile spawn missing");
    check(view(pvp,"b")["self"]["combat"]["hp"]==180,"projectile applied instant damage");
    check(pvp.execute("/v1/cast",bolt())==shot,"retry spawned a second projectile");
    check(pvp.execute("/v1/cast",bolt(1,0,1))["accepted"]==false,"same seq changed direction");
    check(pvp.execute("/v1/cast",bolt(2))["reason"]=="cooldown","cooldown not enforced");
    check(view(pvp)["self"]["combat"]["mp"]==90,"rejected/repeated cast consumed mana");
    pvp.step(1); // 单大步仍须扫过目标，不能越过它。
    check(view(pvp,"b")["self"]["combat"]["hp"]==152,"swept PvP collision failed");
    check(view(pvp)["projectiles"].empty(),"hit projectile survived");
    pvp.step(.05);check(view(pvp,"b")["self"]["combat"]["hp"]==152,"projectile hit twice");

    mmo::Scene nearest(cfg);enter(nearest,role());enter(nearest,role("b",4));enter(nearest,role("c",8));
    nearest.execute("/v1/cast",bolt());nearest.step(.8);
    check(view(nearest,"b")["self"]["combat"]["hp"]==152 && view(nearest,"c")["self"]["combat"]["hp"]==180,"projectile pierced its first target");

    mmo::Scene pve(cfg);enter(pve,role());
    pve.execute("/v1/cast",bolt(1,-1,.5));pve.step(.5);
    auto npcview=view(pve);bool damaged=false;
    for(const auto& n:npcview["mobs"])if(n["id"]=="npc-1")damaged=n["hp"]==52;
    check(damaged,"projectile failed to hit monster");

    mmo::Scene blocked(cfg);enter(blocked,role());enter(blocked,role("b",8));
    auto obstruction=blocked.execute("/v1/cast",wall());
    check(obstruction["accepted"]==true && obstruction["wall"]["length"]==6,"wall not created");
    check(blocked.execute("/v1/cast",wall())==obstruction,"wall retry not idempotent");
    blocked.execute("/v1/cast",bolt(2));blocked.step(.5);
    check(view(blocked,"b")["self"]["combat"]["hp"]==180 && view(blocked)["projectiles"].empty(),"wall did not block projectile");
    for(int i=1;i<=5;++i){auto input=ref();input.update({{"seq",i},{"dx",1},{"dy",0}});blocked.execute("/v1/move",input);blocked.step(.25);}
    check(std::abs(view(blocked)["self"]["x"].get<double>()-2.25)<1e-4,"movement tunneled through wall");
    advance(blocked,4.3,true);
    check(view(blocked)["walls"].empty(),"wall lifetime did not expire");
    auto input=ref();input.update({{"seq",6},{"dx",1},{"dy",0}});blocked.execute("/v1/move",input);blocked.step(.25);
    check(view(blocked)["self"]["x"].get<double>()>3,"expired wall still blocked movement");

    mmo::Scene placement(cfg);enter(placement,role());
    check(placement.execute("/v1/cast",wall(1,10))["reason"]=="out_of_range","range validation missing");
    check(placement.execute("/v1/cast",wall(1,0))["reason"]=="spawn_blocked","wall blocked revival spawn");
    check(placement.execute("/v1/cast",wall(1,1.2))["accepted"]==true,"valid close wall rejected");
    check(placement.execute("/v1/cast",wall(2,4))["reason"]=="cooldown","wall cooldown ignored");
    check(view(placement)["self"]["combat"]["mp"]==80,"wall rejection consumed resources");

    mmo::Scene cleanup(cfg);enter(cleanup,role());cleanup.execute("/v1/cast",wall());cleanup.execute("/v1/cast",bolt(2));
    cleanup.execute("/v1/disconnect",ref());
    enter(cleanup,role("a",0,0,2));
    check(view(cleanup,"a",2)["walls"].empty() && view(cleanup,"a",2)["projectiles"].empty(),"disconnect left combat objects");
    check(view(cleanup,"a",2)["self"]["combat"]["cooldowns"]["stone_wall"].get<int>()>0,"new session reset cooldown");
    fails([&]{cleanup.execute("/v1/cast",bolt(3));});
    auto frozen=ref("a",2);frozen["transfer_id"]="preserve";
    auto saved=cleanup.execute("/v1/freeze",frozen)["role"];
    mmo::Config lobbyCfg=cfg;lobbyCfg.combat_enabled=false;lobbyCfg.abilities_enabled=false;
    mmo::Scene lobby(lobbyCfg);enter(lobby,saved);lobby.step(.5);
    check(lobby.execute("/v1/disconnect",ref("a",2))["role"]["combat"]==saved["combat"],"movement scene stripped ability cooldowns");

    mmo::Config noPvP=cfg;noPvP.pvp_enabled=false;mmo::Scene friendly(noPvP);enter(friendly,role());enter(friendly,role("b",5));
    friendly.execute("/v1/cast",bolt());friendly.step(.5);check(view(friendly,"b")["self"]["combat"]["hp"]==180,"PvP flag ignored");
    mmo::Scene world2(cfg);
    enter(world2,role());check(view(world2)["world_epoch"]!=initial["world_epoch"],"new world reused object namespace");
    check(view(pvp).dump(-1,' ',true).size()<60000,"combat snapshot exceeded frame budget");
    mmo::Scene rotated(cfg);enter(rotated,role());
    check(rotated.execute("/v1/cast",wall(1,3,3,1,1))["accepted"]==true,"rotated wall rejected");
    rotated.execute("/v1/cast",bolt(2,1,1));rotated.step(.5);
    check(view(rotated)["projectiles"].empty(),"rotated wall did not block swept projectile");
    for(int i=0;i<70;++i)rotated.step(.05);
    // view 重新续约前，过期归属的对象已被清理。
    check(view(rotated)["walls"].empty(),"expired owner lease left a wall");

    mmo::Scene death(cfg);enter(death,role());enter(death,role("b",6));
    for(int i=1;i<=7;++i){death.execute("/v1/cast",bolt(i));advance(death,1.25,true);}
    check(view(death,"b")["self"]["combat"]["dead"]==true,"PvP did not produce authoritative death");
    auto deadcast=bolt(1);deadcast.update(ref("b"));
    check(death.execute("/v1/cast",deadcast)["reason"]=="dead","dead player cast a projectile");
    auto early=ref("b");early["seq"]=1;
    check(death.execute("/v1/revive",early)["reason"]=="revive_not_ready","early revive was not recoverable");
    advance(death,5,true);auto resurrection=ref("b");resurrection["seq"]=1;
    check(death.execute("/v1/revive",resurrection)["role"]["combat"]["hp"]==180,"ability player could not revive");

    mmo::Config stressCfg=cfg;stressCfg.aoi_radius=1000;mmo::Scene stress(stressCfg);
    for(int i=0;i<128;++i){auto r=role("actor-"+std::to_string(i)+std::string(110,'r'),(i%8)*10-35,40+(i/8)*3);r["name"]=std::string(96,'\1');enter(stress,r);}
    std::string observer;
    for(int i=0;i<32;++i){const auto id="actor-"+std::to_string(i)+std::string(110,'r');auto shotRequest=bolt();shotRequest.update(ref(id));stress.execute("/v1/cast",shotRequest);if(i==0)observer=id;}
    for(int i=0;i<8;++i){const auto id="actor-"+std::to_string(i)+std::string(110,'r');auto w=wall(2,(i%8)*10-32,40);w.update(ref(id));check(stress.execute("/v1/cast",w)["accepted"]==true,"stress wall placement rejected");}
    const auto crowded=view(stress,observer);
    check(crowded.dump(-1,' ',true).size()<65500 && crowded["players_truncated"]==true,"max combat objects violated frame budget");
    auto excess=bolt();excess.update(ref("actor-32"+std::string(110,'r')));
    check(stress.execute("/v1/cast",excess)["reason"]=="object_limit","projectile capacity ignored");
    auto extraWall=wall(1,-32,52);extraWall.update(ref("actor-32"+std::string(110,'r')));
    check(stress.execute("/v1/cast",extraWall)["reason"]=="object_limit","wall capacity ignored");
    std::cout<<"projectile collision, PvP/PvE, walls, cooldown, retries, lifecycle and persistence passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
