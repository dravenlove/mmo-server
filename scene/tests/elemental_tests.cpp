#include "scene.hpp"
#include <cmath>
#include <iostream>
#include <fstream>
using mmo::Json;
void check(bool v,const char* message){if(!v)throw std::runtime_error(message);}
Json role(std::string scene="world",int owner=1){return {{"role_id","hero"},{"name","Hero"},{"scene_id",scene},{"owner_epoch",owner},{"session_epoch",1},{"x",12},{"y",-7},{"coins",100}};}
Json ref(int owner=1){return {{"role_id","hero"},{"owner_epoch",owner},{"session_epoch",1}};}
Json transfer(int owner,std::string id){auto r=ref(owner);r["transfer_id"]=id;return r;}
Json rotate(mmo::Scene& s,const Json& current,std::string target,std::string id){
    int owner=current["owner_epoch"].get<int>();auto frozen=s.execute("/v1/freeze",transfer(owner,id))["role"];
    frozen["scene_id"]=target;frozen["owner_epoch"]=owner+1;
    s.execute("/v1/prepare",{{"role",frozen},{"transfer_id",id}});
    auto active=s.execute("/v1/activate",transfer(owner+1,id))["role"];
    s.execute("/v1/remove",transfer(owner,id));return active;
}
void tick(mmo::ElementalBattle& b,int count){for(int i=0;i<count;++i)b.step(0,0);}
void compare_reference(const Json& actual,const Json& expected,const std::string& path){
    if(expected.is_number()) {check(actual.is_number(),"reference number type mismatch");if(std::abs(actual.get<double>()-expected.get<double>())>.0005)throw std::runtime_error("reference number mismatch: "+path);return;}
    if(expected.is_object()){if(!actual.is_object()||actual.size()!=expected.size())throw std::runtime_error("reference object mismatch: "+path);for(auto it=expected.begin();it!=expected.end();++it)compare_reference(actual.at(it.key()),it.value(),path+"."+it.key());return;}
    if(expected.is_array()){if(!actual.is_array()||actual.size()!=expected.size())throw std::runtime_error("reference array mismatch: "+path);for(std::size_t i=0;i<expected.size();++i)compare_reference(actual[i],expected[i],path+"["+std::to_string(i)+"]");return;}
    if(actual!=expected)throw std::runtime_error("reference value mismatch: "+path);
}
int main(int argc,char** argv){try{
    if(argc>1){
        std::ifstream input(argv[1]);Json reference;input>>reference;
        mmo::ElementalBattle replay(reference["battle_id"],reference["player_id"]);int frame_number=0;
        for(const auto& frame:reference["frames"]){
            if(frame["op"]=="cast")check(replay.cast(frame["element"],frame["x"],frame["y"])==frame["result"],"reference cast result mismatch");
            if(frame["op"]=="tick")for(int i=0;i<frame["count"].get<int>();++i)replay.step(frame["dx"],frame["dy"]);
            compare_reference(replay.json(),frame["expected"],"frame-"+std::to_string(frame_number++));
        }
        std::cout<<"client reference differential frames passed: "<<frame_number<<'\n';
    }
    mmo::ElementalBattle b("battle","hero");auto initial=b.json();
    check(initial["tiles"].size()==117&&initial["enemies"].size()==6&&initial["player"]["id"]=="hero","initial elemental snapshot invalid");
    check(initial["tiles"][5*13+4]["ground"]=="grass"&&initial["enemies"][2]["y"]==7.5,"Y axis conversion mismatch");
    check(b.cast("ice",4,4)=="unknown_element"&&b.cast("water",13,4)=="outside_arena"&&b.cast("water",12,8)=="out_of_range","normal rejection missing");
    check(b.cast("lightning",4,4).empty(),"wet corridor strike rejected");auto chain=b.json();
    check(chain["last_chain"]==6&&chain["conduction"]==1&&chain["enemies"][0]["hp"]==24&&chain["enemies"][1]["hp"]==24,"wet conduction hit missing or repeated");
    check(b.cast("water",1,4)=="cooldown","cooldown not enforced");tick(b,6);
    check(b.cast("water",1,4).empty(),"water cast rejected");check(b.json()["tiles"][4*13+1]["wet"]==9,"water cross missing");tick(b,6);
    check(b.cast("fire",1,4).empty()&&b.json()["steam"].get<int>()>0,"water fire steam reaction missing");
    mmo::ElementalBattle fire("fire","hero");fire.cast("fire",4,5);auto ignited=fire.json();
    check(ignited["ignitions"].get<int>()>0,"grass did not ignite");tick(fire,12);
    check(fire.json()["ignitions"].get<int>()>ignited["ignitions"].get<int>(),"fire did not spread from prior snapshot");
    mmo::ElementalBattle loss("loss","hero");for(int i=0;i<4;++i){loss.cast("lightning",1,4);tick(loss,6);}
    auto terminal=loss.json();check(terminal["phase"]=="lost"&&terminal["player"]["hp"]==0,"self damage did not lose battle");
    tick(loss,20);check(loss.json()==terminal&&loss.cast("water",1,4)=="battle_finished","terminal battle kept running");
    mmo::ElementalBattle win("win","hero");
    for(int i=0;i<500&&win.playing();++i){
        const auto state=win.json();const auto p=state["player"];double nearest=1000;Json target;
        for(const auto& e:state["enemies"])if(e["hp"].get<double>()>0){const double d=std::hypot(e["x"].get<double>()-p["x"].get<double>(),e["y"].get<double>()-p["y"].get<double>());if(d<nearest){nearest=d;target=e;}}
        if(!target.is_null())win.cast("lightning",static_cast<int>(target["x"].get<double>()),static_cast<int>(target["y"].get<double>()));
        win.step(0,0);
    }
    check(win.json()["phase"]=="won"&&win.json()["living_enemies"]==0,"valid victory was not reached");
    auto effects=terminal["effects"];std::uint64_t previous=0;
    for(const auto& e:effects){check(e["id"].get<std::uint64_t>()>previous,"effect ids regressed");previous=e["id"].get<std::uint64_t>();}

    mmo::Config cfg;cfg.scene_id="world";cfg.combat_enabled=false;cfg.elemental_enabled=true;cfg.elemental_arena_id="arena";
    mmo::Scene scene(cfg);auto current=role();scene.execute("/v1/enter",{{"role",current}});
    check(scene.execute("/v1/view",ref())["features"][0]=="elemental_arena_v1","free world omitted feature");
    auto move=ref();move.update({{"seq",7},{"dx",1},{"dy",0}});scene.execute("/v1/move",move);
    current=rotate(scene,current,"arena","entry");auto arena=scene.execute("/v1/view",ref(2));
    check(arena["battle"]["player"]["x"]==1.5&&arena["battle"]["player"]["y"]==4.5,"arena spawn invalid");
    check(arena["self"]["x"]==12&&arena["self"]["y"]==-7,"trial overwrote stored return position");
    auto cast=ref(2);cast.update({{"scene_id","arena"},{"request_id","cast-1"},{"element","lightning"},{"x",4},{"y",4}});
    auto ack=scene.execute("/v1/elemental_cast",cast);check(ack["accepted"]==true,"elemental scene cast rejected");
    auto state=scene.execute("/v1/view",ref(2));scene.execute("/v1/elemental_cast",cast);
    check(scene.execute("/v1/view",ref(2))["battle"]==state["battle"],"duplicate request applied twice");
    current=rotate(scene,current,"arena","restart");auto restarted=scene.execute("/v1/view",ref(3));
    check(restarted["battle"]["battle_id"]!=arena["battle"]["battle_id"]&&restarted["battle"]["event_sequence"]==0,"restart did not replace battle");
    auto input=ref(3);input.update({{"seq",8},{"dx",0},{"dy",1}});scene.execute("/v1/move",input);scene.step(.05);
    check(scene.execute("/v1/view",ref(3))["battle"]["player"]["y"].get<double>()>4.5,"arena direction Y inverted");
    current=rotate(scene,current,"world","leave");auto world=scene.execute("/v1/view",ref(4));
    check(!world.contains("battle")&&world["self"]["x"]==12&&world["self"]["y"]==-7,"leave failed to restore world");
    auto old=ref(4);old.update({{"seq",7},{"dx",1},{"dy",0}});bool rejected=false;
    try{scene.execute("/v1/move",old);}catch(const mmo::Error&){rejected=true;}check(rejected,"transition reset movement sequence");
    // 候选取消后，冻结的原状态仍能恢复；目标墓碑不销毁原角色。
    auto frozen=scene.execute("/v1/freeze",transfer(4,"abort"))["role"];frozen["owner_epoch"]=5;frozen["scene_id"]="arena";
    scene.execute("/v1/prepare",{{"role",frozen},{"transfer_id","abort"}});scene.execute("/v1/remove",transfer(5,"abort"));scene.execute("/v1/remove",transfer(5,"abort"));scene.execute("/v1/unfreeze",transfer(4,"abort"));
    scene.execute("/v1/disconnect",ref(4));scene.execute("/v1/unfreeze",transfer(4,"abort"));scene.execute("/v1/enter",{{"role",current}});
    check(scene.execute("/v1/view",ref(4))["self"]["owner_epoch"]==4,"abort removed original role");
    std::cout<<"elemental rules, reactions, terminal state, ownership rotation, retries and return position passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
