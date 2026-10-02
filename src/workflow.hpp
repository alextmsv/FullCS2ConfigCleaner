#pragma once
#include "backup.hpp"
namespace reset {
enum class Observation { Conflict, Running, SyncError, Timeout };
enum class Outcome { Started, Incomplete, AwaitingLocal };
struct SteamAdapter {
    virtual ~SteamAdapter()=default;
    virtual void preflight()=0;
    virtual void launch()=0;
    virtual Observation wait_conflict()=0;
    virtual bool confirm_conflict()=0;
    virtual bool invoke_local()=0;
    virtual void manual_local()=0;
    virtual bool wait_game()=0;
};
using Log=std::function<void(const std::string&)>;
Outcome run_reset(Guard& guard,BackupStore& backups,SteamAdapter& steam,const Log& log);
// Pure classifier: actual process verification is performed by the UIA adapter.
struct UiControl {std::wstring name; bool button=false; bool enabled=true; bool visible=true;};
bool conflict_semantics(const std::vector<UiControl>& controls);
bool local_label(const std::wstring& label);
}
