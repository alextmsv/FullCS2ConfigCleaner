#include "win_steam.hpp"
#include <tlhelp32.h>
#include <objbase.h>
#include <UIAutomation.h>
#include <wrl/client.h>
#include <algorithm>
#include <chrono>
#include <thread>

namespace reset {
using Microsoft::WRL::ComPtr;
using Clock=std::chrono::steady_clock;
bool process_running(const wchar_t* name) {
    Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0));
    if(snapshot.get()==INVALID_HANDLE_VALUE) win_error("Process snapshot");
    PROCESSENTRY32W entry{};entry.dwSize=sizeof(entry);
    if(!Process32FirstW(snapshot.get(),&entry)) win_error("Read process snapshot");
    do {if(_wcsicmp(entry.szExeFile,name)==0) return true;} while(Process32NextW(snapshot.get(),&entry));
    if(GetLastError()!=ERROR_NO_MORE_FILES) win_error("Process enumeration"); return false;
}
void require_game_closed() {if(process_running(L"cs2.exe")) throw Error("Close CS2 and run again.");}
fs::path executable_path() {std::wstring path(32768,L'\0');auto n=GetModuleFileNameW(nullptr,path.data(),static_cast<DWORD>(path.size()));if(!n || n>=path.size()) win_error("Find executable");path.resize(n);return fs::path(path);}
static std::wstring registry_string(HKEY hive,const wchar_t* key,const wchar_t* name,REGSAM view=0) {
    wchar_t value[32768]{};DWORD bytes=sizeof(value);
    auto error=RegGetValueW(hive,key,name,RRF_RT_REG_SZ|view,nullptr,value,&bytes);
    return error==ERROR_SUCCESS?std::wstring(value):std::wstring{};
}
fs::path discover_steam() {
    std::vector<fs::path> candidates;
    auto path=registry_string(HKEY_CURRENT_USER,L"Software\\Valve\\Steam",L"SteamPath");if(!path.empty())candidates.emplace_back(path);
    path=registry_string(HKEY_CURRENT_USER,L"Software\\Valve\\Steam",L"SteamExe");if(!path.empty())candidates.emplace_back(fs::path(path).parent_path());
    for(auto view:{RRF_SUBKEY_WOW6432KEY,RRF_SUBKEY_WOW6464KEY}) {path=registry_string(HKEY_LOCAL_MACHINE,L"SOFTWARE\\Valve\\Steam",L"InstallPath",view);if(!path.empty())candidates.emplace_back(path);}
    wchar_t programs[32768]{};if(GetEnvironmentVariableW(L"ProgramFiles(x86)",programs,32768))candidates.emplace_back(fs::path(programs)/L"Steam");
    candidates.emplace_back(L"C:\\Program Files (x86)\\Steam");
    for(auto p:candidates) {p.make_preferred();if(fs::is_regular_file(p/L"steam.exe") && fs::is_directory(p/L"userdata")) {PathLock lock(p);return p;}}
    throw Error("Steam was not found in registry or default locations");
}
std::uint32_t active_account() {DWORD value=0,bytes=sizeof(value);return RegGetValueW(HKEY_CURRENT_USER,L"Software\\Valve\\Steam\\ActiveProcess",L"ActiveUser",RRF_RT_REG_DWORD,nullptr,&value,&bytes)==ERROR_SUCCESS?value:0;}
static Handle spawn(const fs::path& executable,const std::wstring& args,bool hidden) {
    std::wstring cmd=L"\""+executable.wstring()+L"\" "+args;
    STARTUPINFOW si{};si.cb=sizeof(si); if(hidden){si.dwFlags=STARTF_USESHOWWINDOW;si.wShowWindow=SW_HIDE;}
    PROCESS_INFORMATION pi{};
    if(!CreateProcessW(executable.c_str(),cmd.data(),nullptr,nullptr,FALSE,hidden?CREATE_NO_WINDOW:0,nullptr,executable.parent_path().c_str(),&si,&pi)) win_error("Launch process");
    CloseHandle(pi.hThread);return Handle(pi.hProcess);
}
static bool same_path(const fs::path& a,const fs::path& b){return _wcsicmp(a.lexically_normal().c_str(),b.lexically_normal().c_str())==0;}
static bool steam_pid(DWORD pid,const fs::path& root) {
    Handle p(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid));if(!p.get())return false;
    wchar_t path[32768];DWORD n=32768;if(!QueryFullProcessImageNameW(p.get(),0,path,&n))return false;
    return same_path(path,root/L"steam.exe") || same_path(path,root/L"bin/cef/cef.win64/steamwebhelper.exe") || same_path(path,root/L"bin/cef/cef.win7/steamwebhelper.exe");
}
static std::wstring lower(const std::wstring& s){return lower_text(s);}
struct WindowScan {fs::path root;std::vector<HWND> windows;};
static BOOL CALLBACK collect_window(HWND hwnd,LPARAM arg) {
    auto& scan=*reinterpret_cast<WindowScan*>(arg);DWORD pid=0;GetWindowThreadProcessId(hwnd,&pid);
    if(IsWindowVisible(hwnd) && steam_pid(pid,scan.root)) scan.windows.push_back(hwnd); return TRUE;
}
struct Snapshot {std::vector<UiControl> controls;ComPtr<IUIAutomationElement> local;bool sync=false;};
static Snapshot inspect(IUIAutomation* automation,HWND hwnd) {
    Snapshot result; wchar_t title[512]{};GetWindowTextW(hwnd,title,512);
    const auto titleLower=lower(title);
    // A library page containing these words is not a conflict dialog.
    if(titleLower.find(L"conflict")==titleLower.npos && titleLower.find(L"конфликт")==titleLower.npos && titleLower.find(L"counter-strike 2")==titleLower.npos) return result;
    ComPtr<IUIAutomationElement> window; if(FAILED(automation->ElementFromHandle(hwnd,&window))) return result;
    ComPtr<IUIAutomationCondition> all;automation->CreateTrueCondition(&all);
    ComPtr<IUIAutomationElementArray> elements;
    if(!all || FAILED(window->FindAll(TreeScope_Subtree,all.Get(),&elements)))return result;
    int count=0;if(FAILED(elements->get_Length(&count)) || count>400)return result;
    result.controls.push_back({title,false,true,true}); bool game=titleLower.find(L"counter-strike 2")!=titleLower.npos, error=false;
    for(int i=0;i<count;++i) {
        ComPtr<IUIAutomationElement> element;if(FAILED(elements->GetElement(i,&element)))continue;
        BSTR raw=nullptr;if(FAILED(element->get_CurrentName(&raw)))continue;
        std::wstring name=raw?raw:L"";SysFreeString(raw); if(name.size()>1024)continue;
        CONTROLTYPEID type=0;BOOL enabled=FALSE,offscreen=TRUE;
        element->get_CurrentControlType(&type);element->get_CurrentIsEnabled(&enabled);element->get_CurrentIsOffscreen(&offscreen);
        result.controls.push_back({name,type==UIA_ButtonControlTypeId,enabled!=FALSE,offscreen==FALSE});
        auto s=lower(name);if(s.find(L"counter-strike 2")!=s.npos)game=true;
        if(s.find(L"unable to sync")!=s.npos || s.find(L"sync error")!=s.npos || s.find(L"ошибка синхронизации")!=s.npos || s.find(L"не удалось синхронизировать")!=s.npos)error=true;
        if(local_label(name) && type==UIA_ButtonControlTypeId && enabled && !offscreen)result.local=element;
    }
    result.sync=game && error;return result;
}
int probe_steam(const fs::path& root,bool invoke,std::uint32_t expectedAccount) {
    if(invoke && (!expectedAccount || active_account()!=expectedAccount))return 12;
    HRESULT initialized=CoInitializeEx(nullptr,COINIT_MULTITHREADED);if(FAILED(initialized))return 12;
    int outcome=12;
    {
        ComPtr<IUIAutomation> automation;
        if(SUCCEEDED(CoCreateInstance(CLSID_CUIAutomation8,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&automation)))) {
            ComPtr<IUIAutomation2> bounded;if(SUCCEEDED(automation.As(&bounded))){bounded->put_ConnectionTimeout(500);bounded->put_TransactionTimeout(1000);}
            WindowScan scan{root,{}};EnumWindows(collect_window,reinterpret_cast<LPARAM>(&scan));
            HWND candidate=nullptr;unsigned matches=0;
            for(auto window:scan.windows) {auto state=inspect(automation.Get(),window);if(state.sync)outcome=11;if(conflict_semantics(state.controls)){candidate=window;++matches;}}
            if(matches==1 && outcome!=11) {
                outcome=10;
                if(invoke) {
                    require_game_closed(); DWORD pid=0;GetWindowThreadProcessId(candidate,&pid);
                    auto state=inspect(automation.Get(),candidate);
                    if(IsWindowVisible(candidate) && steam_pid(pid,root) && conflict_semantics(state.controls) && state.local) {
                        ComPtr<IUIAutomationInvokePattern> pattern;
                        if(SUCCEEDED(state.local->GetCurrentPatternAs(UIA_InvokePatternId,IID_PPV_ARGS(&pattern)))) {
                            require_game_closed(); if(active_account()==expectedAccount && SUCCEEDED(pattern->Invoke()))outcome=13;
                        }
                    }
                }
            }
        }
    }
    CoUninitialize(); return outcome;
}
int bounded_probe(const fs::path& root,bool invoke,DWORD timeout,std::uint32_t expectedAccount) {
    auto child=spawn(executable_path(),std::wstring(invoke?L"--uia-invoke ":L"--uia-probe ")+L"\""+root.wstring()+L"\" "+std::to_wstring(expectedAccount),true);
    if(WaitForSingleObject(child.get(),timeout)!=WAIT_OBJECT_0) {
        // Only this private, read-only/one-invoke helper is terminated; never Steam.
        TerminateProcess(child.get(),12); WaitForSingleObject(child.get(),1000); return 12;
    }
    DWORD code=12;GetExitCodeProcess(child.get(),&code);return static_cast<int>(code);
}
WindowsSteam::WindowsSteam(fs::path root,std::string account,bool russian,Log log):root_(std::move(root)),account_(std::move(account)),russian_(russian),log_(std::move(log)){}
WindowsSteam::~WindowsSteam(){if(notice_)DestroyWindow(notice_);}
void WindowsSteam::preflight() {
    require_game_closed();
    if(launched_ && !process_running(L"steam.exe"))throw Error("Steam closed during reset");
    if(process_running(L"steam.exe") && active_account()!=std::stoul(account_)) throw Error("Sign in to the selected Steam account before resetting");
}
void WindowsSteam::launch() {
    require_game_closed();
    if(!process_running(L"steam.exe")) {
        log_("Starting Steam after Phase A"); auto steam=spawn(root_/L"steam.exe",L"",false);(void)steam;
        const auto deadline=Clock::now()+std::chrono::seconds(60);
        while(Clock::now()<deadline && active_account()!=std::stoul(account_)) {require_game_closed();std::this_thread::sleep_for(std::chrono::milliseconds(250));}
    }
    preflight(); if(active_account()!=std::stoul(account_)) throw Error("Steam sign-in is not ready for the selected account");
    auto launch=spawn(root_/L"steam.exe",L"-applaunch 730",false);(void)launch;launched_=true;log_("Official Steam launch requested: -applaunch 730");
}
Observation WindowsSteam::wait_conflict() {
    const auto deadline=Clock::now()+std::chrono::seconds(60);
    while(Clock::now()<deadline) {
        if(process_running(L"cs2.exe"))return Observation::Running;
        auto left=std::chrono::duration_cast<std::chrono::milliseconds>(deadline-Clock::now()).count();
        if(left<=0)break;
        auto state=bounded_probe(root_,false,static_cast<DWORD>(std::min<long long>(left,2000)));
        if(process_running(L"cs2.exe"))return Observation::Running;
        if(state==10)return Observation::Conflict;
        if(state==11)return Observation::SyncError;
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    return Observation::Timeout;
}
bool WindowsSteam::confirm_conflict() {
    require_game_closed();
    auto answer=MessageBoxW(nullptr,russian_?L"Появился конфликт Steam Cloud конфигов?\n\nДля Counter-Strike 2 должны быть показаны локальная и облачная версии. Оставьте конфликт открытым и пока ничего в Steam не выбирайте. Ошибка синхронизации — не конфликт.":L"Did the Steam Cloud config conflict appear?\n\nIt must show local and cloud versions for Counter-Strike 2. Leave it open and do not choose either version yet. A sync error is not a conflict.",L"CS2 Full Reset",MB_YESNO|MB_ICONQUESTION|MB_DEFBUTTON2|MB_TOPMOST);
    require_game_closed(); return answer==IDYES;
}
bool WindowsSteam::invoke_local(){preflight();return bounded_probe(root_,true,2000,static_cast<std::uint32_t>(std::stoul(account_)))==13;}
void WindowsSteam::manual_local() {
    notice_=CreateWindowExW(WS_EX_TOPMOST,L"STATIC",russian_?L"Очистка завершена. В Steam выберите ЛОКАЛЬНЫЕ ФАЙЛЫ для продолжения.":L"Reset complete. In Steam choose LOCAL SAVE to continue.",WS_OVERLAPPED|WS_CAPTION|WS_VISIBLE|SS_CENTER, CW_USEDEFAULT,CW_USEDEFAULT,620,110,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    if(!notice_)win_error("Show Local Save instruction");
    log_("Manual action required: choose LOCAL SAVE in the existing Steam conflict");
}
bool WindowsSteam::wait_game() {
    const auto deadline=Clock::now()+std::chrono::seconds(60);
    while(Clock::now()<deadline) {
        if(process_running(L"cs2.exe"))return true;
        MSG msg{};while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)){TranslateMessage(&msg);DispatchMessageW(&msg);}
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return false;
}
}
