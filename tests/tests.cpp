#include "safe_fs.hpp"
#include "backup.hpp"
#include "accounts.hpp"
#include "workflow.hpp"
#include <fstream>
#include <iostream>
#include <sstream>

using namespace reset;
int failed = 0, passed = 0;
#define CHECK(x) do { if (!(x)) throw Error("Assertion: " #x); } while (0)
template<class F> void rejects(F f) { bool caught = false; try { f(); } catch (const std::exception&) { caught = true; } CHECK(caught); }
template<class F> void test(const char* name, F f) { try { f(); ++passed; std::cout << "PASS " << name << '\n'; } catch (const std::exception& e) { ++failed; std::cout << "FAIL " << name << ": " << e.what() << '\n'; } }
void put(const fs::path& p, const std::string& s) { fs::create_directories(p.parent_path()); std::ofstream o(p, std::ios::binary); o << s; if (!o) throw Error("fixture write"); }
std::string get(const fs::path& p) { std::ifstream i(p, std::ios::binary); return {std::istreambuf_iterator<char>(i), {}}; }
struct Fixture {
    fs::path base, steam, target;
    Fixture() {
        static int id = 0;
        base = fs::temp_directory_path() / (L"cs2-reset-test-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(++id));
        if (fs::exists(base)) throw Error("fixture collision");
        steam = base / "Steam"; target = steam / "userdata/123/730";
        put(target / "remote/sub/a.cfg", "remote-data");
        put(target / "local/cfg/b.vcfg", std::string("abc\0def", 7));
        put(target / "remotecache.vdf", "cache");
        put(target / "other/data", "keep");
        fs::create_directories(target / "remote/empty");
        put(steam / "userdata/123/440/sentinel", "neighbor");
    }
    ~Fixture() { std::error_code ec; fs::remove_all(base, ec); }
    Guard guard(IdleCheck idle = [] {}) { return Guard(steam, target, idle); }
};
struct FakeSteam:SteamAdapter {
    Observation observation=Observation::Conflict; bool confirm=false,invoke=true,started=true;
    std::string events; bool switch_account=false;
    void preflight() override {if(switch_account && events.find('C')!=std::string::npos)throw Error("Account changed");events+='P';}
    void launch() override {events+='L';}
    Observation wait_conflict() override {events+='C';return observation;}
    bool confirm_conflict() override {events+='?';return confirm;}
    bool invoke_local() override {events+='I';return invoke;}
    void manual_local() override {events+='M';}
    bool wait_game() override {events+='G';return started;}
};
int main() {
    test("valid 730 accepted", [] { Fixture f; f.guard().check(); });
    test("other AppID rejected", [] { Fixture f; rejects([&] { validate_path(f.steam, f.target.parent_path()/"440"); }); });
    test("non-numeric account rejected", [] { Fixture f; auto p=f.steam/"userdata/abc/730"; fs::create_directories(p); rejects([&] {validate_path(f.steam,p);}); });
    test("traversal rejected before normalization", [] { Fixture f; rejects([&] {validate_path(f.steam,f.target/"../730");}); });
    test("empty and foreign paths rejected", [] { Fixture f; rejects([&] {validate_path(f.steam,{});}); rejects([&] {validate_path(f.base/"OtherSteam",f.target);}); });
    test("phase A truncates files and preserves directories", [] { Fixture f; CHECK(f.guard().phase_a()==2); CHECK(fs::file_size(f.target/"remote/sub/a.cfg")==0); CHECK(fs::file_size(f.target/"local/cfg/b.vcfg")==0); CHECK(fs::is_directory(f.target/"remote/empty")); CHECK(get(f.target/"remotecache.vdf")=="cache"); });
    test("phase B deletes only intended content", [] { Fixture f; f.guard().phase_b(); CHECK(fs::is_empty(f.target/"remote")); CHECK(fs::is_empty(f.target/"local/cfg")); CHECK(!fs::exists(f.target/"remotecache.vdf")); CHECK(get(f.target/"other/data")=="keep"); CHECK(get(f.steam/"userdata/123/440/sentinel")=="neighbor"); });
    test("running game blocks all mutation", [] { Fixture f; bool running=false; auto g=f.guard([&] {if(running) throw Error("running");}); running=true; rejects([&]{g.phase_a();}); rejects([&]{g.phase_b();}); CHECK(get(f.target/"remote/sub/a.cfg")=="remote-data"); });
    test("hard links cannot modify outside data", [] { Fixture f; put(f.base/"outside", "outside"); fs::create_hard_link(f.base/"outside",f.target/"remote/link"); rejects([&]{f.guard().phase_a();}); CHECK(get(f.base/"outside")=="outside"); });
    test("complete copy and byte-for-byte restore", [] { Fixture f; auto b=f.base/"backup"; copy_tree(f.target,b,[]{}); CHECK(get(b/"local/cfg/b.vcfg")==std::string("abc\0def",7)); f.guard().phase_b(); put(f.target/"new","new"); f.guard().restore_from(b); CHECK(get(f.target/"remote/sub/a.cfg")=="remote-data"); CHECK(!fs::exists(f.target/"new")); CHECK(get(f.target/"local/cfg/b.vcfg")==std::string("abc\0def",7)); });
    test("directory symlink escape refused without touching outside", [] { Fixture f; fs::create_directories(f.base/"outside"); put(f.base/"outside/sentinel","outside"); CHECK(CreateSymbolicLinkW((f.target/"local/escape").c_str(),(f.base/"outside").c_str(),SYMBOLIC_LINK_FLAG_DIRECTORY|SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE)); rejects([&]{f.guard().phase_a();}); rejects([&]{f.guard().phase_b();}); CHECK(get(f.base/"outside/sentinel")=="outside"); fs::remove(f.target/"local/escape"); });
    test("verified snapshots retain newest two", [] {Fixture f; BackupStore b(f.base/"backups","123",L"name",[]{}); auto g=f.guard(); auto first=b.create(g); put(f.target/"other/data","second"); auto second=b.create(g); put(f.target/"other/data","third"); auto third=b.create(g); CHECK(b.list().size()==2); CHECK(!fs::exists(first)); CHECK(fs::exists(second)); CHECK(get(third/"730/other/data")=="third");});
    test("restore chooses previous backup before safety backup", [] {Fixture f; BackupStore b(f.base/"backups","123",L"name",[]{}); auto g=f.guard(); b.create(g); put(f.target/"other/data","changed"); b.restore(g); CHECK(get(f.target/"other/data")=="keep"); CHECK(b.list().size()==2); CHECK(get(b.latest()/"730/other/data")=="changed");});
    test("pending reset snapshot survives retention", [] {Fixture f; BackupStore b(f.base/"backups","123",L"name",[]{}); auto g=f.guard(); auto first=b.create(g); b.pin(first); b.create(g); b.create(g); CHECK(b.list().size()==2); CHECK(b.pending()==first); put(f.target/"other/data","changed"); b.restore(g); CHECK(b.pending().empty()); CHECK(get(f.target/"other/data")=="keep");});
    test("corrupt backup blocks restore before changing current state", [] {Fixture f; BackupStore b(f.base/"backups","123",L"name",[]{}); auto g=f.guard(); auto snapshot=b.create(g); put(snapshot/"730/other/data","corrupted-long"); rejects([&]{b.restore(g);}); CHECK(get(f.target/"other/data")=="keep");});
    test("backup failure leaves source untouched", [] {Fixture f; put(f.base/"blocked","file"); rejects([&]{BackupStore b(f.base/"blocked","123",L"name",[]{}); b.create(f.guard());}); CHECK(get(f.target/"remote/sub/a.cfg")=="remote-data");});
    test("VDF accounts sorted newest first with 64-to-32 conversion", [] {auto a=parse_accounts(R"("users" { "76561197960265851" { "PersonaName" "Old" "Timestamp" "100" } "76561197960266049" { "PersonaName" "New" "Timestamp" "200" "MostRecent" "1" } })"); CHECK(a.size()==2); CHECK(a[0].id=="321"); CHECK(a[0].name==L"New"); CHECK(a[1].id=="123");});
    test("settings only reuse existing selected account", [] {Fixture f; auto settings=f.base/"settings.json"; std::vector<Account> a={{"123",L"name",0,false}}; CHECK(!saved_account(settings,a)); save_account(settings,"123"); CHECK(saved_account(settings,a).value()=="123"); CHECK(!saved_account(settings,{})); put(settings,"malformed"); CHECK(!saved_account(settings,a));});
    test("confirmed conflict performs phase B then launch", [] {Fixture f; BackupStore b(f.base/"backups","123",L"name",[]{}); auto g=f.guard(); FakeSteam steam; CHECK(run_reset(g,b,steam,[](auto&){})==Outcome::Started); CHECK(fs::is_empty(f.target/"local/cfg")); CHECK(!fs::exists(f.target/"remotecache.vdf")); CHECK(steam.events=="PLCPPIG"); CHECK(b.pending().empty());});
    test("running game or sync error cannot authorize phase B", [] {for(auto observation:{Observation::Running,Observation::SyncError}) {Fixture f; BackupStore b(f.base/"backups","123",L"name",[]{}); auto g=f.guard(); FakeSteam steam; steam.observation=observation; CHECK(run_reset(g,b,steam,[](auto&){})==Outcome::Incomplete); CHECK(fs::exists(f.target/"remote/sub/a.cfg")); CHECK(fs::file_size(f.target/"remote/sub/a.cfg")==0); CHECK(!b.pending().empty()); CHECK(steam.events=="PLC");}});
    test("timeout needs explicit human yes", [] {Fixture f; BackupStore b(f.base/"backups","123",L"name",[]{}); auto g=f.guard(); FakeSteam steam; steam.observation=Observation::Timeout; CHECK(run_reset(g,b,steam,[](auto&){})==Outcome::Incomplete); CHECK(fs::exists(f.target/"remotecache.vdf")); CHECK(steam.events=="PLC?");});
    test("manual conflict confirmation and local fallback", [] {Fixture f; BackupStore b(f.base/"backups","123",L"name",[]{}); auto g=f.guard(); FakeSteam steam; steam.observation=Observation::Timeout; steam.confirm=true; steam.invoke=false; steam.started=false; CHECK(run_reset(g,b,steam,[](auto&){})==Outcome::AwaitingLocal); CHECK(fs::is_empty(f.target/"local/cfg")); CHECK(steam.events=="PLC?PPIMG"); CHECK(!b.pending().empty()); rejects([&]{run_reset(g,b,steam,[](auto&){});});});
    test("failed backup blocks phase A and launch", [] {Fixture f; BackupStore b(f.base/"backups","123",L"name",[]{}); auto g=f.guard(); FakeSteam steam; Handle locked(CreateFileW((f.target/"remote/sub/a.cfg").c_str(),GENERIC_READ,0,nullptr,OPEN_EXISTING,0,nullptr)); CHECK(locked.get()!=INVALID_HANDLE_VALUE); rejects([&]{run_reset(g,b,steam,[](auto&){});}); CHECK(steam.events.find('L')==std::string::npos); CHECK(fs::file_size(f.target/"local/cfg/b.vcfg")==7);});
    test("UIA classification requires CS2 conflict and local-cloud alternatives", [] {std::vector<UiControl> c={{L"Counter-Strike 2"},{L"Steam Cloud conflict"},{L"Local Save",true},{L"Cloud Save",true}}; CHECK(conflict_semantics(c)); c[0].name=L"Another game"; CHECK(!conflict_semantics(c)); c[0].name=L"Counter-Strike 2"; c[1].name=L"Unable to sync"; CHECK(!conflict_semantics(c)); CHECK(!local_label(L"Download Cloud Save"));});
    test("clock rollback cannot prune newly created safety backup", [] {Fixture f; BackupStore b(f.base/"backups","123",L"name",[]{});auto g=f.guard();auto p=b.create(g);auto future=p.parent_path()/L"2099-01-01_00-00-00-000";fs::rename(p,future);auto second=b.create(g);fs::rename(second,second.parent_path()/L"2099-01-02_00-00-00-000");put(f.target/"other/data","current-state");auto fresh=b.create(g,future);CHECK(fs::exists(fresh));CHECK(get(fresh/"730/other/data")=="current-state");});
    test("interrupted restore retry retains original safety snapshot", [] {Fixture f;bool armed=false;BackupStore b(f.base/"backups","123",L"name",[]{});auto g=f.guard([&]{if(armed && !fs::exists(f.target/"local"))throw Error("simulated interruption");});b.create(g);put(f.target/"local/cfg/b.vcfg","pre-restore");armed=true;rejects([&]{b.restore(g);});armed=false;b.restore(g);CHECK(get(f.target/"other/data")=="keep");bool retained=false;for(auto& p:b.list())if(get(p/"730/local/cfg/b.vcfg")=="pre-restore")retained=true;CHECK(retained);});
    test("interrupted pruning does not leave a corrupt completed backup", [] {Fixture f;bool stop=false;BackupStore b(f.base/"backups","123",L"name",[&]{if(stop)throw Error("interrupted prune");});auto g=f.guard();auto old=b.create(g);b.create(g);stop=true;rejects([&]{b.create(g);});stop=false;auto tomb=fs::path(old.wstring()+L".deleting");fs::rename(old,tomb);fs::remove(tomb/"730/other/data");b.create(g);CHECK(b.list().size()==2);});
    test("account switch during conflict wait blocks phase B", [] {Fixture f;BackupStore b(f.base/"backups","123",L"name",[]{});auto g=f.guard();FakeSteam steam;steam.switch_account=true;rejects([&]{run_reset(g,b,steam,[](auto&){});});CHECK(fs::exists(f.target/"remotecache.vdf"));CHECK(fs::exists(f.target/"remote/sub/a.cfg"));CHECK(steam.events.find('I')==std::string::npos);});
    test("Russian conflict semantics work independently of C locale", [] {CHECK(local_label(L"Локальные файлы"));CHECK(conflict_semantics({{L"Counter-Strike 2"},{L"Конфликт Steam Cloud"},{L"Локальные файлы",true},{L"Облачные файлы",true}}));});
    test("read-only file blocks phase A before truncating any file", [] {Fixture f;auto file=f.target/L"local/cfg/b.vcfg";CHECK(SetFileAttributesW(file.c_str(),FILE_ATTRIBUTE_READONLY));rejects([&]{f.guard().phase_a();});CHECK(get(f.target/"remote/sub/a.cfg")=="remote-data");SetFileAttributesW(file.c_str(),FILE_ATTRIBUTE_NORMAL);});
    test("restore source inside target is rejected before deletion", [] {Fixture f;put(f.target/"nested/sentinel","backup");rejects([&]{f.guard().restore_from(f.target/"nested");});CHECK(get(f.target/"remote/sub/a.cfg")=="remote-data");CHECK(get(f.target/"nested/sentinel")=="backup");});
    test("account discovery lists only matching numeric 730 directories", [] {Fixture f;put(f.steam/"userdata/321/730/config","second");put(f.steam/"userdata/no-id/730/config","ignored");put(f.steam/"config/loginusers.vdf",R"("users" { "76561197960265851" { "PersonaName" "Old" "Timestamp" "100" } "76561197960266049" { "PersonaName" "New" "Timestamp" "200" } })");auto a=find_accounts(f.steam);CHECK(a.size()==2);CHECK(a[0].id=="321");CHECK(a[1].id=="123");CHECK(get(f.steam/"userdata/321/730/config")=="second");});
    std::cout << passed << " passed, " << failed << " failed\n";
    return failed ? 1 : 0;
}
