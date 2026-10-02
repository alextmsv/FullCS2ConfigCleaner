#pragma once
#include "safe_fs.hpp"
#include <optional>
namespace reset {
struct Account {std::string id; std::wstring name; std::uint64_t timestamp=0; bool recent=false;};
std::vector<Account> parse_accounts(const std::string& vdf);
std::vector<Account> find_accounts(const fs::path& steam);
std::optional<std::string> saved_account(const fs::path& settings, const std::vector<Account>& accounts);
void save_account(const fs::path& settings,const std::string& id);
}
