#pragma once
#include <array>
#include <cstdint>
#include <deque>
#include <string>
#include <nlohmann/json.hpp>

namespace mmo {
// 单人试炼场独立状态，不覆盖角色的持久化血蓝与自由区坐标。
class ElementalBattle {
public:
    static constexpr int width = 13, height = 9;
    ElementalBattle(std::string battle_id, std::string role_id);
    std::string cast(const std::string& element, int x, int y);
    void step(double dx, double dy);
    nlohmann::json json() const;
    bool playing() const { return phase_ == "playing"; }
private:
    struct Tile { int ground=0; float wet=0, burn=0, steam=0, charge=0; };
    struct Actor { float x=0, y=0, hp=0, max_hp=0, attack=0; };
    struct Before {
        std::array<bool,117> burning;
        std::array<float,117> steam;
        std::array<float,7> hp;
        int conduction;
        std::string phase;
    };
    std::array<Tile,117> tiles_{};
    std::array<Actor,6> enemies_{};
    Actor player_;
    std::string id_, role_id_, phase_="playing", message_;
    float elapsed_=0, cooldown_=0, spread_=0;
    int conduction_=0, ignitions_=0, steam_=0, last_chain_=0;
    std::uint64_t event_sequence_=0;
    std::deque<nlohmann::json> effects_;
    Tile& tile(int x,int y) { return tiles_.at(y*width+x); }
    const Tile& tile(int x,int y) const { return tiles_.at(y*width+x); }
    static bool inside(int x,int y);
    Tile& tile_at(const Actor& actor);
    Before before() const;
    void record(const Before& previous,double x,double y);
    void emit(const std::string& type,const std::string& actor,const std::string& element,double x,double y,double amount);
    void affect(bool water,int x,int y);
    void strike(int x,int y);
    void spread_fire();
    void damage_at(int x,int y,float amount,bool everyone);
    void outcome();
    nlohmann::json actor_json(const Actor& actor,const std::string& id) const;
};
}
