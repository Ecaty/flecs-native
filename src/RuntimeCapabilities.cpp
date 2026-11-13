#include "flecs_unity.h"
#include "FlecsComponents.hpp"
#include <behaviortree_cpp/bt_factory.h>
#include <behaviortree_cpp/xml_parsing.h>
#include <behaviortree_cpp/contrib/json.hpp>
#include <clipper2/clipper.h>
#define TSF_IMPLEMENTATION
#include "tsf.h"
#include <atomic>
#include <mutex>
#include <array>
#include <algorithm>
#include <cmath>
#include <memory>
#include <unordered_map>
#include <vector>
#include <string>

namespace {
using namespace flecs_unity;
struct NativeFootprint : Label {};
struct NativeTree : Label { int32_t revision=1; bool enabled=true, loop=false; };
struct NativeTreeResult : Label { int32_t status=0, revision=0; int64_t ticks=0; };
struct SimWait : BT::StatefulActionNode {
    double& now; double end=0;
    SimWait(const std::string& name,const BT::NodeConfig& c,double& time): StatefulActionNode(name,c),now(time) {}
    static BT::PortsList providedPorts(){ return {BT::InputPort<double>("seconds",0.0,"Simulation seconds")}; }
    BT::NodeStatus onStart() override { double seconds=getInput<double>("seconds").value(); if(!std::isfinite(seconds)||seconds<0||seconds>3600) throw BT::RuntimeError("Invalid simulation wait"); end=now+seconds; return seconds==0?BT::NodeStatus::SUCCESS:BT::NodeStatus::RUNNING; }
    BT::NodeStatus onRunning() override { return now>=end?BT::NodeStatus::SUCCESS:BT::NodeStatus::RUNNING; }
    void onHalted() override {}
};
struct Trees {
    fu_context* ctx; double now=0; BT::BehaviorTreeFactory factory;
    struct Entry { std::string source; int revision=0; bool finished=false; std::unique_ptr<BT::Tree> tree; };
    std::unordered_map<uint64_t,Entry> entries;
    std::unordered_map<uint64_t,uint64_t> dependencies;
    std::unordered_map<uint64_t,std::string> geometry;
    explicit Trees(fu_context* c):ctx(c){
        factory.registerNodeType<SimWait>("SimWait",std::ref(now));
        factory.registerSimpleAction("FlecsSet",[this](BT::TreeNode& n){
            const auto entity=n.getInput<std::string>("entity").value();
            const auto component=n.getInput<std::string>("component").value();
            const auto& raw=static_cast<const BT::TreeNode&>(n).config().input_ports.at("value");
            const auto value=(!raw.empty() && (raw[0]=='{' || raw[0]=='['))?raw:n.getInput<std::string>("value").value();
            return fu_set_component_json(ctx,entity.c_str(),component.c_str(),value.c_str())==0?BT::NodeStatus::SUCCESS:BT::NodeStatus::FAILURE;
        },{BT::InputPort<std::string>("entity"),BT::InputPort<std::string>("component"),BT::InputPort<std::string>("value")});
        factory.registerSimpleCondition("FlecsQuery",[this](BT::TreeNode& n){
            auto expr=n.getInput<std::string>("expression").value(); auto* w=static_cast<ecs_world_t*>(fu_world(ctx));
            ecs_query_desc_t d{}; d.expr=expr.c_str(); auto* q=ecs_query_init(w,&d); if(!q) return BT::NodeStatus::FAILURE;
            auto it=ecs_query_iter(w,q); bool found=ecs_query_next(&it); if(found) ecs_iter_fini(&it); ecs_query_fini(q);
            return found?BT::NodeStatus::SUCCESS:BT::NodeStatus::FAILURE;
        },{BT::InputPort<std::string>("expression")});
    }
};
std::unordered_map<fu_context*,std::unique_ptr<Trees>> worlds;
struct AudioEvent { int kind,note; float velocity; int program; uint64_t sample; };
std::mutex fontGate;
struct SoundBank { tsf* font; explicit SoundBank(tsf* f):font(f){} ~SoundBank(){tsf_close(font);} };
std::unordered_map<uint64_t,std::weak_ptr<SoundBank>> banks;
struct Audio {
    std::shared_ptr<SoundBank> bank; tsf* font=nullptr; int rate; uint64_t position=0; uint32_t rng=0x12345678;
    std::array<AudioEvent,256> events{},pending{}; int pendingCount=0; std::atomic<uint32_t> write{0},read{0};
    int oscillator=0; float gain=0,frequency=440,phase=0; uint64_t end=0;
    Audio(std::shared_ptr<SoundBank> b,tsf* f,int r):bank(std::move(b)),font(f),rate(r){}
    ~Audio(){std::lock_guard<std::mutex> lock(fontGate);if(font)tsf_close(font);bank.reset();}
};
}
extern "C" {

FU_API int fu_cap_has(fu_context* ctx,uint64_t entity,const char* type){auto* w=static_cast<ecs_world_t*>(fu_world(ctx));auto id=ecs_lookup(w,type);return id&&ecs_is_alive(w,entity)&&ecs_has_id(w,entity,id);}
FU_API void fu_cap_ack(fu_context* ctx,uint64_t entity){auto* w=static_cast<ecs_world_t*>(fu_world(ctx));auto dirty=ecs_lookup(w,"CapDirty");if(dirty&&ecs_is_alive(w,entity))ecs_remove_id(w,entity,dirty);}
struct CapOutput { int32_t phase,revision,generation; void* message; };
static int cap_ready(ecs_world_t* w,uint64_t entity,int depth){
    if(depth>32)return -1;auto relation=ecs_lookup(w,"CapDependsOn"),state=ecs_lookup(w,"CapState");if(!relation||!state)return 1;
    auto binding=ecs_lookup(w,"CapBinding");if(binding){auto* data=ecs_get_id(w,entity,binding);if(data){auto c=ecs_meta_cursor(w,binding,const_cast<void*>(data));ecs_meta_push(&c);if(ecs_meta_member(&c,"enabled")==0&&!ecs_meta_get_bool(&c))return 0;}}
    auto* type=ecs_get_type(w,entity);if(!type)return 0;
    for(int i=0;i<type->count;++i){auto id=type->array[i];if(ECS_IS_PAIR(id)&&ecs_pair_first(w,id)==relation){auto target=ecs_pair_second(w,id);if(!ecs_is_alive(w,target)||target==entity)return -1;int prerequisite=cap_ready(w,target,depth+1);if(prerequisite<0)return -1;if(prerequisite==0)return 0;auto* result=static_cast<const CapOutput*>(ecs_get_id(w,target,state));if(!result||result->phase==0)return 0;if(result->phase<0)return -1;}}
    return 1;
}
FU_API int fu_cap_ready(fu_context* ctx,uint64_t entity){return cap_ready(static_cast<ecs_world_t*>(fu_world(ctx)),entity,0);}
static void coordinate(Trees& state,ecs_world_t* w){
    auto binding=ecs_lookup(w,"CapBinding"),relation=ecs_lookup(w,"CapDependsOn"),output=ecs_lookup(w,"CapState"),dirty=ecs_lookup(w,"CapDirty");if(!binding||!relation||!output||!dirty)return;
    ecs_query_desc_t d{};d.expr="CapBinding";auto* q=ecs_query_init(w,&d);if(!q)return;std::vector<ecs_entity_t> changed;
    auto it=ecs_query_iter(w,q);while(ecs_query_next(&it))for(int n=0;n<it.count;++n){
        auto e=it.entities[n];uint64_t hash=1469598103934665603ull;auto* type=ecs_get_type(w,e);
        for(int i=0;type&&i<type->count;++i){auto id=type->array[i];if(ECS_IS_PAIR(id)&&ecs_pair_first(w,id)==relation){auto target=ecs_pair_second(w,id);auto* result=static_cast<const CapOutput*>(ecs_get_id(w,target,output));hash=(hash^target)*1099511628211ull;hash=(hash^static_cast<uint64_t>(result?result->generation:0))*1099511628211ull;hash=(hash^static_cast<uint64_t>(result?result->phase:0))*1099511628211ull;}}
        auto old=state.dependencies.find(e);if(old==state.dependencies.end()||old->second!=hash){state.dependencies[e]=hash;changed.push_back(e);}
    }
    ecs_query_fini(q);for(auto e:changed)ecs_add_id(w,e,dirty);
    for(auto i=state.dependencies.begin();i!=state.dependencies.end();)if(!ecs_is_alive(w,i->first))i=state.dependencies.erase(i);else ++i;
}
static void footprints(Trees& state,ecs_world_t* w){
    auto type=ecs_lookup(w,"CapSpline");if(!type)return;
    ecs_query_desc_t d{};d.expr="CapSpline";auto* q=ecs_query_init(w,&d);if(!q)return;
    struct Input { ecs_entity_t id; std::string points; double width; bool closed,fill; };std::vector<Input> inputs;
    auto it=ecs_query_iter(w,q);while(ecs_query_next(&it))for(int n=0;n<it.count;++n){
        auto id=it.entities[n];void* data=const_cast<void*>(ecs_get_id(w,id,type));auto c=ecs_meta_cursor(w,type,data);ecs_meta_push(&c);ecs_meta_member(&c,"points");auto* points=ecs_meta_get_string(&c);std::string p=points?points:"";ecs_meta_member(&c,"width");double width=ecs_meta_get_float(&c);ecs_meta_member(&c,"closed");bool closed=ecs_meta_get_bool(&c);ecs_meta_member(&c,"fill");bool fill=ecs_meta_get_bool(&c);inputs.push_back({id,p,width,closed,fill});
    }ecs_query_fini(q);flecs::world view(w);
    for(auto& input:inputs){
        auto key=input.points+std::to_string(input.width)+(input.closed?"closed":"open")+(input.fill?"fill":"stroke");if(state.geometry[input.id]==key)continue;state.geometry[input.id]=key;NativeFootprint result;
        try{
            auto points=nlohmann::json::parse(input.points);if(!points.is_array()||points.size()<2||points.size()>256||!std::isfinite(input.width)||(!input.fill&&(input.width<=0||input.width>10000)))throw std::runtime_error("Invalid footprint points/width");
            Clipper2Lib::PathD path;for(auto& p:points){double x=p.at(0).get<double>(),z=p.at(p.size()>2?2:1).get<double>();if(!std::isfinite(x)||!std::isfinite(z))throw std::runtime_error("Nonfinite footprint point");path.emplace_back(x,z);}
            if((input.closed||input.fill)&&path.size()<3)throw std::runtime_error("Closed footprints need at least three points");
            auto rings=input.fill?Clipper2Lib::Union(Clipper2Lib::PathsD{path},Clipper2Lib::FillRule::NonZero,3):Clipper2Lib::InflatePaths(Clipper2Lib::PathsD{path},input.width*.5,Clipper2Lib::JoinType::Round,input.closed?Clipper2Lib::EndType::Joined:Clipper2Lib::EndType::Round,2,3);
            auto json=nlohmann::json::array();for(auto& ring:rings){auto points=nlohmann::json::array();for(auto& p:ring)points.push_back({p.x,p.y});json.push_back(points);}auto text=json.dump();result.text=ecs_os_strdup(text.c_str());
        }catch(const std::exception& error){auto text=nlohmann::json{{"error",error.what()}}.dump();result.text=ecs_os_strdup(text.c_str());}
        view.entity(input.id).set<NativeFootprint>(result);
    }
    for(auto i=state.geometry.begin();i!=state.geometry.end();)if(!ecs_is_alive(w,i->first))i=state.geometry.erase(i);else ++i;
}
static void caps_tick(fu_context* ctx,double dt);
// Capability coordination runs in the pipeline (visible in Explorer pipeline stats), after OnStore
// like the former post-progress call. Immediate: it adds CapDirty, sets results and ticks behavior
// trees whose FlecsSet nodes must take effect at once. Its queries stay uncached on purpose: Cap*
// components are Script-defined and may be redefined, which would orphan a cached query.
struct CapabilitiesModule {
    explicit CapabilitiesModule(flecs::world& world){
        world.module<CapabilitiesModule>("engine::capabilities");
        auto phase=world.entity("CapabilityPhase").add(flecs::Phase).depends_on(flecs::OnStore);
        auto* ctx=static_cast<fu_context*>(world.get_ctx());
        world.system("CapabilityCoordinationSystem").kind(phase).immediate()
            .run([ctx](flecs::iter& it){ while(it.next()){} caps_tick(ctx,it.delta_time()); });
    }
};
FU_API void fu_caps_init(fu_context* ctx){
    auto* w=static_cast<ecs_world_t*>(fu_world(ctx)); flecs::world view(w);
    view.component<NativeFootprint>("NativeFootprint").member(flecs::String,"json");
    view.component<NativeTree>("NativeTree").member(flecs::String,"xml").member(&NativeTree::revision,"revision").member(&NativeTree::enabled,"enabled").member(&NativeTree::loop,"loop");
    view.component<NativeTreeResult>("NativeTreeResult").member(flecs::String,"error").member(&NativeTreeResult::status,"status").member(&NativeTreeResult::revision,"revision").member(&NativeTreeResult::ticks,"ticks");
    worlds[ctx]=std::make_unique<Trees>(ctx);
    void* previous=view.get_ctx(); view.set_ctx(ctx); view.import<CapabilitiesModule>(); view.set_ctx(previous);
}
FU_API void fu_caps_destroy(fu_context* ctx){ worlds.erase(ctx); }
// Normally ticked by CapabilityCoordinationSystem inside fu_tick; an explicit call ticks once more.
FU_API void fu_caps_tick(fu_context* ctx,double dt){ caps_tick(ctx,dt); }
static void caps_tick(fu_context* ctx,double dt){
    auto wi=worlds.find(ctx); if(wi==worlds.end()) return; auto& state=*wi->second; state.now+=dt;
    auto* w=static_cast<ecs_world_t*>(fu_world(ctx)); coordinate(state,w); footprints(state,w); flecs::world view(w); std::vector<ecs_entity_t> ids;
    view.each<NativeTree>([&](flecs::entity e,NativeTree&){ids.push_back(e.id());});
    for(auto i=state.entries.begin();i!=state.entries.end();) {
        if(std::find(ids.begin(),ids.end(),i->first)==ids.end()){ if(i->second.tree)i->second.tree->haltTree(); i=state.entries.erase(i); } else ++i;
    }
    for(auto id:ids){
        auto e=view.entity(id); auto* def=e.try_get<NativeTree>(); if(!def)continue;
        std::string xml=def->text?def->text:""; const int revision=def->revision; const bool enabled=def->enabled, loop=def->loop;
        auto& entry=state.entries[id]; NativeTreeResult result; if(auto* old=e.try_get<NativeTreeResult>()) result=*old;
        try {
            if(entry.source!=xml||entry.revision!=revision){
                entry.source=xml;entry.revision=revision;
                if(entry.tree){entry.tree->haltTree();entry.tree.reset();}
                if(xml.empty()||xml.size()>262144)throw std::runtime_error("Tree XML is empty or too large");
                BT::XMLParser parser(state.factory);parser.loadFromText(xml,false);
                auto next=std::make_unique<BT::Tree>(parser.instantiateTree(BT::Blackboard::create()));
                if(entry.tree)entry.tree->haltTree(); entry.tree=std::move(next); entry.source=xml; entry.revision=revision; entry.finished=false;
                ecs_os_free(result.text); result.text=nullptr;
            }
            if(!enabled){if(entry.tree)entry.tree->haltTree(); result.status=0;entry.finished=false;}
            else if(entry.tree && (!entry.finished||loop)){result.status=static_cast<int32_t>(entry.tree->tickExactlyOnce());entry.finished=result.status==static_cast<int32_t>(BT::NodeStatus::SUCCESS)||result.status==static_cast<int32_t>(BT::NodeStatus::FAILURE);++result.ticks;}
            result.revision=revision;
        } catch(const std::exception& ex){
            if(entry.tree)entry.tree->haltTree(); ecs_os_free(result.text); result.text=ecs_os_strdup(ex.what()); result.status=-1;
        }
        if(ecs_is_alive(w,id))view.entity(id).set<NativeTreeResult>(result);
    }
}
FU_API void* fu_audio_create(const void* bank,int bytes,int rate){
    if(!bank||bytes<16||rate<8000||rate>192000)return nullptr;
    // Share immutable sample data; each emitter retains its own preallocated voices/channels.
    uint64_t key=1469598103934665603ull;auto* data=static_cast<const unsigned char*>(bank);for(int i=0;i<bytes;++i)key=(key^data[i])*1099511628211ull;
    std::lock_guard<std::mutex> lock(fontGate);auto shared=banks[key].lock();
    if(!shared){tsf* source=tsf_load_memory(bank,bytes);if(!source)return nullptr;shared=std::make_shared<SoundBank>(source);banks[key]=shared;}
    tsf* f=tsf_copy(shared->font);if(!f)return nullptr;
    tsf_set_output(f,TSF_MONO,rate,0);tsf_set_max_voices(f,64);tsf_channel_set_presetnumber(f,0,0,0);return new Audio(shared,f,rate);
}
FU_API void fu_audio_destroy(void* p){delete static_cast<Audio*>(p);}
// Single main-thread producer, one audio-thread consumer. Times are absolute renderer samples.
FU_API int fu_audio_note(void* p,int kind,int note,float velocity,int program,uint64_t sample){
    auto* a=static_cast<Audio*>(p); if(!a||kind<0||kind>4||note<0||note>127||!std::isfinite(velocity))return -1;
    auto w=a->write.load(std::memory_order_relaxed),r=a->read.load(std::memory_order_acquire);
    if(w-r>=a->events.size())return -2;
    a->events[w%a->events.size()]={kind,note,std::clamp(velocity,0.f,1.f),program,sample}; a->write.store(w+1,std::memory_order_release); return 0;
}
FU_API void fu_audio_render(void* p,float* output,int count){
    auto* a=static_cast<Audio*>(p); if(!a||!output||count<0||count>1048576)return;
    for(int offset=0;offset<count;){
        auto r=a->read.load(std::memory_order_relaxed),w=a->write.load(std::memory_order_acquire);
        while(r!=w && a->pendingCount<256){
            auto event=a->events[r%a->events.size()];int k=a->pendingCount++;
            while(k>0 && a->pending[k-1].sample>event.sample){a->pending[k]=a->pending[k-1];--k;}
            a->pending[k]=event;a->read.store(++r,std::memory_order_release);
        }
        while(a->pendingCount && a->pending[0].sample<=a->position){
            auto e=a->pending[0];for(int k=1;k<a->pendingCount;++k)a->pending[k-1]=a->pending[k];--a->pendingCount;
            if(e.kind==0){tsf_channel_set_presetnumber(a->font,0,std::clamp(e.program,0,127),0);tsf_channel_note_on(a->font,0,e.note,e.velocity);}
            else if(e.kind==1)tsf_channel_note_off(a->font,0,e.note);
            else if(e.kind==2){tsf_note_off_all(a->font);a->gain=0;}
            else {a->oscillator=e.kind;a->frequency=440.f*std::pow(2.f,(e.note-69)/12.f);a->gain=e.velocity*.25f;a->end=a->position+static_cast<uint64_t>(a->rate*.3);}
        }
        int block=std::min(64,count-offset);
        if(a->pendingCount){auto next=a->pending[0].sample;if(next>a->position)block=static_cast<int>(std::min<uint64_t>(block,next-a->position));}
        tsf_render_float(a->font,output+offset,block,0);
        for(int i=0;i<block;++i){
            if(a->position+static_cast<uint64_t>(i)<a->end && a->gain>0){
                a->rng^=a->rng<<13;a->rng^=a->rng>>17;a->rng^=a->rng<<5;
                const float wave=a->oscillator==4?static_cast<float>(static_cast<int32_t>(a->rng))/2147483648.f:std::sin(a->phase);
                const float envelope=static_cast<float>(a->end-a->position-i)/(a->rate*.3f);
                output[offset+i]+=wave*a->gain*envelope; a->phase+=6.283185307f*a->frequency/a->rate;
            }
            output[offset+i]=std::clamp(output[offset+i],-1.f,1.f);
        }
        offset+=block;a->position+=block;
    }
}
FU_API int fu_path_outline(const double* points,int count,double radius,double* output,int capacity,int* starts,int ringsCapacity,int* ringCount){
    if(!points||count<2||count>4096||!std::isfinite(radius)||radius<=0||radius>100000)return -1;
    Clipper2Lib::PathD path;for(int i=0;i<count;++i){if(!std::isfinite(points[i*2])||!std::isfinite(points[i*2+1]))return -1;path.emplace_back(points[i*2],points[i*2+1]);}
    try { auto rings=Clipper2Lib::InflatePaths(Clipper2Lib::PathsD{path},radius,Clipper2Lib::JoinType::Round,Clipper2Lib::EndType::Round,2,3);
        int total=0;for(auto& ring:rings)total+=static_cast<int>(ring.size());if(ringCount)*ringCount=static_cast<int>(rings.size());
        if(output&&starts&&capacity>=total&&ringsCapacity>=static_cast<int>(rings.size())+1){int k=0,j=0;for(auto& ring:rings){starts[j++]=k;for(auto& v:ring){output[k*2]=v.x;output[k*2+1]=v.y;++k;}}starts[j]=k;}
        return total;
    }catch(...){return -1;}
}
}
