#include "accounts.hpp"
#include "win_steam.hpp"
#include <shellapi.h>
#include <conio.h>
#include <fcntl.h>
#include <io.h>
#include <algorithm>
#include <iostream>

namespace {
using namespace reset;
bool ru=PRIMARYLANGID(GetUserDefaultUILanguage())==LANG_RUSSIAN;
const wchar_t* tr(const wchar_t* en,const wchar_t* russian) {return ru?russian:en;}
void info(const std::wstring& text,UINT flags=MB_OK|MB_ICONINFORMATION) {MessageBoxW(nullptr,text.c_str(),L"CS2 Full Reset",flags|MB_TOPMOST);}
class Logger {
    Handle file_;
public:
    explicit Logger(const fs::path& directory):file_([&]{
        ensure_directory(directory); PathLock lock(directory);
        auto p=directory/L"last.log";if(fs::exists(p))erase_tree(directory,p,[]{});
        auto h=CreateFileW(p.c_str(),FILE_APPEND_DATA,FILE_SHARE_READ,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(h==INVALID_HANDLE_VALUE)win_error("Create log");return h;
    }()){}
    void write(const std::string& event) {
        SYSTEMTIME t{};GetLocalTime(&t);char stamp[64];sprintf_s(stamp,"%04u-%02u-%02u %02u:%02u:%02u ",t.wYear,t.wMonth,t.wDay,t.wHour,t.wMinute,t.wSecond);
        auto text=std::string(stamp)+event+"\r\n";DWORD written=0;
        if(!WriteFile(file_.get(),text.data(),static_cast<DWORD>(text.size()),&written,nullptr) || written!=text.size() || !FlushFileBuffers(file_.get()))win_error("Write log");
    }
};
std::size_t select(const std::vector<std::wstring>& entries) {
    if(entries.empty())throw Error("No accounts with an existing 730 directory");
    DWORD mode=0;auto out=GetStdHandle(STD_OUTPUT_HANDLE);CONSOLE_SCREEN_BUFFER_INFO state{};
    if(!GetConsoleMode(GetStdHandle(STD_INPUT_HANDLE),&mode) || !GetConsoleScreenBufferInfo(out,&state))throw Error("Interactive selection needs a console; supply --account ID explicitly");
    std::size_t chosen=0;
    for(;;) {
        SetConsoleCursorPosition(out,state.dwCursorPosition);
        for(std::size_t i=0;i<entries.size();++i)std::wcout<<(i==chosen?L" > ":L"   ")<<entries[i]<<L"\n";
        std::wcout.flush();auto key=_getwch();
        if(key==13)return chosen;
        if(key==27)throw Error("Cancelled");
        if(key==0 || key==224){key=_getwch();if(key==72)chosen=chosen?chosen-1:entries.size()-1;else if(key==80)chosen=(chosen+1)%entries.size();}
    }
}
void help() {
    std::wcout<<L"CS2 Full Reset 0.1.0\n"
        <<tr(L"Creates a backup before changing anything.\n",L"Создаёт резервную копию перед изменениями.\n")
        <<L"  --run                 "<<tr(L"Reset and launch immediately",L"Сбросить и запустить без меню")<<L"\n"
        <<L"  --backup-only         "<<tr(L"Back up the whole 730 directory",L"Создать копию всей папки 730")<<L"\n"
        <<L"  --restore-latest      "<<tr(L"Restore pending/or latest snapshot",L"Восстановить исходную/последнюю копию")<<L"\n"
        <<L"  --select-account      "<<tr(L"Select and save an account",L"Выбрать и сохранить аккаунт")<<L"\n"
        <<L"  --account ID          "<<tr(L"Explicit account selection for this run",L"Явно выбрать аккаунт для этого запуска")<<L"\n"
        <<L"  --diagnose            "<<tr(L"Read-only Steam/account/UIA diagnostics",L"Диагностика Steam/аккаунтов/UIA без изменений")<<L"\n"
        <<L"  --help\n";
}
}
int wmain(int argc,wchar_t** argv) {
    using namespace reset;
    _setmode(_fileno(stdout),_O_U8TEXT);_setmode(_fileno(stderr),_O_U8TEXT);
    // UIA workers are bounded children and never open the normal workflow.
    if(argc==4 && (std::wstring(argv[1])==L"--uia-probe" || std::wstring(argv[1])==L"--uia-invoke")) {
        try {auto steam=discover_steam();if(_wcsicmp(steam.c_str(),fs::path(argv[2]).c_str())!=0)return 12;auto expected=std::stoull(argv[3]);if(expected>UINT32_MAX)return 12;return probe_steam(steam,std::wstring(argv[1])==L"--uia-invoke",static_cast<std::uint32_t>(expected));}catch(...){return 12;}
    }
    std::unique_ptr<Logger> logger;
    fs::path backupRoot;
    std::string action,explicitAccount;
    bool modified=false;
    try {
        for(int i=1;i<argc;++i) {
            std::wstring arg=argv[i];
            if(arg==L"--help"){help();return 0;}
            if(arg==L"--account" && i+1<argc){if(!explicitAccount.empty())throw Error("Duplicate account option");explicitAccount=utf8(argv[++i]);continue;}
            if(arg==L"--run" || arg==L"--backup-only" || arg==L"--restore-latest" || arg==L"--select-account" || arg==L"--diagnose") {if(!action.empty())throw Error("Choose exactly one action");action=utf8(arg);}
            else throw Error("Unknown option; use --help");
        }
        Handle instance(CreateMutexW(nullptr,FALSE,L"Local\\CS2FullReset.Workflow"));
        if(!instance.get())win_error("Create single-instance lock");if(GetLastError()==ERROR_ALREADY_EXISTS)throw Error("Another CS2 Full Reset instance is running");
        auto steam=discover_steam(); auto accounts=find_accounts(steam);
        if(action=="--diagnose") {
            std::wcout<<L"Steam: "<<steam.wstring()<<L"\nActive SteamID32: "<<active_account()<<L"\ncs2.exe: "<<(process_running(L"cs2.exe")?L"running":L"closed")<<L"\n";
            for(const auto& account:accounts)std::wcout<<wide(account.id)<<L" - "<<account.name<<L" (login "<<account.timestamp<<L")\n";
            std::wcout<<L"UIA probe: "<<bounded_probe(steam,false)<<L" (10 conflict, 11 sync error, 12 unconfirmed)\n";return 0;
        }
        require_game_closed();
        backupRoot=executable_path().parent_path()/L"cs2_backup";
        ensure_directory(backupRoot);logger=std::make_unique<Logger>(backupRoot/L"logs");
        auto log=[&](const std::string& text){logger->write(text);};
        log("CS2 Full Reset 0.1.0 x64");log("Steam path: "+utf8(steam.wstring()));
        if(accounts.empty())throw Error("No accounts with an existing 730 directory");
        const auto settings=backupRoot/L"settings.json";
        std::optional<std::string> selected;
        if(!explicitAccount.empty()) {
            for(const auto& account:accounts)if(account.id==explicitAccount)selected=account.id;
            if(!selected)throw Error("Explicit account does not have a valid 730 directory");
        } else if(action!="--select-account")selected=saved_account(settings,accounts);
        if(!selected) {
            std::wcout<<tr(L"Select account (Up/Down, Enter; Esc cancels):\n",L"Выберите аккаунт (стрелки, Enter; Esc — отмена):\n");
            std::vector<std::wstring> names;for(const auto& a:accounts)names.push_back(wide(a.id)+L" - "+a.name);
            selected=accounts[select(names)].id;save_account(settings,*selected);
        }
        if(action=="--select-account"){save_account(settings,*selected);std::wcout<<tr(L"Account saved.\n",L"Аккаунт сохранён.\n");return 0;}
        auto account=*std::find_if(accounts.begin(),accounts.end(),[&](const auto& a){return a.id==*selected;});
        auto target=steam/L"userdata"/wide(account.id)/L"730";
        log("Selected SteamID32: "+account.id);log("Source: "+utf8(target.wstring()));
        Guard guard(steam,target,require_game_closed);
        BackupStore backups(backupRoot,account.id,account.name,require_game_closed);
        std::wcout<<L"CS2 Full Reset — "<<wide(account.id)<<L" — "<<account.name<<L"\n"
            <<tr(L"A full backup is created before any changes.\n",L"Перед изменениями создаётся полная резервная копия.\n")
            <<tr(L"Recommendation: close Steam before starting; it will be launched after truncation.\n",L"Рекомендация: перед началом закройте Steam; после обнуления он запустится автоматически.\n");
        if(action.empty()) {
            std::vector<std::wstring> options={tr(L"Reset & Launch CS2",L"Сбросить и запустить CS2"),tr(L"Restore latest backup",L"Восстановить последнюю копию"),tr(L"Backup only",L"Только резервная копия"),tr(L"Open backup folder",L"Открыть папку копий"),tr(L"Open log",L"Открыть лог"),tr(L"Exit",L"Выход")};
            auto choice=select(options);
            if(choice==5)return 0;
            if(choice==3 || choice==4){auto p=choice==3?backups.directory():backupRoot/L"logs/last.log";if(reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr,L"open",p.c_str(),nullptr,nullptr,SW_SHOWNORMAL))<=32)throw Error("Cannot open file/folder");return 0;}
            action=choice==0?"--run":choice==1?"--restore-latest":"--backup-only";
        }
        if(action=="--backup-only") {
            std::wcout<<tr(L"Creating backup...\n",L"Создание резервной копии...\n");auto snapshot=backups.create(guard);log("Backup verified: "+utf8(snapshot.wstring()));std::wcout<<snapshot.wstring()<<L"\n";return 0;
        }
        if(action=="--restore-latest") {
            std::wcout<<tr(L"Restoring (current state will be backed up first)...\n",L"Восстановление (сначала будет сохранено текущее состояние)...\n");
            auto restoreSource=backups.pending();if(restoreSource.empty())restoreSource=backups.latest();
            log("Restore source backup: "+utf8(restoreSource.wstring()));
            modified=true;backups.restore(guard);log("Current-state safety backup: "+utf8(backups.latest().wstring()));
            log("Restore verified");std::wcout<<tr(L"Backup restored.\n",L"Копия восстановлена.\n");return 0;
        }
        WindowsSteam adapter(steam,account.id,ru,log);
        std::wcout<<tr(L"Reset in progress. Waiting for a Cloud Conflict can take 60 seconds.\n",L"Выполняется сброс. Ожидание конфликта Cloud — до 60 секунд.\n");
        modified=true;auto outcome=run_reset(guard,backups,adapter,log);
        if(outcome==Outcome::Started){std::wcout<<tr(L"Reset completed; cs2.exe started.\n",L"Сброс завершён; cs2.exe запущен.\n");return 0;}
        if(outcome==Outcome::AwaitingLocal) {
            info(tr(L"Cleanup completed, but game launch was not observed. In the open Steam conflict choose LOCAL SAVE. Your original backup is retained. If you cancel, close CS2 before using --restore-latest.",L"Очистка завершена, но запуск игры не обнаружен. В открытом конфликте Steam выберите ЛОКАЛЬНЫЕ ФАЙЛЫ. Исходная копия сохранена. Если отмените запуск, закройте CS2 перед --restore-latest.")); return 2;
        }
        if(process_running(L"cs2.exe")) {
            info(tr(L"The exact reset flow could not be completed. Close CS2 before restoring with --restore-latest. No further files will be changed.",L"Точный сценарий сброса не завершён. Закройте CS2 перед восстановлением через --restore-latest. Дальнейшие изменения файлов запрещены."),MB_OK|MB_ICONWARNING);return 2;
        }
        if(MessageBoxW(nullptr,tr(L"Reset was not completed. Cancel the pending game launch in Steam before restoring. Restore the original backup now?",L"Сброс не завершён. Перед восстановлением отмените ожидающий запуск игры в Steam. Восстановить исходную копию сейчас?"),L"CS2 Full Reset",MB_YESNO|MB_DEFBUTTON2|MB_ICONWARNING|MB_TOPMOST)==IDYES) {
            require_game_closed();backups.restore(guard);log("Recovery restore verified");
        }
        return 2;
    } catch(const std::exception& error) {
        if(logger)try{logger->write(std::string("ERROR: ")+error.what());}catch(...){}
        std::wcout<<tr(L"Stopped: ",L"Остановлено: ")<<wide(error.what())<<L"\n";
        auto message=std::wstring(tr(L"Operation stopped. See the details below and cs2_backup/logs/last.log.\n\n",L"Операция остановлена. Подробности ниже и в cs2_backup/logs/last.log.\n\n"))+wide(error.what());
        if(modified)message+=tr(L"\n\nA backup is retained if its creation completed. Close CS2, cancel any pending Steam launch, then run --restore-latest to recover.",L"\n\nРезервная копия сохранена, если её создание завершилось. Закройте CS2, отмените ожидающий запуск в Steam и выполните --restore-latest для восстановления.");
        if(action!="--diagnose")info(message,MB_OK|MB_ICONERROR);
        return 1;
    }
}
