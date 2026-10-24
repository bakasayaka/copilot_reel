#include "app_types.h"
#include "appearance.h"
#include "core.h"
#include "renderer.h"
#include "settings.h"
#include "localization.h"
#include "osd.h"
#include "behavior.h"
#include "presets.h"
#include "official_icons.h"
#include "artwork.h"
#include "radial_menu.h"
#include "local_launch.h"
#include "display_names.h"
#include "test_input.h"
#include "tray_menu.h"
#include "vi_tokens.h"
#include <windowsx.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <propsys.h>
#include <sddl.h>
#include <wtsapi32.h>
#include <commctrl.h>
#include <commdlg.h>
#include <imm.h>
#include <dwmapi.h>
#include <appmodel.h>
#include <tlhelp32.h>
#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.ApplicationModel.Activation.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <deque>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <thread>
#include <variant>

namespace reel {
namespace {
constexpr UINT kSmokeTimer=101, kIdleTimer=102, kCloseTimer=103, kKeyPollTimer=104;
constexpr UINT kLaunchFailedMessage=WM_APP+21, kPresetsReadyMessage=WM_APP+22, kIdleArtworkMessage=WM_APP+23;
constexpr UINT kCommandPreview=201,kCommandSettings=202,kCommandDiagnostics=203,kCommandExit=204;
constexpr ULONG_PTR kIpcMagic=0x5245454c;
constexpr PROPERTYKEY kFastPathKey={{0x38652bca,0x4329,0x4e74,{0x86,0xf9,0x39,0xcf,0x29,0x34,0x5e,0xea}},2};

struct CommandPacket { unsigned version=1; Command command=Command::Invalid; uint64_t stamp=0; POINT pointer{}; };
uint64_t EventStamp(bool processCreation=false) {
    FILETIME time{},exit{},kernel{},user{};
    if(!processCreation || !GetProcessTimes(GetCurrentProcess(),&time,&exit,&kernel,&user))
        GetSystemTimePreciseAsFileTime(&time);
    return (static_cast<uint64_t>(time.dwHighDateTime)<<32)|time.dwLowDateTime;
}
// The reel draws its DIP geometry at vi::kReelScale times the user's reel size on every display:
// one whole effective DPI keeps placement, hit testing, surfaces and artwork on the same
// device-pixel grid, and the edge clamp and the work-area shrink see the size as it is drawn.
double ReelScale() {return vi::kReelScale*vi::kReelSizeSteps[CurrentReelSize()];}
UINT ReelDpi(UINT monitorDpi) {return (std::max)(1u,static_cast<UINT>(std::lround(monitorDpi*ReelScale())));}
CommandPacket CaptureCommand(Command command,bool cold=false) {
    CommandPacket result;result.command=command;result.stamp=EventStamp(cold);result.pointer=CursorPosition();return result;
}

std::wstring DataDirectory() {
    const auto path=std::filesystem::path(ConfigPath()).parent_path();
    if(path.empty())throw std::runtime_error("The application data path is unavailable");
    std::filesystem::create_directories(path);return path.wstring();
}
void Log(const std::wstring& value) {
    try {
        SYSTEMTIME time{}; GetLocalTime(&time);
        wchar_t prefix[80]{}; swprintf_s(prefix,L"%04u-%02u-%02u %02u:%02u:%02u.%03u ",time.wYear,time.wMonth,time.wDay,time.wHour,time.wMinute,time.wSecond,time.wMilliseconds);
        const auto text=winrt::to_string(std::wstring(prefix)+value+L"\r\n");
        HANDLE file=CreateFileW((DataDirectory()+L"\\run.log").c_str(),FILE_APPEND_DATA,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(file!=INVALID_HANDLE_VALUE) { DWORD written=0; WriteFile(file,text.data(),static_cast<DWORD>(text.size()),&written,nullptr); CloseHandle(file); }
    } catch(...) {}
}
std::wstring UserSuffix() {
    HANDLE token=nullptr;
    if(!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token)) return L"CurrentUser";
    DWORD bytes=0; GetTokenInformation(token,TokenUser,nullptr,0,&bytes);
    std::vector<BYTE> buffer(bytes);
    std::wstring result=L"CurrentUser";
    if(GetTokenInformation(token,TokenUser,buffer.data(),bytes,&bytes)) {
        LPWSTR sid=nullptr;
        if(ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid,&sid)) {result=sid;LocalFree(sid);}
    }
    CloseHandle(token); return result;
}

Command CommandLine() {
    int count=0; auto args=CommandLineToArgvW(GetCommandLineW(),&count);
    Command result=Command::Preview;
    if(count==2) {
        const std::wstring_view arg=args[1];
        if(arg==L"--background") result=Command::Background;
        else if(arg==L"--preview") result=Command::Preview;
        else if(arg==L"--tap") result=Command::Tap;
        else if(arg==L"--settings") result=Command::Settings;
        else if(arg==L"--hold-start") result=Command::HoldStart;
        else if(arg==L"--hold-stop") result=Command::HoldStop;
        else if(arg==L"--cancel") result=Command::Cancel;
        else if(arg==L"--quit") result=Command::Quit;
        else if(arg==L"--diagnostics") result=Command::Diagnostics;
        else if(arg==L"--smoke-test") result=Command::SmokeTest;
        else result=Command::Invalid;
    } else if(count==3 && std::wstring_view(args[1])==L"--copilot-uri") result=ParseUri(args[2]);
    else if(count!=1) result=Command::Invalid;
    LocalFree(args); return result;
}
bool BareLaunch() {
    int count=0; auto args=CommandLineToArgvW(GetCommandLineW(),&count);
    LocalFree(args); return count==1;
}
bool DisableStartupTaskCommand() {
    int count=0; auto args=CommandLineToArgvW(GetCommandLineW(),&count);
    const bool result=count==2 && std::wstring_view(args[1])==L"--disable-startup-task";
    LocalFree(args); return result;
}
// The startup task's state lives outside the package data, so removing and reinstalling the
// package turns it on again. An app may turn its own task off (only the user can turn it on),
// which restores the user's choice; the packaged identity is required.
bool DisableStartupTask() {
    bool disabled=false;
    std::thread([&disabled] {
        try {
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
            const auto task=winrt::Windows::ApplicationModel::StartupTask::GetAsync(L"CopilotReelStartup").get();
            task.Disable();
            disabled=task.State()==winrt::Windows::ApplicationModel::StartupTaskState::Disabled;
            winrt::uninit_apartment();
        } catch(...) {}
    }).join();
    Log(disabled?L"Startup task turned off":L"Startup task could not be turned off");
    return disabled;
}

// The Copilot key is Win+Shift+F23; while it is held, F23 or Win+Shift stay down.
bool CopilotKeyHeld() {
    const auto down=[](int key){return (GetAsyncKeyState(key)&0x8000)!=0;};
    return down(VK_F23) || ((down(VK_LWIN) || down(VK_RWIN)) && down(VK_SHIFT));
}
std::wstring CopilotKeyState() {
    const auto down=[](int key){return (GetAsyncKeyState(key)&0x8000)!=0;};
    return std::wstring(L"F23=")+(down(VK_F23)?L"down":L"up")+L",Win="+(down(VK_LWIN) || down(VK_RWIN)?L"down":L"up")+
        L",Shift="+(down(VK_SHIFT)?L"down":L"up");
}
std::wstring ProcessImage(DWORD pid) {
    HANDLE process=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid);
    if(!process)return {};
    std::wstring path(32768,L'\0');DWORD size=static_cast<DWORD>(path.size());
    if(!QueryFullProcessImageNameW(process,0,path.data(),&size))size=0;
    CloseHandle(process);path.resize(size);return path;
}
std::wstring ParentImage() {
    DWORD parent=0;
    HANDLE snapshot=CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0);
    if(snapshot!=INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W entry{sizeof(entry)};
        for(BOOL ok=Process32FirstW(snapshot,&entry);ok;ok=Process32NextW(snapshot,&entry))
            if(entry.th32ProcessID==GetCurrentProcessId()) {parent=entry.th32ParentProcessID;break;}
        CloseHandle(snapshot);
    }
    const auto image=parent?ProcessImage(parent):std::wstring();
    return image.empty()?L"unknown":std::filesystem::path(image).filename().wstring();
}
// The window that had the foreground when this process started, as "<exe>:<class>". A start
// from Start or search leaves one of those in front; the Shell's start for a key
// press leaves the user's own app there.
std::wstring WindowOwner(HWND window) {
    if(!window)return L"none";
    wchar_t name[128]{};GetClassNameW(window,name,128);
    DWORD pid=0;GetWindowThreadProcessId(window,&pid);
    const auto image=ProcessImage(pid);
    return (image.empty()?std::wstring(L"unknown"):std::filesystem::path(image).filename().wstring())+L":"+name;
}
// The taskbar is not one of them: it keeps the foreground after Win+T, Win+B or a click on a
// taskbar button, and a key tap made then must still open the preferred agent.
bool ShellLauncherWindow(HWND window) {
    if(!window)return false;
    DWORD pid=0;GetWindowThreadProcessId(window,&pid);
    const auto file=LaunchIdentity(std::filesystem::path(ProcessImage(pid)).filename().wstring());
    return file==L"startmenuexperiencehost.exe" || file==L"searchhost.exe" || file==L"searchapp.exe";
}
// Packaged activations only; an unpackaged process has no activation arguments.
winrt::Windows::ApplicationModel::Activation::ActivationKind ActivatedKind(bool& known) {
    known=false;
    try {
        if(const auto args=winrt::Windows::ApplicationModel::AppInstance::GetActivatedEventArgs()) {known=true;return args.Kind();}
    } catch(...) {}
    return winrt::Windows::ApplicationModel::Activation::ActivationKind::Launch;
}

bool RegisterFastPath(HWND hwnd) {
    winrt::com_ptr<IPropertyStore> store;
    HRESULT hr=SHGetPropertyStoreForWindow(hwnd,IID_PPV_ARGS(store.put()));
    if(FAILED(hr)) return false;
    PROPVARIANT value{};value.vt=VT_UINT;value.uintVal=kCopilotMessage;
    hr=store->SetValue(kFastPathKey,value);
    return SUCCEEDED(hr) && SUCCEEDED(store->Commit());
}

// Shell activation can block for a cold start. It runs on any COM STA thread and has no UI.
HRESULT Activate(const AppTarget& target) {
    HRESULT hr=S_OK;
    if(target.kind==TargetKind::Aumid) {
        winrt::com_ptr<IApplicationActivationManager> manager;
        hr=CoCreateInstance(CLSID_ApplicationActivationManager,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(manager.put()));
        DWORD pid=0;
        if(SUCCEEDED(hr)) hr=manager->ActivateApplication(target.target.c_str(),target.arguments.c_str(),AO_NONE,&pid);
    } else {
        SHELLEXECUTEINFOW info{sizeof(info)};
        info.fMask=SEE_MASK_FLAG_NO_UI|SEE_MASK_NOASYNC;
        info.lpVerb=L"open";info.lpFile=target.target.c_str();
        info.lpParameters=target.arguments.empty()?nullptr:target.arguments.c_str();
        info.lpDirectory=target.workingDirectory.empty()?nullptr:target.workingDirectory.c_str();
        info.nShow=SW_SHOWNORMAL;
        if(!ShellExecuteExW(&info)) hr=HRESULT_FROM_WIN32(GetLastError());
    }
    return hr;
}
void ReportLaunchFailure(const AppTarget& target,HRESULT hr) {
    wchar_t number[40]{};swprintf_s(number,L"0x%08X",static_cast<unsigned>(hr));
    Log(L"Launch failed "+std::wstring(number));
    MessageBoxW(nullptr,(std::wstring(Tr(L"Could not open: ",L"无法打开："))+AppDisplayName(target)+Tr(L"\nCheck the target and arguments in App settings.\n\n",L"\n请检查应用设置中的目标与参数。\n\n")+number).c_str(),kAppName,MB_OK|MB_ICONERROR);
}
struct LaunchFailure { AppTarget target; HRESULT hr=S_OK; };
std::atomic<int> launchesInFlight{0};
// Discovery workers read static tables that exit() destroys; exit waits for them too.
std::atomic<int> workersInFlight{0};
// One short-lived STA worker per commit, so the reel's exit is never held by Shell activation.
// Failures return to the UI thread, which owns the message box and the log order.
void Launch(const AppTarget& target,HWND owner) {
    Log(L"Launching target: "+target.name);
    // The reel never takes the foreground, so a launched process cannot raise a window
    // on its own, and a running target's existing instance just flashes in the taskbar.
    // Pass on the foreground right the Copilot key event gave this process.
    if(!AllowSetForegroundWindow(ASFW_ANY))Log(L"Foreground grant unavailable: "+std::to_wstring(GetLastError()));
    ++launchesInFlight;
    try {
        std::thread([target,owner] {
            try {
                const HRESULT apartment=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED|COINIT_DISABLE_OLE1DDE);
                const HRESULT hr=Activate(target);
                if(SUCCEEDED(apartment))CoUninitialize();
                if(FAILED(hr)) {
                    auto failure=std::make_unique<LaunchFailure>(LaunchFailure{target,hr});
                    if(PostMessageW(owner,kLaunchFailedMessage,0,reinterpret_cast<LPARAM>(failure.get())))failure.release();
                }
            } catch(...) {}
            --launchesInFlight;
        }).detach();
    } catch(...) {
        // No worker thread: activate inline rather than drop the commit.
        --launchesInFlight;
        const HRESULT hr=Activate(target);
        if(FAILED(hr))ReportLaunchFailure(target,hr);
    }
}
void WaitForLaunches() {
    for(int i=0;i<500 && (launchesInFlight.load() || workersInFlight.load());++i)Sleep(10);
}
std::wstring ProcessAumid(DWORD pid) {
    HANDLE process=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid);
    if(!process)return {};
    std::wstring aumid(APPLICATION_USER_MODEL_ID_MAX_LENGTH,L'\0');UINT32 length=static_cast<UINT32>(aumid.size());
    if(GetApplicationUserModelId(process,&length,aumid.data())!=ERROR_SUCCESS)length=1;
    CloseHandle(process);aumid.resize(length-1);return aumid;
}
std::map<DWORD,DWORD> ParentProcesses() {
    std::map<DWORD,DWORD> parents;
    HANDLE snapshot=CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0);
    if(snapshot==INVALID_HANDLE_VALUE)return parents;
    PROCESSENTRY32W entry{sizeof(entry)};
    for(BOOL ok=Process32FirstW(snapshot,&entry);ok;ok=Process32NextW(snapshot,&entry))parents[entry.th32ProcessID]=entry.th32ParentProcessID;
    CloseHandle(snapshot);return parents;
}
// An agent that is already running comes forward instead of starting again: VS Code forks
// open an empty window for every start without arguments, and an Electron single-instance
// handoff only flashes the taskbar once the foreground right has lapsed. Runs on the UI
// thread while the reel still holds the right the Copilot key gave it.
// A window stands for the target when its process has the target's AUMID (a package), is the
// target's executable, or is a same-named child of it: a launcher stub such as Claude
// Science's keeps running while the app runs from another folder. A shortcut with arguments
// of its own (a profile, a project) is always started.
bool ActivateRunning(const AppTarget& target) {
    // A working directory set on the entry is a deliberate session (a CLI agent in one folder).
    if(!target.arguments.empty() || !target.workingDirectory.empty())return false;
    const auto identity=InstalledIdentity(target);
    struct Search {
        std::wstring aumid,executable;std::map<DWORD,DWORD> parents;std::map<DWORD,bool> matches;HWND found=nullptr;
        bool Matches(DWORD pid) {
            if(!aumid.empty())return LaunchIdentity(ProcessAumid(pid))==aumid;
            const auto image=LaunchIdentity(ProcessImage(pid));
            if(image==executable)return true;
            if(image.empty() || std::filesystem::path(image).filename()!=std::filesystem::path(executable).filename())return false;
            const auto parent=parents.find(pid);
            for(DWORD ancestor=parent==parents.end()?0:parent->second,depth=0;ancestor && depth<2;++depth) {
                if(LaunchIdentity(ProcessImage(ancestor))==executable)return true;
                const auto next=parents.find(ancestor);ancestor=next==parents.end()?0:next->second;
            }
            return false;
        }
    } search;
    if(identity.starts_with(L"aumid:"))search.aumid=identity.substr(6);
    else if(identity.starts_with(L"file:") && identity.find(L'\\')!=std::wstring::npos) {
        if(LaunchIdentity(target.target).ends_with(L".lnk") && !ShortcutArguments(target.target).empty())return false;
        search.executable=identity.substr(5);search.parents=ParentProcesses();
    } else return false;
    // EnumWindows walks top-level windows in z-order, so the first match is the most recent window.
    EnumWindows([](HWND hwnd,LPARAM data)->BOOL {
        auto& s=*reinterpret_cast<Search*>(data);
        if(!IsWindowVisible(hwnd) || GetWindow(hwnd,GW_OWNER) || (GetWindowLongPtrW(hwnd,GWL_EXSTYLE)&WS_EX_TOOLWINDOW))return TRUE;
        BOOL cloaked=FALSE;
        if(SUCCEEDED(DwmGetWindowAttribute(hwnd,DWMWA_CLOAKED,&cloaked,sizeof(cloaked))) && cloaked)return TRUE;
        DWORD pid=0;GetWindowThreadProcessId(hwnd,&pid);
        if(pid==GetCurrentProcessId())return TRUE;
        auto [match,inserted]=s.matches.try_emplace(pid,false);
        if(inserted)match->second=s.Matches(pid);
        if(!match->second)return TRUE;
        s.found=hwnd;return FALSE;
    },reinterpret_cast<LPARAM>(&search));
    if(!search.found || IsHungAppWindow(search.found))return false;
    if(IsIconic(search.found))ShowWindowAsync(search.found,SW_RESTORE);
    const bool raised=SetForegroundWindow(search.found) && GetForegroundWindow()==search.found;
    if(!raised)SwitchToThisWindow(search.found,TRUE);
    Log(L"Activated running target: "+target.name+(raised?L"":L" (switched; foreground refused)"));
    return true;
}

class Controller {
public:
    // A press on a Manage tile (from, screen pixels) becomes a drag (active) past the drag threshold.
    struct Drag { int from=-1; POINT press{}; bool active=false; int slot=-1; bool overTrash=false; };
    HWND window=nullptr;
    bool smoke=false;
    explicit Controller(bool isSmoke):smoke(isSmoke) {}
    ~Controller() {
        appearance_.reset(); renderer_.reset();
        if(window) { WTSUnRegisterSessionNotification(window); RemoveTray(); }
    }
    bool Initialize() {
        // The reel accepts commands, not text. Leave the separate native editor's
        // IME intact while ensuring A/D also work under Chinese input layouts.
        ImmAssociateContextEx(window,nullptr,0);
        appearance_=std::make_unique<AppearanceSource>(window);
        renderer_=std::make_unique<OrbitRenderer>(window);
        std::wstring error;
        if(!renderer_->Initialize(error)) {
            Log(L"Renderer initialization failed: "+error);
            MessageBoxW(nullptr,(std::wstring(Tr(L"Could not initialize native Windows rendering.\n",L"无法初始化 Windows 原生绘制。\n"))+error).c_str(),kAppName,MB_OK|MB_ICONERROR); return false;
        }
        WTSRegisterSessionNotification(window,NOTIFY_FOR_THIS_SESSION);
        if(!CurrentVisualOverride().raw.empty())Log(L"Visual override: "+CurrentVisualOverride().raw);
        renderer_->SetLanguage(CurrentLanguage());renderer_->SetReelSize(ReelSizePercent(CurrentReelSize()));
        PreloadOfficialIconAssets();
        RefreshAppearance(); Reload();
        // A cold start serves a Copilot key press; discovery waits on a worker unless Reload needed it.
        RefreshPresets();
        if(CurrentTestInput().active) {
            // A test instance is invisible to the Shell, the tray and the user's hotkey.
            ready_=true;const auto pointer=CursorPosition();
            Log(L"Test input seam active: run="+CurrentTestInput().runId+L"; pointer="+std::to_wstring(pointer.x)+L","+std::to_wstring(pointer.y));
            return true;
        }
        AddTray();
        // This ordinary optional shortcut opens click-to-select preview; it never intercepts Copilot.
        hotkey_=RegisterHotKey(window,1,MOD_CONTROL|MOD_ALT|MOD_NOREPEAT,'R')!=FALSE;
        ready_=true;
        RegisterFastPath(window);
        Log(L"Ready; fast-path HWND registered; Ctrl+Alt+R "+std::wstring(hotkey_?L"available":L"unavailable"));
        return true;
    }
    void CommandReceived(Command command) { CommandReceived(CaptureCommand(command)); }
    // Commands and test input share one queue, so a test click never overtakes a queued release.
    bool QueueCommand(const std::variant<CommandPacket,TestPacket>& event) {
        pending_.push_back(event);
        if(PostMessageW(window,WM_APP+20,0,0))return true;
        pending_.pop_back();return false;
    }
    void DispatchQueued() {
        if(pending_.empty())return;
        const auto event=pending_.front();pending_.pop_front();
        if(const auto* command=std::get_if<CommandPacket>(&event))CommandReceived(*command);
        else TestInputReceived(std::get<TestPacket>(event));
    }
    // Synthetic pointer, click and key input for a test instance; it follows the real input paths.
    void TestInputReceived(const TestPacket& input) {
        const auto through=[&](POINT screen) {
            SetSyntheticCursor(screen);
            if(HitTestAt(screen)==HTCLIENT)return true;
            Log(L"Test input "+std::to_wstring(input.sequence)+L" passed through the reel");return false;
        };
        POINT client=input.screen;ScreenToClient(window,&client);
        switch(input.action) {
        case TestAction::Pointer:SetSyntheticCursor(input.screen);MovePointer();break;
        case TestAction::Click:
            if(through(input.screen)) {MovePointer();Press(client);Click(client);}
            break;
        case TestAction::Press:if(through(input.screen)) {MovePointer();Press(client);}break;
        case TestAction::Release:
            // A press on a Manage tile holds the capture, so its release arrives wherever the pointer is.
            if(drag_.from>=0) {SetSyntheticCursor(input.screen);MovePointer();Click(client);}
            else if(through(input.screen)) {MovePointer();Click(client);}
            break;
        case TestAction::RightClick:if(through(input.screen))Back();break;
        case TestAction::Key:
            if(input.vk==VK_ESCAPE && gesture_.Active() && !input_.Persistent())Cancel();else KeyDown(input.vk);
            break;
        default:break;
        }
        Log(L"Test input "+std::to_wstring(input.sequence)+L" done; page="+std::to_wstring(static_cast<int>(page_))+
            L"; selected="+std::to_wstring(gesture_.Selected())+L"; active="+(gesture_.Active()?L"1":L"0")+
            L"; drag="+std::to_wstring(drag_.active?drag_.from:-1)+L"; target="+DragTarget());
    }
    void RealInputIgnored() {++realInputIgnored_;}
    // Nothing reaches the reel behind an editor or picker, and the Copilot key waits while a tile is dragged.
    ReelContext Context() const {return {gesture_.Active(),editorOpen_ || drag_.active};}
    void CommandReceived(const CommandPacket& event) {
        switch(event.command) {
        case Command::Background: break;
        case Command::Settings: Cancel();Start(true,event.pointer,MenuPage::Settings,event.stamp);break;
        case Command::Preview: Start(true,event.pointer,MenuPage::Main,event.stamp);break;
        case Command::Tap:
            switch(input_.Tap(event.stamp,Context())) {
            case CopilotRoute::LaunchPreferred:LaunchPreferred();break;
            // Posted messages overtake queued mouse input: select from where the cursor is now.
            case CopilotRoute::Confirm:MovePointer();Confirm(gesture_.Selected());break;
            default:if(gesture_.Active() || editorOpen_)Log(L"Copilot tap ignored");break;
            }break;
        case Command::HoldStart:case Command::KeyHold: {
            const bool poll=event.command==Command::KeyHold;
            // The key was down when the process started but is up before the reel could open:
            // nothing could be selected, so the press was a tap.
            if(poll && !CurrentTestInput().active && !CopilotKeyHeld()) {
                Log(L"Copilot key released before the reel opened; taken as a tap");
                CommandPacket tap=event;tap.command=Command::Tap;CommandReceived(tap);break;
            }
            switch(input_.HoldStart(event.stamp,Context())) {
            case CopilotRoute::Open:if(Start(false,event.pointer,MenuPage::Main,event.stamp) && poll)WatchCopilotKey();break;
            case CopilotRoute::Resume:Log(L"Hold resumed");KeepAlive();if(poll)WatchCopilotKey();break;
            default:break;
            }break;
        }
        case Command::HoldStop:
            switch(input_.HoldStop(event.stamp,Context())) {
            case CopilotRoute::Release:Release();break;
            case CopilotRoute::Confirm:MovePointer();Confirm(gesture_.Selected());break;
            default:break;
            }break;
        case Command::Cancel: Cancel();break;
        case Command::Quit:
            quitting_=true;
            if(CloseOsd())DestroyWindow(window);else quitting_=false;break;
        case Command::Diagnostics: SaveDiagnostics();break;
        case Command::SmokeTest: BeginSmoke();break;
        default: break;
        }
    }
    void LaunchPreferred() {
        const int preferred=PreferredTargetIndex(items_);
        if(preferred<0)return;
        const auto& item=items_[preferred];
        if(IsVerifiedLocalApplication(item))LaunchTarget(item);
        // A saved executable can be missing for a moment while its updater swaps it. That is not a
        // website profile: try it and report a failure rather than asking to replace the profile.
        else if(item.kind==TargetKind::Shell && LaunchIdentity(item.target).ends_with(L".exe") &&
                std::filesystem::path(item.target).is_absolute())LaunchTarget(item);
        else ChooseLocalApplication(preferred,true);
    }
    // A cold Shell activation for a held Copilot key has no release message: poll the key.
    void WatchCopilotKey() {if(input_.Held())SetTimer(window,kKeyPollTimer,15,nullptr);}
    void PollCopilotKey() {
        if(!input_.Held()) {KillTimer(window,kKeyPollTimer);return;}
        if(CopilotKeyHeld())return;
        KillTimer(window,kKeyPollTimer);CommandReceived(CaptureCommand(Command::HoldStop));
    }
    void KeepAlive() {SetTimer(window,kIdleTimer,60000,nullptr);}
    // The effective DPI, canvas size and position of a reel opened at `pointer`, at the
    // current reel size. The canvas always stays inside the monitor's work area.
    bool PlaceReel(POINT pointer,UINT& dpi,int& size,WheelPlacement& placement) {
        const HMONITOR monitor=MonitorFromPoint(pointer,MONITOR_DEFAULTTONEAREST);
        dpi=CurrentVisualOverride().dpi;
        if(!dpi) {
            UINT xDpi=96,yDpi=96;
            using GetDpiForMonitorFn=HRESULT(WINAPI*)(HMONITOR,int,UINT*,UINT*);
            HMODULE shcore=LoadLibraryExW(L"shcore.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
            if(shcore) {
                auto getDpi=reinterpret_cast<GetDpiForMonitorFn>(GetProcAddress(shcore,"GetDpiForMonitor"));
                if(getDpi)getDpi(monitor,0,&xDpi,&yDpi);
                FreeLibrary(shcore);
            }
            dpi=xDpi;
        }
        MONITORINFO info{sizeof(info)};
        if(!GetMonitorInfoW(monitor,&info)) {Log(L"Opening skipped: monitor information unavailable");return false;}
        const auto& work=info.rcWork;
        dpi=ReelDpi(dpi);
        size=MulDiv(static_cast<int>(kCanvasDip),dpi,96);
        // A tiny work area reduces the complete canvas uniformly, never opens a
        // different settings dashboard and never places buttons off screen.
        if(size>work.right-work.left || size>work.bottom-work.top) {
            size=std::min(work.right-work.left,work.bottom-work.top);
            // Rounded down, so the reduced canvas never exceeds the work area.
            dpi=std::max(1,size*96/static_cast<int>(kCanvasDip));
            size=MulDiv(static_cast<int>(kCanvasDip),dpi,96);
        }
        placement=PlaceOrbit(pointer.x,pointer.y,{work.left,work.top,work.right,work.bottom},size,kRadiusDip*dpi/96.0);
        if(!placement.fits) {Log(L"Opening skipped: the work area cannot contain the reel");return false;}
        return true;
    }
    bool Start(bool preview,POINT pointer,MenuPage page=MenuPage::Main,uint64_t stamp=0) {
        if(gesture_.Active() || editorOpen_)return false;
        // A pending exit finishes first: no stale page, timer or half-faded root
        // survives into the new gesture, and an early return leaves the window hidden.
        const bool interrupted=closing_;
        if(closing_)FinishHide();
        UINT dpi=96;int size=0;WheelPlacement placement{};
        if(!PlaceReel(pointer,dpi,size,placement))return false;
        previousForeground_=GetForegroundWindow();
        page_=page;anchor_=pointer;dpi_=dpi;
        centerX_=placement.centerX;centerY_=placement.centerY;
        placing_=true;
        // A minimized window ignores the move and later shows at its old normal position.
        if(IsIconic(window) || IsZoomed(window)) {
            WINDOWPLACEMENT restore{sizeof(restore)};GetWindowPlacement(window,&restore);
            restore.flags=0;restore.showCmd=SW_HIDE;
            restore.rcNormalPosition={placement.left,placement.top,placement.left+size,placement.top+size};
            SetWindowPlacement(window,&restore);Log(L"Minimized reel window restored before placement");
        }
        SetWindowPos(window,HWND_TOPMOST,placement.left,placement.top,size,size,SWP_NOACTIVATE);
        placing_=false;
        input_.Opened(preview,stamp?stamp:EventStamp());
        gesture_.Start(!preview && placement.awaitingCenter);
        // The opening position itself selects nothing: an edge can leave the pointer over a tile,
        // and a release before the pointer moves must cancel, as it does at the center.
        pageCursor_=pointer;observed_=false;moved_=false;
        renderer_->Resize(dpi_);renderer_->SetAppearance(currentAppearance_);
        pageShown_=false;RenderMenu();
        ShowWindow(window,SW_SHOWNOACTIVATE);renderer_->Show();
        RegisterRawInput(true);
        if(preview)ActivateMenu();
        KeepAlive();
        RECT bounds{};GetWindowRect(window,&bounds);
        if(bounds.left!=placement.left || bounds.top!=placement.top) {
            Log(L"Placement corrected; window was at "+std::to_wstring(bounds.left)+L","+std::to_wstring(bounds.top));
            placing_=true;SetWindowPos(window,HWND_TOPMOST,placement.left,placement.top,size,size,SWP_NOACTIVATE);placing_=false;
            GetWindowRect(window,&bounds);
        }
        if(!(GetWindowLongPtrW(window,GWL_EXSTYLE)&WS_EX_TOPMOST))Log(L"Warning: the reel window is not topmost; another window can cover it");
        Log(std::wstring(preview?L"Preview opened":L"Hold started")+L"; pointer="+std::to_wstring(anchor_.x)+L","+std::to_wstring(anchor_.y)+L"; window="+std::to_wstring(bounds.left)+L","+std::to_wstring(bounds.top)+L","+std::to_wstring(bounds.right)+L","+std::to_wstring(bounds.bottom)+L"; awaitingCenter="+(gesture_.AwaitingCenter()?L"true":L"false")+(interrupted?L"; interrupted=exit":L""));
        // Add is reached from the hub and from Manage; discovery refreshes behind them.
        if(page==MenuPage::Settings || page==MenuPage::Manage)RefreshPresets();
        return true;
    }
    void ActivateMenu() {
        SetWindowLongPtrW(window,GWL_EXSTYLE,GetWindowLongPtrW(window,GWL_EXSTYLE)&~WS_EX_NOACTIVATE);
        // A test instance never takes the user's focus.
        if(CurrentTestInput().active) {activationRequested_=true;return;}
        SetForegroundWindow(window);SetFocus(window);
    }
    std::wstring MenuTitle() const {
        switch(page_) {
        case MenuPage::Manage:return Tr(L"Your apps",L"管理应用");
        case MenuPage::Presets:return Tr(L"Add an agent",L"添加应用");
        case MenuPage::Settings:return Tr(L"Settings",L"设置");
        case MenuPage::Entry:return editing_>=0 && static_cast<size_t>(editing_)<items_.size()?AppDisplayName(items_[editing_]):L"";
        default:return L"Copilot Reel";
        }
    }
    static int Depth(MenuPage page) {
        switch(page) {
        case MenuPage::Settings:return 1;
        case MenuPage::Manage:return 2;
        case MenuPage::Presets:case MenuPage::Entry:return 3;
        default:return 0;
        }
    }
    // One page model and one SetPage per change. Only a page already on screen
    // animates: deeper pages drill in from the chosen slot, shallower pages drill
    // out, and preset paging swaps. Language and reload re-render in place.
    // A drop or a trash on Manage reflows the saved order from where the tiles are (`reflow`).
    void RenderMenu(int origin=-1,bool reflow=false) {
        if(page_==MenuPage::Entry && (editing_<0 || static_cast<size_t>(editing_)>=items_.size()))page_=MenuPage::Manage;
        menu_=BuildMenu(page_,items_,addCatalog_,presetOffset_,editing_,addParked_);
        PageModel model;
        for(const auto& entry:menu_)model.items.push_back({entry.visual,entry.enabled});
        model.title=MenuTitle();model.isSubmenu=page_!=MenuPage::Main;
        const auto adjust=AdjustFor(page_);
        model.adjustControl=adjust.control;model.adjustEarlier=adjust.earlier;model.adjustLater=adjust.later;
        model.preferredIndex=page_==MenuPage::Main || page_==MenuPage::Manage?PreferredTargetIndex(items_):
            page_==MenuPage::Entry && PreferredTargetIndex(items_)==editing_?0:-1;
        model.selection=gesture_.Selected();model.awaitCenter=gesture_.AwaitingCenter();
        auto transition=PageTransition::None;
        if(reflow && pageShown_ && gesture_.Active() && shownPage_==page_) {
            transition=PageTransition::Reflow;
            if(reflowFrom_.size()==model.items.size())model.from=reflowFrom_;
        }
        else if(pageShown_ && gesture_.Active()) {
            const int from=Depth(shownPage_),to=Depth(page_);
            if(to>from)transition=PageTransition::DrillIn;
            else if(to<from)transition=PageTransition::DrillOut;
            else if(page_!=shownPage_ || presetOffset_!=shownOffset_)transition=PageTransition::Swap;
        }
        if(transition!=PageTransition::DrillIn || origin<0 || static_cast<size_t>(origin)>=shownCount_)origin=-1;
        renderer_->SetPage(model,transition,origin);
        reflowFrom_.clear();
        pageShown_=gesture_.Active();shownPage_=page_;shownOffset_=presetOffset_;shownCount_=menu_.size();
        Log(L"Radial menu: "+MenuTitle()+L"; entries="+std::to_wstring(menu_.size())+L"; transition="+TransitionName(transition));
    }
    // origin is the chosen slot on the current page, so its tile can hand off to the new center.
    void Navigate(MenuPage page,int origin=-1) {
        EndDrag(DragEnd::Cancel);
        page_=page;input_.PageShown(EventStamp());
        gesture_.Cancel();gesture_.Start();
        pageCursor_=CursorPosition();observed_=false;moved_=false;
        // Discovery refreshes behind the hub, so Add opens with what is installed now.
        if(page==MenuPage::Settings || page==MenuPage::Manage)RefreshPresets();
        if(page==MenuPage::Presets) {FlushArtwork();UpdateAddCatalog();}
        RenderMenu(origin);ActivateMenu();KeepAlive();
    }
    void Back() {
        if(!gesture_.Active())return;
        // Back during a drag puts the tile back and stays on the page.
        if(drag_.active) {EndDrag(DragEnd::Cancel);Log(L"Drag canceled");return;}
        switch(page_) {
        case MenuPage::Presets:Navigate(presetsParent_);break;
        case MenuPage::Entry:Navigate(MenuPage::Manage);break;
        case MenuPage::Manage:Navigate(MenuPage::Settings);break;
        case MenuPage::Main:Cancel();break;
        default:Navigate(MenuPage::Main);break;
        }
    }
    POINT ToLocal(POINT point) const {
        ScreenToClient(window,&point);
        point.x=MulDiv(point.x,96,dpi_);point.y=MulDiv(point.y,96,dpi_);return point;
    }
    // -1 on the earlier (left) chevron, +1 on the later (right) one, 0 elsewhere. The slot-0 seat
    // and its preferred badge reach into both hit boxes; a click there is on the tile.
    int AdjustArrow(POINT point) const {
        if(!AdjustFor(page_).control)return 0;
        const auto bounds=renderer_->AdjustArrowBounds();
        const float tile=vi::kBadgeOffset+vi::kBadgeRingRadius+.5f;
        if(std::abs(point.x-kCenterDip)<=tile && std::abs(point.y-(bounds.first.top+bounds.first.bottom)/2)<=tile)return 0;
        const auto inside=[&](const D2D1_RECT_F& r){return point.x>=r.left && point.x<=r.right && point.y>=r.top && point.y<=r.bottom;};
        return inside(bounds.first)?-1:inside(bounds.second)?1:0;
    }
    void MovePointer() {
        if(!gesture_.Active())return;
        const POINT screen=CursorPosition();
        // An unmoved cursor keeps a keyboard selection.
        if(observed_ && screen.x==lastPointer_.x && screen.y==lastPointer_.y)return;
        observed_=true;lastPointer_=screen;
        if(DragPointer(screen))return;
        if(!moved_) {
            // A page that just appeared ignores the cursor until it moves, so the release or
            // click that opened it cannot also select whatever lies beneath.
            const int slop=MulDiv(4,dpi_,96);
            if(std::abs(screen.x-pageCursor_.x)<=slop && std::abs(screen.y-pageCursor_.y)<=slop)return;
            moved_=true;
        }
        if(AdjustArrow(ToLocal(screen))) {
            // Both chevrons belong to slot 0, including its hover name.
            if(gesture_.Select(0,static_cast<unsigned>(menu_.size())))renderer_->SetSelection(0);
            return;
        }
        const bool wasAwaiting=gesture_.AwaitingCenter();
        if(gesture_.ObservePointer((screen.x-centerX_)*96.0/dpi_,(screen.y-centerY_)*96.0/dpi_,static_cast<unsigned>(menu_.size()))) {
            renderer_->SetAwaitCenter(gesture_.AwaitingCenter());renderer_->SetSelection(gesture_.Selected());
            if(wasAwaiting && !gesture_.AwaitingCenter())Log(L"Center entered; direction selection armed");
        }
    }
    void SwitchLanguage() {
        const auto language=CurrentLanguage()==Language::English?Language::SimplifiedChinese:Language::English;
        if(!SaveLanguage(language)) {Log(L"Language save failed");return;}
        SetLanguage(language);renderer_->SetLanguage(language);RenderMenu();UpdateTrayTip();RefreshOsdAppearance();
        Log(language==Language::English?L"Language changed: English":L"Language changed: Simplified Chinese");
    }
    // The hub's Reel size moves to the next step and wraps. The open reel is placed again around
    // the point it opened at, so the edges clamp the new size exactly as they clamp an opening,
    // and selection by direction from the plate center carries on unchanged.
    void NextReelSize() {
        const unsigned next=(CurrentReelSize()+1)%vi::kReelSizeCount;
        if(!SaveReelSize(next)) {Log(L"Reel size save failed");return;}
        SetReelSize(next);renderer_->SetReelSize(ReelSizePercent(next));
        UINT dpi=96;int size=0;WheelPlacement placement{};
        if(gesture_.Active() && PlaceReel(anchor_,dpi,size,placement)) {
            dpi_=dpi;centerX_=placement.centerX;centerY_=placement.centerY;
            placing_=true;SetWindowPos(window,HWND_TOPMOST,placement.left,placement.top,size,size,SWP_NOACTIVATE);placing_=false;
            renderer_->Resize(dpi_);
        }
        RenderMenu();
        Log(L"Reel size: "+std::to_wstring(ReelSizePercent(next))+L"%; DPI="+std::to_wstring(dpi_));
    }
    // The native editor changed and saved the language.
    void LanguageChanged() {
        renderer_->SetLanguage(CurrentLanguage());RenderMenu();UpdateTrayTip();
    }
    void KeyDown(WPARAM key) {
        if(!input_.Persistent() || !gesture_.Active())return;
        const int count=static_cast<int>(menu_.size()),selected=gesture_.Selected();
        const bool manage=page_==MenuPage::Manage;
        const auto route=RouteKey(static_cast<UINT>(key),GetKeyState(VK_SHIFT)<0,AdjustFor(page_).control,selected,manage);
        // While a tile is pressed or lifted only Back (Esc) does anything: it puts the tile back.
        if(drag_.from>=0 && route!=KeyRoute::Back)return;
        const bool app=manage && selected>=0 && static_cast<size_t>(selected)<items_.size();
        switch(route) {
        case KeyRoute::Back:Back();break;
        case KeyRoute::Confirm:Confirm(selected);break;
        case KeyRoute::Adjust:
            input_.EndHold();KeepAlive();
            // On Manage, A and D move the selected app one place.
            if(manage) {if(app)MoveApp(selected,selected+AdjustDirection(static_cast<UINT>(key)),false);break;}
            if(gesture_.Select(0,static_cast<unsigned>(count)))renderer_->SetSelection(0);
            Adjust(AdjustDirection(static_cast<UINT>(key)));break;
        case KeyRoute::Remove:
            input_.EndHold();KeepAlive();
            if(app && TakeOut(selected)) {gesture_.Select(-1,static_cast<unsigned>(count));RenderMenu(-1,true);}
            break;
        case KeyRoute::Next:case KeyRoute::Previous: {
            if(!count)break;
            const int delta=route==KeyRoute::Previous?-1:1;
            gesture_.Select(selected<0?(delta<0?count-1:0):(selected+delta+count)%count,static_cast<unsigned>(count));
            renderer_->SetSelection(gesture_.Selected());KeepAlive();break;
        }
        default:break;
        }
    }
    // A click only counts when its press began on the page now shown. On Manage, a press on an
    // app tile may become a drag once the pointer passes the system drag threshold.
    void Press(POINT client) {
        pressGeneration_=gesture_.Generation();
        EndDrag(DragEnd::Cancel);
        if(page_!=MenuPage::Manage || !input_.Persistent() || input_.Held() || !gesture_.Active())return;
        const int index=HitTest(client.x*96.0/dpi_-kCenterDip,client.y*96.0/dpi_-kCenterDip,static_cast<unsigned>(menu_.size()));
        if(index<0 || static_cast<size_t>(index)>=items_.size() || menu_[index].action!=MenuAction::Entry)return;
        POINT screen=client;ClientToScreen(window,&screen);
        drag_.from=index;drag_.press=screen;
        // The release must come back here even when the pointer leaves the reel. A test
        // instance never takes the user's mouse.
        if(!CurrentTestInput().active)SetCapture(window);
    }
    // Lifts the pressed tile past the drag threshold, then moves it: the pointer opens the slot
    // nearest its angle, or arms the trash inside the center zone. True while a tile is lifted.
    bool DragPointer(POINT screen) {
        if(drag_.from<0)return false;
        // The page changed, or the renderer lost the lifted tile (a device or appearance rebuild):
        // a drop must never act on a tile the user can no longer see.
        if(pressGeneration_!=gesture_.Generation() || page_!=MenuPage::Manage || static_cast<size_t>(drag_.from)>=items_.size() ||
           (drag_.active && !renderer_->Dragging())) {EndDrag(DragEnd::Cancel);return false;}
        if(!drag_.active) {
            // The drag threshold follows the display's own DPI, not the reel's smaller drawing DPI.
            const UINT monitorDpi=static_cast<UINT>(std::lround(dpi_/ReelScale()));
            const int cx=GetSystemMetricsForDpi(SM_CXDRAG,monitorDpi),cy=GetSystemMetricsForDpi(SM_CYDRAG,monitorDpi);
            if(std::abs(screen.x-drag_.press.x)<=cx && std::abs(screen.y-drag_.press.y)<=cy)return false;
            // A Copilot hold resumed during the press keeps the menu; the press stays a click.
            if(input_.Held()) {EndDrag(DragEnd::Cancel);return false;}
            drag_.active=true;moved_=true;
            gesture_.Select(-1,static_cast<unsigned>(menu_.size()));
            renderer_->BeginDrag(drag_.from,items_.size()>1);
            if(!renderer_->Dragging()) {EndDrag(DragEnd::Cancel);return false;}
            Log(L"Drag started: "+AppDisplayName(items_[drag_.from]));
        }
        const double x=(screen.x-centerX_)*96.0/dpi_,y=(screen.y-centerY_)*96.0/dpi_;
        const bool center=x*x+y*y<=kDeadZoneDip*kDeadZoneDip;
        drag_.overTrash=center && items_.size()>1;
        drag_.slot=center?drag_.from:DropSlot(x,y,menu_.size(),items_.size());
        renderer_->DragTo({static_cast<float>(kCenterDip+x),static_cast<float>(kCenterDip+y)},drag_.slot,drag_.overTrash);
        KeepAlive();
        return true;
    }
    // Forgets a press and puts a lifted tile back with the rest of the page. The release of a
    // canceled drag is not a click.
    void EndDrag(DragEnd end) {
        const bool active=drag_.active;
        drag_={};
        if(GetCapture()==window)ReleaseCapture();
        if(active) {renderer_->EndDrag(end);pressGeneration_=~0u;}
    }
    // Another window took the mouse (or the system canceled the mode): the drag cannot finish.
    void CaptureLost() {
        if(drag_.from<0)return;
        const bool active=drag_.active;
        EndDrag(DragEnd::Cancel);
        if(active)Log(L"Drag canceled: the mouse capture was lost");
    }
    std::wstring DragTarget() const {
        if(!drag_.active)return L"none";
        return drag_.overTrash?L"trash":L"slot:"+std::to_wstring(drag_.slot);
    }
    // The release position travels with the message, so a click queued behind slow
    // work still lands where the user released, not where the pointer is now.
    void Click(POINT client) {
        const auto drag=drag_;
        drag_={};
        if(GetCapture()==window)ReleaseCapture();
        if(drag.active) {
            if(renderer_->Dragging())Drop(drag,client);
            else pressGeneration_=~0u;
            return;
        }
        if(!input_.Persistent() || !gesture_.Active() || pressGeneration_!=gesture_.Generation())return;
        const POINT point{MulDiv(client.x,96,dpi_),MulDiv(client.y,96,dpi_)};
        if(const int arrow=AdjustArrow(point)) {Adjust(arrow);return;}
        const int selected=HitTest(point.x-kCenterDip,point.y-kCenterDip,static_cast<unsigned>(menu_.size()));
        if(selected<0)Back();else Confirm(selected);
    }
    // Where the lifted tile is released decides: the armed trash takes the app out of the reel,
    // another slot moves it there, and its own slot puts it back.
    void Drop(const Drag& drag,POINT client) {
        KeepAlive();
        const unsigned count=static_cast<unsigned>(menu_.size());
        if(!gesture_.Active() || page_!=MenuPage::Manage || pressGeneration_!=gesture_.Generation() ||
           drag.from<0 || static_cast<size_t>(drag.from)>=items_.size()) {renderer_->EndDrag(DragEnd::Cancel);return;}
        const double x=client.x*96.0/dpi_-kCenterDip,y=client.y*96.0/dpi_-kCenterDip;
        if(x*x+y*y<=kDeadZoneDip*kDeadZoneDip) {
            if(items_.size()>1 && TakeOut(drag.from,true)) {
                renderer_->EndDrag(DragEnd::Remove);gesture_.Select(-1,count);RenderMenu(-1,true);
            } else {renderer_->EndDrag(DragEnd::Cancel);Log(L"Drop on the trash ignored: the last app stays");}
            return;
        }
        const int slot=DropSlot(x,y,count,items_.size());
        if(slot<0 || slot==drag.from) {renderer_->EndDrag(DragEnd::Cancel);Log(L"Drag returned");return;}
        MoveApp(drag.from,slot,true);
    }
    // Saves the reel with one app moved; the preferred agent stays the same app. The page reflows
    // from where its tiles are, and the selection follows the app.
    bool MoveApp(int from,int to,bool dropped) {
        auto candidate=items_;
        const auto fail=[&](const std::wstring& message) {
            if(dropped)renderer_->EndDrag(DragEnd::Cancel);
            if(!message.empty())MessageBoxW(window,message.c_str(),kAppName,MB_OK|MB_ICONERROR);
            return false;
        };
        if(!MoveEntry(candidate,from,to))return fail({});
        const int preferred=PreferredTargetIndex(items_);
        const int kept=preferred<0?-1:MovedIndex(preferred,from,to);
        std::wstring error;
        if(!SaveTargets(candidate,error))return fail(error);
        // Without a saved preference the first app is preferred; moving must not change what a tap opens.
        if(kept>=0 && PreferredTargetIndex(candidate)!=kept && !SavePreferredTarget(candidate[kept])) {
            std::wstring rollback;
            if(SaveTargets(items_,rollback)) {Log(L"Move rolled back: the preferred agent could not be kept");return fail({});}
            Log(L"Move kept: neither the preferred agent nor the previous order could be saved");
        }
        // Each tile reflows from its own previous slot; Add keeps the last one.
        reflowFrom_.assign(items_.size()+1,static_cast<int>(items_.size()));
        for(int position=0;position<static_cast<int>(items_.size());++position)reflowFrom_[MovedIndex(position,from,to)]=position;
        items_=std::move(candidate);
        Log(L"App moved: "+AppDisplayName(items_[to])+L" to position "+std::to_wstring(to+1));
        if(dropped)renderer_->EndDrag(DragEnd::Drop);
        gesture_.Select(to,static_cast<unsigned>(menu_.size()));
        RenderMenu(-1,true);
        return true;
    }
    // Takes an app out of the reel without losing it: its settings wait first on Add. The parked
    // list is written first, so a failure in between can only leave a copy that Add hides while
    // the app is still in the reel. The caller renders the page that follows.
    bool TakeOut(int index,bool dropped=false) {
        if(index<0 || static_cast<size_t>(index)>=items_.size() || items_.size()<2)return false;
        const auto fail=[&](const std::wstring& error) {
            Log(L"Taking an app out failed: "+error);
            if(dropped)renderer_->EndDrag(DragEnd::Cancel);
            MessageBoxW(window,error.c_str(),kAppName,MB_OK|MB_ICONERROR);
            return false;
        };
        // A parked list that could not be read is kept beside the new one, never overwritten.
        const auto parkedPath=ParkedPath();
        if(parkedUnread_ && GetFileAttributesW(parkedPath.c_str())!=INVALID_FILE_ATTRIBUTES) {
            const auto backup=parkedPath+L".unreadable."+std::to_wstring(EventStamp())+L".bak";
            if(!CopyFileW(parkedPath.c_str(),backup.c_str(),TRUE))
                return fail(Tr(L"parked.ini could not be read or backed up, so no app can be taken out of the reel now.",
                    L"无法读取或备份 parked.ini，暂时不能将应用移出轮盘。"));
            Log(L"Unreadable parked list kept as "+backup);
        }
        auto parked=parked_;Park(parked,items_[index]);
        auto candidate=items_;candidate.erase(candidate.begin()+index);
        std::wstring error;
        if(!SaveParked(parked,error))return fail(error);
        parkedUnread_=false;
        if(!SaveTargets(candidate,error)) {
            std::wstring ignored;
            if(!SaveParked(parked_,ignored))Log(L"Parked list not rolled back; the app stays in the reel and Add hides its copy");
            return fail(error);
        }
        // When the preferred agent leaves, the first remaining app becomes preferred, so putting
        // the old one back later does not silently take the tap over again.
        if(PreferredTargetIndex(items_)==index && !SavePreferredTarget(candidate.front()))
            Log(L"Preferred agent not saved after taking it out; a tap uses the first app");
        const auto name=AppDisplayName(items_[index]);
        // On Manage the others reflow from their own slots and the app taken out shrinks away.
        reflowFrom_.assign(candidate.size()+1,static_cast<int>(items_.size()));
        for(int position=0;position<static_cast<int>(candidate.size());++position)reflowFrom_[position]=position<index?position:position+1;
        parked_=std::move(parked);items_=std::move(candidate);
        if(editing_>=static_cast<int>(items_.size()))editing_=-1;
        Log(L"App taken out of the reel: "+name+L"; parked="+std::to_wstring(parked_.size()));
        return true;
    }
    // Tap, the release of a resumed hold, a click and Return all confirm through here.
    void Confirm(int index) {
        input_.EndHold();KeepAlive();
        if(!CanConfirm(menu_,index)) {Log(index<0?L"Confirm ignored: no selection":L"Confirm ignored: unavailable item");return;}
        Choose(index);
    }
    // Slot 0 of the hub is the language; both chevrons switch it.
    void Adjust(int) {
        input_.EndHold();KeepAlive();
        if(page_==MenuPage::Settings)SwitchLanguage();
    }
    void Release() {
        if(!gesture_.Active())return;
        MovePointer();const auto selected=gesture_.Release();
        if(!selected) {Hide();Log(L"Gesture canceled before selection");}
        else if(static_cast<size_t>(*selected)<menu_.size() && !menu_[*selected].enabled) {Hide();Log(L"Gesture released on a disabled item");}
        else Choose(*selected);
    }
    void LaunchTarget(const AppTarget& target) {
        if(smoke)Log(L"Smoke simulated launch: "+target.name);
        else if(!ActivateRunning(target))Launch(target,window);
    }
    bool PersistLocalTargets(const std::vector<AppTarget>& candidate,int preferred=-1) {
        const auto path=ConfigPath();
        if(GetFileAttributesW(path.c_str())!=INVALID_FILE_ATTRIBUTES) {
            const auto backup=path+L".before-local-apps."+std::to_wstring(EventStamp())+L".bak";
            if(!CopyFileW(path.c_str(),backup.c_str(),TRUE)) {Log(L"Local app migration backup failed");return false;}
        }
        std::wstring error;
        if(!SaveTargets(candidate,error)) {Log(L"Local app configuration save failed");return false;}
        if(preferred<0)preferred=PreferredTargetIndex(items_);
        if(preferred>=0 && static_cast<size_t>(preferred)<candidate.size() && !SavePreferredTarget(candidate[preferred])) {
            std::wstring rollbackError;
            if(!SaveTargets(items_,rollbackError))Log(L"Preference save and configuration rollback failed; backup retained");
            else Log(L"Preference save failed; original configuration restored");
            return false;
        }
        items_=candidate;
        return true;
    }
    void ChooseLocalApplication(int index,bool launchAfterSelection) {
        if(index<0 || static_cast<size_t>(index)>=items_.size() || editorOpen_)return;
        gesture_.Cancel();Hide(false);FinishHide();
        std::array<wchar_t,32768> path{};
        OPENFILENAMEW picker{sizeof(picker)};picker.hwndOwner=window;
        picker.lpstrTitle=Tr(L"Choose a local application",L"选择本机应用");
        picker.lpstrFilter=Tr(L"Applications and shortcuts\0*.exe;*.lnk\0\0",L"应用程序和快捷方式\0*.exe;*.lnk\0\0");
        picker.lpstrFile=path.data();picker.nMaxFile=static_cast<DWORD>(path.size());
        picker.Flags=OFN_FILEMUSTEXIST|OFN_PATHMUSTEXIST|OFN_NOCHANGEDIR|OFN_NODEREFERENCELINKS;
        editorOpen_=true;
        const auto previousStyle=GetWindowLongPtrW(window,GWL_EXSTYLE);
        SetWindowLongPtrW(window,GWL_EXSTYLE,previousStyle&~WS_EX_NOACTIVATE);
        Log(L"Short tap requires a local application: "+items_[index].name);
        const bool chosen=GetOpenFileNameW(&picker)!=FALSE;
        SetWindowLongPtrW(window,GWL_EXSTYLE,previousStyle);
        editorOpen_=false;
        if(chosen) {
            auto candidate=items_;
            candidate[index]={items_[index].name,TargetKind::Shell,path.data(),L"",L""};
            if(IsVerifiedLocalApplication(candidate[index]) && PersistLocalTargets(candidate,index)) {
                if(launchAfterSelection)LaunchTarget(items_[index]);else Start(true,anchor_,MenuPage::Manage);
            } else MessageBoxW(nullptr,Tr(L"Could not save this local application. Choose an executable or a shortcut to an installed application.",
                L"无法保存此本机应用。请选择可执行程序或指向已安装应用的快捷方式。"),kAppName,MB_OK|MB_ICONWARNING);
        } else if(!launchAfterSelection)Start(true,anchor_,MenuPage::Manage);
    }
    void Choose(int selected) {
        if(selected<0 || static_cast<size_t>(selected)>=menu_.size())return;
        const auto entry=menu_[selected];
        if(!entry.enabled)return;
        switch(entry.action) {
        case MenuAction::Launch:gesture_.Cancel();Hide(false,HideStyle::Commit);LaunchTarget(entry.visual);Log(L"Gesture committed");break;
        case MenuAction::Manage:Navigate(MenuPage::Manage,selected);break;
        case MenuAction::Settings:Navigate(MenuPage::Settings,selected);break;
        case MenuAction::Language:SwitchLanguage();break;
        case MenuAction::Size:NextReelSize();break;
        case MenuAction::Appearance:
            gesture_.Cancel();Hide(false,HideStyle::Commit);LaunchTarget({L"Windows appearance",TargetKind::Shell,L"ms-settings:colors",L"",L""});break;
        case MenuAction::Entry:editing_=entry.index;Navigate(MenuPage::Entry,selected);break;
        case MenuAction::Edit:OpenEditor(entry.index);break;
        case MenuAction::SetPreferred:
            if(entry.index>=0 && static_cast<size_t>(entry.index)<items_.size()) {
                if(!IsVerifiedLocalApplication(items_[entry.index]))ChooseLocalApplication(entry.index,false);
                else if(SavePreferredTarget(items_[entry.index])) {
                    Log(L"Preferred agent saved: "+items_[entry.index].name);Navigate(MenuPage::Manage);
                }
            }break;
        // The app leaves the reel and waits on Add with its settings, as from the Manage trash.
        case MenuAction::Remove:if(TakeOut(entry.index))Navigate(MenuPage::Manage);break;
        case MenuAction::Presets:presetsParent_=page_;presetOffset_=0;Navigate(MenuPage::Presets,selected);break;
        case MenuAction::Previous:presetOffset_=presetOffset_>=5?presetOffset_-5:0;Navigate(MenuPage::Presets);break;
        case MenuAction::Next:presetOffset_+=5;Navigate(MenuPage::Presets);break;
        case MenuAction::Custom:OpenEditor(-1);break;
        case MenuAction::Done:Navigate(MenuPage::Manage);break;
        // A parked app goes back as it was saved, without the local-target check a preset needs.
        case MenuAction::Restore:
            if(items_.size()<kMaxApps) {
                const auto app=entry.visual;
                auto candidate=items_;candidate.push_back(app);std::wstring error;
                if(!SaveTargets(candidate,error)) {MessageBoxW(window,error.c_str(),kAppName,MB_OK|MB_ICONERROR);break;}
                items_=std::move(candidate);
                // Add hides a parked app that is back in the reel, so a failed write here only leaves a hidden copy.
                std::erase_if(parked_,[&](const AppTarget& other){return SameApp(other,app);});
                if(!SaveParked(parked_,error))Log(L"Parked list not updated: "+error);
                Log(L"App put back in the reel: "+AppDisplayName(app));
                Navigate(MenuPage::Manage);
            }break;
        case MenuAction::Preset:
            if(items_.size()<kMaxApps) {
                // The entry carries the catalog item it showed, so a catalog refresh cannot shift it.
                const auto preset=entry.visual;
                if(!IsVerifiedLocalApplication(preset))OpenEditor(-1,&preset);
                else {
                    auto candidate=items_;candidate.push_back(preset);std::wstring error;
                    if(SaveTargets(candidate,error)) {items_=std::move(candidate);Navigate(MenuPage::Manage);}
                    else MessageBoxW(window,error.c_str(),kAppName,MB_OK|MB_ICONERROR);
                }
            }break;
        }
    }
    void OpenEditor(int index,const AppTarget* preset=nullptr) {
        gesture_.Cancel();Hide(false);FinishHide();editorOpen_=true;
        if(preset)ShowPresetTargetEditor(window,*preset);else ShowTargetEditor(window,index);
        // An editor that could not open posts no close message; never leave the Copilot key blocked.
        if(!OsdOpen()) {editorOpen_=false;Log(L"Editor did not open");}
    }
    void EditorClosed() {
        editorOpen_=false;if(quitting_)return;
        std::wstring error;items_=LoadTargets(error);
        Start(true,anchor_,MenuPage::Manage);
    }
    void Cancel() {
        if(!gesture_.Active())return;
        gesture_.Cancel();Hide();Log(L"Canceled");
    }
    // Commit holds the chosen item for one beat (launch, appearance); everything else cancels.
    void Hide(bool restoreFocus=true,HideStyle style=HideStyle::Cancel) {
        EndDrag(DragEnd::Cancel);
        KillTimer(window,kIdleTimer);KillTimer(window,kKeyPollTimer);RegisterRawInput(false);input_.Closed();pageShown_=false;
        if(restoreFocus && GetForegroundWindow()==window && IsWindow(previousForeground_))SetForegroundWindow(previousForeground_);
        SetWindowLongPtrW(window,GWL_EXSTYLE,GetWindowLongPtrW(window,GWL_EXSTYLE)|WS_EX_NOACTIVATE);
        const unsigned duration=renderer_->BeginHide(style);closing_=true;
        if(duration)SetTimer(window,kCloseTimer,duration,nullptr);else FinishHide();
    }
    void FinishHide() {
        KillTimer(window,kCloseTimer);closing_=false;renderer_->Hide();ShowWindow(window,SW_HIDE);
        if(artworkPending_)PostMessageW(window,kIdleArtworkMessage,0,0);
        if(!failures_.empty())PostMessageW(window,kLaunchFailedMessage,0,0);
    }
    void RefreshAppearance() {
        if(!appearance_ || !renderer_)return;
        MSG ignored{};
        while(PeekMessageW(&ignored,window,kAppearanceMessage,kAppearanceMessage,PM_REMOVE)) {}
        // A new appearance redraws every surface; a lifted tile goes back first.
        EndDrag(DragEnd::Cancel);
        currentAppearance_=appearance_->Read();
        renderer_->SetAppearance(currentAppearance_);
        ApplyMenuTheme(currentAppearance_);
        RefreshOsdAppearance();
        Log(L"Appearance: "+std::wstring(currentAppearance_.dark?L"dark":L"light")+L", transparency="+(currentAppearance_.transparency?L"on":L"off"));
    }
    void Reload() {
        const bool firstRun=GetFileAttributesW(ConfigPath().c_str())==INVALID_FILE_ATTRIBUTES;
        std::wstring error;items_=LoadTargets(error);
        std::wstring parkedError;parked_=LoadParked(parkedError);
        parkedUnread_=!parkedError.empty();
        if(parkedUnread_)Log(parkedError);
        // Seeding a first profile and migrating a website default need the catalog now.
        if(error.empty() && presets_.empty() && (firstRun ||
            std::any_of(items_.begin(),items_.end(),[](const AppTarget& item){return !IsLocalApplication(item);})))
            presets_=AgentPresets();
        if(error.empty()) {
            auto candidate=items_;
            bool changed=false;
            if(firstRun) {
                candidate.clear();
                for(const auto& preset:presets_)if(IsVerifiedLocalApplication(preset) && candidate.size()<6)candidate.push_back(preset);
                candidate.insert(candidate.end(),items_.begin(),items_.end());
                changed=candidate.size()!=items_.size();
            } else {
                for(auto& item:candidate)if(!IsLocalApplication(item)) {
                    if(auto replacement=LocalReplacement(item,presets_);replacement && IsVerifiedLocalApplication(*replacement)) {item=*replacement;changed=true;}
                }
            }
            if(changed && PersistLocalTargets(candidate))Log(L"Local application profiles saved; previous configuration backed up");
        }
        // Shell icon extraction can take hundreds of milliseconds. Doing it here keeps
        // the first hold from blocking input while the reel opens.
        auto artwork=items_;artwork.insert(artwork.end(),presets_.begin(),presets_.end());
        PrepareArtwork(artwork,static_cast<float>(ReelDpi(GetDpiForSystem())));
        if(gesture_.Active())Navigate(page_);else RenderMenu();
        if(!error.empty()) {
            Log(L"Configuration validation failed; original file preserved, fallback targets loaded.");
            // Never block initialization in a modal message loop. Settings shows the full error.
            NOTIFYICONDATAW notice{sizeof(notice)};notice.hWnd=window;notice.uID=1;notice.uFlags=NIF_INFO;
            wcscpy_s(notice.szInfoTitle,Tr(L"Copilot Reel configuration needs attention",L"Copilot Reel 配置需要检查"));
            wcscpy_s(notice.szInfo,Tr(L"The original file is preserved. Default targets are temporarily loaded. Open App settings to review the error.",L"原配置已保留，暂时使用默认入口。请从托盘打开应用设置查看错误。"));
            notice.dwInfoFlags=NIIF_WARNING;Shell_NotifyIconW(NIM_MODIFY,&notice);
        }
    }
    void RegisterRawInput(bool enable) {
        // The user's keystrokes and mouse are never sunk by a test instance.
        if(CurrentTestInput().active) {if(enable)Log(L"Raw Input skipped: test input seam");return;}
        RAWINPUTDEVICE devices[2]{};
        // Keyboard raw input only supplies Escape in this app's visible gesture. No key logging.
        for(int i=0;i<2;++i) { devices[i].usUsagePage=1;devices[i].usUsage=i==0?2:6;devices[i].dwFlags=enable?RIDEV_INPUTSINK:RIDEV_REMOVE;devices[i].hwndTarget=enable?window:nullptr; }
        if(!RegisterRawInputDevices(devices,2,sizeof(RAWINPUTDEVICE))) Log(L"Raw Input registration failed");
    }
    void RawInput(HRAWINPUT input) {
        UINT size=0;
        if(GetRawInputData(input,RID_INPUT,nullptr,&size,sizeof(RAWINPUTHEADER))!=0 || size>65536) return;
        std::vector<BYTE> data(size);
        if(GetRawInputData(input,RID_INPUT,data.data(),&size,sizeof(RAWINPUTHEADER))!=size) return;
        auto raw=reinterpret_cast<const RAWINPUT*>(data.data());
        if(raw->header.dwType==RIM_TYPEMOUSE) MovePointer();
        else if(!input_.Persistent() && raw->header.dwType==RIM_TYPEKEYBOARD && raw->data.keyboard.VKey==VK_ESCAPE && !(raw->data.keyboard.Flags&RI_KEY_BREAK)) Cancel();
    }
    static const wchar_t* TrayTip() {return Tr(L"Copilot Reel - Ctrl+Alt+R to preview",L"Copilot Reel · Ctrl+Alt+R 预览");}
    // NOTIFYICON_VERSION_4 suppresses the standard tooltip unless NIF_SHOWTIP is set.
    void AddTray() {
        NOTIFYICONDATAW data{sizeof(data)};data.hWnd=window;data.uID=1;
        data.uFlags=NIF_MESSAGE|NIF_ICON|NIF_TIP|NIF_SHOWTIP;data.uCallbackMessage=kTrayMessage;
        data.hIcon=LoadIconW(GetModuleHandleW(nullptr),MAKEINTRESOURCEW(1));
        if(!data.hIcon) data.hIcon=LoadIconW(nullptr,IDI_APPLICATION);
        wcscpy_s(data.szTip,TrayTip());
        Shell_NotifyIconW(NIM_ADD,&data);data.uVersion=NOTIFYICON_VERSION_4;Shell_NotifyIconW(NIM_SETVERSION,&data);
    }
    // Updates the existing icon in place; removing and re-adding it flashes and may reorder it.
    void UpdateTrayTip() {
        NOTIFYICONDATAW data{sizeof(data)};data.hWnd=window;data.uID=1;data.uFlags=NIF_TIP|NIF_SHOWTIP;
        wcscpy_s(data.szTip,TrayTip());Shell_NotifyIconW(NIM_MODIFY,&data);
    }
    void RemoveTray() {NOTIFYICONDATAW data{sizeof(data)};data.hWnd=window;data.uID=1;Shell_NotifyIconW(NIM_DELETE,&data);}
    // `anchor` is the icon's anchor point, so a menu opened from the keyboard sits beside it too.
    void TrayMenu(POINT anchor) {
        const UINT choice=ShowTrayMenu(window,anchor,currentAppearance_,{
            {kCommandPreview,Tr(L"Preview reel (click to open)",L"预览轮盘（点击打开）"),vi::kGlyphTrayPreview,true},
            {kCommandSettings,Tr(L"Quick menu...",L"快捷菜单…"),vi::kGlyphTraySettings},
            {kCommandDiagnostics,Tr(L"Write diagnostics",L"写出诊断信息"),vi::kGlyphTrayDiagnostics},
            {},
            {kCommandExit,Tr(L"Exit",L"退出"),vi::kGlyphTrayExit}});
        if(choice) PostMessageW(window,WM_COMMAND,choice,0);
    }
    void SaveDiagnostics() {
        std::wostringstream text;
        const auto& visual=CurrentVisualOverride();
        std::wstring names,art;
        for(const auto& entry:menu_)names+=(names.empty()?L"":L"|")+AppDisplayName(entry.visual);
        // What each tile should show, independently of the renderer's cache: tiles with different
        // identities must render differently (tools/capture-seam.py compares their pixels).
        for(const auto& entry:menu_)art+=(art.empty()?L"":L"\t")+ArtworkIdentity(entry.visual,currentAppearance_.dark);
        const auto adjust=AdjustFor(page_);
        const wchar_t* adjustName=!adjust.control?L"none":adjust.earlier && adjust.later?L"both":adjust.earlier?L"earlier":adjust.later?L"later":L"blocked";
        std::wstring parkedNames;
        for(const auto& app:parked_)parkedNames+=(parkedNames.empty()?L"":L"|")+AppDisplayName(app);
        text<<L"Copilot Reel 0.3.6.1\r\n"<<L"DPI="<<dpi_<<L"\r\nreelScale="<<vi::kReelScale<<L"\r\nreelSize="<<ReelSizePercent(CurrentReelSize())<<L"\r\n"<<L"dark="<<currentAppearance_.dark<<L"\r\ntransparencyRequested="<<currentAppearance_.transparency
            <<L"\r\nanimations="<<currentAppearance_.animations<<L"\r\nhighContrast="<<currentAppearance_.highContrast<<L"\r\ntargetCount="<<items_.size()
            <<L"\r\nmenuPage="<<static_cast<int>(page_)<<L"\r\nmenuEntries="<<menu_.size()<<L"\r\npresetOffset="<<presetOffset_
            <<L"\r\npreferredIndex="<<PreferredTargetIndex(items_)<<L"\r\ngestureActive="<<gesture_.Active()
            <<L"\r\ngestureSelected="<<gesture_.Selected()<<L"\r\ngestureAwaitingCenter="<<gesture_.AwaitingCenter()
            <<L"\r\npreview="<<input_.Persistent()<<L"\r\nheld="<<input_.Held()<<L"\r\neditorOpen="<<editorOpen_
            <<L"\r\nmenuNames="<<names<<L"\r\nmenuArt="<<art<<L"\r\nadjust="<<adjustName<<L"\r\naddCatalog="<<addCatalog_.size()<<L"\r\naddInstalled="<<addInstalled_
            <<L"\r\naddParked="<<addParked_<<L"\r\nparked="<<parked_.size()<<L"\r\nparkedNames="<<parkedNames
            <<L"\r\ndrag="<<(drag_.active?drag_.from:-1)<<L"\r\ndragTarget="<<DragTarget()
            <<L"\r\ntopmost="<<((GetWindowLongPtrW(window,GWL_EXSTYLE)&WS_EX_TOPMOST)?1:0)
            <<L"\r\ndataPath="<<ConfigPath()<<L"\r\nlanguage="<<LocaleName(CurrentLanguage())
            <<L"\r\nvisualOverride="<<(visual.raw.empty()?std::wstring(L"none"):visual.raw)
            // Captured pointer relative to the plate center in DIP; nonzero only when an edge clamped the plate.
            <<L"\r\nanchor="<<(anchor_.x-centerX_)*96/dpi_<<L","<<(anchor_.y-centerY_)*96/dpi_;
        if(CurrentTestInput().active)
            text<<L"\r\ntestInput=on\r\ntestRun="<<CurrentTestInput().runId<<L"\r\nrealInputIgnored="<<realInputIgnored_
                <<L"\r\nactivationRequested="<<activationRequested_;
        text<<L"\r\n"<<renderer_->Diagnostics();
        const auto bytes=winrt::to_string(text.str());
        const auto path=DataDirectory()+L"\\diagnostics.txt";
        HANDLE file=CreateFileW(path.c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(file!=INVALID_HANDLE_VALUE) {DWORD written=0;WriteFile(file,bytes.data(),static_cast<DWORD>(bytes.size()),&written,nullptr);CloseHandle(file);}
        Log(L"Diagnostics written");
    }
    void BeginSmoke() {
        smokeStep_=0;Start(true,CursorPosition());SetTimer(window,kSmokeTimer,1000,nullptr);
    }
    void SmokeStep() {
        ++smokeStep_;
        if(smokeStep_==1) {gesture_.Select(1,static_cast<unsigned>(items_.size()));renderer_->SetSelection(1);SaveDiagnostics();}
        if(smokeStep_==2) {auto a=currentAppearance_;a.transparency=false;renderer_->SetAppearance(a);}
        if(smokeStep_==3) {RefreshAppearance();}
        if(smokeStep_==4) {Cancel();SaveDiagnostics();KillTimer(window,kSmokeTimer);Log(L"Smoke test completed");DestroyWindow(window);}
    }
    bool IsPreview() const {return input_.Persistent();}
    // Discovery walks the Start menu, the Uninstall keys and install folders off the UI thread.
    void RefreshPresets() {
        if(discovering_)return;
        discovering_=true;
        const HWND target=window;
        ++workersInFlight;
        try {
            std::thread([target] {
                std::unique_ptr<std::vector<AppTarget>> found;
                try {
                    const HRESULT apartment=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
                    found=std::make_unique<std::vector<AppTarget>>(AgentPresets());
                    if(SUCCEEDED(apartment))CoUninitialize();
                } catch(...) {found.reset();}
                if(PostMessageW(target,kPresetsReadyMessage,0,reinterpret_cast<LPARAM>(found.get())))found.release();
                --workersInFlight;
            }).detach();
        } catch(...) {--workersInFlight;discovering_=false;}
    }
    void PresetsReady(std::unique_ptr<std::vector<AppTarget>> found) {
        discovering_=false;
        if(!found)return;
        Log(L"Agent discovery: "+std::to_wstring(found->size())+L" catalog entries");
        const auto same=[](const AppTarget& a,const AppTarget& b){return a.name==b.name && a.kind==b.kind && a.target==b.target;};
        if(std::equal(found->begin(),found->end(),presets_.begin(),presets_.end(),same))return;
        presets_=std::move(*found);
        // Shell icon extraction can take hundreds of milliseconds; it waits until the reel is idle
        // unless the Add page needs the new entries now.
        if(gesture_.Active() && page_!=MenuPage::Presets)artworkPending_=true;
        else PrepareArtwork(presets_,static_cast<float>(ReelDpi(GetDpiForSystem())));
        if(gesture_.Active() && page_==MenuPage::Presets) {
            // Entries moved under the selection: clear it until the cursor moves again.
            UpdateAddCatalog();gesture_.Select(-1,static_cast<unsigned>(menu_.size()));
            pageCursor_=CursorPosition();observed_=false;moved_=false;
            RenderMenu();
        }
    }
    void FlushArtwork() {
        if(!artworkPending_)return;
        artworkPending_=false;PrepareArtwork(presets_,static_cast<float>(ReelDpi(GetDpiForSystem())));
    }
    void UpdateAddCatalog() {addCatalog_=AddCatalog(items_,parked_,presets_,InstalledIdentity,&addParked_,&addInstalled_);}
    bool Ready() const {return ready_;}
    bool Active() const {return gesture_.Active();}
    // A closing or idle reel never handles clicks. HTTRANSPARENT passes them only to
    // windows of this thread; other applications beneath still do not receive them.
    bool HitTestable() const {return gesture_.Active() && !closing_;}
    LRESULT HitTestAt(POINT screen) const {
        if(!HitTestable())return HTTRANSPARENT;
        ScreenToClient(window,&screen);
        const double x=screen.x*96.0/dpi_-kCenterDip,y=screen.y*96.0/dpi_-kCenterDip;
        return x*x+y*y<kRadiusDip*kRadiusDip?HTCLIENT:HTTRANSPARENT;
    }
    // A slow activation can fail after a new gesture began: its modal message box waits
    // until the reel is hidden instead of opening beneath the topmost reel.
    void LaunchFailed(std::unique_ptr<LaunchFailure> failure) {
        if(failure)failures_.push_back(std::move(*failure));
        if(gesture_.Active() || closing_ || IsWindowVisible(window))return;
        auto ready=std::move(failures_);failures_.clear();
        for(const auto& item:ready)ReportLaunchFailure(item.target,item.hr);
    }
    void DpiChanged(UINT dpi) {
        // A capture DPI override keeps its fixed render scale on every monitor.
        if(placing_ || CurrentVisualOverride().dpi)return;
        if(renderer_ && gesture_.Active()) {Cancel();FinishHide();}
        dpi_=ReelDpi(dpi);if(renderer_)renderer_->Resize(dpi_);
    }
private:
    std::unique_ptr<AppearanceSource> appearance_;
    std::unique_ptr<OrbitRenderer> renderer_;
    Appearance currentAppearance_;
    // parked_: apps taken out of the reel, with their settings (parked.ini).
    std::vector<AppTarget> items_,parked_,presets_,addCatalog_;
    size_t addParked_=0,addInstalled_=0;
    // parked.ini exists but could not be read: it is backed up before it is first rewritten.
    bool parkedUnread_=false;
    Drag drag_;
    // The previous slot of each item for the next Reflow page, or empty to match tiles by app.
    std::vector<int> reflowFrom_;
    bool artworkPending_=false;
    std::vector<MenuEntry> menu_;
    MenuPage page_=MenuPage::Main;
    MenuPage presetsParent_=MenuPage::Manage;
    size_t presetOffset_=0;
    // The page currently on screen in this gesture; transitions are chosen relative to it.
    bool pageShown_=false;
    MenuPage shownPage_=MenuPage::Main;
    size_t shownOffset_=0,shownCount_=0;
    int editing_=-1;
    bool closing_=false,editorOpen_=false,quitting_=false,placing_=false,discovering_=false;
    HWND previousForeground_=nullptr;
    ReelInput input_;
    std::deque<std::variant<CommandPacket,TestPacket>> pending_;
    unsigned realInputIgnored_=0;
    bool activationRequested_=false;
    std::vector<LaunchFailure> failures_;
    GestureState gesture_;
    POINT anchor_{},pageCursor_{},lastPointer_{};
    // observed_: lastPointer_ is valid; moved_: the cursor left the slop around pageCursor_.
    bool observed_=false,moved_=true;
    unsigned pressGeneration_=~0u;
    double centerX_=0,centerY_=0;
    UINT dpi_=96;
    bool hotkey_=false,ready_=false;
    int smokeStep_=0;
};

LRESULT CALLBACK WindowProcedure(HWND hwnd,UINT message,WPARAM wParam,LPARAM lParam) {
    auto self=reinterpret_cast<Controller*>(GetWindowLongPtrW(hwnd,GWLP_USERDATA));
    if(message==WM_NCCREATE) {
        self=static_cast<Controller*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
        self->window=hwnd;SetWindowLongPtrW(hwnd,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self));
    }
    if(!self) return DefWindowProcW(hwnd,message,wParam,lParam);
    try {
        static const UINT taskbarCreated=RegisterWindowMessageW(L"TaskbarCreated");
        if(message==taskbarCreated) {if(!CurrentTestInput().active)self->AddTray();return 0;}
        if(CurrentTestInput().active) switch(message) {
            case WM_INPUT:case WM_MOUSEMOVE:case WM_LBUTTONDOWN:case WM_LBUTTONUP:case WM_RBUTTONUP:case WM_KEYDOWN:
                self->RealInputIgnored();return 0;
            default:break;
        }
        switch(message) {
        case kCopilotMessage:
            if(!self->Ready()) return 0;
            if(wParam==0) self->CommandReceived(Command::Tap);
            else if(wParam==1) self->CommandReceived(Command::HoldStart);
            else if(wParam==2) self->CommandReceived(Command::HoldStop);
            return 0;
        case WM_COPYDATA: {
            if(!self->Ready()) return FALSE;
            auto copy=reinterpret_cast<const COPYDATASTRUCT*>(lParam);
            if(copy && copy->dwData==kTestIpcMagic) {
                const auto input=DecodeTestPacket(copy->dwData,copy->cbData,copy->lpData,CurrentTestInput().active);
                return input?self->QueueCommand(*input):FALSE;
            }
            if(!copy || copy->dwData!=kIpcMagic || copy->cbData!=sizeof(CommandPacket) || !copy->lpData) return FALSE;
            CommandPacket event;memcpy(&event,copy->lpData,sizeof(event));
            if(event.version!=1 || event.command>=Command::Invalid || !event.stamp) return FALSE;
            // Queue after acknowledging the client. No rendering or Shell activation inside SendMessage.
            return self->QueueCommand(event);
        }
        case WM_APP+20:self->DispatchQueued();return 0;
        case kAppearanceMessage:case WM_SETTINGCHANGE:case WM_THEMECHANGED:case WM_SYSCOLORCHANGE:self->RefreshAppearance();return 0;
        case kConfigChangedMessage:self->Reload();return 0;
        case kTargetEditorClosedMessage:self->EditorClosed();return 0;
        case kLanguageChangedMessage:self->LanguageChanged();return 0;
        case kLaunchFailedMessage:self->LaunchFailed(std::unique_ptr<LaunchFailure>(reinterpret_cast<LaunchFailure*>(lParam)));return 0;
        case kIdleArtworkMessage:if(!self->Active())self->FlushArtwork();return 0;
        case kPresetsReadyMessage:self->PresetsReady(std::unique_ptr<std::vector<AppTarget>>(reinterpret_cast<std::vector<AppTarget>*>(lParam)));return 0;
        case WM_INPUT:self->RawInput(reinterpret_cast<HRAWINPUT>(lParam));break;
        case WM_MOUSEMOVE:self->MovePointer();return 0;
        case WM_LBUTTONDOWN:self->Press({GET_X_LPARAM(lParam),GET_Y_LPARAM(lParam)});return 0;
        case WM_LBUTTONUP:self->Click({GET_X_LPARAM(lParam),GET_Y_LPARAM(lParam)});return 0;
        case WM_CAPTURECHANGED:if(reinterpret_cast<HWND>(lParam)!=hwnd)self->CaptureLost();return 0;
        case WM_CANCELMODE:self->CaptureLost();break;
        case WM_SYSCOMMAND: {
            // After a fast-path tap the Shell minimizes a provider window that has the foreground.
            // The reel has no minimized or maximized state; a minimized window reopens in place.
            const UINT command=static_cast<UINT>(wParam&0xFFF0);
            if(command==SC_MINIMIZE || command==SC_MAXIMIZE || command==SC_RESTORE) {Log(L"Shell window command ignored");return 0;}
            break;
        }
        case WM_ACTIVATE:
            if(LOWORD(wParam)!=WA_INACTIVE && !IsWindowVisible(hwnd))Log(L"Activated while hidden");
            break;
        case WM_RBUTTONUP:self->Back();return 0;
        case WM_KEYDOWN:if(!(lParam&(1LL<<30)))self->KeyDown(wParam);return 0;
        case WM_MOUSEACTIVATE:return self->IsPreview()?MA_ACTIVATE:MA_NOACTIVATE;
        case WM_NCHITTEST:
            // A test instance is fed only through its own channel; real clicks pass by it.
            return CurrentTestInput().active?HTTRANSPARENT:self->HitTestAt({GET_X_LPARAM(lParam),GET_Y_LPARAM(lParam)});
        case WM_HOTKEY:if(wParam==1)self->CommandReceived(Command::Preview);return 0;
        case kTrayMessage:
            if(LOWORD(lParam)==WM_CONTEXTMENU) self->TrayMenu({GET_X_LPARAM(wParam),GET_Y_LPARAM(wParam)});
            else if(LOWORD(lParam)==NIN_SELECT || LOWORD(lParam)==NIN_KEYSELECT)self->CommandReceived(Command::Preview);
            return 0;
        case WM_COMMAND:
            switch(LOWORD(wParam)) {
            case kCommandPreview:self->CommandReceived(Command::Preview);break;
            case kCommandSettings:self->CommandReceived(Command::Settings);break;
            case kCommandDiagnostics:self->CommandReceived(Command::Diagnostics);break;
            case kCommandExit:self->CommandReceived(Command::Quit);break;
            }return 0;
        case WM_TIMER:
            if(wParam==kSmokeTimer)self->SmokeStep();
            else if(wParam==kIdleTimer)self->Cancel();
            else if(wParam==kCloseTimer)self->FinishHide();
            else if(wParam==kKeyPollTimer)self->PollCopilotKey();
            return 0;
        case WM_DPICHANGED:self->DpiChanged(HIWORD(wParam));return 0;
        case WM_WTSSESSION_CHANGE:if(wParam==WTS_SESSION_LOCK || wParam==WTS_CONSOLE_DISCONNECT || wParam==WTS_REMOTE_DISCONNECT)self->Cancel();return 0;
        case WM_DISPLAYCHANGE:case WM_POWERBROADCAST:self->Cancel();return 0;
        case WM_ERASEBKGND:return 1;
        case WM_PAINT:{PAINTSTRUCT ps{};BeginPaint(hwnd,&ps);EndPaint(hwnd,&ps);return 0;}
        case WM_CLOSE:self->Cancel();return 0;
        case WM_DESTROY:
            UnregisterHotKey(hwnd,1);self->RemoveTray();WTSUnRegisterSessionNotification(hwnd);PostQuitMessage(0);return 0;
        }
    } catch(const winrt::hresult_error& error) {
        Log(L"Window error: "+std::wstring(error.message()));
        ShowWindow(hwnd,SW_HIDE);
    } catch(...) {Log(L"Unexpected window error");ShowWindow(hwnd,SW_HIDE);}
    return DefWindowProcW(hwnd,message,wParam,lParam);
}
}
}

int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,PWSTR,int) {
    using namespace reel;
    try {
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        // tools/install-development.ps1 restores a turned-off startup task after a reinstall.
        if(DisableStartupTaskCommand())return DisableStartupTask()?0:10;
        const bool bare=BareLaunch();
        const Command command=CommandLine();
        // Capture before COM, icon decode, preset discovery, and GPU startup. A held Copilot
        // key can be released while the process starts, so read it first.
        const bool keyHeld=bare && !CurrentTestInput().active && CopilotKeyHeld();
        const std::wstring keys=bare?CopilotKeyState():std::wstring();
        const HWND foreground=bare?GetForegroundWindow():nullptr;
        CommandPacket initialEvent=CaptureCommand(command,true);
        winrt::init_apartment(winrt::apartment_type::single_threaded);
        LoadLanguage();LoadReelSize();
        INITCOMMONCONTROLSEX controls{sizeof(controls),ICC_STANDARD_CLASSES};InitCommonControlsEx(&controls);
        if(command==Command::Invalid) {MessageBoxW(nullptr,Tr(L"Usage: CopilotReel.exe [--settings | --preview | --background | --diagnostics | --quit]",L"用法：CopilotReel.exe [--settings | --preview | --background | --diagnostics | --quit]"),kAppName,MB_OK|MB_ICONINFORMATION);return 2;}
        const bool smoke=command==Command::SmokeTest;
        const auto& test=CurrentTestInput();
        // A misconfigured test run must never fall into the user's own instance. It logs only
        // into a valid isolated data directory, never into the user's own run.log.
        if(test.present && !test.active && test.reason!=L"packaged") {
            wchar_t directory[MAX_PATH]{};
            if(GetEnvironmentVariableW(L"COPILOT_REEL_DATA_DIR",directory,MAX_PATH) && IsAbsoluteTestDirectory(directory))
                Log(L"Test input refused: "+test.reason);
            return 9;
        }
        // The title always matches the namespace suffix, so a client finds only its own instance.
        const std::wstring title=test.active?L"Copilot Reel Test "+test.runId+(smoke?L" Smoke":L""):
            smoke?L"Copilot Reel Smoke":std::wstring(kAppName);
        const std::wstring suffix=UserSuffix()+(smoke?L".Smoke":L"")+TestInstanceSuffix();
        const std::wstring mutexName=L"Local\\CopilotReel.Instance."+suffix,readyName=L"Local\\CopilotReel.Ready."+suffix;
        HANDLE mutex=CreateMutexW(nullptr,FALSE,mutexName.c_str());
        const bool exists=GetLastError()==ERROR_ALREADY_EXISTS;
        if(!mutex) return 3;
        if(bare) {
            // Without arguments this is the startup task, the Shell's fallback for a Copilot key
            // press while no fast-path window exists (it activates the provider's AUMID with no
            // arguments), or a deliberate start from Start or Explorer. Only the last asks for a
            // preview. Both reach the app as the same activation of the same AUMID, and the
            // provider cannot be moved to a hidden entry (the Settings key picker drops it), so a
            // deliberate start is recognized while the reel is resident, or when Start or search still
            // had the foreground; otherwise a cold start without a held key is
            // taken as the key's tap. An unpackaged build cannot receive the Shell's fallback.
            using winrt::Windows::ApplicationModel::Activation::ActivationKind;
            bool known=false;const auto kind=ActivatedKind(known);
            const bool startup=known && kind==ActivationKind::StartupTask;
            // A resident that has not registered its fast path yet still gets the Shell's fallback.
            bool residentReady=false;
            if(exists)if(HANDLE probe=OpenEventW(SYNCHRONIZE,FALSE,readyName.c_str())) {
                residentReady=WaitForSingleObject(probe,0)==WAIT_OBJECT_0;CloseHandle(probe);
            }
            const bool fromShell=ShellLauncherWindow(foreground);
            initialEvent.command=startup?Command::Background:keyHeld?Command::KeyHold:
                residentReady || fromShell || !known?Command::Preview:Command::Tap;
            const std::wstring kindName=!known?L"none":kind==ActivationKind::Launch?L"Launch":startup?L"StartupTask":
                kind==ActivationKind::Protocol?L"Protocol":std::to_wstring(static_cast<int>(kind));
            Log(L"Started without arguments; activation="+kindName+L"; parent="+ParentImage()+L"; resident="+(residentReady?L"ready":exists?L"starting":L"no")+
                L"; foreground="+WindowOwner(foreground)+L"; "+keys+L"; command="+
                (startup?L"background":keyHeld?L"hold":initialEvent.command==Command::Preview?L"preview":L"tap"));
            // A sign-in start after an earlier cold start has nothing to add.
            if(startup && exists) {CloseHandle(mutex);return 0;}
        }
        if(exists) {
            HANDLE ready=CreateEventW(nullptr,TRUE,FALSE,readyName.c_str());
            DWORD readyResult=WAIT_FAILED;
            if(ready) {readyResult=WaitForSingleObject(ready,5000);CloseHandle(ready);}
            if(readyResult!=WAIT_OBJECT_0) {CloseHandle(mutex);return 4;}
            HWND prior=FindWindowW(kWindowClass,title.c_str());
            bool forwarded=false;
            if(prior) {
                DWORD pid=0;GetWindowThreadProcessId(prior,&pid);AllowSetForegroundWindow(pid);
                COPYDATASTRUCT copy{kIpcMagic,sizeof(initialEvent),const_cast<CommandPacket*>(&initialEvent)};
                DWORD_PTR result=0;
                forwarded=SendMessageTimeoutW(prior,WM_COPYDATA,0,reinterpret_cast<LPARAM>(&copy),SMTO_ABORTIFHUNG,2000,&result) && result;
            }
            CloseHandle(mutex);return forwarded?0:4;
        }
        HANDLE ready=CreateEventW(nullptr,TRUE,FALSE,readyName.c_str());
        if(ready) ResetEvent(ready);
        if(command==Command::Quit || command==Command::Cancel) {
            if(ready)CloseHandle(ready);CloseHandle(mutex);return 0;
        }
        WNDCLASSEXW cls{sizeof(cls)};cls.lpfnWndProc=WindowProcedure;cls.hInstance=instance;cls.lpszClassName=kWindowClass;cls.hCursor=LoadCursorW(nullptr,IDC_ARROW);
        cls.hIcon=LoadIconW(instance,MAKEINTRESOURCEW(1));RegisterClassExW(&cls);
        Controller controller(smoke);
        // Topmost from creation: SetWindowPos(HWND_TOPMOST) is silently ignored for a process without
        // foreground rights, which is how the startup task and an activation in the background start
        // the reel, and a hidden reel would then open beneath a maximized window.
        HWND hwnd=CreateWindowExW(WS_EX_TOPMOST|WS_EX_NOREDIRECTIONBITMAP|WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE,kWindowClass,title.c_str(),WS_POPUP,0,0,384,384,nullptr,nullptr,instance,&controller);
        if(!hwnd || !controller.Initialize()) {if(hwnd)DestroyWindow(hwnd);if(ready)CloseHandle(ready);CloseHandle(mutex);return 5;}
        if(ready)SetEvent(ready);
        controller.CommandReceived(initialEvent);
        MSG message{};int status;
        while((status=GetMessageW(&message,nullptr,0,0))>0) {
            HWND active=GetActiveWindow();
            if(active && active!=hwnd && IsDialogMessageW(active,&message)) continue;
            TranslateMessage(&message);DispatchMessageW(&message);
        }
        // Let an activation that is already in flight finish before process exit.
        WaitForLaunches();
        if(ready)CloseHandle(ready);CloseHandle(mutex);
        return status<0?6:static_cast<int>(message.wParam);
    } catch(const winrt::hresult_error& error) {
        Log(L"Startup error: "+std::wstring(error.message()));
        MessageBoxW(nullptr,error.message().c_str(),L"Copilot Reel - startup failed",MB_OK|MB_ICONERROR);return 7;
    } catch(...) {Log(L"Unexpected startup error");return 8;}
}
