#include "elemental.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <queue>
#include <utility>

namespace mmo {
namespace {
constexpr float dt=.05f;
float distance(float x,float y,float bx,float by){return std::hypot(x-bx,y-by);}
void increment(int& value){if(value<std::numeric_limits<int>::max())++value;}
}
bool ElementalBattle::inside(int x,int y){return x>=0&&x<width&&y>=0&&y<height;}
ElementalBattle::ElementalBattle(std::string battle_id,std::string role_id):id_(std::move(battle_id)),role_id_(std::move(role_id)){
    // 内部按参考核心的 Y 向下计算，只在输入/快照边界转换到协议的 Y 向上。
    for(int x=4;x<=7;++x)for(int y=1;y<=3;++y)tile(x,y).ground=1;
    for(int x=8;x<=10;++x)for(int y=5;y<=7;++y)tile(x,y).ground=1;
    for(int x=4;x<=9;++x)tile(x,4).wet=30;
    player_={1.5f,4.5f,100,100,0};
    enemies_={{{8.5f,4.5f,56,56,0},{9.5f,4.5f,56,56,0},{10.5f,1.5f,56,56,0},
        {11.5f,3.5f,56,56,0},{10.5f,6.5f,56,56,0},{11.5f,7.5f,56,56,0}}};
    message_="Prepare the ground. Defeat all six pursuers.";
}
ElementalBattle::Tile& ElementalBattle::tile_at(const Actor& a){return tile(static_cast<int>(std::clamp(a.x,0.f,width-.001f)),static_cast<int>(std::clamp(a.y,0.f,height-.001f)));}
ElementalBattle::Before ElementalBattle::before()const{
    Before b{};b.conduction=conduction_;b.phase=phase_;b.hp[0]=player_.hp;
    for(std::size_t i=0;i<tiles_.size();++i){b.burning[i]=tiles_[i].burn>0;b.steam[i]=tiles_[i].steam;}
    for(std::size_t i=0;i<enemies_.size();++i)b.hp[i+1]=enemies_[i].hp;
    return b;
}
void ElementalBattle::emit(const std::string& type,const std::string& actor,const std::string& element,double x,double y,double amount){
    effects_.push_back({{"id",++event_sequence_},{"type",type},{"actor_id",actor},{"element",element},{"x",x},{"y",y},{"amount",amount}});
    while(effects_.size()>64)effects_.pop_front();
}
void ElementalBattle::record(const Before& b,double x,double y){
    for(int cx=0;cx<width;++cx)for(int cy=0;cy<height;++cy){
        const int i=cy*width+cx;const auto& t=tile(cx,cy);
        if(!b.burning[i]&&t.burn>0)emit("ignite","","fire",cx+.5,height-cy-.5,0);
        if(t.steam>b.steam[i]+.0001f)emit("steam","","",cx+.5,height-cy-.5,0);
    }
    if(conduction_>b.conduction)emit("conduction","","lightning",x,y,last_chain_);
    for(int i=0;i<7;++i){
        const auto& a=i==0?player_:enemies_[i-1];const auto actor=i==0?role_id_:"enemy-"+std::to_string(i-1);
        if(b.hp[i]>a.hp)emit("damage",actor,"",a.x,height-static_cast<double>(a.y),b.hp[i]-a.hp);
        if(b.hp[i]>0&&a.hp<=0)emit("death",actor,"",a.x,height-static_cast<double>(a.y),0);
    }
    if(b.phase=="playing"&&phase_!=b.phase)emit(phase_=="won"?"win":"loss",role_id_,"",player_.x,height-static_cast<double>(player_.y),0);
}
void ElementalBattle::damage_at(int x,int y,float amount,bool everyone){
    for(auto& a:enemies_)if(a.hp>0&&static_cast<int>(a.x)==x&&static_cast<int>(a.y)==y)a.hp=std::max(0.f,a.hp-amount);
    if(everyone&&static_cast<int>(player_.x)==x&&static_cast<int>(player_.y)==y)player_.hp=std::max(0.f,player_.hp-amount);
}
void ElementalBattle::affect(bool water,int x,int y){
    if(!inside(x,y))return;
    auto& t=tile(x,y);
    if(water){const bool burning=t.burn>0;t.burn=0;t.wet=9;if(burning){t.steam=3.5f;increment(steam_);}else{message_="Water: wet ground lasts 9 seconds.";return;}}
    else if(t.wet>0){t.wet=0;t.burn=0;t.steam=3.5f;increment(steam_);}
    else{
        damage_at(x,y,16,false);
        if(t.ground==1&&t.burn<=0){t.burn=4;increment(ignitions_);message_="Grass ignited. Fire spreads every 0.6 seconds.";}
        else message_="Fire: direct damage on dry ground.";
        return;
    }
    message_="Steam: enemies inside move at 40% speed.";
}
void ElementalBattle::strike(int x,int y){
    if(tile(x,y).wet<=0){tile(x,y).charge=.24f;damage_at(x,y,28,true);message_="Lightning: a single dry tile was struck.";return;}
    std::array<bool,117> visited{};std::queue<int> pending;pending.push(y*width+x);visited[y*width+x]=true;
    while(!pending.empty()){
        const int i=pending.front();pending.pop();const int cx=i%width,cy=i/width;
        tile(cx,cy).charge=.4f;damage_at(cx,cy,32,true);++last_chain_;
        for(auto d:{std::pair<int,int>{-1,0},{1,0},{0,-1},{0,1}}){
            const int nx=cx+d.first,ny=cy+d.second;
            if(inside(nx,ny)&&!visited[ny*width+nx]&&tile(nx,ny).wet>0){visited[ny*width+nx]=true;pending.push(ny*width+nx);}
        }
    }
    increment(conduction_);message_="Conduction: "+std::to_string(last_chain_)+" connected wet tiles. Beware friendly fire!";
}
std::string ElementalBattle::cast(const std::string& element,int x,int y){
    if(element!="water"&&element!="fire"&&element!="lightning")return "unknown_element";
    if(!inside(x,y))return "outside_arena";
    if(!playing())return "battle_finished";
    if(cooldown_>0)return "cooldown";
    const int cy=height-1-y;
    if(distance(player_.x,player_.y,x+.5f,cy+.5f)>7)return "out_of_range";
    emit("cast",role_id_,element,x+.5,y+.5,0);const auto b=before();cooldown_=.28f;last_chain_=0;
    if(element=="lightning")strike(x,cy);
    else{
        const int steam_before=steam_,ignitions_before=ignitions_;const bool water=element=="water";
        affect(water,x,cy);affect(water,x-1,cy);affect(water,x+1,cy);affect(water,x,cy-1);affect(water,x,cy+1);
        if(steam_>steam_before)message_="Steam: enemies inside move at 40% speed.";
        else if(ignitions_>ignitions_before)message_="Grass ignited. Fire spreads every 0.6 seconds.";
    }
    outcome();record(b,x+.5,y+.5);return "";
}
void ElementalBattle::spread_fire(){
    std::array<bool,117> marked{};
    for(int x=0;x<width;++x)for(int y=0;y<height;++y)if(tile(x,y).burn>0){
        for(auto d:{std::pair<int,int>{-1,0},{1,0},{0,-1},{0,1}}){
            const int nx=x+d.first,ny=y+d.second;
            if(inside(nx,ny)){const auto& t=tile(nx,ny);if(t.ground==1&&t.wet<=0&&t.burn<=0)marked[ny*width+nx]=true;}
        }
    }
    for(int x=0;x<width;++x)for(int y=0;y<height;++y)if(marked[y*width+x]){tile(x,y).burn=4;increment(ignitions_);}
}
void ElementalBattle::outcome(){
    if(player_.hp<=0){phase_="lost";message_="Defeated. Press R to try another strategy.";}
    else if(std::none_of(enemies_.begin(),enemies_.end(),[](const Actor& a){return a.hp>0;})){phase_="won";message_="All enemies defeated. Press R to play again.";}
}
void ElementalBattle::step(double dx,double dy){
    if(!playing())return;
    const auto b=before();elapsed_=std::min(1e9f,elapsed_+dt);cooldown_=std::max(0.f,cooldown_-dt);
    float mx=static_cast<float>(dx),my=static_cast<float>(-dy);const float length=std::hypot(mx,my);
    if(length>1){mx/=length;my/=length;}
    player_.x=std::clamp(player_.x+mx*3.2f*dt,.2f,width-.2f);player_.y=std::clamp(player_.y+my*3.2f*dt,.2f,height-.2f);
    for(auto& enemy:enemies_){
        if(enemy.hp<=0)continue;
        enemy.attack=std::max(0.f,enemy.attack-dt);
        const float d=distance(enemy.x,enemy.y,player_.x,player_.y);
        if(d>.65f){const float speed=tile_at(enemy).steam>0?.4f:1.f;const float travel=std::min(d-.65f,1.05f*speed*dt);enemy.x+=(player_.x-enemy.x)/d*travel;enemy.y+=(player_.y-enemy.y)/d*travel;}
        else if(enemy.attack<=0){player_.hp=std::max(0.f,player_.hp-8);enemy.attack=.9f;}
    }
    if(tile_at(player_).burn>0)player_.hp=std::max(0.f,player_.hp-14*dt);
    for(auto& a:enemies_)if(a.hp>0&&tile_at(a).burn>0)a.hp=std::max(0.f,a.hp-14*dt);
    spread_+=dt;if(spread_>=.6f){spread_-=.6f;spread_fire();}
    for(auto& t:tiles_){const bool was_burning=t.burn>0;t.wet=std::max(0.f,t.wet-dt);t.burn=std::max(0.f,t.burn-dt);t.steam=std::max(0.f,t.steam-dt);t.charge=std::max(0.f,t.charge-dt);if(was_burning&&t.burn<=0)t.ground=2;}
    outcome();record(b,player_.x,height-static_cast<double>(player_.y));
}
nlohmann::json ElementalBattle::actor_json(const Actor& a,const std::string& id)const{return {{"id",id},{"x",a.x},{"y",height-static_cast<double>(a.y)},{"hp",a.hp},{"max_hp",a.max_hp}};}
nlohmann::json ElementalBattle::json()const{
    nlohmann::json tiles=nlohmann::json::array(),enemies=nlohmann::json::array(),effects=nlohmann::json::array();int alive=0;
    for(int y=0;y<height;++y)for(int x=0;x<width;++x){const auto& t=tile(x,height-1-y);tiles.push_back({{"ground",t.ground==1?"grass":t.ground==2?"charred":"stone"},{"wet",t.wet},{"burn",t.burn},{"steam",t.steam},{"charge",t.charge}});}
    for(std::size_t i=0;i<enemies_.size();++i){enemies.push_back(actor_json(enemies_[i],"enemy-"+std::to_string(i)));if(enemies_[i].hp>0)++alive;}
    for(const auto& e:effects_)effects.push_back(e);
    return {{"version",1},{"battle_id",id_},{"width",width},{"height",height},{"phase",phase_},{"player",actor_json(player_,role_id_)},{"enemies",enemies},{"tiles",tiles},
        {"cooldown",cooldown_},{"range",7},{"elapsed",elapsed_},{"living_enemies",alive},{"conduction",conduction_},{"ignitions",ignitions_},{"steam",steam_},{"last_chain",last_chain_},
        {"message",message_},{"event_sequence",event_sequence_},{"effects",effects}};
}
}
