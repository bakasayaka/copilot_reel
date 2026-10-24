#include "installed_apps.h"
#include <winrt/base.h>
#include <cstdlib>
#include <iostream>
#include <tuple>

int main() {
    using namespace reel;
    unsigned checks=0;
    const auto check=[&](bool okay,const char* label){++checks;if(!okay){std::cerr<<"FAIL: "<<label<<'\n';std::exit(1);}};

    // Start entries as shell:AppsFolder reports them: display name, parsing name, link target, link arguments.
    const auto package=StartEntryApp(L"Notepad",L"Microsoft.WindowsNotepad_8wekyb3d8bbwe!App",L"",L"");
    check(package && package->packaged && package->target.kind==TargetKind::Aumid &&
          package->target.target==L"Microsoft.WindowsNotepad_8wekyb3d8bbwe!App" && package->target.name==L"Notepad",
          "a package becomes its AUMID");
    check(IsLocalApplication(package->target),"a package is a local application for the short tap");
    const auto sparse=StartEntryApp(L"Quick Share",L"NearbyShare_21hpf16v5xp10!NearbyShare",L"C:\\Program Files\\Google\\NearbyShare\\nearby_share.exe",L"");
    check(sparse && sparse->packaged && sparse->target.kind==TargetKind::Aumid,"a package with a link target still activates as a package");

    const auto edge=StartEntryApp(L"Microsoft Edge",L"MSEdge",L"C:\\Program Files (x86)\\Microsoft\\Edge\\Application\\msedge.exe",
        L"--source-shortcut-location=start-menu");
    check(edge && !edge->packaged && edge->target.kind==TargetKind::Shell && edge->item==L"MSEdge" &&
          edge->target.target==L"C:\\Program Files (x86)\\Microsoft\\Edge\\Application\\msedge.exe" &&
          edge->target.arguments==L"--source-shortcut-location=start-menu" && edge->target.workingDirectory.empty(),
          "a shortcut becomes its executable with the shortcut's arguments");
    check(IsLocalApplication(edge->target),"a shortcut's executable is a local application for the short tap");
    const auto knownFolder=StartEntryApp(L"Steam",L"{7C5A40EF-A0FB-4BFC-874A-C0F2E0B9FA8E}\\Steam\\Steam.exe",L"C:\\Program Files (x86)\\Steam\\steam.exe",L"");
    check(knownFolder && knownFolder->target.target==L"C:\\Program Files (x86)\\Steam\\steam.exe","a known-folder parsing name uses the link target");

    for(const auto& [name,item,target]:{
            std::tuple{L"Git FAQs",L"https://gitforwindows.org/faq",L"https://gitforwindows.org/faq"},
            std::tuple{L"War Thunder",L"steam://rungameid/236390",L"steam://rungameid/236390"},
            std::tuple{L"Release Notes",L"{6D809377-6AF0-444B-8957-A3773F02200E}\\Git\\ReleaseNotes.html",L"C:\\Program Files\\Git\\ReleaseNotes.html"},
            std::tuple{L"Services",L"{1AC14E77-02E7-4E5D-B744-2EB1AE5198B7}\\services.msc",L"C:\\Windows\\system32\\services.msc"},
            std::tuple{L"File Explorer",L"Microsoft.Windows.Explorer",L"::{52205FD8-5DFB-447D-801A-D0B52F2E83E1}"},
            std::tuple{L"Uninstall",L"Vendor.Uninstall",L"C:\\Program Files\\Vendor\\unins000.exe"},
            std::tuple{L"Uninstall Tool",L"Vendor.Remove",L"C:\\Program Files\\Vendor\\Uninstall Tool.exe"},
            std::tuple{L"Relative",L"Vendor.Relative",L"tool.exe"},
            std::tuple{L"",L"Vendor.App",L"C:\\Program Files\\Vendor\\app.exe"}})
        check(!StartEntryApp(name,item,target,L""),"websites, documents, consoles, namespaces, uninstallers and nameless entries are left out");
    check(!StartEntryApp(L"Packaged",L"C:\\Folder\\Name!x.exe",L"",L""),"a path with an exclamation mark is not an AUMID");

    check(MatchesQuery(L"Microsoft Edge",L""),"an empty search matches everything");
    check(MatchesQuery(L"Microsoft Edge",L"EDGE"),"search ignores case");
    check(MatchesQuery(L"Microsoft Edge",L"soft  edge "),"every word must occur, in any position");
    check(!MatchesQuery(L"Microsoft Edge",L"edge chrome"),"a missing word excludes the app");
    check(MatchesQuery(L"微信",L"微"),"Chinese names match by character");

    // This machine's Start entries: whatever is installed, every listed app passes the tap's own verifier.
    winrt::init_apartment(winrt::apartment_type::single_threaded);
    const auto apps=InstalledApps();
    for(const auto& app:apps) {
        check(IsVerifiedLocalApplication(app.target),"every listed app is a verified local application");
        check(!app.target.name.empty() && !app.item.empty(),"every listed app has a name and a Start item");
    }
    std::wcout<<L"Installed apps tests passed: "<<checks<<L" checks; "<<apps.size()<<L" Start apps listed.\n";
    return 0;
}
