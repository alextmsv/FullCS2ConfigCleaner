#pragma once
#include "safe_fs.hpp"
namespace reset {
void write_new(const fs::path& path, const std::string& text);
std::string read_small(const fs::path& path, std::size_t limit=8*1024*1024);
void ensure_directory(const fs::path& path);
class BackupStore {
    fs::path directory_;
    std::string account_;
    IdleCheck idle_;
    void verify(const fs::path& snapshot) const;
    void prune(const fs::path& newest, const fs::path& keep={});
public:
    BackupStore(fs::path root, std::string account, std::wstring nickname, IdleCheck idle);
    const fs::path& directory() const { return directory_; }
    std::vector<fs::path> list() const;
    fs::path create(const Guard& guard, const fs::path& keep={});
    fs::path latest() const;
    fs::path pending() const;
    void pin(const fs::path& snapshot);
    void clear_pending();
    void restore(Guard& guard);
};
}
