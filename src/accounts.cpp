#include "accounts.hpp"
#include "backup.hpp"
#include <algorithm>
#include <charconv>
#include <cctype>
#include <regex>

namespace reset {
struct Vdf {std::string value; std::map<std::string,Vdf> children;};
class Parser {
    const std::string& s_; std::size_t pos_=0;
    std::string token() {
        for(;;) {while(pos_<s_.size() && std::isspace(static_cast<unsigned char>(s_[pos_]))) ++pos_; if(pos_+1<s_.size() && s_[pos_]=='/' && s_[pos_+1]=='/') {while(pos_<s_.size() && s_[pos_]!='\n') ++pos_;} else break;}
        if(pos_==s_.size()) return {};
        char c=s_[pos_++]; if(c=='{' || c=='}') return {c};
        if(c!='"') throw Error("Unsupported VDF token");
        std::string out;
        while(pos_<s_.size()) {c=s_[pos_++]; if(c=='"') return "\""+out; if(c=='\\') {if(pos_==s_.size()) break; c=s_[pos_++]; if(c=='n') c='\n'; if(c=='t') c='\t';} out+=c;}
        throw Error("Unterminated VDF string");
    }
    Vdf object(unsigned depth,bool nested) {
        if(depth>16) throw Error("VDF nesting too deep"); Vdf result;
        for(;;) {auto key=token(); if(key.empty()) {if(nested) throw Error("Truncated VDF"); return result;} if(key=="}") {if(!nested) throw Error("Unexpected VDF brace"); return result;} if(key[0]!='"') throw Error("Invalid VDF key"); key.erase(0,1); auto value=token(); Vdf node; if(value=="{") node=object(depth+1,true); else if(!value.empty() && value[0]=='"') node.value=value.substr(1); else throw Error("Invalid VDF value"); if(!result.children.emplace(key,std::move(node)).second) throw Error("Duplicate VDF key");}
    }
public: explicit Parser(const std::string& s):s_(s) {} Vdf parse(){return object(0,false);}
};
static std::uint64_t number(const std::string& s) {std::uint64_t n=0;auto [end,ec]=std::from_chars(s.data(),s.data()+s.size(),n);return ec==std::errc{} && end==s.data()+s.size()?n:0;}
static void sort_accounts(std::vector<Account>& a) {std::sort(a.begin(),a.end(),[](const auto& x,const auto& y){if(x.timestamp!=y.timestamp) return x.timestamp>y.timestamp; if(x.recent!=y.recent)return x.recent; return x.id<y.id;});}
std::vector<Account> parse_accounts(const std::string& vdf) {
    auto doc=Parser(vdf).parse(); auto users=doc.children.find("users"); if(users==doc.children.end()) throw Error("Missing users in loginusers.vdf");
    std::vector<Account> result; constexpr std::uint64_t base=76561197960265728ULL;
    for(const auto& [id,node]:users->second.children) {
        auto n=number(id); if(n<=base || n-base>UINT32_MAX) continue;
        auto field=[&](const char* key){auto it=node.children.find(key);return it==node.children.end()?std::string{}:it->second.value;};
        auto name=wide(field("PersonaName")); if(name.empty()) name=L"Unknown";
        for(auto& c:name) if(c<32 || c==127) c=L' ';
        result.push_back({std::to_string(n-base),name,number(field("Timestamp")),field("MostRecent")=="1"});
    }
    sort_accounts(result); return result;
}
std::vector<Account> find_accounts(const fs::path& steam) {
    std::vector<Account> known;
    try {known=parse_accounts(read_small(steam/L"config/loginusers.vdf"));} catch(const std::exception&) { /* IDs remain selectable when names are unavailable. */ }
    PathLock lock(steam/L"userdata"); std::vector<Account> result;
    for(const auto& entry:fs::directory_iterator(steam/L"userdata")) {
        const auto id=utf8(entry.path().filename().wstring()); auto n=number(id); if(!n || n>UINT32_MAX || std::to_string(n)!=id) continue;
        if(!fs::exists(entry.path()/L"730")) continue;
        validate_path(steam,entry.path()/L"730"); auto it=std::find_if(known.begin(),known.end(),[&](const auto& a){return a.id==id;});
        result.push_back(it==known.end()?Account{id,L"Unknown",0,false}:*it);
    }
    sort_accounts(result); return result;
}
std::optional<std::string> saved_account(const fs::path& settings,const std::vector<Account>& accounts) {
    if(!fs::exists(settings)) return {};
    auto text=read_small(settings,1024); std::smatch m;
    const std::regex pattern(R"json(^\s*\{\s*"steam_id32"\s*:\s*"([0-9]{1,10})"\s*\}\s*$)json");
    if(!std::regex_match(text,m,pattern)) return {};
    auto id=m[1].str(); for(const auto& a:accounts) if(a.id==id) return id; return {};
}
void save_account(const fs::path& settings,const std::string& id) {
    if(!number(id) || number(id)>UINT32_MAX || std::to_string(number(id))!=id) throw Error("Invalid selected account");
    PathLock lock(settings.parent_path()); auto staging=fs::path(settings.wstring()+L".tmp");
    if(fs::exists(staging)) erase_tree(settings.parent_path(),staging,[]{});
    write_new(staging,"{\"steam_id32\":\""+id+"\"}\n");
    if(fs::exists(settings)) {PathLock verify(settings);}
    if(!MoveFileExW(staging.c_str(),settings.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)) win_error("Save account selection");
}
}
