#include "backup.hpp"
#include <algorithm>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>

namespace reset {
void write_new(const fs::path& path,const std::string& text) {
    PathLock lock(path.parent_path());
    Handle h(CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
    if(h.get()==INVALID_HANDLE_VALUE) win_error("Create metadata");
    DWORD written=0;
    if(text.size()>MAXDWORD || !WriteFile(h.get(),text.data(),static_cast<DWORD>(text.size()),&written,nullptr) || written!=text.size() || !FlushFileBuffers(h.get())) win_error("Write metadata");
}
std::string read_small(const fs::path& path,std::size_t limit) {
    PathLock lock(path);
    if(fs::file_size(path)>limit) throw Error("Metadata exceeds size limit");
    std::ifstream file(path,std::ios::binary); if(!file) throw Error("Cannot read metadata");
    std::string data{std::istreambuf_iterator<char>(file),{}};
    if(file.bad() || data.size()>limit) throw Error("Metadata read failed"); return data;
}
void ensure_directory(const fs::path& path) {
    if(path.empty() || !path.is_absolute())throw Error("Storage path must be absolute");
    if(fs::exists(path)) {PathLock lock(path); if(!fs::is_directory(path)) throw Error("Expected directory"); return;}
    ensure_directory(path.parent_path()); PathLock parent(path.parent_path());
    if(!CreateDirectoryW(path.c_str(),nullptr) && GetLastError()!=ERROR_ALREADY_EXISTS) win_error("Create storage directory");
    PathLock verify(path);
}
static bool digits(const std::string& s) {return !s.empty() && std::all_of(s.begin(),s.end(),[](char c){return c>='0' && c<='9';});}
static std::wstring safe_name(std::wstring name) {
    if(name.size()>32) name.resize(32);
    for(auto& c:name) if(c<32 || std::wstring(L"<>:\"/\\|?*.").find(c)!=std::wstring::npos) c=L'_';
    if(name.empty()) name=L"profile";
    return L"profile_"+name;
}
BackupStore::BackupStore(fs::path root,std::string account,std::wstring nickname,IdleCheck idle):account_(std::move(account)),idle_(std::move(idle)) {
    if(!digits(account_) || account_.size()>10 || std::stoull(account_)==0 || std::stoull(account_)>UINT32_MAX) throw Error("Invalid backup account");
    idle_(); ensure_directory(root); PathLock rootLock(root);
    const auto suffix=L"_"+wide(account_);
    for(const auto& entry:fs::directory_iterator(root)) if(entry.path().filename().wstring().ends_with(suffix)) {
        PathLock lock(entry.path()); if(!entry.is_directory()) continue;
        if(!directory_.empty()) throw Error("Multiple backup directories for this account"); directory_=entry.path();
    }
    if(directory_.empty()) {directory_=root/(safe_name(nickname)+suffix); ensure_directory(directory_);}
    // A committed restore only needs metadata finalization after an interruption.
    if(fs::exists(directory_/L"restore.done")) {
        clear_pending(); erase_tree(directory_,directory_/L"restore.done",idle_);
    }
}
static std::string manifest_text(const fs::path& tree,const std::string& account) {
    std::ostringstream text; text<<"CS2FullReset 1 "<<account<<'\n';
    for(const auto& [rel,size]:manifest(tree)) text<<size<<' '<<std::quoted(utf8(rel.wstring()))<<'\n';
    return text.str();
}
void BackupStore::verify(const fs::path& snapshot) const {
    if(snapshot.parent_path()!=directory_) throw Error("Snapshot is outside account directory");
    PathLock lock(snapshot);
    if(read_small(snapshot/L"complete.txt")!=manifest_text(snapshot/L"730",account_)) throw Error("Backup manifest mismatch");
}
std::vector<fs::path> BackupStore::list() const {
    PathLock lock(directory_); std::vector<fs::path> result;
    for(const auto& e:fs::directory_iterator(directory_)) {
        const auto name=e.path().filename().wstring();
        if(name.size()<23 || name[4]!=L'-' || name[7]!=L'-' || name[10]!=L'_' || name.ends_with(L".partial") || name.ends_with(L".deleting")) continue;
        PathLock entry(e.path());
        if(e.is_directory() && fs::exists(e.path()/L"complete.txt")) result.push_back(e.path());
    }
    std::sort(result.begin(),result.end()); return result;
}
fs::path BackupStore::latest() const {auto snapshots=list(); if(snapshots.empty()) throw Error("No completed backup for this account"); verify(snapshots.back()); return snapshots.back();}
fs::path BackupStore::pending() const {
    auto file=directory_/L"pending.txt"; if(!fs::exists(file)) return {};
    const auto name=wide(read_small(file,256)); fs::path relative(name);
    if(relative.empty() || relative.filename()!=relative || name==L"." || name==L"..") throw Error("Invalid pending reset marker");
    auto snapshot=directory_/relative; verify(snapshot); return snapshot;
}
void BackupStore::pin(const fs::path& snapshot) {verify(snapshot); if(!pending().empty()) throw Error("An unfinished reset already exists; restore it first"); write_new(directory_/L"pending.txt",utf8(snapshot.filename().wstring()));}
void BackupStore::clear_pending() {if(fs::exists(directory_/L"pending.txt")) erase_tree(directory_,directory_/L"pending.txt",[]{});}
void BackupStore::prune(const fs::path& newest,const fs::path& keep) {
    auto snapshots=list(); if(snapshots.size()<=2) return;
    const auto pinned=pending(); std::vector<fs::path> retained;
    if(!newest.empty()) retained.push_back(newest);
    if(!pinned.empty() && std::find(retained.begin(),retained.end(),pinned)==retained.end()) retained.push_back(pinned);
    if(!keep.empty() && std::find(retained.begin(),retained.end(),keep)==retained.end()) retained.push_back(keep);
    if(retained.size()>2) throw Error("Retention would discard a protected recovery snapshot");
    for(auto it=snapshots.rbegin();it!=snapshots.rend() && retained.size()<2;++it) if(std::find(retained.begin(),retained.end(),*it)==retained.end()) retained.push_back(*it);
    for(const auto& snapshot:snapshots) if(std::find(retained.begin(),retained.end(),snapshot)==retained.end()) {
        verify(snapshot); idle_(); auto tombstone=fs::path(snapshot.wstring()+L".deleting");
        if(!MoveFileExW(snapshot.c_str(),tombstone.c_str(),MOVEFILE_WRITE_THROUGH)) win_error("Retire old backup");
        erase_tree(directory_,tombstone,idle_);
    }
}
fs::path BackupStore::create(const Guard& guard,const fs::path& keep) {
    guard.check(); idle_(); PathLock lock(directory_);
    if(fs::exists(directory_/L"restore.txt")) throw Error("Restore transaction is unfinished; retry --restore-latest first");
    if(guard.path().parent_path().filename()!=wide(account_)) throw Error("Backup account does not match source");
    std::uintmax_t bytes=1024*1024;
    for(const auto& [rel,size]:manifest(guard.path())) { (void)rel; if(size!=std::numeric_limits<std::uintmax_t>::max()) {if(size>UINTMAX_MAX-bytes) throw Error("Backup size overflow"); bytes+=size;} }
    if(fs::space(directory_).available<bytes) throw Error("Not enough free space for backup");
    SYSTEMTIME t{}; GetLocalTime(&t); wchar_t stamp[80];
    swprintf_s(stamp,L"%04u-%02u-%02u_%02u-%02u-%02u-%03u",t.wYear,t.wMonth,t.wDay,t.wHour,t.wMinute,t.wSecond,t.wMilliseconds);
    // Monotonic directory ordering survives wall-clock rollback and DST changes.
    std::wstring base=stamp;auto existing=list();
    if(!existing.empty() && existing.back().filename().wstring()>base)base=existing.back().filename().wstring().substr(0,23);
    auto final=directory_/base;
    for(unsigned seq=1;fs::exists(final) || fs::exists(fs::path(final.wstring()+L".partial")) || (!existing.empty() && final<=existing.back());++seq) {
        if(seq>9999)throw Error("Too many snapshots with the same timestamp");
        wchar_t tail[20];swprintf_s(tail,L"-%04u",seq);final=directory_/(base+tail);
    }
    auto staging=fs::path(final.wstring()+L".partial"); ensure_directory(staging);
    copy_tree(guard.path(),staging/L"730",[&]{guard.check();});
    write_new(staging/L"complete.txt",manifest_text(staging/L"730",account_));
    idle_(); if(!MoveFileExW(staging.c_str(),final.c_str(),MOVEFILE_WRITE_THROUGH)) win_error("Commit backup");
    verify(final); prune(final,keep); return final;
}
void BackupStore::restore(Guard& guard) {
    guard.check();const auto transaction=directory_/L"restore.txt";
    fs::path target,safety;
    if(fs::exists(transaction)) {
        std::istringstream saved(read_small(transaction,1024));std::string a,b,extra;
        if(!std::getline(saved,a) || !std::getline(saved,b) || std::getline(saved,extra))throw Error("Invalid restore transaction");
        auto resolve=[&](const std::string& name){fs::path p(wide(name));if(p.empty() || p.filename()!=p || p==L"." || p==L"..")throw Error("Invalid transaction path");auto resolved=directory_/p;verify(resolved);return resolved;};
        target=resolve(a);safety=resolve(b);
        if(target==safety || pending()!=target)throw Error("Restore transaction does not match recovery marker");
    } else {
        target=pending();if(target.empty())target=latest();verify(target);
        if(pending().empty())pin(target);
        safety=create(guard,target);
        write_new(transaction,utf8(target.filename().wstring())+"\n"+utf8(safety.filename().wstring())+"\n");
    }
    guard.restore_from(target/L"730");
    if(!MoveFileExW(transaction.c_str(),(directory_/L"restore.done").c_str(),MOVEFILE_WRITE_THROUGH))win_error("Commit restore");
    clear_pending();erase_tree(directory_,directory_/L"restore.done",idle_);
}
}
