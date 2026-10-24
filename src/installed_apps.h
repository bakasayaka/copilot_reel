#pragma once
#include "app_types.h"
#include "local_launch.h"
#include <algorithm>
#include <cwctype>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Start's All apps (shell:AppsFolder) as the editor's app list: the applications Windows
// has registered, so a user picks one instead of typing a path or an AUMID.
namespace reel {
struct InstalledApp {
    AppTarget target;   // what the reel stores: an AUMID, or the shortcut's executable and arguments
    std::wstring item;  // the entry's AppsFolder parsing name, which shows its Start icon
    bool packaged=false;
};

// The reel entry a Start entry becomes, from its display name, parsing name and shortcut
// destination. Only targets the short tap accepts as local applications qualify: a package
// (its parsing name is an AUMID) or a shortcut to an absolute .exe. Websites, documents,
// MMC consoles, shell namespaces such as File Explorer, and uninstallers are left out.
inline std::optional<InstalledApp> StartEntryApp(std::wstring_view name,std::wstring_view item,
    std::wstring_view linkTarget,std::wstring_view linkArguments) {
    if(name.empty() || item.empty())return {};
    if(item.find(L'!')!=item.npos && item.find_first_of(L":\\/")==item.npos)
        return InstalledApp{{std::wstring(name),TargetKind::Aumid,std::wstring(item),{},{}},std::wstring(item),true};
    const auto target=LaunchIdentity(std::wstring(linkTarget));
    if(target.size()<4 || !std::iswalpha(target[0]) || target[1]!=L':' || (target[2]!=L'\\' && target[2]!=L'/') ||
       !target.ends_with(L".exe"))return {};
    const auto file=target.substr(target.find_last_of(L"\\/")+1);
    if(file.starts_with(L"unins") || file.starts_with(L"uninstall"))return {};
    return InstalledApp{{std::wstring(name),TargetKind::Shell,std::wstring(linkTarget),std::wstring(linkArguments),{}},
        std::wstring(item),false};
}

// Every space-separated word of `query` occurs in `name`, ignoring case.
inline bool MatchesQuery(std::wstring_view name,std::wstring_view query) {
    const auto haystack=LaunchIdentity(std::wstring(name)),needles=LaunchIdentity(std::wstring(query));
    for(size_t start=0;start<needles.size();) {
        const auto end=std::min(needles.find(L' ',start),needles.size());
        if(end>start && haystack.find(needles.substr(start,end-start))==haystack.npos)return false;
        start=end+1;
    }
    return true;
}

// Start's All apps that StartEntryApp accepts and whose executable exists, one per target
// and arguments, sorted by name for the user's locale. Reads the Shell and the disk, so it
// runs off the UI thread. Requires COM.
std::vector<InstalledApp> InstalledApps();

// The entry's Start icon, `size` pixels square, as premultiplied BGRA; empty when the Shell
// has none. Requires COM.
std::vector<BYTE> InstalledAppIcon(const std::wstring& item,UINT size);
}
