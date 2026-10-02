#pragma once
#include "workflow.hpp"
#include <memory>
namespace reset {
bool process_running(const wchar_t* name);
void require_game_closed();
fs::path executable_path();
fs::path discover_steam();
std::uint32_t active_account();
// Isolated UIA worker: exit 10 conflict, 11 sync error, 12 unknown, 13 invoked.
int probe_steam(const fs::path& root,bool invoke,std::uint32_t expectedAccount=0);
int bounded_probe(const fs::path& root,bool invoke,DWORD timeout=2000,std::uint32_t expectedAccount=0);
class WindowsSteam final:public SteamAdapter {
    fs::path root_; std::string account_; bool russian_; Log log_;
    HWND notice_=nullptr;
    bool launched_=false;
public:
    WindowsSteam(fs::path root,std::string account,bool russian,Log log);
    ~WindowsSteam() override;
    void preflight() override;
    void launch() override;
    Observation wait_conflict() override;
    bool confirm_conflict() override;
    bool invoke_local() override;
    void manual_local() override;
    bool wait_game() override;
};
}
