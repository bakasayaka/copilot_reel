#include "installed_apps.h"

#include <knownfolders.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <propkey.h>  // after shobjidl.h, which declares PROPERTYKEY
#include <winrt/base.h>
#include <algorithm>
#include <set>

namespace reel {
namespace {
std::wstring DisplayName(IShellItem* item,SIGDN form) {
    PWSTR value=nullptr;
    if(FAILED(item->GetDisplayName(form,&value)) || !value)return {};
    std::wstring result(value);CoTaskMemFree(value);
    return result;
}

std::wstring Property(IShellItem2* item,REFPROPERTYKEY key) {
    PWSTR value=nullptr;
    if(FAILED(item->GetString(key,&value)) || !value)return {};
    std::wstring result(value);CoTaskMemFree(value);
    return result;
}

bool IsFile(const std::wstring& path) {
    const DWORD attributes=GetFileAttributesW(path.c_str());
    return attributes!=INVALID_FILE_ATTRIBUTES && !(attributes&FILE_ATTRIBUTE_DIRECTORY);
}
}

std::vector<InstalledApp> InstalledApps() {
    std::vector<InstalledApp> result;
    winrt::com_ptr<IShellItem> folder;
    winrt::com_ptr<IEnumShellItems> items;
    if(FAILED(SHGetKnownFolderItem(FOLDERID_AppsFolder,KF_FLAG_DEFAULT,nullptr,IID_PPV_ARGS(folder.put()))) ||
       FAILED(folder->BindToHandler(nullptr,BHID_EnumItems,IID_PPV_ARGS(items.put()))))return result;
    std::set<std::wstring> seen;
    for(winrt::com_ptr<IShellItem> item;items->Next(1,item.put(),nullptr)==S_OK;item=nullptr) {
        const auto entry=item.try_as<IShellItem2>();
        if(!entry)continue;
        // The parent-relative parsing name is the AUMID of a package and of most shortcuts.
        auto app=StartEntryApp(DisplayName(item.get(),SIGDN_NORMALDISPLAY),DisplayName(item.get(),SIGDN_PARENTRELATIVEPARSING),
            Property(entry.get(),PKEY_Link_TargetParsingPath),Property(entry.get(),PKEY_Link_Arguments));
        if(!app)continue;
        // A shortcut can outlive its program. An executable on a share counts as present, as in
        // IsVerifiedLocalApplication, so an offline server never stalls the list.
        if(!app->packaged && !IsNetworkPath(app->target.target) && !IsFile(app->target.target))continue;
        const auto key=LaunchIdentity(app->target.target)+L'|'+app->target.arguments;
        if(seen.insert(key).second)result.push_back(std::move(*app));
    }
    std::stable_sort(result.begin(),result.end(),[](const InstalledApp& a,const InstalledApp& b) {
        return CompareStringEx(LOCALE_NAME_USER_DEFAULT,LINGUISTIC_IGNORECASE|SORT_DIGITSASNUMBERS,
            a.target.name.c_str(),-1,b.target.name.c_str(),-1,nullptr,nullptr,0)==CSTR_LESS_THAN;
    });
    return result;
}

// IShellItemImageFactory returns straight-alpha BGRA, as in artwork.cpp's ShellImage.
std::vector<BYTE> InstalledAppIcon(const std::wstring& item,UINT size) {
    winrt::com_ptr<IShellItemImageFactory> factory;
    HBITMAP bitmap=nullptr;
    const auto parsing=L"shell:AppsFolder\\"+item;
    if(FAILED(SHCreateItemFromParsingName(parsing.c_str(),nullptr,IID_PPV_ARGS(factory.put()))) ||
       FAILED(factory->GetImage(SIZE{static_cast<LONG>(size),static_cast<LONG>(size)},SIIGBF_ICONONLY|SIIGBF_RESIZETOFIT,&bitmap)) ||
       !bitmap)return {};
    std::vector<BYTE> pixels;
    DIBSECTION dib{};
    if(GetObjectW(bitmap,sizeof(dib),&dib)==sizeof(dib) && dib.dsBm.bmBitsPixel==32 && dib.dsBm.bmBits &&
       dib.dsBm.bmWidth==static_cast<LONG>(size) && dib.dsBm.bmHeight==static_cast<LONG>(size)) {
        pixels.resize(static_cast<size_t>(size)*size*4);
        const bool bottomUp=dib.dsBmih.biHeight>0;
        for(UINT y=0;y<size;++y)
            std::copy_n(static_cast<const BYTE*>(dib.dsBm.bmBits)+static_cast<size_t>(bottomUp?size-1-y:y)*dib.dsBm.bmWidthBytes,
                size*4,pixels.data()+static_cast<size_t>(y)*size*4);
    }
    DeleteObject(bitmap);
    bool alpha=false,straight=false;
    for(size_t i=0;i<pixels.size();i+=4) {
        alpha|=pixels[i+3]!=0;
        straight|=pixels[i]>pixels[i+3] || pixels[i+1]>pixels[i+3] || pixels[i+2]>pixels[i+3];
    }
    for(size_t i=0;i<pixels.size();i+=4) {
        if(!alpha) {pixels[i+3]=255;continue;}  // legacy icon without an alpha channel
        if(straight)for(size_t c=0;c<3;++c)pixels[i+c]=static_cast<BYTE>((pixels[i+c]*pixels[i+3]+127)/255);
    }
    return pixels;
}
}
