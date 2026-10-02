#include "workflow.hpp"
#include <algorithm>
#include <cwctype>
namespace reset {
Outcome run_reset(Guard& guard,BackupStore& backups,SteamAdapter& steam,const Log& log) {
    guard.preflight();
    if(!backups.pending().empty()) throw Error("Unfinished reset: restore the pinned backup before another reset");
    steam.preflight();
    auto snapshot=backups.create(guard); log("Backup verified: "+utf8(snapshot.wstring()));
    backups.pin(snapshot);
    auto count=guard.phase_a(); log("Phase A files: "+std::to_string(count));
    steam.launch(); const auto observed=steam.wait_conflict();
    if(observed==Observation::Running) {log("Cloud conflict was not detected; exact full-reset flow could not be reproduced."); return Outcome::Incomplete;}
    if(observed==Observation::SyncError) {log("Steam sync/network error; Phase B blocked."); return Outcome::Incomplete;}
    bool automatic=observed==Observation::Conflict;
    log(automatic?"Cloud Conflict detected by UI Automation":"Cloud Conflict not detected within 60 seconds");
    if(!automatic && !steam.confirm_conflict()) {log("User did not confirm Cloud Conflict; Phase B blocked.");return Outcome::Incomplete;}
    if(!automatic) log("Cloud Conflict confirmed explicitly by user");
    steam.preflight(); guard.check(); guard.phase_b(); log("Phase B verified: remote/local cleared, local/cfg empty, remotecache removed");
    steam.preflight(); guard.check(); bool invoked=steam.invoke_local(); log(invoked?"UI Automation: Local Save invoked":"UI Automation: no safe Local Save control; manual fallback");
    if(!invoked) steam.manual_local();
    if(steam.wait_game()) {log("cs2.exe started after Phase B"); backups.clear_pending(); return Outcome::Started;}
    log("cs2.exe launch not observed; original backup remains pinned");
    return invoked?Outcome::Incomplete:Outcome::AwaitingLocal;
}
static std::wstring lower(const std::wstring& text) {return lower_text(text);}
bool local_label(const std::wstring& label) {const auto s=lower(label);return s==L"local save" || s==L"local files" || s==L"локальные файлы" || s==L"локальное сохранение";}
bool conflict_semantics(const std::vector<UiControl>& controls) {
    bool game=false,conflict=false,cloud=false; unsigned local=0;
    for(const auto& c:controls) {
        if(!c.visible) continue; auto s=lower(c.name);
        if(s.find(L"counter-strike 2")!=s.npos) game=true;
        if(s.find(L"cloud conflict")!=s.npos || (s.find(L"конфликт")!=s.npos && s.find(L"cloud")!=s.npos)) conflict=true;
        if(c.button && c.enabled && local_label(c.name)) ++local;
        if(c.button && c.enabled && (s==L"cloud save" || s==L"cloud files" || s==L"облачные файлы" || s==L"облачное сохранение")) cloud=true;
    }
    return game && conflict && cloud && local==1;
}
}
