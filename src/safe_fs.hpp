#pragma once
#include <windows.h>
#include <filesystem>
#include <functional>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace reset {
namespace fs = std::filesystem;
using IdleCheck = std::function<void()>;
struct Error : std::runtime_error { using runtime_error::runtime_error; };
std::string utf8(const std::wstring& text);
std::wstring wide(const std::string& text);
std::wstring lower_text(const std::wstring& text);
[[noreturn]] void win_error(const char* operation);
class Handle {
    HANDLE h_ = INVALID_HANDLE_VALUE;
public:
    Handle() = default;
    explicit Handle(HANDLE h) : h_(h) {}
    ~Handle() { if (h_ != INVALID_HANDLE_VALUE && h_ != nullptr) CloseHandle(h_); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    Handle(Handle&& other) noexcept : h_(other.h_) { other.h_ = INVALID_HANDLE_VALUE; }
    HANDLE get() const { return h_; }
};
// Pins each existing ancestor against rename, and rejects all reparse points.
class PathLock {
    std::vector<Handle> handles_;
public:
    explicit PathLock(const fs::path& path);
};
using Manifest = std::map<fs::path, std::uintmax_t>; // directories use UINTMAX_MAX
void validate_path(const fs::path& steam, const fs::path& target);
Manifest manifest(const fs::path& root);
void copy_tree(const fs::path& source, const fs::path& destination, const IdleCheck& idle);
void erase_tree(const fs::path& root, const fs::path& target, const IdleCheck& idle);
class Guard {
    fs::path steam_, target_;
    IdleCheck idle_;
public:
    Guard(fs::path steam, fs::path target, IdleCheck idle);
    const fs::path& path() const { return target_; }
    void check() const;
    void preflight() const;
    std::size_t phase_a();
    void phase_b();
    void restore_from(const fs::path& backup);
};
}
