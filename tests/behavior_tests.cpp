// Every storage operation uses a unique temporary directory through the same
// data-directory override as production. Existing user files are never read.
#include "../src/behavior.h"
#include "../src/localization.h"
#include "../src/settings.h"
#include "../src/vi_tokens.h"

#include <array>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
unsigned int checks = 0;

void Require(bool condition, const char* message) {
    ++checks;
    if (!condition) throw std::runtime_error(message);
}

struct IsolatedData {
    std::wstring root, previous;
    bool hadPrevious = false;
    IsolatedData() {
        const DWORD required = GetEnvironmentVariableW(L"COPILOT_REEL_DATA_DIR", nullptr, 0);
        hadPrevious = required != 0;
        if (hadPrevious) {
            previous.resize(required);
            const auto copied = GetEnvironmentVariableW(L"COPILOT_REEL_DATA_DIR", previous.data(), required);
            previous.resize(copied);
        }
        std::array<wchar_t, MAX_PATH + 1> temporary{}, unique{};
        Require(GetTempPathW(static_cast<DWORD>(temporary.size()), temporary.data()) != 0,
            "temporary directory should be available");
        Require(GetTempFileNameW(temporary.data(), L"CRB", 0, unique.data()) != 0,
            "a unique test path should be created");
        root = unique.data();
        Require(DeleteFileW(root.c_str()) != 0 && CreateDirectoryW(root.c_str(), nullptr) != 0,
            "test fixture should own an isolated directory");
        Use(L"profile-a");
    }
    void Use(const wchar_t* name) {
        const auto path = root + L"\\" + name;
        Require(CreateDirectoryW(path.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS,
            "test profile directory should exist");
        Require(SetEnvironmentVariableW(L"COPILOT_REEL_DATA_DIR", path.c_str()) != 0,
            "test data override should be applied");
        Require(reel::ConfigPath() == path + L"\\apps.ini", "production config path should honor the isolated override");
    }
    ~IsolatedData() {
        SetEnvironmentVariableW(L"COPILOT_REEL_DATA_DIR", hadPrevious ? previous.c_str() : nullptr);
        // Remove only the exact files created by this fixture, never a tree.
        for (const auto* profile : {L"profile-a", L"profile-b"}) {
            const auto directory = root + L"\\" + profile;
            for (const auto* name : {L"behavior.ini", L"apps.ini", L"preferences.ini"})
                DeleteFileW((directory + L"\\" + name).c_str());
            RemoveDirectoryW(directory.c_str());
        }
        RemoveDirectoryW(root.c_str());
    }
};

std::wstring Sibling(const wchar_t* name) {
    const auto path = reel::ConfigPath();
    return path.substr(0, path.find_last_of(L"\\/") + 1) + name;
}

std::vector<unsigned char> ReadBytes(const std::wstring& path) {
    const auto file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    Require(file != INVALID_HANDLE_VALUE, "fixture file should be readable");
    const auto size = GetFileSize(file, nullptr);
    std::vector<unsigned char> bytes(size);
    DWORD read = 0;
    const bool okay = ReadFile(file, bytes.data(), size, &read, nullptr) && read == size;
    CloseHandle(file);
    Require(okay, "fixture file should be read completely");
    return bytes;
}

void WriteBytes(const std::wstring& path, const std::vector<unsigned char>& bytes) {
    const auto file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    Require(file != INVALID_HANDLE_VALUE, "fixture file should be writable");
    DWORD written = 0;
    const bool okay = WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) &&
        written == bytes.size();
    CloseHandle(file);
    Require(okay, "fixture file should be written completely");
}

void TestIdentity() {
    const reel::AppTarget app{L"Agent", reel::TargetKind::Shell, L"C:\\Agent\\agent.exe", L"--profile=work", L"C:\\Work"};
    const auto key = reel::StableTargetKey(app);
    auto changed = app;
    changed.name = L"My renamed agent";
    Require(reel::StableTargetKey(changed) == key, "renaming must retain launch identity");
    changed.arguments = L"--profile=personal";
    Require(reel::StableTargetKey(changed) != key, "profiles of one executable must remain distinct");
    changed = app; changed.kind = reel::TargetKind::Aumid;
    Require(reel::StableTargetKey(changed) != key, "launch method must be part of identity");
    changed = app; changed.workingDirectory = L"C:\\Other";
    Require(reel::StableTargetKey(changed) != key, "working directory must be part of identity");
    changed = app; changed.target = L"C:\\Other\\agent.exe";
    Require(reel::StableTargetKey(changed) != key, "launch target must be part of identity");
    changed = app; changed.target = L"ab"; changed.arguments = L"c";
    auto split = changed; split.target = L"a"; split.arguments = L"bc";
    Require(reel::StableTargetKey(changed) != reel::StableTargetKey(split), "field boundaries must prevent ambiguous concatenation");
    changed = app; changed.target = L"https://example.test/Agent";
    split = changed; split.target = L"https://example.test/agent";
    Require(reel::StableTargetKey(changed) != reel::StableTargetKey(split), "case-sensitive URI paths must retain their identity");
    changed = app; changed.target = L"C:\\\u4ee3\u7406\\agent.exe";
    Require(reel::StableTargetKey(changed) != key, "Unicode target names must participate in identity");
}

void TestStorage() {
    IsolatedData data;
    const reel::AppTarget first{L"First agent", reel::TargetKind::Shell, L"https://example.test/first", L"", L""};
    const reel::AppTarget preferred{L"Preferred agent", reel::TargetKind::Shell, L"C:\\Agent\\agent.exe", L"--profile=work", L"C:\\Work"};
    const std::vector<reel::AppTarget> targets{first, preferred};
    Require(reel::PreferredTargetIndex({}) == -1, "an empty list has no preferred entry");
    Require(reel::PreferredTargetIndex(targets) == 0, "a missing preference defaults to the first entry");
    std::wstring error;
    Require(reel::SaveTargets(targets, error), "test applications should save in the isolated directory");
    Require(reel::SaveLanguage(reel::Language::SimplifiedChinese), "test language should save in the isolated directory");
    const auto apps = ReadBytes(reel::ConfigPath());
    const auto language = ReadBytes(Sibling(L"preferences.ini"));
    Require(reel::SavePreferredTarget(preferred), "preferred target should save");
    const auto stored = ReadBytes(Sibling(L"behavior.ini"));
    Require(stored.size() > 2 && stored[0] == 0xff && stored[1] == 0xfe && stored.size() % 2 == 0,
        "behavior preferences should be BOM-prefixed UTF-16LE");
    Require(reel::PreferredTargetIndex(targets) == 1, "stored preference should resolve to the chosen target");
    Require(ReadBytes(reel::ConfigPath()) == apps, "saving behavior must leave application data unchanged");
    Require(ReadBytes(Sibling(L"preferences.ini")) == language, "saving behavior must leave language data unchanged");
    auto renamed = preferred; renamed.name = L"Renamed preferred agent";
    Require(reel::PreferredTargetIndex({first, renamed}) == 1, "rename should preserve the saved preference");
    Require(reel::PreferredTargetIndex({renamed, first}) == 0, "reordering should preserve the saved launch identity");
    Require(reel::PreferredTargetIndex({first}) == 0, "deleting the preferred target should fall back to the first entry");
    auto personal = preferred; personal.arguments = L"--profile=personal";
    Require(reel::PreferredTargetIndex({first, personal}) == 0, "another profile must not inherit the preference");

    data.Use(L"profile-b");
    Require(reel::PreferredTargetIndex(targets) == 0, "another data directory must not inherit the preference");
    Require(reel::SavePreferredTarget(first), "a second profile should save independently");
    data.Use(L"profile-a");
    Require(reel::PreferredTargetIndex(targets) == 1, "switching back should retain the original preference");

    const auto path = Sibling(L"behavior.ini");
    const auto locked = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    Require(locked != INVALID_HANDLE_VALUE, "fixture should lock the destination against replacement");
    const bool savedWhileLocked = reel::SavePreferredTarget(first);
    CloseHandle(locked);
    Require(!savedWhileLocked, "failed atomic replacement should be reported");
    Require(ReadBytes(path) == stored, "a failed save must retain the original preference file");
    WIN32_FIND_DATAW found{};
    const auto leftovers = FindFirstFileW((path + L".*.tmp").c_str(), &found);
    if (leftovers != INVALID_HANDLE_VALUE) FindClose(leftovers);
    Require(leftovers == INVALID_HANDLE_VALUE, "a failed save should remove its temporary file");

    WriteBytes(path, {0xef, 0xbb, 0xbf, 'b', 'a', 'd'});
    Require(reel::PreferredTargetIndex(targets) == 0, "corrupt preferences should use the first entry");
    Require(reel::SavePreferredTarget(preferred), "a valid save should recover from corrupt preference data");
    Require(reel::PreferredTargetIndex(targets) == 1, "recovered data should resolve normally");
    auto invalid = preferred; invalid.target.clear();
    Require(!reel::SavePreferredTarget(invalid), "empty launch targets must not replace the preference");
    Require(reel::PreferredTargetIndex(targets) == 1, "invalid input must retain the prior preference");
    Require(SetEnvironmentVariableW(L"COPILOT_REEL_DATA_DIR", L"relative-directory") != 0,
        "invalid override fixture should be set");
    Require(!reel::SavePreferredTarget(first), "an invalid override must fail instead of writing user data");
    Require(reel::PreferredTargetIndex(targets) == 0, "invalid data paths should have a safe default");
    data.Use(L"profile-a");
}

// preferences.ini holds the language and the reel size; saving either keeps the other.
void TestPreferences() {
    IsolatedData data;
    data.Use(L"profile-b");
    Require(reel::LoadReelSize() == reel::vi::kReelSizeDefault && reel::ReelSizePercent(reel::CurrentReelSize()) == 100,
        "a missing reel size selects the default, drawn as today");
    Require(reel::SaveReelSize(0), "a reel size step should save");
    reel::SetReelSize(0);
    Require(reel::SaveLanguage(reel::Language::SimplifiedChinese), "the language should save");
    reel::SetReelSize(reel::vi::kReelSizeDefault);
    Require(reel::LoadReelSize() == 0 && reel::ReelSizePercent(0) == 70, "saving the language keeps the reel size");
    Require(reel::LoadLanguage() == reel::Language::SimplifiedChinese, "saving the reel size keeps the language");
    Require(reel::SaveReelSize(reel::vi::kReelSizeCount - 1) && reel::LoadReelSize() == reel::vi::kReelSizeCount - 1,
        "the largest step round-trips");
    const auto path = data.root + L"\\profile-b\\preferences.ini";
    Require(WritePrivateProfileStringW(L"General", L"ReelSize", L"93", path.c_str()) != 0, "fixture should write a stray value");
    Require(reel::LoadReelSize() == reel::vi::kReelSizeDefault, "an unknown percentage selects the default");
    reel::SetReelSize(99);
    Require(reel::CurrentReelSize() == reel::vi::kReelSizeDefault, "an out-of-range step is never current");
    reel::SetLanguage(reel::Language::English);
    data.Use(L"profile-a");
}
}

int main() {
    try {
        TestIdentity();
        TestStorage();
        TestPreferences();
        std::cout << "Behavior tests passed: " << checks << " checks\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Behavior test failed: " << error.what() << '\n';
        return 1;
    }
}
