#include "scene.hpp"
#include <cmath>
#include <iostream>
#include <stdexcept>
using mmo::Json;
void check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
Json role(std::string id,int session=1,double x=0){return {{"role_id",id},{"name",id},{"scene_id","scene-1"},{"owner_epoch",1},{"session_epoch",session},{"x",x},{"y",0},{"coins",0}};}
Json ref(std::string id,int session=1){return {{"role_id",id},{"owner_epoch",1},{"session_epoch",session}};}
Json move(std::string id,int seq,double dx,double dy){auto r=ref(id);r.update({{"seq",seq},{"dx",dx},{"dy",dy}});return r;}
int main(){try{
    mmo::Config cfg;cfg.combat_enabled=false;
    mmo::Scene scene(cfg);
    scene.execute("/v1/enter",{{"role",role("alice")}});
    scene.execute("/v1/enter",{{"role",role("bob",1,30)}});
    auto view=scene.execute("/v1/view",ref("alice"));
    check(view["players"].size()==2,"AOI boundary must be inclusive");
    check(!view.contains("mobs")&&!view["self"].contains("combat"),"movement world exposes combat");
    auto batch=ref("alice");batch["inputs"]=Json::array({{{"seq",1},{"dx",1},{"dy",1}},{{"seq",2},{"dx",0},{"dy",0}},{{"seq",1},{"dx",-1},{"dy",0}},{{"seq",3},{"dx",1},{"dy",1}}});
    auto accepted=scene.execute("/v1/move_batch",batch)["results"];
    check(accepted.size()==4&&accepted[0]["ok"]==true&&accepted[1]["ok"]==true&&accepted[2]["ok"]==false&&accepted[3]["ok"]==true,"batch must return every input outcome");
    scene.step(.1);
    auto position=scene.execute("/v1/view",ref("alice"))["self"];
    check(std::abs(std::hypot(position["x"].get<double>(),position["y"].get<double>())-.6)<1e-9,"batch count accelerated movement");
    auto final=scene.execute("/v1/disconnect",ref("alice"))["role"];
    for(int i=0;i<8;++i)scene.step(.05);
    view=scene.execute("/v1/view",ref("bob"));
    check(view["players"].size()==1,"disconnected player remained visible");
    try{scene.execute("/v1/move",move("alice",4,1,0));throw std::runtime_error("disconnected session moved");}catch(const mmo::Error& e){check(e.status==409,"wrong disconnect fence");}
    scene.execute("/v1/enter",{{"role",role("alice",2)}});
    auto resumed=scene.execute("/v1/view",ref("alice",2))["self"];
    check(resumed["x"]==final["x"]&&resumed["y"]==final["y"],"reconnect changed final position");
    try{scene.execute("/v1/disconnect",ref("alice"));throw std::runtime_error("old disconnect accepted");}catch(const mmo::Error& e){check(e.status==409,"old disconnect changed new session");}
    auto fresh=ref("alice",2);fresh.update({{"seq",1},{"dx",1},{"dy",0}});scene.execute("/v1/move",fresh);
    for(int i=0;i<20;++i)scene.step(.05);
    auto expired=scene.execute("/v1/view",ref("alice",2))["self"];
    check(std::abs(expired["x"].get<double>()-resumed["x"].get<double>()-1.5)<1e-8,"input expiry exceeded 250ms");
    for(int i=0;i<64;++i){scene.step(.05);scene.execute("/v1/view",ref("alice",2));}
    check(scene.execute("/v1/view",ref("alice",2))["players"].size()==1,"expired presence remained in AOI");
    mmo::Config one=cfg;one.max_players=1;mmo::Scene limited(one);
    limited.execute("/v1/enter",{{"role",role("offline")}});limited.execute("/v1/disconnect",ref("offline"));
    limited.execute("/v1/enter",{{"role",role("online")}});check(limited.player_count()==1,"offline record consumed online slot");
    mmo::Config large=cfg;large.max_players=200;mmo::Scene crowded(large);
    for(int i=0;i<140;++i)crowded.execute("/v1/enter",{{"role",role("p"+std::to_string(i))}});
    view=crowded.execute("/v1/view",ref("p99"));
    check(view["players"].size()==128&&view["players_truncated"]==true&&view["visible_player_count"]==140,"AOI capacity metadata incorrect");
    check(view["players"][0]["role_id"]=="p99"&&view.dump().size()<60000,"self missing or payload budget exceeded");
    // 无战斗场景不清空既有战斗快照，返回战斗场景时不能刷新消耗。
    mmo::Scene combat({});auto identity=role("preserve");combat.execute("/v1/enter",{{"role",identity}});
    auto cast=ref("preserve");cast.update({{"seq",1},{"skill_id","whirlwind"},{"target_id",""}});
    auto combat_role=combat.execute("/v1/cast",cast)["role"];
    mmo::Scene lobby(cfg);lobby.execute("/v1/enter",{{"role",combat_role}});lobby.step(.5);
    check(lobby.execute("/v1/disconnect",ref("preserve"))["role"]["combat"]==combat_role["combat"],"movement realm reset stored combat state");
    std::cout<<"movement, presence, session fencing, batch and AOI limits passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
