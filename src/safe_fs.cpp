#include "safe_fs.hpp"
#include <algorithm>
#include <limits>

namespace reset {
std::string utf8(const std::wstring& s) {
    if (s.empty()) return {};
    int n=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,s.data(),static_cast<int>(s.size()),nullptr,0,nullptr,nullptr);
    if (!n) win_error("UTF-8 encoding");
    std::string out(n,'\0'); WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,s.data(),static_cast<int>(s.size()),out.data(),n,nullptr,nullptr); return out;
}
std::wstring wide(const std::string& s) {
    if (s.empty()) return {};
    int n=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,s.data(),static_cast<int>(s.size()),nullptr,0);
    if (!n) win_error("UTF-8 decoding");
    std::wstring out(n,L'\0'); MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,s.data(),static_cast<int>(s.size()),out.data(),n); return out;
}
[[noreturn]] void win_error(const char* operation) { throw Error(std::string(operation)+" (Win32 "+std::to_string(GetLastError())+")"); }
std::wstring lower_text(const std::wstring& text) {
    if(text.empty())return {};
    int n=LCMapStringEx(LOCALE_NAME_INVARIANT,LCMAP_LOWERCASE,text.data(),static_cast<int>(text.size()),nullptr,0,nullptr,nullptr,0);
    if(!n)win_error("Map text case");std::wstring out(n,L'\0');
    if(!LCMapStringEx(LOCALE_NAME_INVARIANT,LCMAP_LOWERCASE,text.data(),static_cast<int>(text.size()),out.data(),n,nullptr,nullptr,0))win_error("Map text case");return out;
}
static bool equal(const fs::path& a,const fs::path& b) {
    auto x=a.lexically_normal().wstring(), y=b.lexically_normal().wstring();
    return CompareStringOrdinal(x.c_str(),-1,y.c_str(),-1,TRUE)==CSTR_EQUAL;
}
static void lexical(const fs::path& p) {
    auto s=p.wstring();
    if (s.empty() || !p.is_absolute() || s.size()<3 || s[1]!=L':' || (s[2]!=L'\\' && s[2]!=L'/') || s.size()>240)
        throw Error("Expected a local absolute path of at most 240 characters");
    for (const auto& part:p.relative_path()) {
        auto t=part.wstring();
        if (t.empty() || t==L"." || t==L".." || t.back()==L'.' || t.back()==L' ' || t.find_first_of(L":*?\"<>|")!=std::wstring::npos)
            throw Error("Unsafe path component");
    }
}
static Handle open_checked(const fs::path& p,DWORD access,DWORD share=FILE_SHARE_READ|FILE_SHARE_WRITE) {
    lexical(p);
    Handle h(CreateFileW(p.c_str(),access,share,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_BACKUP_SEMANTICS,nullptr));
    if (h.get()==INVALID_HANDLE_VALUE) win_error("Open guarded path");
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(h.get(),&info)) win_error("Inspect guarded path");
    if (info.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT) throw Error("Reparse points are not supported");
    if (!(info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY) && info.nNumberOfLinks!=1) throw Error("Hard-linked files are not supported");
    std::wstring final(32768,L'\0');
    DWORD n=GetFinalPathNameByHandleW(h.get(),final.data(),static_cast<DWORD>(final.size()),FILE_NAME_NORMALIZED|VOLUME_NAME_DOS);
    if (!n || n>=final.size()) win_error("Resolve guarded path");
    final.resize(n); if(final.starts_with(L"\\\\?\\")) final.erase(0,4);
    if (!equal(p,fs::path(final))) throw Error("Resolved path differs from requested path");
    return h;
}
PathLock::PathLock(const fs::path& path) {
    lexical(path); auto p=path.root_path();
    handles_.push_back(open_checked(p,FILE_READ_ATTRIBUTES));
    for (const auto& part:path.relative_path()) { p/=part; handles_.push_back(open_checked(p,FILE_READ_ATTRIBUTES)); }
}
void validate_path(const fs::path& steam,const fs::path& target) {
    lexical(steam); lexical(target);
    const auto account=target.parent_path().filename().wstring();
    if (target.filename()!=L"730" || account.empty() || account.size()>10 ||
        !std::all_of(account.begin(),account.end(),[](wchar_t c){return c>=L'0' && c<=L'9';}) ||
        std::stoull(account)>UINT32_MAX || std::stoull(account)==0 ||
        !equal(target.parent_path().parent_path(),steam/L"userdata")) throw Error("Not this Steam root's numeric userdata/730 directory");
    PathLock lock(target);
    if (!fs::is_directory(target)) throw Error("730 is not a directory");
}
static void scan(const fs::path& root,const fs::path& p,Manifest& out) {
    PathLock parent(p.parent_path()); auto h=open_checked(p,FILE_READ_ATTRIBUTES,FILE_SHARE_READ);
    BY_HANDLE_FILE_INFORMATION info{}; if(!GetFileInformationByHandle(h.get(),&info)) win_error("Read file information");
    auto rel=p.lexically_relative(root);
    if(info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY) {
        if(p!=root) out.emplace(rel,std::numeric_limits<std::uintmax_t>::max());
        for(const auto& entry:fs::directory_iterator(p)) scan(root,entry.path(),out);
    } else out.emplace(rel,(static_cast<std::uintmax_t>(info.nFileSizeHigh)<<32)|info.nFileSizeLow);
}
Manifest manifest(const fs::path& root) { Manifest m; scan(root,root,m); return m; }
static void make_directory(const fs::path& p,const IdleCheck& idle) {
    PathLock lock(p.parent_path()); idle();
    if (!CreateDirectoryW(p.c_str(),nullptr) && GetLastError()!=ERROR_ALREADY_EXISTS) win_error("Create directory");
    PathLock verify(p); if(!fs::is_directory(p)) throw Error("Expected directory");
}
static bool inside(const fs::path& root,const fs::path& target);
void copy_tree(const fs::path& source,const fs::path& destination,const IdleCheck& idle) {
    if(inside(source,destination) || inside(destination,source))throw Error("Backup source and destination overlap");
    PathLock srcLock(source), dstLock(destination.parent_path());
    const auto before=manifest(source);
    if(fs::exists(destination)) throw Error("Backup destination already exists");
    make_directory(destination,idle);
    for(const auto& [rel,size]:before) {
        idle(); auto src=source/rel,dst=destination/rel;
        if(size==std::numeric_limits<std::uintmax_t>::max()) make_directory(dst,idle);
        else {
            PathLock s(src.parent_path()), d(dst.parent_path());
            auto file=open_checked(src,GENERIC_READ,FILE_SHARE_READ);
            if(!CopyFileW(src.c_str(),dst.c_str(),TRUE)) win_error("Copy backup file");
        }
    }
    if(manifest(source)!=before || manifest(destination)!=before) throw Error("Backup structure/size verification failed");
}
static bool inside(const fs::path& root,const fs::path& target) {
    auto cur=target; while(!cur.empty()) {if(equal(root,cur)) return true; auto parent=cur.parent_path(); if(parent==cur) break; cur=parent;} return false;
}
void erase_tree(const fs::path& root,const fs::path& target,const IdleCheck& idle) {
    lexical(root); lexical(target);
    if(!inside(root,target) || equal(root,target)) throw Error("Deletion escaped its guarded root");
    PathLock parent(target.parent_path());
    {
        auto h=open_checked(target,FILE_READ_ATTRIBUTES);
        BY_HANDLE_FILE_INFORMATION info{}; if(!GetFileInformationByHandle(h.get(),&info)) win_error("Inspect deletion target");
        if(info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY) {
            std::vector<fs::path> children;
            for(const auto& entry:fs::directory_iterator(target)) children.push_back(entry.path());
            for(const auto& child:children) erase_tree(root,child,idle);
        }
    }
    idle(); auto h=open_checked(target,DELETE|FILE_READ_ATTRIBUTES);
    FILE_DISPOSITION_INFO disposition{TRUE};
    if(!SetFileInformationByHandle(h.get(),FileDispositionInfo,&disposition,sizeof(disposition))) win_error("Delete guarded entry");
}
Guard::Guard(fs::path s,fs::path t,IdleCheck i):steam_(std::move(s)),target_(std::move(t)),idle_(std::move(i)) {check();}
void Guard::check() const {idle_(); validate_path(steam_,target_);}
void Guard::preflight() const {
    check();PathLock lock(target_);
    auto directory=open_checked(target_,FILE_LIST_DIRECTORY|FILE_ADD_FILE|FILE_ADD_SUBDIRECTORY|FILE_DELETE_CHILD);
    for(const auto& [rel,size]:manifest(target_)) {
        idle_();auto p=target_/rel;PathLock parent(p.parent_path());
        const bool isDirectory=size==std::numeric_limits<std::uintmax_t>::max();
        if(!isDirectory && (GetFileAttributesW(p.c_str())&FILE_ATTRIBUTE_READONLY))throw Error("Read-only config file: reset stopped before changes");
        auto file=open_checked(p,isDirectory?FILE_LIST_DIRECTORY|FILE_ADD_FILE|FILE_ADD_SUBDIRECTORY|FILE_DELETE_CHILD:GENERIC_READ|GENERIC_WRITE|DELETE,FILE_SHARE_READ|FILE_SHARE_WRITE);
    }
}
std::size_t Guard::phase_a() {
    preflight(); PathLock lock(target_); std::size_t count=0;
    for(const auto* name:{L"remote",L"local"}) {
        auto p=target_/name; if(!fs::exists(p)) continue;
        for(const auto& [rel,size]:manifest(p)) if(size!=std::numeric_limits<std::uintmax_t>::max()) {
            check(); PathLock parent((p/rel).parent_path()); auto h=open_checked(p/rel,GENERIC_WRITE,FILE_SHARE_READ);
            LARGE_INTEGER zero{}; idle_();
            if(!SetFilePointerEx(h.get(),zero,nullptr,FILE_BEGIN) || !SetEndOfFile(h.get()) || !FlushFileBuffers(h.get())) win_error("Truncate guarded file");
            LARGE_INTEGER length{}; if(!GetFileSizeEx(h.get(),&length) || length.QuadPart!=0) throw Error("Truncation verification failed"); ++count;
        }
        for(const auto& [rel,size]:manifest(p)) { (void)rel; if(size!=0 && size!=std::numeric_limits<std::uintmax_t>::max()) throw Error("File changed during Phase A"); }
    }
    return count;
}
void Guard::phase_b() {
    preflight(); PathLock lock(target_);
    for(const auto* name:{L"remote",L"local"}) {
        auto p=target_/name;
        if(fs::exists(p)) { std::vector<fs::path> children; for(const auto& e:fs::directory_iterator(p)) children.push_back(e.path()); for(const auto& child:children) erase_tree(target_,child,[&]{check();}); }
        else make_directory(p,idle_);
    }
    auto cache=target_/L"remotecache.vdf";
    if(fs::exists(cache)) erase_tree(target_,cache,[&]{check();});
    make_directory(target_/L"local/cfg",idle_);
    if(!fs::is_empty(target_/L"local/cfg")) throw Error("local/cfg is not empty");
}
void Guard::restore_from(const fs::path& backup) {
    if(inside(target_,backup) || inside(backup,target_))throw Error("Restore source and target overlap");
    preflight(); PathLock lock(target_), backupLock(backup);
    const auto expected=manifest(backup); (void)manifest(target_);
    std::vector<fs::path> children; for(const auto& e:fs::directory_iterator(target_)) children.push_back(e.path());
    for(const auto& child:children) erase_tree(target_,child,[&]{check();});
    for(const auto& [rel,size]:expected) {
        check(); const auto dst=target_/rel;
        if(size==std::numeric_limits<std::uintmax_t>::max()) make_directory(dst,idle_);
        else {
            PathLock parent(dst.parent_path()), srcParent((backup/rel).parent_path());
            auto src=open_checked(backup/rel,GENERIC_READ,FILE_SHARE_READ);
            if(!CopyFileW((backup/rel).c_str(),dst.c_str(),TRUE)) win_error("Restore file");
        }
    }
    if(manifest(target_)!=expected) throw Error("Restore verification failed");
}
}
