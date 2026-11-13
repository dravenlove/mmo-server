#include "gateway/runtime.hpp"
#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <httplib.h>
#include <openssl/sha.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <csignal>
#include <deque>
#include <functional>
#include <iomanip>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <thread>
#include <unordered_map>

namespace mmo::gateway {
namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
using tcp = asio::ip::tcp;
using Clock = std::chrono::steady_clock;
using ErrorCode = boost::system::error_code;

std::string sha256_prefix(const std::string& value) {
    unsigned char digest[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(value.data()), value.size(), digest);
    std::ostringstream out;
    for (int i = 0; i < 12; ++i) out << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(digest[i]);
    return out.str();
}
namespace {
struct Result {
    Json value;
    std::optional<Failure> error;
    Json established_route;
    Result(Json data=nullptr,std::optional<Failure> problem=std::nullopt) : value(std::move(data)),error(std::move(problem)) {}
};
class Http {
    const Config& config_;
public:
    explicit Http(const Config& config) : config_(config) {}
    Result call(const std::string& base, const std::string& path, const Json& body, int timeout = 2, bool get = false) const noexcept {
        try {
            if (base.rfind("http://",0) != 0 && base.rfind("https://",0) != 0) throw Failure("invalid service address");
            auto slash = base.find('/',base.find("://")+3);
            const auto origin = base.substr(0,slash);
            auto prefix = slash == std::string::npos ? std::string() : base.substr(slash);
            while (!prefix.empty() && prefix.back() == '/') prefix.pop_back();
            // 复用客户端配置；有限线程的内部 HTTP 服务不能被空闲 keep-alive 占满。
            thread_local std::map<std::string,std::unique_ptr<httplib::Client>> clients;
            auto& client = clients[origin];
            if (!client) {
                client = std::make_unique<httplib::Client>(origin);
                client->set_follow_location(false);
                client->set_keep_alive(false);
                client->set_payload_max_length(1024*1024);
                client->enable_server_certificate_verification(true);
                client->set_connection_timeout(2,0);
                client->set_read_timeout(timeout,0);
                client->set_write_timeout(timeout,0);
            }
            client->set_read_timeout(timeout,0); client->set_write_timeout(timeout,0); client->set_max_timeout(timeout*1000);
            httplib::Headers headers{{"X-Internal-Token",config_.token}};
            auto reply = get ? client->Get((prefix+path).c_str(),headers) : client->Post((prefix+path).c_str(),headers,body.dump(),"application/json");
            if (!reply) {
                std::cerr << "upstream failure path=" << path << " phase=transport\n";
                client.reset();
                throw Failure("service unavailable or timed out; retry with the same operation id",503);
            }
            if (reply->body.size() > 1024*1024) throw Failure("invalid service response");
            Json data = Json::parse(reply->body,nullptr,false);
            if (reply->status < 200 || reply->status >= 300) {
                std::string message = "service returned status " + std::to_string(reply->status);
                if (data.is_object() && data.contains("error") && data["error"].is_string()) message = data["error"];
                throw Failure(message,reply->status);
            }
            if (data.is_discarded()) throw Failure("invalid service response");
            return {std::move(data),std::nullopt};
        } catch (const Failure& failure) { return {nullptr,failure}; }
        catch (...) { return {nullptr,Failure("invalid service response")}; }
    }
};
struct Pools {
    asio::thread_pool commands{8}, moves{8}, views{8}, sessions{4}, checkpoints{2};
    void join() { commands.join(); moves.join(); views.join(); sessions.join(); checkpoints.join(); }
};
std::pair<std::string,unsigned short> endpoint_parts(const std::string& address) {
    auto colon = address.find_last_of(':');
    if (colon == std::string::npos) throw Failure("listen address must be host:port");
    std::string host = address.substr(0,colon);
    if (host.size() >= 2 && host.front() == '[' && host.back() == ']') host = host.substr(1,host.size()-2);
    std::size_t consumed = 0;
    unsigned long port;
    try { port = std::stoul(address.substr(colon+1),&consumed); } catch (...) { throw Failure("invalid listen port"); }
    if (consumed != address.size()-colon-1 || port > 65535) throw Failure("invalid listen port");
    return {host,static_cast<unsigned short>(port)};
}
std::int64_t unix_ms() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count(); }
Json require_route(Result& result) {
    if (result.error) throw *result.error;
    auto& route = result.value;
    if (!route.is_object() || !route.contains("role") || !route["role"].is_object() || !route.contains("scene_url") || !route["scene_url"].is_string()) throw Failure("invalid service response");
    auto& role = route["role"];
    for (const auto* key : {"role_id","scene_id","name"}) if (!role.contains(key) || !role[key].is_string()) throw Failure("invalid service response");
    for (const auto* key : {"owner_epoch","session_epoch","coins"}) if (!role.contains(key) || !role[key].is_number_integer()) throw Failure("invalid service response");
    return route;
}
}

class Connection : public std::enable_shared_from_this<Connection> {
public:
    using Send = std::function<void(std::string,std::function<void(bool)>)>;
    using Close = std::function<void()>;
    using Removed = std::function<void()>;
    Connection(asio::io_context& io,const Config& config,Http& http, Pools& pools,Send send,Close close,Removed removed)
      : strand_(asio::make_strand(io)), timer_(strand_), config_(config), http_(http), pools_(pools), send_(std::move(send)), close_(std::move(close)), removed_(std::move(removed)) {}
    void start() { asio::dispatch(strand_,[self=shared_from_this()] { self->tick(); }); }
    void input(std::string line) { asio::post(strand_,[self=shared_from_this(),line=std::move(line)] { self->accept_input(line); }); }
    void stop() { asio::post(strand_,[self=shared_from_this()] { self->begin_close(); }); }
private:
    asio::strand<asio::io_context::executor_type> strand_;
    asio::steady_timer timer_;
    const Config& config_;
    Http& http_; Pools& pools_;
    Send send_; Close close_; Removed removed_;
    Json route_, last_view_, room_meta_;
    std::set<std::string> retired_room_worlds_;
    std::uint64_t room_world_generation_=0;
    std::deque<Input> commands_, moves_;
    std::deque<std::string> writes_;
    std::optional<Json> checkpoint_pending_;
    bool command_busy_=false, move_busy_=false, view_busy_=false, session_busy_=false, checkpoint_busy_=false, writing_=false;
    bool view_wanted_=false;
    bool ownership_transition_=false;
    bool closing_=false, disconnect_started_=false, disconnected_=false, removed_done_=false;
    std::uint64_t generation_=0, sent_event_=0;
    std::string sent_world_;
    unsigned count_=0;
    Clock::time_point rate_window_=Clock::now(), valid_until_{}, next_session_=Clock::now(), next_save_=Clock::now()+std::chrono::seconds(2), view_retry_at_{};
    bool ownership_change(const Input& in) const {
        const auto op=in.data.value("op",std::string());
        return op == "transfer" || op == "login" || op == "enter_arena" || op == "leave_arena" || op == "restart_battle" || op == "room_join" || op == "room_leave";
    }
    void reset_room_metadata() {
        room_meta_=nullptr; retired_room_worlds_.clear(); room_world_generation_=0;
    }
    bool current_room(const Json& room) const {
        if (!valid_room_metadata(room) || !route_.is_object()) return false;
        if (room["room_id"]==route_["role"]["scene_id"]) return true;
        // 旧Elemental个人arena是同一默认Scene的别名，不是动态房间。
        return config_.profile=="elemental" && room.value("legacy",false) && room["room_id"]==config_.login_scene &&
               (route_["role"]["scene_id"]==config_.login_scene || route_["role"]["scene_id"]==config_.arena_scene);
    }
    bool remember_room(const Json& room, std::uint64_t issued_room_generation) {
        if (!current_room(room)) throw Failure("invalid room metadata");
        const auto world=room["world_epoch"].get<std::string>();
        if (retired_room_worlds_.count(world)) return false;
        if (room_meta_.is_object() && room_meta_["room_id"]==room["room_id"]) {
            if (room_meta_["world_epoch"]==room["world_epoch"]) {
                const auto previous=room_meta_["revision"].get<std::uint64_t>(), next=room["revision"].get<std::uint64_t>();
                if (next<previous) return false;
                if (next==previous) {
                    // 同代次完整ACK与摘要view互补；摘要不能删掉已知的成员ready状态。
                    const auto tick=room_meta_.value("tick",Json());
                    room_meta_.update(room);
                    if (tick.is_number_integer() && room_meta_.contains("tick") && room_meta_["tick"].is_number_integer() && room_meta_["tick"]<tick) room_meta_["tick"]=tick;
                } else room_meta_=room;
            } else {
                if (issued_room_generation!=room_world_generation_) return false;
                if (retired_room_worlds_.size()>=64) throw Failure("room world history exhausted; reconnect",409);
                retired_room_worlds_.insert(room_meta_["world_epoch"].get<std::string>());
                ++room_world_generation_; room_meta_=room; last_view_=nullptr;
            }
        } else {
            ++room_world_generation_; room_meta_=room;
            // 首次看到Worker房间身份时，不把不含世界代次的旧快照当成同世界数据。
            if (last_view_.is_object() && !last_view_.contains("room")) last_view_=nullptr;
        }
        if (last_view_.is_object()) last_view_["room"]=room_meta_;
        return true;
    }
    bool remember_view_room(Json& view, std::uint64_t issued_room_generation) {
        if (!view.is_object()) throw Failure("invalid service response");
        if (!view.contains("room")) return room_meta_.is_null();
        const auto& room=view["room"];
        if (view.contains("world_epoch") && view["world_epoch"].is_string() && !view["world_epoch"].get_ref<const std::string&>().empty() &&
            valid_room_metadata(room) && view["world_epoch"]!=room["world_epoch"]) throw Failure("view world and room world disagree");
        if (!remember_room(room,issued_room_generation)) return false;
        view["room"]=room_meta_;
        return true;
    }
    template<class Work,class Done> void execute(asio::thread_pool& pool,Work work,Done done) {
        auto self=shared_from_this();
        asio::post(pool,[self,work=std::move(work),done=std::move(done)]() mutable {
            Result result;
            try { result=work(); } catch (const Failure& e) { result.error=e; }
            catch (...) { result.error=Failure("invalid service response"); }
            asio::post(self->strand_,[self,done=std::move(done),result=std::move(result)]() mutable { done(std::move(result)); });
        });
    }
    void write(Json out) {
        if (closing_) return;
        try {
            if ((out.value("op",std::string()) == "state" || out.value("op",std::string()) == "view") && out.contains("data") && out["data"].is_object()) {
                auto& view=out["data"];
                auto world=view.value("world_epoch",std::string());
                auto cursor=view.value("event_cursor",std::uint64_t(0));
                if (!world.empty()) {
                    if (out["op"] == "state" && world == sent_world_ && cursor >= sent_event_ && view.contains("events") && view["events"].is_array()) {
                        Json kept=Json::array();
                        for (const auto& event:view["events"]) if (!event.contains("id") || !event["id"].is_number_unsigned() || event["id"].get<std::uint64_t>() > sent_event_) kept.push_back(event);
                        view["events"]=std::move(kept);
                    }
                    sent_world_=std::move(world); sent_event_=cursor;
                }
            }
            auto wire=out.dump(-1,' ',false,Json::error_handler_t::strict);
            if (wire.size() > max_message_bytes || writes_.size() >= 64) { begin_close(); return; }
            writes_.push_back(std::move(wire));
            flush_write();
        } catch (...) { begin_close(); }
    }
    void flush_write() {
        if (writing_ || writes_.empty() || closing_) return;
        writing_=true;
        auto self=shared_from_this();
        send_(writes_.front(),[self](bool ok) {
            asio::post(self->strand_,[self,ok] {
                self->writing_=false;
                if (!self->writes_.empty()) self->writes_.pop_front();
                if (!ok) self->begin_close(); else self->flush_write();
            });
        });
    }
    void accept_input(const std::string& line) {
        if (closing_) return;
        auto now=Clock::now();
        if (now-rate_window_ >= std::chrono::seconds(1)) { count_=0; rate_window_=now; }
        if (++count_ > 40) { write(rejected(Json::object(),Failure("message rate limit exceeded"))); begin_close(); return; }
        Input in;
        try { in=parse_input(line,config_.profile); }
        catch (const Failure& e) { write(rejected(Json::object(),e)); begin_close(); return; }
        in.received_route=route_;
        if (!in.error.empty()) { write(rejected(in.data,Failure(in.error))); return; }
        if (in.data["op"] == "move") {
            if (moves_.size()+(move_busy_?1:0) >= 4) { write(rejected(in.data,Failure("movement input queue full"))); return; }
            moves_.push_back(std::move(in)); start_move(); return;
        }
        if (commands_.size() >= 32) { write(rejected(in.data,Failure("input queue full"))); begin_close(); return; }
        commands_.push_back(std::move(in)); start_command();
    }
    void start_move() {
        if (move_busy_ || moves_.empty() || closing_) return;
        if (command_busy_ && !commands_.empty() && ownership_change(commands_.front())) return;
        std::vector<Input> batch;
        while (!moves_.empty() && batch.size()<4) {
            auto in=std::move(moves_.front()); moves_.pop_front();
            if (route_.is_null()) { write(rejected(in.data,Failure("login required"))); continue; }
            if ((config_.profile == "elemental" || config_.profile == "skill_lab") && (in.received_route.is_null() || !same_role(in.received_route["role"],route_["role"]))) {
                write(in.received_route.is_null()?rejected(in.data,Failure("login required")):success(in.data,{{"accepted",false},{"reason","scene_changed"}})); continue;
            }
            if (Clock::now() > valid_until_) { write(rejected(in.data,Failure("session verification expired; reconnect"))); begin_close(); return; }
            batch.push_back(std::move(in));
        }
        if (batch.empty()) return;
        Json route=route_, body=reference(route["role"]);
        Json inputs=Json::array();
        for (const auto& in:batch) inputs.push_back({{"seq",in.data.value("seq",std::int64_t(0))},{"dx",in.data.value("dx",0.0)},{"dy",in.data.value("dy",0.0)},{"trace_id",in.data["id"]}});
        const bool multiple=batch.size()>1;
        if (multiple) body["inputs"]=std::move(inputs); else body.update(inputs[0]);
        move_busy_=true;
        execute(pools_.moves,[this,route,body,multiple] { return http_.call(route["scene_url"],multiple?"/v1/move_batch":"/v1/move",body); },[self=shared_from_this(),batch=std::move(batch),route,multiple](Result result) {
            self->move_busy_=false;
            if (!self->closing_) {
                if (!same_role(route["role"],self->route_.value("role",Json()))) result.error=Failure("scene or session changed while applying input");
                if (!result.error && multiple && (!result.value.is_object() || !result.value.contains("results") || !result.value["results"].is_array() || result.value["results"].size()!=batch.size())) result.error=Failure("invalid movement batch response");
                for (std::size_t i=0;i<batch.size();++i) {
                    if (result.error) self->write(rejected(batch[i].data,*result.error));
                    else if (!multiple) self->write(success(batch[i].data,std::move(result.value)));
                    else {
                        try {
                            const auto& reply=result.value["results"][i];
                            if (reply.value("ok",false)) self->write(success(batch[i].data,reply.value("data",Json())));
                            else self->write(rejected(batch[i].data,Failure(reply.value("error",std::string("movement was rejected")))));
                        } catch (...) { self->write(rejected(batch[i].data,Failure("invalid movement batch response"))); }
                    }
                }
                self->start_move(); self->start_command();
            }
            self->maybe_disconnect();
        });
    }
    void start_command() {
        if (command_busy_ || commands_.empty() || closing_) return;
        const auto& first=commands_.front();
        if (ownership_change(first) && move_busy_) return;
        command_busy_=true;
        Input in=first;
        Json route=route_;
        const auto op=in.data.value("op",std::string());
        ownership_transition_=ownership_change(in);
        if (op == "view" || op == "skill_init" || ownership_transition_) ++generation_;
        const auto command_generation=generation_;
        const auto command_room_generation=room_world_generation_;
        execute(pools_.commands,[this,in,route] { return perform(in,route); },[self=shared_from_this(),in,route,command_generation,command_room_generation](Result result) {
            self->command_busy_=false;
            self->ownership_transition_=false;
            self->commands_.pop_front();
            const auto op=in.data.value("op",std::string());
            try {
                if (op=="login" && result.error && result.established_route.is_object()) {
                    // 入场已成功而初始视图失败，仍持有精确会话，供重试/断开时清理。
                    Result accepted(result.established_route);
                    self->route_=require_route(accepted); self->last_view_=nullptr; self->reset_room_metadata();
                    self->valid_until_=Clock::now()+std::chrono::seconds(4); self->next_session_=Clock::now()+std::chrono::seconds(1);
                }
                if (!result.error) {
                    bool returns_route=op=="login" || op=="transfer" || op=="room_join" || op=="room_leave";
                    if (self->config_.profile=="elemental" && (op=="enter_arena" || op=="leave_arena" || op=="restart_battle") && result.value.is_object() && result.value.contains("scene_url")) returns_route=true;
                    if (returns_route) {
                        self->route_=require_route(result); self->last_view_=nullptr; self->reset_room_metadata();
                        if (self->route_.contains("initial_view")) {
                            auto view=self->route_["initial_view"]; self->route_.erase("initial_view");
                            overlay_balance(view,self->route_["role"]);
                            if (!self->remember_view_room(view,0)) throw Failure("room changed while requesting initial view",409);
                            self->last_view_=std::move(view);
                        }
                        self->valid_until_=Clock::now()+std::chrono::seconds(4); self->next_session_=Clock::now()+std::chrono::seconds(1);
                        if (op=="enter_arena" || op=="leave_arena" || op=="restart_battle") result.value={{"accepted",true},{"reason",""}};
                        else if (op=="login" && self->config_.profile=="elemental" && self->last_view_.is_object()) result.value=client_view(self->last_view_,self->config_.profile).at("self");
                        else result.value=client_role(self->route_["role"],self->config_.profile);
                    } else if (op=="view") {
                        if (command_generation!=self->generation_ || !same_role(route.value("role",Json()),self->route_.value("role",Json()))) throw Failure("scene or session changed while requesting view",409);
                        if (self->route_.is_object()) {
                            if (!same_role(result.value.value("self",Json()),self->route_["role"])) throw Failure("invalid service response");
                            overlay_balance(result.value,self->route_["role"]);
                        }
                        if (self->remember_view_room(result.value,command_room_generation) && fresh_view(self->last_view_,result.value)) self->last_view_=result.value;
                        else if (self->last_view_.is_object()) result.value=self->last_view_;
                        else throw Failure("room changed while requesting view; retry",409);
                        result.value=client_view(result.value,self->config_.profile);
                    } else if ((op=="room_ready" || op=="room_start" || op=="room_finish" || op=="room_status") &&
                               self->route_.is_object() && same_role(route.value("role",Json()),self->route_["role"]) &&
                               result.value.is_object() && result.value.value("room_id",std::string())==self->route_["role"].value("scene_id",std::string())) {
                        self->remember_room(result.value,command_room_generation);
                        result.value=self->room_meta_;
                    } else if (op=="cast" || op=="revive") {
                        if (self->route_.is_object() && same_role(route.value("role",Json()),self->route_["role"])) {
                            self->last_view_=action_snapshot(result.value,self->route_,self->last_view_);
                            if (result.value.value("accepted",true)) self->submit_checkpoint(self->last_view_);
                        }
                    } else if (op=="claim" && self->route_.is_object()) self->route_["role"]["coins"]=result.value.at("coins");
                }
            } catch (const Failure& e) { result.error=e; } catch (...) { result.error=Failure("invalid service response"); }
            if (!self->closing_) {
                self->write(result.error?rejected(in.data,*result.error):success(in.data,std::move(result.value)));
                self->start_command(); self->start_move();
            }
            self->maybe_disconnect();
        });
    }
    Result perform(const Input& in,Json route) {
        const Json& request=in.data;
        const auto op=request.value("op",std::string());
        if (!permitted(config_.profile,op)) throw Failure("operation is not available in "+config_.profile+" profile");
        const bool account=op=="account_register" || op=="account_login" || op=="account_bind";
        if (!account && !request.value("password",std::string()).empty()) throw Failure("use account_register, account_login or account_bind for passwords",400);
        if (account) {
            if (!route.is_null()) throw Failure("account operations require a connection that has not entered the game");
            if ((op=="account_bind" && !request.value("name",std::string()).empty()) || (op!="account_bind" && !request.value("token",std::string()).empty())) throw Failure("account request contains conflicting credential fields",400);
            Json p{{"password",request.value("password",std::string())}};
            if (op=="account_bind") p["token"]=request.value("token",std::string()); else p["name"]=request.value("name",std::string());
            return http_.call(config_.backend_url,"/v1/"+op,p,20);
        }
        if (op=="ping") {
            Json data{{"unix_ms",unix_ms()},{"account_protocol","account-password-v1"}};
            if (config_.profile!="full") data["protocol_version"]=config_.protocol();
            return {std::move(data),std::nullopt};
        }
        if (op=="register") return http_.call(config_.backend_url,"/v1/register",{{"name",request.value("name",std::string())}},20);
        if (op=="login") {
            if (!route.is_null()) throw Failure("already logged in; reconnect to log in again");
            auto result=http_.call(config_.backend_url,"/v1/login",{{"token",request.value("token",std::string())}},20);
            route=require_route(result);
            // 后续任何步骤失败，都保留最后一份已验证归属；超时不推测迁移是否提交。
            Json established=route;
            try {
                const auto desired_profile=config_.profile=="full" || config_.profile=="elemental" ? std::string("combat") : config_.profile;
                if (route.contains("room_id") && (config_.profile=="elemental" || route.value("room_profile",std::string())!=desired_profile)) {
                    auto leave=reference(route["role"]); leave["profile"]=desired_profile;
                    leave["request_id"]="entry-profile-"+sha256_prefix(route["role"]["role_id"].get<std::string>())+"-"+std::to_string(route["role"]["session_epoch"].get<std::int64_t>());
                    auto left=http_.call(config_.backend_url,"/v1/room_leave",leave,20);
                    auto next=require_route(left);
                    if (next.contains("room_id") || next["role"]["role_id"]!=route["role"]["role_id"] || next["role"]["session_epoch"]!=route["role"]["session_epoch"] ||
                        next["role"]["owner_epoch"].get<std::int64_t>()<=route["role"]["owner_epoch"].get<std::int64_t>()) throw Failure("invalid room exit ownership transition");
                    result=std::move(left); route=std::move(next); established=route;
                }
                if (!route.contains("room_id") && !config_.login_scene.empty() && route["role"].value("scene_id",std::string())!=config_.login_scene && !(config_.profile=="elemental" && route["role"]["scene_id"]==config_.arena_scene)) {
                    auto body=reference(route["role"]); body["target_scene_id"]=config_.login_scene;
                    body["request_id"]="entry-"+route["role"]["role_id"].get<std::string>()+"-"+std::to_string(route["role"]["session_epoch"].get<std::int64_t>());
                    result=http_.call(config_.backend_url,"/v1/transfer",body,20);
                    auto next=require_route(result);
                    if (next["role"]["role_id"]!=route["role"]["role_id"] || next["role"]["session_epoch"]!=route["role"]["session_epoch"] || next["role"]["scene_id"]!=config_.login_scene || next["role"]["owner_epoch"].get<std::int64_t>()<=route["role"]["owner_epoch"].get<std::int64_t>()) throw Failure("invalid entry ownership transition");
                    route=std::move(next); established=route;
                }
                if (config_.profile=="elemental" && route["role"]["scene_id"]==config_.arena_scene) {
                    auto initial=http_.call(route["scene_url"],"/v1/view",reference(route["role"]));
                    if (initial.error) throw *initial.error;
                    if (!initial.value.is_object() || !same_role(initial.value.value("self",Json()),route["role"])) throw Failure("invalid service response");
                    result.value["initial_view"]=std::move(initial.value);
                }
                return result;
            } catch (const Failure& error) {
                Result failed(nullptr,error); failed.established_route=std::move(established); return failed;
            } catch (...) {
                Result failed(nullptr,Failure("invalid service response")); failed.established_route=std::move(established); return failed;
            }
        }
        if (route.is_null()) throw Failure("login required");
        if (config_.profile=="elemental" && (op=="cast" || op=="enter_arena" || op=="leave_arena" || op=="restart_battle")) return perform_elemental(request,route);
        auto body=reference(route["role"]);
        if (op.rfind("room_",0)==0) {
            body["profile"]=config_.profile=="full" ? "combat" : config_.profile;
            for (const auto* field:{"room_id","request_id","capacity","ready"})
                if (request.contains(field)) body[field]=request[field];
            return http_.call(config_.backend_url,"/v1/"+op,body,20);
        }
        if (op=="view") return http_.call(route["scene_url"],"/v1/view",body);
        if (op=="skill_init") {
            if (config_.profile!="skill_lab") throw Failure("skill initialization is disabled");
            auto id=request.value("skill_id",std::string());
            if (id.empty() || id.size()>32 || !request.contains("definition") || request["definition"].dump().size()>2048) throw Failure("skill_id and definition are required");
            body["skill_id"]=id; body["definition"]=request["definition"]; body["world_epoch"]=request.value("world_epoch",std::string());
            body["expected_definition_version"]=request.value("expected_definition_version",std::uint64_t(0));
            return http_.call(route["scene_url"],"/v1/skill_init",body);
        }
        if (op=="cast" || op=="revive") {
            auto seq=request.value("seq",std::int64_t(0));
            if (seq<=0) throw Failure("positive action seq is required");
            body["seq"]=seq;
            if (op=="cast") {
                auto id=request.value("skill_id",std::string()), target=request.value("target_id",std::string());
                if (id.empty() || id.size()>32 || target.size()>128) throw Failure("invalid skill or target");
                body["skill_id"]=id; body["target_id"]=target;
                if (config_.profile=="skill_lab" || id=="arc_bolt" || id=="stone_wall") {
                    body["dx"]=request.value("dx",0.0); body["dy"]=request.value("dy",0.0);
                    for (const auto* key:{"x","y"}) if (request.contains(key)) body[key]=request[key];
                }
                if (config_.profile=="skill_lab") { body["definition_version"]=request.value("definition_version",std::uint64_t(0)); body["world_epoch"]=request.value("world_epoch",std::string()); }
            }
            return http_.call(route["scene_url"],"/v1/"+op,body);
        }
        if (op=="transfer") {
            auto id=request.value("request_id",std::string());
            if (id.empty() || id.size()>128) throw Failure("request_id is required (1..128 characters)");
            body["request_id"]=id; body["target_scene_id"]=request.value("target_scene_id",std::string());
            return http_.call(config_.backend_url,"/v1/transfer",body,20);
        }
        if (op=="claim") return http_.call(config_.backend_url,"/v1/claim",{{"role_id",route["role"]["role_id"]},{"session_epoch",route["role"]["session_epoch"]}},20);
        throw Failure("unknown operation");
    }
    Result perform_elemental(const Json& request,const Json& route) {
        const auto op=request.value("op",std::string()), scene=request.value("scene_id",std::string());
        auto owner=request.value("owner_epoch",std::int64_t(0));
        if (scene.empty() || scene.size()>64 || owner<=0) throw Failure("scene_id and positive owner_epoch are required");
        if (scene!=route["role"].value("scene_id",std::string()) || request["owner_epoch"]!=route["role"]["owner_epoch"]) return {{{"accepted",false},{"reason","scene_changed"}},std::nullopt};
        auto body=reference(route["role"]);
        if (op=="cast") {
            if (!request.contains("x") || !request.contains("y") || std::trunc(request["x"].get<double>())!=request["x"].get<double>() || std::trunc(request["y"].get<double>())!=request["y"].get<double>() || request.value("element",std::string()).size()>32) throw Failure("element and integer target cells are required");
            if (scene!=config_.arena_scene) return {{{"accepted",false},{"reason","outside_arena"}},std::nullopt};
            body["scene_id"]=scene; body["request_id"]=request["id"]; body["element"]=request.value("element",std::string()); body["x"]=request["x"]; body["y"]=request["y"];
            return http_.call(route["scene_url"],"/v1/elemental_cast",body);
        }
        if (op=="enter_arena" && scene!=config_.login_scene) return {{{"accepted",false},{"reason","already_in_arena"}},std::nullopt};
        if (op!="enter_arena" && scene!=config_.arena_scene) return {{{"accepted",false},{"reason","outside_arena"}},std::nullopt};
        auto target=op=="leave_arena"?config_.login_scene:config_.arena_scene;
        body["target_scene_id"]=target;
        body["request_id"]="elemental-"+std::to_string(route["role"]["session_epoch"].get<std::int64_t>())+"-"+std::to_string(owner)+"-"+op+"-"+sha256_prefix(request["id"]);
        if (op=="restart_battle") body["restart"]=true;
        auto result=http_.call(config_.backend_url,"/v1/transfer",body,20);
        auto next=require_route(result);
        if (next["role"]["role_id"]!=route["role"]["role_id"] || next["role"]["session_epoch"]!=route["role"]["session_epoch"] || next["role"]["scene_id"]!=target || next["role"]["owner_epoch"].get<std::int64_t>()<=owner) throw Failure("invalid elemental ownership transition");
        return result;
    }
    void tick() {
        if (closing_) return;
        auto now=Clock::now();
        if (!route_.is_null() && !ownership_transition_) {
            if (!session_busy_ && now>=next_session_) {
                session_busy_=true; next_session_=now+std::chrono::seconds(1);
                auto previous=route_;
                execute(pools_.sessions,[this,previous] { return http_.call(config_.backend_url,"/v1/session",{{"role_id",previous["role"]["role_id"]},{"session_epoch",previous["role"]["session_epoch"]}},3); },[self=shared_from_this(),previous](Result result) {
                    self->session_busy_=false;
                    if (!self->closing_ && !self->ownership_transition_ && same_role(previous["role"],self->route_.value("role",Json()))) {
                        if (result.error) { self->write(rejected({{"op","session"}},*result.error)); self->begin_close(); }
                        else try {
                            auto current=require_route(result);
                            // 在途旧 session 结果不能倒退刚完成的迁移。
                            if (!same_role(current["role"],previous["role"])) { ++self->generation_; self->last_view_=nullptr; self->reset_room_metadata(); }
                            self->route_=std::move(current); self->valid_until_=Clock::now()+std::chrono::seconds(4);
                        } catch (...) { self->begin_close(); }
                    } else if (!self->closing_) {
                        // 迁移前已发出的会话查询（包括超时）不属于新 owner。
                        self->next_session_=Clock::now();
                    }
                    self->maybe_disconnect();
                });
            }
            if (now>=view_retry_at_) { view_wanted_=true; if (!view_busy_) start_view(); }
            if (now>=next_save_) { next_save_=now+std::chrono::seconds(2); submit_checkpoint(last_view_); }
        }
        timer_.expires_after(std::chrono::microseconds(1000000/config_.state_hz));
        timer_.async_wait([self=shared_from_this()](ErrorCode e) { if (!e) self->tick(); });
    }
    void start_view() {
        if (route_.is_null() || closing_ || ownership_transition_) return;
        view_busy_=true; view_wanted_=false;
        auto route=route_; auto generation=generation_; auto room_generation=room_world_generation_;
        execute(pools_.views,[this,route] { return http_.call(route["scene_url"],"/v1/view",reference(route["role"])); },[self=shared_from_this(),route,generation,room_generation](Result result) {
            self->view_busy_=false;
            if (!self->closing_ && generation==self->generation_ && same_role(route["role"],self->route_.value("role",Json()))) {
                if (result.error) { self->view_retry_at_=Clock::now()+std::chrono::seconds(1); self->view_wanted_=false; self->write(rejected({{"op","state"}},*result.error)); }
                else try {
                    if (!same_role(result.value.value("self",Json()),self->route_["role"])) throw Failure("invalid service response");
                    if (self->remember_view_room(result.value,room_generation) && fresh_view(self->last_view_,result.value)) {
                        overlay_balance(result.value,self->route_["role"]); self->last_view_=result.value;
                        self->write(success({{"op","state"}},client_view(std::move(result.value),self->config_.profile)));
                    }
                } catch (...) { self->write(rejected({{"op","state"}},Failure("invalid service response"))); }
            }
            if (!self->closing_ && self->view_wanted_ && Clock::now()>=self->view_retry_at_) self->start_view();
            self->maybe_disconnect();
        });
    }
    void submit_checkpoint(const Json& view) {
        if (!route_.is_object() || !view.is_object() || !same_role(view.value("self",Json()),route_["role"])) return;
        checkpoint_pending_=view["self"]; start_checkpoint();
    }
    void start_checkpoint() {
        if (checkpoint_busy_ || !checkpoint_pending_) { finish_close(); return; }
        auto role=std::move(*checkpoint_pending_); checkpoint_pending_.reset(); checkpoint_busy_=true;
        execute(pools_.checkpoints,[this,role] { return http_.call(config_.backend_url,"/v1/checkpoint",{{"role",role}}); },[self=shared_from_this()](Result result) {
            self->checkpoint_busy_=false;
            if (result.error) std::cerr << "checkpoint failed status=" << result.error->status << '\n';
            self->start_checkpoint();
        });
    }
    void begin_close() {
        if (!closing_) {
            closing_=true; ++generation_; timer_.cancel(); close_(); writes_.clear(); moves_.clear();
            // 已开始的事务继续完成；未发送的命令丢弃。
            if (command_busy_) commands_.erase(commands_.begin()+1,commands_.end()); else commands_.clear();
        }
        maybe_disconnect();
    }
    void maybe_disconnect() {
        if (!closing_ || disconnect_started_ || command_busy_ || move_busy_ || view_busy_ || session_busy_) return;
        disconnect_started_=true;
        if (!route_.is_object()) { disconnected_=true; finish_close(); return; }
        const auto route=route_;
        execute(pools_.views,[this,route] { return http_.call(route["scene_url"],"/v1/disconnect",reference(route["role"])); },[self=shared_from_this(),route](Result result) {
            if (!result.error && result.value.is_object() && result.value.contains("role") && same_role(result.value["role"],route["role"])) {
                auto role=result.value["role"]; role["coins"]=route["role"]["coins"];
                self->last_view_={{"self",role},{"scene_id",role["scene_id"]}};
            }
            self->disconnected_=true; self->submit_checkpoint(self->last_view_); self->finish_close();
        });
    }
    void finish_close() {
        if (!closing_ || !disconnected_ || checkpoint_busy_ || checkpoint_pending_ || removed_done_) return;
        removed_done_=true; send_={}; close_={}; removed_(); removed_={};
    }
};

struct Runtime::Impl {
    Config config;
    asio::io_context io;
    Http http_client;
    Pools pools;
    std::vector<std::unique_ptr<tcp::acceptor>> acceptors;
    std::mutex connections_mutex;
    std::map<std::uint64_t,std::shared_ptr<Connection>> connections;
    std::uint64_t next_id=0;
    std::atomic<int> active{0};
    std::atomic<bool> stopping{false};
    asio::signal_set signals;
    asio::steady_timer shutdown_timer;
    explicit Impl(Config c):config(std::move(c)),http_client(config),signals(io,SIGINT,SIGTERM),shutdown_timer(io) {}
    std::shared_ptr<Connection> create(Connection::Send send,Connection::Close close) {
        std::lock_guard<std::mutex> lock(connections_mutex);
        if (stopping || connections.size()>=static_cast<std::size_t>(config.max_connections)) return {};
        auto id=++next_id;
        auto result=std::make_shared<Connection>(io,config,http_client,pools,std::move(send),std::move(close),[this,id] {
            std::lock_guard<std::mutex> guard(connections_mutex); connections.erase(id); --active;
        });
        connections[id]=result; ++active; return result;
    }
    void listen(const std::string& address,bool web);
    void accept(tcp::acceptor& acceptor,bool web);
    void shutdown() {
        stopping=true;
        for (auto& acceptor:acceptors) { ErrorCode ignored; acceptor->close(ignored); }
        std::vector<std::shared_ptr<Connection>> snapshot;
        { std::lock_guard<std::mutex> lock(connections_mutex); for (auto& entry:connections) snapshot.push_back(entry.second); }
        for (auto& conn:snapshot) conn->stop();
        drain();
    }
    void drain() {
        if (active==0) { io.stop(); return; }
        shutdown_timer.expires_after(std::chrono::milliseconds(100));
        shutdown_timer.async_wait([this](ErrorCode e) { if (!e) drain(); });
    }
    Json health() const { return {{"ok",true},{"service","native-gateway"},{"connections",active.load()},{"profile",config.profile},{"state_hz",config.state_hz},{"protocol_version",config.protocol()},{"implementation","cpp"}}; }
};

namespace {
class TcpPeer : public std::enable_shared_from_this<TcpPeer> {
    beast::tcp_stream stream_;
    std::string incoming_;
    std::weak_ptr<Connection> connection_;
public:
    explicit TcpPeer(tcp::socket socket):stream_(std::move(socket)) {}
    void attach(const std::shared_ptr<Connection>& connection) { connection_=connection; connection->start(); read(); }
    void send(std::string message,std::function<void(bool)> done) {
        auto self=shared_from_this();
        asio::post(stream_.get_executor(),[self,message=std::make_shared<std::string>(std::move(message)+"\n"),done=std::move(done)]() mutable {
            self->stream_.expires_after(std::chrono::seconds(3));
            asio::async_write(self->stream_,asio::buffer(*message),[self,message,done=std::move(done)](ErrorCode e,std::size_t) { done(!e); });
        });
    }
    void close() { asio::post(stream_.get_executor(),[self=shared_from_this()] { ErrorCode ignored; self->stream_.socket().cancel(ignored); self->stream_.socket().shutdown(tcp::socket::shutdown_both,ignored); self->stream_.socket().close(ignored); }); }
private:
    void read() {
        stream_.expires_after(std::chrono::seconds(60));
        asio::async_read_until(stream_,asio::dynamic_buffer(incoming_,max_message_bytes+2),'\n',[self=shared_from_this()](ErrorCode e,std::size_t n) {
            auto conn=self->connection_.lock(); if (!conn) return;
            if (e || n>max_message_bytes+1) { conn->stop(); return; }
            auto line=self->incoming_.substr(0,n-1); self->incoming_.erase(0,n);
            if (!line.empty() && line.back()=='\r') line.pop_back();
            conn->input(std::move(line)); self->read();
        });
    }
};
class WebPeer : public std::enable_shared_from_this<WebPeer> {
    websocket::stream<beast::tcp_stream> ws_;
    beast::flat_buffer buffer_;
    http::request_parser<http::string_body> parser_;
    std::weak_ptr<Connection> connection_;
    std::function<std::shared_ptr<Connection>(Connection::Send,Connection::Close)> create_;
    const Config& config_;
    Http& client_;
    Pools& pools_;
    std::function<Json()> health_;
    bool write_busy_=false, closing_=false, close_started_=false;
    websocket::close_code close_code_=websocket::close_code::normal;
public:
    WebPeer(tcp::socket socket,const Config& config,Http& client,Pools& pools,std::function<std::shared_ptr<Connection>(Connection::Send,Connection::Close)> create,std::function<Json()> health)
      :ws_(std::move(socket)),create_(std::move(create)),config_(config),client_(client),pools_(pools),health_(std::move(health)) {}
    void run() {
        parser_.body_limit(0); parser_.header_limit(16384);
        beast::get_lowest_layer(ws_).expires_after(std::chrono::seconds(5));
        http::async_read(ws_.next_layer(),buffer_,parser_,[self=shared_from_this()](ErrorCode e,std::size_t) { if (!e) self->handshake(); });
    }
private:
    void reply(unsigned status,std::string body) {
        auto response=std::make_shared<http::response<http::string_body>>(static_cast<http::status>(status),11);
        response->set(http::field::content_type,"application/json"); response->keep_alive(false); response->body()=std::move(body); response->prepare_payload();
        beast::get_lowest_layer(ws_).expires_after(std::chrono::seconds(3));
        http::async_write(ws_.next_layer(),*response,[self=shared_from_this(),response](ErrorCode,std::size_t) { self->close(); });
    }
    void handshake() {
        const auto& request=parser_.get();
        if (request.method()!=http::verb::get) { reply(404,"{\"error\":\"not found\"}"); return; }
        if (request.target()=="/healthz") { reply(200,health_().dump()); return; }
        if (request.target()=="/readyz") {
            auto self=shared_from_this();
            asio::post(pools_.sessions,[self] {
                auto result=self->client_.call(self->config_.backend_url,"/healthz",Json::object(),2,true);
                asio::post(self->ws_.get_executor(),[self,result=std::move(result)] { self->reply(result.error?503:200,result.error?"{\"error\":\"backend unavailable\"}":"{\"ok\":true}"); });
            }); return;
        }
        if (request.target()!="/ws") { reply(request.target().rfind("/ws?",0)==0?400:404,"{\"error\":\"unsupported path or query\"}"); return; }
        auto origin=std::string(request[http::field::origin]);
        if (std::find(config_.origins.begin(),config_.origins.end(),origin)==config_.origins.end()) { reply(403,"{\"error\":\"origin is not allowed\"}"); return; }
        if (!websocket::is_upgrade(request)) { reply(400,"{\"error\":\"invalid upgrade\"}"); return; }
        auto self=shared_from_this();
        auto conn=create_([self](std::string message,std::function<void(bool)> done) { self->send(std::move(message),std::move(done)); },[self] { self->close(); });
        if (!conn) { reply(503,"{\"error\":\"gateway unavailable\"}"); return; }
        connection_=conn;
        ws_.read_message_max(max_message_bytes);
        websocket::permessage_deflate deflate; deflate.server_enable=true; deflate.client_enable=true; deflate.compLevel=1; deflate.server_no_context_takeover=true; deflate.client_no_context_takeover=true; ws_.set_option(deflate);
        ws_.set_option(websocket::stream_base::timeout{std::chrono::seconds(5),std::chrono::seconds(60),true});
        beast::get_lowest_layer(ws_).expires_never();
        ws_.async_accept(request,[self,conn](ErrorCode e) { if(e) conn->stop(); else { conn->start(); self->read(); } });
    }
    void read() {
        ws_.async_read(buffer_,[self=shared_from_this()](ErrorCode e,std::size_t) {
            auto conn=self->connection_.lock(); if (!conn) return;
            if(e) { conn->stop(); return; }
            if(!self->ws_.got_text()) { self->request_close(websocket::close_code::unknown_data); conn->stop(); return; }
            auto message=beast::buffers_to_string(self->buffer_.data()); self->buffer_.consume(self->buffer_.size());
            auto parsed=Json::parse(message,nullptr,false);
            if (parsed.is_discarded() || !parsed.is_object()) { self->request_close(websocket::close_code::bad_payload); conn->stop(); return; }
            conn->input(std::move(message)); self->read();
        });
    }
    void send(std::string message,std::function<void(bool)> done) {
        auto self=shared_from_this();
        asio::post(ws_.get_executor(),[self,message=std::make_shared<std::string>(std::move(message)),done=std::move(done)]() mutable {
            if (self->closing_) { done(false); return; }
            self->write_busy_=true;
            self->ws_.text(true);
            self->ws_.async_write(asio::buffer(*message),[self,message,done=std::move(done)](ErrorCode e,std::size_t) {
                self->write_busy_=false; done(!e); if (self->closing_) self->do_close();
            });
        });
    }
    void request_close(websocket::close_code code) { close_code_=code; closing_=true; do_close(); }
    void hard_close() {
        ErrorCode ignored; auto& socket=beast::get_lowest_layer(ws_).socket(); socket.cancel(ignored); socket.shutdown(tcp::socket::shutdown_both,ignored); socket.close(ignored);
    }
    void do_close() {
        if (write_busy_ || close_started_) return;
        close_started_=true;
        if (!ws_.is_open()) { hard_close(); return; }
        ws_.async_close(close_code_,[self=shared_from_this()](ErrorCode) { self->hard_close(); });
    }
    void close() {
        asio::post(ws_.get_executor(),[self=shared_from_this()] {
            self->closing_=true; self->do_close();
        });
    }
};
}
void Runtime::Impl::listen(const std::string& address,bool web) {
    if (address.empty()) return;
    auto parts=endpoint_parts(address);
    auto acceptor=std::make_unique<tcp::acceptor>(io);
    tcp::endpoint endpoint(asio::ip::make_address(parts.first),parts.second);
    acceptor->open(endpoint.protocol()); acceptor->set_option(asio::socket_base::reuse_address(true)); acceptor->bind(endpoint); acceptor->listen();
    std::cerr << "native-gateway listener=" << acceptor->local_endpoint() << " transport=" << (web?"http-websocket":"tcp-jsonl") << '\n';
    acceptors.push_back(std::move(acceptor)); accept(*acceptors.back(),web);
}
void Runtime::Impl::accept(tcp::acceptor& acceptor,bool web) {
    acceptor.async_accept(asio::make_strand(io),[this,&acceptor,web](ErrorCode e,tcp::socket socket) {
        if (!e) {
            socket.set_option(tcp::no_delay(true));
            if (web) std::make_shared<WebPeer>(std::move(socket),config,http_client,pools,[this](Connection::Send send,Connection::Close close) { return create(std::move(send),std::move(close)); },[this] { return health(); })->run();
            else {
                auto peer=std::make_shared<TcpPeer>(std::move(socket));
                auto connection=create([peer](std::string message,std::function<void(bool)> done) { peer->send(std::move(message),std::move(done)); },[peer] { peer->close(); });
                if (connection) peer->attach(connection); else peer->close();
            }
        }
        if (!stopping && acceptor.is_open()) accept(acceptor,web);
    });
}
Runtime::Runtime(Config config):impl_(std::make_unique<Impl>(std::move(config))) {}
Runtime::~Runtime()=default;
int Runtime::run() {
    impl_->listen(impl_->config.tcp_listen,false);
    impl_->listen(impl_->config.ws_listen,true);
    if (impl_->config.health_listen!=impl_->config.ws_listen) impl_->listen(impl_->config.health_listen,true);
    impl_->signals.async_wait([this](ErrorCode e,int) { if (!e) impl_->shutdown(); });
    std::vector<std::thread> workers;
    for (int i=0;i<3;++i) workers.emplace_back([this] { impl_->io.run(); });
    impl_->io.run(); for(auto& worker:workers) worker.join(); impl_->pools.join(); return 0;
}
}
