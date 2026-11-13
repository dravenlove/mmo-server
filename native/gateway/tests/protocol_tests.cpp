#include "gateway/protocol.hpp"
#include <iostream>
#include <stdexcept>
using namespace mmo::gateway;
namespace {
void require(bool condition,const char* message) { if(!condition) throw std::runtime_error(message); }
template<class F> void fails(F fn) { bool did=false; try { fn(); } catch(const Failure&) { did=true; } require(did,"invalid input was accepted"); }
Json role(std::int64_t revision=1) { return {{"role_id","alice"},{"name","Alice"},{"scene_id","arena"},{"owner_epoch",3},{"session_epoch",7},{"coins",100},{"combat",{{"revision",revision},{"hp",100}}}}; }
}
int main() {
    try {
        auto input=parse_input(R"({"id":"a","op":"move","seq":1,"dx":1,"dy":0})","movement");
        require(input.error.empty(),"valid movement rejected");
        require(!parse_input(R"({"id":"a","op":"move","seq":1,"dy":0})","movement").error.empty(),"missing direction accepted");
        require(!parse_input(R"({"id":"a","op":"cast","skill_id":"custom","seq":1,"dx":null,"dy":0})","skill_lab").error.empty(),"null aim accepted");
        require(!parse_input(R"({"id":"a","op":"move","seq":1,"dx":0,"dy":0,"role_id":"victim"})","movement").error.empty(),"spoofed identity accepted");
        fails([] { parse_input(R"({"id":"a","op":"login","token":"a","token":"b"})","full"); });
        fails([] { parse_input(R"({"id":"a","op":"login"} {"op":"cast"})","full"); });
        fails([] { parse_input(std::string(65536,' '),"full"); });
        fails([] { parse_input(R"({"id":"a","op":"login","password":42})","full"); });
        auto a=role(),b=a; b["session_epoch"]=8;
        require(!same_role(a,b),"old session considered equal");
        Json old{{"self",role(5)},{"tick",20},{"world_epoch","old"}};
        Json next{{"self",role(4)},{"tick",21},{"world_epoch","old"}};
        require(!fresh_view(old,next),"old combat response replaced new action");
        next["self"]=role(6); next["tick"]=19;
        require(!fresh_view(old,next),"old tick accepted");
        next["world_epoch"]="new";
        require(fresh_view(old,next),"new scene epoch rejected");
        Json route{{"role",a},{"scene_url","http://127.0.0.1:9001"}};
        Json output{{"accepted",true},{"role",role(4)}};
        auto saved=action_snapshot(output,route,old);
        require(saved["self"]["combat"]["revision"]==5,"idempotent action rolled checkpoint backwards");
        Json view{{"self",a},{"players",Json::array({a,b})},{"mobs",Json::array()},{"world_epoch","epoch"},{"events",Json::array()},{"projectiles",Json::array()}};
        auto movement=client_view(view,"movement");
        require(!movement["self"].contains("combat") && !movement.contains("events") && !movement.contains("world_epoch"),"movement projection exposed combat");
        require(client_view(view,"skill_lab").contains("projectiles"),"skill projection lost objects");
        view["battle"]={{"player",{{"id","alice"},{"x",4},{"y",7},{"hp",0}}}};
        auto elemental=client_view(view,"elemental");
        require(elemental["self"]["x"]==4 && elemental["self"]["combat"]["dead"]==true,"elemental projection mismatch");
        Config c; c.token=std::string(32,'x'); c.origins={"https://example.com"}; c.validate();
        c.origins={"https://example.com/path"}; fails([&] {c.validate();});
        c.origins={"https://example.com"}; c.backend_url="https://user:secret@example.com"; fails([&] {c.validate();});
        require(permitted("skill_lab","account_login") && permitted("skill_lab","skill_init") && !permitted("movement","cast"),"profile boundary mismatch");
        std::cout << "native gateway protocol invariants passed\n";
        return 0;
    } catch(const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
