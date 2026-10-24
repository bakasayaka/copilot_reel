#include "osd.h"
#include "appearance.h"
#include "iconography.h"
#include "localization.h"
#include "display_names.h"
#include "installed_apps.h"
#include "settings.h"
#include "vi_tokens.h"

#include <commdlg.h>
#include <commctrl.h>
#include <d2d1.h>
#include <dwrite_2.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <uxtheme.h>
#include <windowsx.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <utility>

namespace reel {
namespace {
using Microsoft::WRL::ComPtr;
constexpr wchar_t kOsdClass[] = L"CopilotReel.Osd";
constexpr int kWidth = 560, kHeight = 360;
// One grid for every page, in DIP: 24 margins, a 32 header row at 20, content rows of a
// label and a 32 field every 72 from 72, and a 36 footer row at 304 that ends 20 above the edge.
constexpr int kMargin = 24, kHeaderY = 20, kHeaderSize = 32, kContentY = 72, kRowPitch = 72;
constexpr int kLabelToField = 24, kFieldHeight = 32, kFooterY = 304, kFooterHeight = 36;
constexpr int kGap = 8, kSideButton = 112, kFieldRadius = 6;
constexpr int kListRows = 5, kRowHeight = 34;
enum class Page { Home, Apps, Appearance, Settings, Editor, Advanced, Picker, About };
enum Id : int {
    Back = 100, Close, Apps, AppearancePage, SettingsPage, AppList, Add, Edit,
    Remove, Up, Down, Save, Name, Target, Browse, Advanced, Kind, Arguments,
    Directory, LanguageChoice, About, WindowsAppearance, Done, ChooseApp, Search,
    InstalledList, Pick, InstalledViewport, FirstLabel = 500
};
constexpr wchar_t kViewportClass[] = L"CopilotReel.ListViewport";
// Start's All apps arrive from a worker: the list, then each icon as it is extracted.
constexpr UINT kInstalledAppsMessage = WM_APP + 30, kInstalledIconMessage = WM_APP + 31;
struct InstalledIcon { size_t index; std::vector<BYTE> pixels; };
HWND gOsd = nullptr;

D2D1_COLOR_F Color(Rgba value) { return {value.r, value.g, value.b, value.a}; }
COLORREF GdiColor(Rgba value) {
    return RGB(static_cast<BYTE>(value.r * 255), static_cast<BYTE>(value.g * 255), static_cast<BYTE>(value.b * 255));
}
Rgba Mix(Rgba first, Rgba second, float amount) {
    return {first.r + (second.r - first.r) * amount, first.g + (second.g - first.g) * amount,
        first.b + (second.b - first.b) * amount, 1};
}
std::wstring Text(HWND control) {
    const int length = GetWindowTextLengthW(control);
    std::wstring result(static_cast<size_t>(length) + 1, L'\0');
    const int copied = GetWindowTextW(control, result.data(), length + 1);
    result.resize(static_cast<size_t>(std::max(0, copied)));
    return result;
}

struct State {
    HWND window = nullptr, notifyOwner = nullptr;
    UINT dpi = 96;
    Page page = Page::Home;
    std::vector<AppTarget> apps;
    std::map<int, HWND> controls;
    // Text fields are borderless EDIT controls inside these painted frames (DIP, by control ID).
    std::map<int, D2D1_RECT_F> frames;
    int selected = 0, nextLabel = FirstLabel;
    AppTarget draft;
    bool adding = false, dirty = false, creating = false, focusedEditor = false;
    bool updatingScroll = false;
    int scrollX = 0, scrollY = 0;
    std::wstring status;
    // Start's All apps for the picker, read once per window; `visible` indexes the search result.
    std::vector<InstalledApp> installed;
    std::vector<std::vector<BYTE>> icons;
    std::vector<size_t> visible;
    std::wstring query;
    UINT iconSize = 0;
    bool listing = false, listed = false;
    // The picker list's pixel scroll: its full height, the hidden part above, and wheel travel
    // not yet applied (a fraction of a pixel).
    int listHeight = 0, listOffset = 0;
    double listWheel = 0;
    HFONT font = nullptr;
    HBRUSH surfaceBrush = nullptr, inputBrush = nullptr;
    Appearance appearance;
    std::unique_ptr<AppearanceSource> source;
    ComPtr<ID2D1Factory> factory;
    ComPtr<IDWriteFactory> textFactory;
    ComPtr<ID2D1HwndRenderTarget> target;
    ComPtr<ID2D1DCRenderTarget> controlTarget;
    std::map<int, ComPtr<IDWriteTextFormat>> formats;
    Language formatLanguage = Language::English;
    ComPtr<IDWriteFontFallback> fallback;
    bool fallbackTried = false;
    ~State() {
        if (font) DeleteObject(font);
        if (surfaceBrush) DeleteObject(surfaceBrush);
        if (inputBrush) DeleteObject(inputBrush);
    }
    int Px(int dip) const { return MulDiv(dip, static_cast<int>(dpi), 96); }
    HWND Control(int id) const {
        const auto found = controls.find(id);
        return found == controls.end() ? nullptr : found->second;
    }
    Rgba Tile() const { return Mix(appearance.surface, appearance.foreground, appearance.highContrast ? 0 : .035f); }
};

void BuildPage(State& state);
void UpdateAppearance(State& state);
void NavigateBack(State& state);

const wchar_t* PageTitle(const State& state) {
    switch(state.page) {
    case Page::Home:return Tr(L"Quick menu",L"快捷菜单");
    case Page::Apps:return Tr(L"Apps",L"应用");
    case Page::Appearance:return Tr(L"Appearance",L"外观");
    case Page::Settings:return Tr(L"Settings",L"设置");
    case Page::Editor:return state.adding?Tr(L"Add app",L"添加应用"):Tr(L"Edit app",L"编辑应用");
    case Page::Advanced:return Tr(L"Advanced",L"高级选项");
    case Page::Picker:return Tr(L"Choose app",L"选择应用");
    default:return Tr(L"About",L"关于");
    }
}

void UpdateScrollbars(State& state) {
    if(state.updatingScroll)return;
    state.updatingScroll=true;
    // Derive the area before scrollbars. Measuring the already reduced client
    // area would make two unnecessary bars perpetuate each other's overflow.
    RECT client{};GetClientRect(state.window,&client);
    const auto style=GetWindowLongPtrW(state.window,GWL_STYLE);
    const int barWidth=GetSystemMetricsForDpi(SM_CXVSCROLL,state.dpi);
    const int barHeight=GetSystemMetricsForDpi(SM_CYHSCROLL,state.dpi);
    const int fullWidth=client.right+((style&WS_VSCROLL)?barWidth:0);
    const int fullHeight=client.bottom+((style&WS_HSCROLL)?barHeight:0);
    bool horizontalNeeded=state.Px(kWidth)>fullWidth;
    bool verticalNeeded=state.Px(kHeight)>fullHeight;
    for(int pass=0;pass<2;++pass) {
        horizontalNeeded=state.Px(kWidth)>fullWidth-(verticalNeeded?barWidth:0);
        verticalNeeded=state.Px(kHeight)>fullHeight-(horizontalNeeded?barHeight:0);
    }
    ShowScrollBar(state.window,SB_HORZ,horizontalNeeded);
    ShowScrollBar(state.window,SB_VERT,verticalNeeded);
    GetClientRect(state.window,&client);
    SCROLLINFO horizontal{sizeof(horizontal),SIF_RANGE|SIF_PAGE|SIF_POS,0,state.Px(kWidth)-1,
        static_cast<UINT>(std::max(1L,client.right)),state.scrollX,0};
    SCROLLINFO vertical{sizeof(vertical),SIF_RANGE|SIF_PAGE|SIF_POS,0,state.Px(kHeight)-1,
        static_cast<UINT>(std::max(1L,client.bottom)),state.scrollY,0};
    SetScrollInfo(state.window,SB_HORZ,&horizontal,TRUE);
    SetScrollInfo(state.window,SB_VERT,&vertical,TRUE);
    state.scrollX=GetScrollPos(state.window,SB_HORZ);
    state.scrollY=GetScrollPos(state.window,SB_VERT);
    state.updatingScroll=false;
}

void Scroll(State& state,int bar,int request) {
    SCROLLINFO info{sizeof(info),SIF_ALL};GetScrollInfo(state.window,bar,&info);
    int position=info.nPos;
    switch(request) {
    case SB_LINEUP:position-=state.Px(32);break;
    case SB_LINEDOWN:position+=state.Px(32);break;
    case SB_PAGEUP:position-=static_cast<int>(info.nPage);break;
    case SB_PAGEDOWN:position+=static_cast<int>(info.nPage);break;
    case SB_THUMBTRACK:position=info.nTrackPos;break;
    case SB_TOP:position=info.nMin;break;
    case SB_BOTTOM:position=info.nMax;break;
    default:return;
    }
    info.fMask=SIF_POS;info.nPos=position;SetScrollInfo(state.window,bar,&info,TRUE);
    position=GetScrollPos(state.window,bar);
    const int dx=bar==SB_HORZ?state.scrollX-position:0;
    const int dy=bar==SB_VERT?state.scrollY-position:0;
    if(bar==SB_HORZ)state.scrollX=position;else state.scrollY=position;
    ScrollWindowEx(state.window,dx,dy,nullptr,nullptr,nullptr,nullptr,SW_SCROLLCHILDREN|SW_INVALIDATE);
    RedrawWindow(state.window,nullptr,nullptr,RDW_INVALIDATE|RDW_ALLCHILDREN);
}

// Han and CJK punctuation resolve to Microsoft YaHei UI instead of per-glyph JhengHei UI
// or Yu Gothic UI; the system fallback follows. The mapping has no locale so Chinese
// app names typed while the interface is English use the same face.
IDWriteFontFallback* CjkFallback(State& state) {
    if (state.fallbackTried || !state.textFactory) return state.fallback.Get();
    state.fallbackTried = true;
    ComPtr<IDWriteFactory2> factory;
    ComPtr<IDWriteFontFallbackBuilder> builder;
    ComPtr<IDWriteFontFallback> system;
    if (FAILED(state.textFactory.As(&factory)) || FAILED(factory->CreateFontFallbackBuilder(&builder)) ||
        FAILED(factory->GetSystemFontFallback(&system))) return nullptr;
    DWRITE_UNICODE_RANGE ranges[std::size(vi::kCjkRanges)]{};
    for (size_t i = 0; i != std::size(ranges); ++i) ranges[i] = {vi::kCjkRanges[i].first, vi::kCjkRanges[i].last};
    const wchar_t* family[] = {vi::kCjkFamily};
    if (SUCCEEDED(builder->AddMapping(ranges, static_cast<UINT32>(std::size(ranges)), family, 1, nullptr, nullptr, nullptr, 1.0f)) &&
        SUCCEEDED(builder->AddMappings(system.Get())))
        builder->CreateFontFallback(&state.fallback);
    return state.fallback.Get();
}

IDWriteTextFormat* Format(State& state, int size, bool bold = false) {
    // Formats carry the interface locale (zh-CN selects mainland CJK forms).
    if (state.formatLanguage != CurrentLanguage()) { state.formats.clear(); state.formatLanguage = CurrentLanguage(); }
    const int key = size + (bold ? 100 : 0);
    auto& format = state.formats[key];
    if (!format && state.textFactory) {
        state.textFactory->CreateTextFormat(L"Segoe UI", nullptr,
            bold ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, static_cast<float>(size),
            LocaleName(state.formatLanguage), &format);
        if (format) {
            ComPtr<IDWriteTextFormat1> withFallback;
            if (auto* fallback = CjkFallback(state); fallback && SUCCEEDED(format.As(&withFallback)))
                withFallback->SetFontFallback(fallback);
            format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
            ComPtr<IDWriteInlineObject> ellipsis;
            state.textFactory->CreateEllipsisTrimmingSign(format.Get(), &ellipsis);
            format->SetTrimming(&trimming, ellipsis.Get());
        }
    }
    return format.Get();
}
void DrawText(State& state, ID2D1RenderTarget* target, const std::wstring& text,
    D2D1_RECT_F rect, Rgba color, int size = 14, bool bold = false,
    DWRITE_TEXT_ALIGNMENT alignment = DWRITE_TEXT_ALIGNMENT_LEADING) {
    auto* format = Format(state, size, bold);
    if (!format) return;
    format->SetTextAlignment(alignment);
    ComPtr<ID2D1SolidColorBrush> brush;
    if (SUCCEEDED(target->CreateSolidColorBrush(Color(color), &brush)))
        target->DrawTextW(text.c_str(), static_cast<UINT32>(text.size()), format, rect, brush.Get());
}
void RoundRect(ID2D1RenderTarget* target, D2D1_RECT_F rect, float radius, Rgba fill,
    Rgba stroke, float strokeWidth = 0) {
    ComPtr<ID2D1SolidColorBrush> brush;
    if (FAILED(target->CreateSolidColorBrush(Color(fill), &brush))) return;
    const auto rounded = D2D1::RoundedRect(rect, radius, radius);
    target->FillRoundedRectangle(rounded, brush.Get());
    if (strokeWidth > 0) {
        brush->SetColor(Color(stroke));
        target->DrawRoundedRectangle(rounded, brush.Get(), strokeWidth);
    }
}
void CategoryIcon(ID2D1RenderTarget* target, int id, float x, float y, ID2D1Brush* brush) {
    if (id == Apps) {
        for (int row = 0; row != 2; ++row) for (int column = 0; column != 2; ++column)
            target->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x-9+column*11.f,y-9+row*11.f,
                x-3+column*11.f,y-3+row*11.f),1.5f,1.5f),brush,1.5f);
    } else if (id == AppearancePage) {
        target->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(x,y),10,10),brush,1.6f);
        target->DrawLine(D2D1::Point2F(x,y-9),D2D1::Point2F(x,y+9),brush,1.6f);
        target->DrawLine(D2D1::Point2F(x+4,y-7),D2D1::Point2F(x+4,y+7),brush,2);
    } else {
        for (int i=0;i<3;++i) {
            const float yy=y-7+i*7.f, xx=x+(i==1?5.f:-3.f);
            target->DrawLine(D2D1::Point2F(x-10,yy),D2D1::Point2F(x+10,yy),brush,1.5f);
            target->FillEllipse(D2D1::Ellipse(D2D1::Point2F(xx,yy),2.5f,2.5f),brush);
        }
    }
}

void Paint(State& state) {
    PAINTSTRUCT paint{};
    BeginPaint(state.window, &paint);
    RECT client{}; GetClientRect(state.window, &client);
    if (!state.target && state.factory) {
        state.factory->CreateHwndRenderTarget(D2D1::RenderTargetProperties(),
            D2D1::HwndRenderTargetProperties(state.window, D2D1::SizeU(client.right, client.bottom)), &state.target);
        if (state.target) state.target->SetDpi(static_cast<float>(state.dpi), static_cast<float>(state.dpi));
    }
    if (state.target) {
        auto* target = state.target.Get();
        target->BeginDraw();
        target->Clear(Color(state.appearance.surface));
        target->SetTransform(D2D1::Matrix3x2F::Translation(-state.scrollX*96.f/state.dpi,-state.scrollY*96.f/state.dpi));
        const float width = static_cast<float>(kWidth);
        const auto title = state.page==Page::Home?L"Copilot Reel":PageTitle(state);
        // An 18 DIP line is 24 high, so a top of 24 centers the title on the 32 DIP header buttons.
        DrawText(state, target, title, D2D1::RectF(state.page == Page::Home ? kMargin : kMargin+kHeaderSize+12.f, kHeaderY+4.f,
            width-kMargin-kHeaderSize-12, kHeaderY+kHeaderSize), state.appearance.foreground, 18, true);
        for (const auto& [id,frame] : state.frames) {
            const HWND field = state.Control(id);
            const bool focused = field && GetFocus() == field;
            RoundRect(target,frame,kFieldRadius,state.Tile(),focused?state.appearance.accent:state.appearance.outline,focused?1.5f:1.f);
        }
        if (state.page == Page::Home) {
            DrawText(state,target,Tr(L"Quick menu",L"快捷菜单"),D2D1::RectF(24,77,width-24,113),state.appearance.foreground,26,true);
            DrawText(state,target,L"Soft Orbit",D2D1::RectF(24,317,260,340),state.appearance.secondary,12);
            DrawText(state,target,Tr(L"Esc closes",L"Esc 关闭"),D2D1::RectF(300,317,width-24,340),state.appearance.secondary,12,false,DWRITE_TEXT_ALIGNMENT_TRAILING);
        } else if (state.page == Page::Apps) {
            const auto count = std::to_wstring(state.apps.size()) + Tr(L" of 8 entries", L" / 8 个入口");
            DrawText(state,target,count,D2D1::RectF(24,65,width-24,86),state.appearance.secondary,12);
        } else if (state.page == Page::Appearance) {
            const wchar_t* labels[] = {Tr(L"Theme",L"主题"),Tr(L"Material",L"材质"),Tr(L"Selection color",L"选中颜色")};
            const wchar_t* values[] = {
                state.appearance.highContrast ? Tr(L"High contrast",L"高对比度") : state.appearance.dark ? Tr(L"Windows dark",L"Windows 深色") : Tr(L"Windows light",L"Windows 浅色"),
                state.appearance.transparency ? Tr(L"Windows transparency",L"Windows 透明效果") : Tr(L"Solid color",L"纯色"),
                Tr(L"Windows accent",L"Windows 强调色")};
            for (int i=0;i<3;++i) {
                const float y = 82+i*56.f;
                RoundRect(target,D2D1::RectF(24,y,width-24,y+47),8,state.Tile(),state.appearance.outline);
                DrawText(state,target,labels[i],D2D1::RectF(40,y+13,230,y+36),state.appearance.foreground);
                DrawText(state,target,values[i],D2D1::RectF(225,y+13,width-40,y+36),state.appearance.secondary,13,false,DWRITE_TEXT_ALIGNMENT_TRAILING);
            }
            DrawText(state,target,Tr(L"Follows Windows. No duplicate theme switches.",L"跟随 Windows，无需重复设置主题。"),D2D1::RectF(24,267,width-24,290),state.appearance.secondary,12);
        } else if (state.page == Page::Settings) {
            RoundRect(target,D2D1::RectF(24,82,width-24,138),8,state.Tile(),state.appearance.outline);
            DrawText(state,target,Tr(L"Changes apply immediately.",L"更改立即生效。"),D2D1::RectF(24,277,width-24,299),state.appearance.secondary,12);
        } else if (state.page == Page::About) {
            DrawText(state,target,L"Copilot Reel",D2D1::RectF(24,86,width-24,127),state.appearance.foreground,28,true);
            DrawText(state,target,Tr(L"Soft Orbit / native Windows launcher",L"柔光轨道 / Windows 原生启动器"),D2D1::RectF(24,136,width-24,164),state.appearance.secondary,14);
            DrawText(state,target,Tr(L"Version 0.3.6.1",L"版本 0.3.6.1"),D2D1::RectF(24,166,width-24,189),state.appearance.secondary,12);
            DrawText(state,target,Tr(L"Hold the Copilot key, choose an app, then release.",L"按住 Copilot 键，选择应用，然后松开。"),D2D1::RectF(24,194,width-24,220),state.appearance.foreground,14);
            DrawText(state,target,Tr(L"Windows controls the theme, transparency and motion.",L"主题、透明效果和动画均遵循 Windows 设置。"),D2D1::RectF(24,233,width-24,259),state.appearance.secondary,13);
        } else if (state.page == Page::Picker) {
            // The list hides while there is nothing to show; its area then explains why.
            const float listTop = kContentY+kFieldHeight+12.f;
            if (!state.listed || state.visible.empty()) {
                const auto* message = !state.listed ? Tr(L"Reading apps from Start…",L"正在读取开始菜单中的应用…") :
                    state.installed.empty() ? Tr(L"No apps were found in Start.",L"开始菜单中没有找到应用。") :
                    Tr(L"No apps match your search.",L"没有匹配的应用。");
                DrawText(state,target,message,D2D1::RectF(kMargin,listTop+kListRows*kRowHeight/2.f-10,width-kMargin,listTop+kListRows*kRowHeight/2.f+12),
                    state.appearance.secondary,14,false,DWRITE_TEXT_ALIGNMENT_CENTER);
            }
            if (state.listed)
                DrawText(state,target,std::to_wstring(state.visible.size())+Tr(L" apps",L" 个应用"),
                    D2D1::RectF(kMargin,kFooterY+kFooterHeight/2.f-9,300,kFooterY+kFooterHeight/2.f+9),state.appearance.secondary,12);
        }
        if (!state.status.empty() && state.page != Page::Home)
            DrawText(state,target,state.status,D2D1::RectF(24,337,width-24,358),state.appearance.secondary,11);
        const HRESULT result = target->EndDraw();
        if (result == D2DERR_RECREATE_TARGET) {
            state.target.Reset();
            InvalidateRect(state.window,nullptr,FALSE);
        }
    }
    EndPaint(state.window, &paint);
}

bool BindControlTarget(State& state, HDC dc, RECT rectangle) {
    if (!state.controlTarget && state.factory) {
        auto properties = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));
        state.factory->CreateDCRenderTarget(&properties, &state.controlTarget);
    }
    if (!state.controlTarget || FAILED(state.controlTarget->BindDC(dc,&rectangle))) return false;
    state.controlTarget->SetDpi(static_cast<float>(state.dpi),static_cast<float>(state.dpi));
    return true;
}

std::wstring ComboItemText(HWND control,int item) {
    if(item<0)item=static_cast<int>(SendMessageW(control,CB_GETCURSEL,0,0));
    if(item<0)return {};
    const LRESULT length=SendMessageW(control,CB_GETLBTEXTLEN,item,0);
    if(length==CB_ERR)return {};
    std::wstring text(static_cast<size_t>(length)+1,L'\0');
    const LRESULT copied=SendMessageW(control,CB_GETLBTEXT,item,reinterpret_cast<LPARAM>(text.data()));
    text.resize(copied==CB_ERR?0:static_cast<size_t>(copied));
    return text;
}

void DrawClosedCombo(State& state,HWND control,HDC dc) {
    RECT client{};GetClientRect(control,&client);
    if(!BindControlTarget(state,dc,client))return;
    auto* target=state.controlTarget.Get();
    const float width=client.right*96.f/state.dpi,height=client.bottom*96.f/state.dpi;
    const bool focused=GetFocus()==control;
    const bool dropped=SendMessageW(control,CB_GETDROPPEDSTATE,0,0)!=0;
    const auto background=state.page==Page::Settings?state.Tile():state.appearance.surface;
    target->BeginDraw();target->Clear(Color(background));
    RoundRect(target,D2D1::RectF(1,1,width-1,height-1),6,state.Tile(),
        focused||dropped?state.appearance.accent:state.appearance.outline,focused||dropped?1.5f:1.f);
    DrawText(state,target,ComboItemText(control,-1),D2D1::RectF(11,(height-20)/2,width-32,height),
        state.appearance.foreground,14);
    ComPtr<ID2D1SolidColorBrush> brush;
    target->CreateSolidColorBrush(Color(state.appearance.secondary),&brush);
    if(brush) {
        const float x=width-17,y=height/2;
        target->DrawLine(D2D1::Point2F(x-4,y-2),D2D1::Point2F(x,y+2),brush.Get(),1.5f);
        target->DrawLine(D2D1::Point2F(x,y+2),D2D1::Point2F(x+4,y-2),brush.Get(),1.5f);
    }
    if(target->EndDraw()==D2DERR_RECREATE_TARGET)state.controlTarget.Reset();
}

LRESULT CALLBACK ComboProc(HWND control,UINT message,WPARAM wParam,LPARAM lParam,
    UINT_PTR subclassId,DWORD_PTR reference) {
    auto* state=reinterpret_cast<State*>(reference);
    if(message==WM_NCDESTROY) {
        RemoveWindowSubclass(control,ComboProc,subclassId);
        return DefSubclassProc(control,message,wParam,lParam);
    }
    if(message==WM_PAINT) {
        PAINTSTRUCT paint{};BeginPaint(control,&paint);
        DrawClosedCombo(*state,control,paint.hdc);EndPaint(control,&paint);return 0;
    }
    if(message==WM_PRINTCLIENT) {
        DrawClosedCombo(*state,control,reinterpret_cast<HDC>(wParam));return 0;
    }
    if(message==WM_ERASEBKGND)return 1;
    const auto result=DefSubclassProc(control,message,wParam,lParam);
    if(message==WM_SETFOCUS||message==WM_KILLFOCUS||message==CB_SETCURSEL||
        message==CB_SHOWDROPDOWN||message==WM_ENABLE||message==WM_THEMECHANGED)
        InvalidateRect(control,nullptr,FALSE);
    return result;
}

// Arrow and page keys in the search box move the list's selection, so typing and choosing
// never needs the mouse or a Tab; Enter then picks it through IDOK.
LRESULT CALLBACK SearchProc(HWND control,UINT message,WPARAM wParam,LPARAM lParam,
    UINT_PTR subclassId,DWORD_PTR reference) {
    auto* state=reinterpret_cast<State*>(reference);
    if(message==WM_NCDESTROY) {
        RemoveWindowSubclass(control,SearchProc,subclassId);
        return DefSubclassProc(control,message,wParam,lParam);
    }
    if(message==WM_KEYDOWN && (wParam==VK_DOWN||wParam==VK_UP||wParam==VK_NEXT||wParam==VK_PRIOR)) {
        const HWND list=state->Control(InstalledList);
        if(list && IsWindowVisible(list)) {SendMessageW(list,message,wParam,lParam);return 0;}
    }
    return DefSubclassProc(control,message,wParam,lParam);
}

// List scrollbars follow the Windows theme; the classic light bar shows on a dark surface otherwise.
void ThemeList(State& state,HWND list) {
    if(list)SetWindowTheme(list,state.appearance.dark && !state.appearance.highContrast?L"DarkMode_Explorer":L"Explorer",nullptr);
}

// The picker list scrolls by pixels. A LISTBOX scrolls by whole rows only, which turns a precision
// touchpad's small wheel deltas into 34 DIP jumps; so the LISTBOX is as tall as all its rows, inside
// a clipping viewport that moves it and owns the scroll bar. The LISTBOX keeps selection, keys,
// double-click and accessibility.
int ListRow(const State& state) {return state.Px(kRowHeight);}
int ViewportHeight(HWND viewport) {RECT client{};GetClientRect(viewport,&client);return client.bottom;}

void ScrollListTo(State& state,int offset) {
    const HWND viewport=state.Control(InstalledViewport),list=state.Control(InstalledList);
    if(!viewport||!list)return;
    offset=std::clamp(offset,0,std::max(0,state.listHeight-ViewportHeight(viewport)));
    if(offset==state.listOffset)return;
    state.listOffset=offset;
    SCROLLINFO info{sizeof(info),SIF_POS};info.nPos=offset;SetScrollInfo(viewport,SB_VERT,&info,TRUE);
    // Moving the child keeps its drawn rows; only the strip it uncovers repaints.
    SetWindowPos(list,nullptr,0,-offset,0,0,SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE);
    UpdateWindow(list);
}

// After the rows change: size the LISTBOX to them and the scroll bar to the viewport.
void LayoutList(State& state) {
    const HWND viewport=state.Control(InstalledViewport),list=state.Control(InstalledList);
    if(!viewport||!list)return;
    const int rows=std::max(0,static_cast<int>(SendMessageW(list,LB_GETCOUNT,0,0)));
    const int view=ViewportHeight(viewport);
    state.listHeight=std::max(view,rows*ListRow(state));
    state.listOffset=std::clamp(state.listOffset,0,state.listHeight-view);
    state.listWheel=0;
    SCROLLINFO info{sizeof(info),SIF_RANGE|SIF_PAGE|SIF_POS,0,state.listHeight-1,static_cast<UINT>(view),state.listOffset};
    SetScrollInfo(viewport,SB_VERT,&info,TRUE);
    // Read the width after the bar shows or hides.
    RECT client{};GetClientRect(viewport,&client);
    SetWindowPos(list,nullptr,0,-state.listOffset,client.right,state.listHeight,SWP_NOZORDER|SWP_NOACTIVATE);
}

void RevealSelection(State& state) {
    const HWND viewport=state.Control(InstalledViewport),list=state.Control(InstalledList);
    if(!viewport||!list)return;
    const auto selected=SendMessageW(list,LB_GETCURSEL,0,0);
    if(selected==LB_ERR)return;
    const int row=ListRow(state),top=static_cast<int>(selected)*row,view=ViewportHeight(viewport);
    if(top<state.listOffset)ScrollListTo(state,top);
    else if(top+row>state.listOffset+view)ScrollListTo(state,top+row-view);
}

// Each delta moves the list by its share of the system's lines per notch, so a touchpad's small
// deltas move it a few pixels at a time and a mouse notch still moves three rows.
void WheelList(State& state,int delta) {
    const HWND viewport=state.Control(InstalledViewport);
    if(!viewport)return;
    UINT lines=3;SystemParametersInfoW(SPI_GETWHEELSCROLLLINES,0,&lines,0);
    const int view=ViewportHeight(viewport);
    const double notch=lines==WHEEL_PAGESCROLL?view:std::min<double>(view,static_cast<double>(lines)*ListRow(state));
    const double pixels=-static_cast<double>(delta)*notch/WHEEL_DELTA;
    if(pixels*state.listWheel<0)state.listWheel=0;  // a reversal drops the leftover
    state.listWheel+=pixels;
    const int step=static_cast<int>(state.listWheel);
    state.listWheel-=step;
    if(step)ScrollListTo(state,state.listOffset+step);
}

void ScrollListBar(State& state,int request) {
    const HWND viewport=state.Control(InstalledViewport);
    if(!viewport)return;
    SCROLLINFO info{sizeof(info),SIF_ALL};GetScrollInfo(viewport,SB_VERT,&info);
    int offset=state.listOffset;
    switch(request) {
    case SB_LINEUP:offset-=ListRow(state);break;
    case SB_LINEDOWN:offset+=ListRow(state);break;
    case SB_PAGEUP:offset-=static_cast<int>(info.nPage);break;
    case SB_PAGEDOWN:offset+=static_cast<int>(info.nPage);break;
    case SB_THUMBTRACK:case SB_THUMBPOSITION:offset=info.nTrackPos;break;
    case SB_TOP:offset=0;break;
    case SB_BOTTOM:offset=state.listHeight;break;
    default:return;
    }
    ScrollListTo(state,offset);
}

LRESULT CALLBACK ViewportProc(HWND viewport,UINT message,WPARAM wParam,LPARAM lParam) {
    auto* state=reinterpret_cast<State*>(GetWindowLongPtrW(viewport,GWLP_USERDATA));
    switch(message) {
    // The list's owner-draw, color and notification messages belong to the editor window.
    case WM_DRAWITEM:case WM_MEASUREITEM:case WM_CTLCOLORLISTBOX:
        return SendMessageW(GetParent(viewport),message,wParam,lParam);
    case WM_COMMAND:
        if(state && LOWORD(wParam)==InstalledList && HIWORD(wParam)==LBN_SELCHANGE)RevealSelection(*state);
        return SendMessageW(GetParent(viewport),message,wParam,lParam);
    case WM_VSCROLL:if(state)ScrollListBar(*state,LOWORD(wParam));return 0;
    case WM_MOUSEWHEEL:if(state)WheelList(*state,GET_WHEEL_DELTA_WPARAM(wParam));return 0;
    case WM_ERASEBKGND:return 1;  // the LISTBOX always covers the client area
    }
    return DefWindowProcW(viewport,message,wParam,lParam);
}

LRESULT CALLBACK ListProc(HWND list,UINT message,WPARAM wParam,LPARAM lParam,
    UINT_PTR subclassId,DWORD_PTR reference) {
    auto* state=reinterpret_cast<State*>(reference);
    if(message==WM_NCDESTROY) {
        RemoveWindowSubclass(list,ListProc,subclassId);
        return DefSubclassProc(list,message,wParam,lParam);
    }
    if(message==WM_MOUSEWHEEL) {WheelList(*state,GET_WHEEL_DELTA_WPARAM(wParam));return 0;}
    // The LISTBOX shows all its rows at once, so its own page would be the whole list.
    if(message==WM_KEYDOWN && (wParam==VK_NEXT||wParam==VK_PRIOR)) {
        const int count=static_cast<int>(SendMessageW(list,LB_GETCOUNT,0,0));
        if(count<=0)return 0;
        const int rows=std::max(1,ViewportHeight(GetParent(list))/ListRow(*state)-1);
        const auto current=SendMessageW(list,LB_GETCURSEL,0,0);
        const int selected=std::clamp((current==LB_ERR?0:static_cast<int>(current))+(wParam==VK_NEXT?rows:-rows),0,count-1);
        SendMessageW(list,LB_SETCURSEL,selected,0);
        SendMessageW(GetParent(list),WM_COMMAND,MAKEWPARAM(InstalledList,LBN_SELCHANGE),reinterpret_cast<LPARAM>(list));
        return 0;
    }
    return DefSubclassProc(list,message,wParam,lParam);
}

void StyleCombo(State& state,HWND control) {
    // The closed field is the item height plus a 1 DIP border on each side: a 32 DIP field.
    SendMessageW(control,CB_SETITEMHEIGHT,static_cast<WPARAM>(-1),state.Px(kFieldHeight-2));
    SendMessageW(control,CB_SETITEMHEIGHT,0,state.Px(32));
    SetWindowSubclass(control,ComboProc,1,reinterpret_cast<DWORD_PTR>(&state));
}

void DrawControl(State& state, const DRAWITEMSTRUCT& item) {
    if (item.CtlType == ODT_LISTBOX && item.itemID == static_cast<UINT>(-1)) return;
    if (!BindControlTarget(state,item.hDC,item.rcItem)) return;
    auto* target = state.controlTarget.Get();
    const float width = (item.rcItem.right-item.rcItem.left)*96.f/state.dpi;
    const float height = (item.rcItem.bottom-item.rcItem.top)*96.f/state.dpi;
    const bool disabled = (item.itemState&ODS_DISABLED) != 0;
    const bool selected = (item.itemState&ODS_SELECTED) != 0;
    const bool focused = (item.itemState&ODS_FOCUS) != 0;
    // A disabled primary button (Choose with nothing to choose) draws as an ordinary disabled one.
    const bool primary = (item.CtlID == Save || item.CtlID == Done || item.CtlID == Pick) && !disabled;
    const bool tile = item.CtlID == Apps || item.CtlID == AppearancePage || item.CtlID == SettingsPage;
    const auto foreground = disabled ? state.appearance.secondary : state.appearance.foreground;
    const auto fill = selected || primary ? state.appearance.selection : state.Tile();
    target->BeginDraw();
    target->Clear(Color(state.appearance.surface));
    if(item.CtlType==ODT_COMBOBOX) {
        target->Clear(Color(selected?state.appearance.selection:state.Tile()));
        DrawText(state,target,ComboItemText(item.hwndItem,static_cast<int>(item.itemID)),
            D2D1::RectF(10,(height-20)/2,width-10,height),
            selected?state.appearance.selectionText:foreground,14);
        if(target->EndDraw()==D2DERR_RECREATE_TARGET)state.controlTarget.Reset();
        return;
    }
    RoundRect(target,D2D1::RectF(1,1,width-1,height-1),tile?10.f:7.f,fill,
        focused || primary ? state.appearance.accent : state.appearance.outline,focused?2.f:1.f);
    if (item.CtlID == AppList && item.itemID < state.apps.size()) {
        const auto& app = state.apps[item.itemID];
        ComPtr<ID2D1SolidColorBrush> brush;
        target->CreateSolidColorBrush(Color(selected?state.appearance.selectionText:foreground),&brush);
        if (brush) DrawAppSymbol(target,app,D2D1::RectF(12,6,34,28),brush.Get(),1.6f);
        DrawText(state,target,AppDisplayName(app),D2D1::RectF(46,7,width-115,31),
            selected?state.appearance.selectionText:foreground,14);
        const bool website = app.target.starts_with(L"https:") || app.target.starts_with(L"http:");
        DrawText(state,target,website?Tr(L"Website",L"网页"):Tr(L"Application",L"应用"),
            D2D1::RectF(width-112,8,width-16,31),selected?state.appearance.selectionText:state.appearance.secondary,12,false,DWRITE_TEXT_ALIGNMENT_TRAILING);
    } else if (item.CtlID == InstalledList && item.itemID < state.visible.size()) {
        const auto index = state.visible[item.itemID];
        const auto& app = state.installed[index];
        const auto text = selected ? state.appearance.selectionText : foreground;
        ComPtr<ID2D1Bitmap> icon;
        if (index < state.icons.size() && !state.icons[index].empty())
            target->CreateBitmap(D2D1::SizeU(state.iconSize,state.iconSize),state.icons[index].data(),state.iconSize*4,
                D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_PREMULTIPLIED)),&icon);
        if (icon) target->DrawBitmap(icon.Get(),D2D1::RectF(12,5,36,29),1,D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        else {
            ComPtr<ID2D1SolidColorBrush> brush;
            target->CreateSolidColorBrush(Color(text),&brush);
            if (brush) DrawAppSymbol(target,app.target,D2D1::RectF(12,6,34,28),brush.Get(),1.6f);
        }
        DrawText(state,target,app.target.name,D2D1::RectF(46,7,width-130,31),text,14);
        DrawText(state,target,app.packaged?Tr(L"Packaged app",L"打包应用"):Tr(L"Desktop app",L"桌面应用"),
            D2D1::RectF(width-124,8,width-16,31),selected?state.appearance.selectionText:state.appearance.secondary,12,false,DWRITE_TEXT_ALIGNMENT_TRAILING);
    } else if (tile) {
        // Accent-colored marks use the text accent; accent itself is reserved for focus outlines.
        ComPtr<ID2D1SolidColorBrush> brush;
        target->CreateSolidColorBrush(Color(focused?state.appearance.accentText:foreground),&brush);
        if (brush) CategoryIcon(target,static_cast<int>(item.CtlID),28,33,brush.Get());
        DrawText(state,target,Text(item.hwndItem),D2D1::RectF(16,69,width-12,96),foreground,16,true);
        const auto detail = item.CtlID == Apps ? std::to_wstring(state.apps.size())+Tr(L" entries",L" 个入口") :
            std::wstring(item.CtlID == AppearancePage ? Tr(L"Follows Windows",L"跟随 Windows") : Tr(L"Language & more",L"语言与更多设置"));
        DrawText(state,target,detail,D2D1::RectF(16,104,width-10,130),state.appearance.secondary,12);
    } else if (item.CtlID == Back || item.CtlID == Close) {
        ComPtr<ID2D1SolidColorBrush> brush;
        target->CreateSolidColorBrush(Color(foreground),&brush);
        if (brush) {
            const float x=width/2,y=height/2;
            if (item.CtlID == Back) {
                target->DrawLine(D2D1::Point2F(x+3,y-5),D2D1::Point2F(x-3,y),brush.Get(),1.6f);
                target->DrawLine(D2D1::Point2F(x-3,y),D2D1::Point2F(x+3,y+5),brush.Get(),1.6f);
            } else {
                target->DrawLine(D2D1::Point2F(x-4,y-4),D2D1::Point2F(x+4,y+4),brush.Get(),1.6f);
                target->DrawLine(D2D1::Point2F(x+4,y-4),D2D1::Point2F(x-4,y+4),brush.Get(),1.6f);
            }
        }
    } else {
        DrawText(state,target,Text(item.hwndItem),D2D1::RectF(10,(height-19)/2,width-10,height),
            primary?state.appearance.selectionText:foreground,13,primary,DWRITE_TEXT_ALIGNMENT_CENTER);
    }
    if (focused && !(item.itemState&ODS_NOFOCUSRECT)) {
        ComPtr<ID2D1SolidColorBrush> brush;
        target->CreateSolidColorBrush(Color(state.appearance.accent),&brush);
        if (brush) target->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(4,4,width-4,height-4),4,4),brush.Get(),1);
    }
    if (target->EndDraw() == D2DERR_RECREATE_TARGET) state.controlTarget.Reset();
}

HWND Control(State& state, int id, const wchar_t* type, const wchar_t* text, DWORD style,
    int x, int y, int width, int height, DWORD extended = 0) {
    HWND control=CreateWindowExW(extended,type,text,WS_CHILD|WS_VISIBLE|style,
        state.Px(x)-state.scrollX,state.Px(y)-state.scrollY,state.Px(width),state.Px(height),state.window,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),GetModuleHandleW(nullptr),nullptr);
    state.controls[id]=control;
    SendMessageW(control,WM_SETFONT,reinterpret_cast<WPARAM>(state.font),TRUE);
    return control;
}
void Button(State& state,int id,const wchar_t* text,int x,int y,int width,int height=34) {
    Control(state,id,L"BUTTON",text,WS_TABSTOP|BS_OWNERDRAW,x,y,width,height);
}
void Label(State& state,const wchar_t* text,int x,int y,int width) {
    Control(state,state.nextLabel++,L"STATIC",text,SS_LEFT,x,y,width,23);
}
// A single-line EDIT draws its text at its top, so a bordered one never centers it. The field
// is a painted rounded frame like the buttons and the combo box (Paint), with a borderless
// EDIT one text line high centered inside it.
HWND InputField(State& state,int id,const std::wstring& value,int x,int y,int width,int limit=8192) {
    constexpr int line=20,inset=10;
    state.frames[id]=D2D1::RectF(static_cast<float>(x),static_cast<float>(y),static_cast<float>(x+width),static_cast<float>(y+kFieldHeight));
    const auto control=Control(state,id,L"EDIT",value.c_str(),WS_TABSTOP|ES_AUTOHSCROLL,
        x+inset,y+(kFieldHeight-line)/2,width-2*inset,line);
    SendMessageW(control,EM_SETLIMITTEXT,limit,0);
    SendMessageW(control,EM_SETMARGINS,EC_LEFTMARGIN|EC_RIGHTMARGIN,0);
    return control;
}
void EditField(State& state,int id,const wchar_t* label,const std::wstring& value,
    int x,int y,int width,int limit=8192) {
    Label(state,label,x,y,width);
    InputField(state,id,value,x,y+kLabelToField,width,limit);
}
void UpdateFont(State& state) {
    auto font=CreateFontW(-state.Px(14),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
    if (font) { if(state.font)DeleteObject(state.font); state.font=font; }
}
void ResizeToPage(State& state) {
    state.updatingScroll=true;
    ShowScrollBar(state.window,SB_BOTH,FALSE);
    RECT bounds{0,0,state.Px(kWidth),state.Px(kHeight)};
    const DWORD style=static_cast<DWORD>(GetWindowLongPtrW(state.window,GWL_STYLE))&~(WS_HSCROLL|WS_VSCROLL);
    AdjustWindowRectExForDpi(&bounds,style,FALSE,
        static_cast<DWORD>(GetWindowLongPtrW(state.window,GWL_EXSTYLE)),state.dpi);
    MONITORINFO monitor{sizeof(monitor)};GetMonitorInfoW(MonitorFromWindow(state.window,MONITOR_DEFAULTTONEAREST),&monitor);
    RECT current{};GetWindowRect(state.window,&current);
    const LONG width=std::min(bounds.right-bounds.left,monitor.rcWork.right-monitor.rcWork.left);
    const LONG height=std::min(bounds.bottom-bounds.top,monitor.rcWork.bottom-monitor.rcWork.top);
    const LONG x=std::clamp(current.left,monitor.rcWork.left,monitor.rcWork.right-width);
    const LONG y=std::clamp(current.top,monitor.rcWork.top,monitor.rcWork.bottom-height);
    SetWindowPos(state.window,nullptr,x,y,width,height,SWP_NOZORDER|SWP_NOACTIVATE);
    state.updatingScroll=false;
    UpdateScrollbars(state);
}

// Lists the installed apps whose name matches the search and selects the first, so Enter picks it.
void Filter(State& state) {
    state.visible.clear();
    for(size_t i=0;i<state.installed.size();++i)
        if(MatchesQuery(state.installed[i].target.name,state.query))state.visible.push_back(i);
    if(const auto list=state.Control(InstalledList)) {
        SendMessageW(list,WM_SETREDRAW,FALSE,0);
        SendMessageW(list,LB_RESETCONTENT,0,0);
        for(const auto index:state.visible)
            SendMessageW(list,LB_ADDSTRING,0,reinterpret_cast<LPARAM>(state.installed[index].target.name.c_str()));
        SendMessageW(list,LB_SETCURSEL,state.visible.empty()?-1:0,0);
        state.listOffset=0;LayoutList(state);
        SendMessageW(list,WM_SETREDRAW,TRUE,0);
        InvalidateRect(list,nullptr,TRUE);
        // An empty list would be a blank box; Paint explains the empty or loading state instead.
        ShowWindow(state.Control(InstalledViewport),state.listed && !state.visible.empty()?SW_SHOWNA:SW_HIDE);
    }
    EnableWindow(state.Control(Pick),!state.visible.empty());
    InvalidateRect(state.window,nullptr,FALSE);
}

void BuildPage(State& state) {
    state.creating=true;
    for (const auto& [id,control]:state.controls) { (void)id; DestroyWindow(control); }
    state.controls.clear(); state.frames.clear(); state.nextLabel=FirstLabel;
    state.scrollX=0;state.scrollY=0;
    const auto windowTitle=std::wstring(L"Copilot Reel — ")+PageTitle(state);
    SetWindowTextW(state.window,windowTitle.c_str());
    ResizeToPage(state);
    if(state.page!=Page::Home) Button(state,Back,Tr(L"Back",L"返回"),kMargin,kHeaderY,kHeaderSize,kHeaderSize);
    Button(state,Close,Tr(L"Close",L"关闭"),kWidth-kMargin-kHeaderSize,kHeaderY,kHeaderSize,kHeaderSize);
    // Content rows, the right-hand side buttons of a field, and the footer's primary button.
    const auto row=[](int index){return kContentY+index*kRowPitch;};
    const int contentWidth=kWidth-2*kMargin, primaryX=kWidth-kMargin-kSideButton;
    switch(state.page) {
    case Page::Home:
        Button(state,Apps,Tr(L"Apps",L"应用"),24,132,160,144);
        Button(state,AppearancePage,Tr(L"Appearance",L"外观"),200,132,160,144);
        Button(state,SettingsPage,Tr(L"Settings",L"设置"),376,132,160,144);
        break;
    case Page::Apps: {
        Button(state,Add,Tr(L"+ Add app",L"+ 添加应用"),374,20,120,32);
        EnableWindow(state.Control(Add),state.apps.size()<8);
        const auto list=Control(state,AppList,L"LISTBOX",Tr(L"Applications",L"应用列表"),
            WS_TABSTOP|WS_VSCROLL|LBS_NOTIFY|LBS_HASSTRINGS|LBS_OWNERDRAWFIXED|LBS_NOINTEGRALHEIGHT,24,90,512,204);
        SendMessageW(list,LB_SETITEMHEIGHT,0,state.Px(34));
        for(const auto& app:state.apps)SendMessageW(list,LB_ADDSTRING,0,reinterpret_cast<LPARAM>(AppDisplayName(app).c_str()));
        state.selected=std::clamp(state.selected,0,static_cast<int>(state.apps.size())-1);
        SendMessageW(list,LB_SETCURSEL,state.selected,0);
        Button(state,Edit,Tr(L"Edit",L"编辑"),24,304,112);
        Button(state,Remove,Tr(L"Remove",L"移除"),148,304,112);
        Button(state,Up,Tr(L"Move up",L"上移"),284,304,120);
        Button(state,Down,Tr(L"Move down",L"下移"),416,304,120);
        EnableWindow(state.Control(Remove),state.apps.size()>1);
        EnableWindow(state.Control(Up),state.selected>0);
        EnableWindow(state.Control(Down),state.selected+1<static_cast<int>(state.apps.size()));
        break;
    }
    case Page::Settings: {
        Label(state,Tr(L"Language",L"语言"),40,99,165);
        const auto combo=Control(state,LanguageChoice,L"COMBOBOX",L"",WS_TABSTOP|CBS_DROPDOWNLIST|CBS_OWNERDRAWFIXED|CBS_HASSTRINGS,266,94,254,150);
        SendMessageW(combo,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(L"English"));
        SendMessageW(combo,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(Tr(L"Simplified Chinese",L"简体中文")));
        SendMessageW(combo,CB_SETCURSEL,CurrentLanguage()==Language::SimplifiedChinese?1:0,0);
        StyleCombo(state,combo);
        Button(state,About,Tr(L"About Copilot Reel",L"关于 Copilot Reel"),24,154,512,47);
        break;
    }
    case Page::Appearance:
        Button(state,WindowsAppearance,Tr(L"Open Windows appearance",L"打开 Windows 外观设置"),24,302,250);
        break;
    case Page::Editor: {
        EditField(state,Name,Tr(L"Name",L"名称"),AppDisplayName(state.draft),kMargin,row(0),contentWidth,128);
        // The target shares its row with two equal buttons, each kGap apart.
        const int targetWidth=contentWidth-2*(kSideButton+kGap), fieldY=row(1)+kLabelToField;
        EditField(state,Target,state.draft.kind==TargetKind::Aumid?
            Tr(L"Application ID (AUMID)",L"应用 ID (AUMID)"):
            Tr(L"Open with · app, shortcut or website",L"打开目标 · 应用、快捷方式或网页"),state.draft.target,kMargin,row(1),targetWidth);
        Button(state,ChooseApp,Tr(L"Choose app…",L"选择应用…"),kMargin+targetWidth+kGap,fieldY,kSideButton,kFieldHeight);
        Button(state,Browse,Tr(L"Browse…",L"浏览…"),primaryX,fieldY,kSideButton,kFieldHeight);
        EnableWindow(state.Control(Browse),state.draft.kind==TargetKind::Shell);
        // Secondary navigation sits opposite the primary action in the footer.
        Button(state,Advanced,Tr(L"Advanced  ›",L"高级选项  ›"),kMargin,kFooterY,150,kFooterHeight);
        Button(state,Save,Tr(L"Save",L"保存"),primaryX,kFooterY,kSideButton,kFooterHeight);
        break;
    }
    case Page::Advanced: {
        Label(state,Tr(L"Launch method",L"启动方式"),kMargin,row(0),contentWidth);
        const auto combo=Control(state,Kind,L"COMBOBOX",L"",WS_TABSTOP|CBS_DROPDOWNLIST|CBS_OWNERDRAWFIXED|CBS_HASSTRINGS,
            kMargin,row(0)+kLabelToField,contentWidth,150);
        SendMessageW(combo,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(Tr(L"Application, shortcut or URI",L"应用、快捷方式或 URI")));
        SendMessageW(combo,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(Tr(L"Packaged application (AUMID)",L"打包应用 (AUMID)")));
        SendMessageW(combo,CB_SETCURSEL,state.draft.kind==TargetKind::Aumid?1:0,0);
        StyleCombo(state,combo);
        EditField(state,Arguments,Tr(L"Arguments (optional)",L"启动参数（可选）"),state.draft.arguments,kMargin,row(1),contentWidth);
        EditField(state,Directory,Tr(L"Working directory (optional)",L"工作目录（可选）"),state.draft.workingDirectory,kMargin,row(2),contentWidth);
        EnableWindow(state.Control(Directory),state.draft.kind==TargetKind::Shell);
        Button(state,Done,Tr(L"Done",L"完成"),primaryX,kFooterY,kSideButton,kFooterHeight);
        break;
    }
    case Page::Picker: {
        const auto search=InputField(state,Search,state.query,kMargin,kContentY,contentWidth,128);
        SendMessageW(search,EM_SETCUEBANNER,TRUE,reinterpret_cast<LPARAM>(Tr(L"Search apps",L"搜索应用")));
        SetWindowSubclass(search,SearchProc,1,reinterpret_cast<DWORD_PTR>(&state));
        // Whole rows at rest: a half-cut last row reads as a rendering fault.
        const auto viewport=Control(state,InstalledViewport,kViewportClass,L"",WS_VSCROLL|WS_CLIPCHILDREN,
            kMargin,kContentY+kFieldHeight+12,contentWidth,kListRows*kRowHeight,WS_EX_CONTROLPARENT);
        SetWindowLongPtrW(viewport,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(&state));
        const auto list=CreateWindowExW(0,L"LISTBOX",Tr(L"Installed apps",L"已安装的应用"),
            WS_CHILD|WS_VISIBLE|WS_TABSTOP|LBS_NOTIFY|LBS_HASSTRINGS|LBS_OWNERDRAWFIXED|LBS_NOINTEGRALHEIGHT,
            0,0,state.Px(contentWidth),state.Px(kListRows*kRowHeight),viewport,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(InstalledList)),GetModuleHandleW(nullptr),nullptr);
        state.controls[InstalledList]=list;
        SendMessageW(list,WM_SETFONT,reinterpret_cast<WPARAM>(state.font),TRUE);
        SetWindowSubclass(list,ListProc,1,reinterpret_cast<DWORD_PTR>(&state));
        SendMessageW(list,LB_SETITEMHEIGHT,0,state.Px(kRowHeight));
        ThemeList(state,viewport);
        Button(state,Pick,Tr(L"Choose",L"选择"),primaryX,kFooterY,kSideButton,kFooterHeight);
        Filter(state);
        break;
    }
    case Page::About: break;
    }
    state.creating=false;
    InvalidateRect(state.window,nullptr,TRUE);
    HWND focus=state.Control(state.page==Page::Home?Apps:state.page==Page::Apps?AppList:
        state.page==Page::Editor?Name:state.page==Page::Advanced?Kind:state.page==Page::Picker?Search:
        state.page==Page::Settings?LanguageChoice:Back);
    if(focus)SetFocus(focus);
}

void UpdateAppearance(State& state) {
    state.appearance=state.source?state.source->Read():Appearance{};
    if(state.surfaceBrush)DeleteObject(state.surfaceBrush);
    if(state.inputBrush)DeleteObject(state.inputBrush);
    state.surfaceBrush=CreateSolidBrush(GdiColor(state.appearance.surface));
    state.inputBrush=CreateSolidBrush(GdiColor(state.Tile()));
    BOOL dark=state.appearance.dark&&!state.appearance.highContrast;
    DwmSetWindowAttribute(state.window,DWMWA_USE_IMMERSIVE_DARK_MODE,&dark,sizeof(dark));
    DWM_WINDOW_CORNER_PREFERENCE corners=DWMWCP_ROUND;
    DwmSetWindowAttribute(state.window,DWMWA_WINDOW_CORNER_PREFERENCE,&corners,sizeof(corners));
    ThemeList(state,state.Control(InstalledViewport));
    ThemeList(state,state.Control(AppList));
    RedrawWindow(state.window,nullptr,nullptr,RDW_INVALIDATE|RDW_ALLCHILDREN);
}

bool Persist(State& state,const std::vector<AppTarget>& candidate) {
    std::wstring error;
    if(!SaveTargets(candidate,error)) {
        MessageBoxW(state.window,error.c_str(),Tr(L"Cannot save applications",L"无法保存应用"),MB_OK|MB_ICONWARNING);
        return false;
    }
    state.apps=candidate;
    state.status=Tr(L"Saved",L"已保存");
    PostMessageW(state.notifyOwner,kConfigChangedMessage,0,0);
    return true;
}
bool SaveDraft(State& state) {
    auto candidate=state.apps;
    auto app=state.draft;
    if(app.kind==TargetKind::Aumid)app.workingDirectory.clear();
    if(state.adding)candidate.push_back(app); else candidate.at(static_cast<size_t>(state.selected))=app;
    if(!Persist(state,candidate))return false;
    if(state.adding)state.selected=static_cast<int>(state.apps.size())-1;
    state.dirty=false;
    return true;
}
bool MayLeaveEditor(State& state) {
    if(!state.dirty || (state.page!=Page::Editor&&state.page!=Page::Advanced&&state.page!=Page::Picker))return true;
    const int response=MessageBoxW(state.window,
        Tr(L"Save your changes to this application?",L"保存对此应用的修改？"),kAppName,MB_YESNOCANCEL|MB_ICONQUESTION);
    if(response==IDCANCEL)return false;
    return response!=IDYES || SaveDraft(state);
}
void ChangePage(State& state,Page page) {
    state.page=page;
    state.status.clear();
    BuildPage(state);
}
void NavigateBack(State& state) {
    if(state.page==Page::Home) {SendMessageW(state.window,WM_CLOSE,0,0);return;}
    if(state.page==Page::Advanced || state.page==Page::Picker) {ChangePage(state,Page::Editor);return;}
    if(state.page==Page::Editor) {
        if(MayLeaveEditor(state)) {
            if(state.focusedEditor)DestroyWindow(state.window);
            else ChangePage(state,Page::Apps);
        }
    } else if(state.page==Page::About)ChangePage(state,Page::Settings);
    else ChangePage(state,Page::Home);
}
void StartEdit(State& state,bool adding,const AppTarget* preset=nullptr) {
    state.adding=adding; state.dirty=false;
    state.draft=adding?AppTarget{}:state.apps.at(static_cast<size_t>(state.selected));
    if(adding && preset)state.draft=*preset;
    ChangePage(state,Page::Editor);
}
void CompleteEdit(State& state) {
    if(!SaveDraft(state))return;
    if(state.focusedEditor)DestroyWindow(state.window);
    else ChangePage(state,Page::Apps);
}
// Start's All apps are read on a worker once per window; the list posts first, then the icons.
void ReadInstalledApps(State& state) {
    if(state.listing || state.listed)return;
    state.listing=true;
    state.iconSize=static_cast<UINT>(state.Px(24));
    const HWND window=state.window;
    const UINT size=state.iconSize;
    try {
        std::thread([window,size] {
            const HRESULT apartment=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED|COINIT_DISABLE_OLE1DDE);
            try {
                auto apps=std::make_unique<std::vector<InstalledApp>>();
                try {*apps=InstalledApps();} catch(...) {apps->clear();}
                std::vector<std::wstring> items;
                for(const auto& app:*apps)items.push_back(app.item);
                if(PostMessageW(window,kInstalledAppsMessage,0,reinterpret_cast<LPARAM>(apps.get()))) {
                    apps.release();
                    for(size_t i=0;i<items.size();++i) {
                        auto icon=std::make_unique<InstalledIcon>(InstalledIcon{i,InstalledAppIcon(items[i],size)});
                        if(icon->pixels.empty())continue;
                        if(!PostMessageW(window,kInstalledIconMessage,0,reinterpret_cast<LPARAM>(icon.get())))break;
                        icon.release();
                    }
                }
            } catch(...) {}
            if(SUCCEEDED(apartment))CoUninitialize();
        }).detach();
    } catch(...) {state.listing=false;}
}
// The chosen app replaces the whole draft: its name, launch method, target and arguments.
void PickInstalled(State& state) {
    const auto selected=SendMessageW(state.Control(InstalledList),LB_GETCURSEL,0,0);
    if(selected==LB_ERR || static_cast<size_t>(selected)>=state.visible.size())return;
    state.draft=state.installed[state.visible[static_cast<size_t>(selected)]].target;
    state.dirty=true;
    ChangePage(state,Page::Editor);
}

void BrowseTarget(State& state) {
    wchar_t path[32768]{};
    OPENFILENAMEW dialog{sizeof(dialog)};
    dialog.hwndOwner=state.window;
    dialog.lpstrFilter=Tr(L"Applications and shortcuts\0*.exe;*.lnk;*.url\0All files\0*.*\0\0",
        L"应用和快捷方式\0*.exe;*.lnk;*.url\0所有文件\0*.*\0\0");
    dialog.lpstrFile=path; dialog.nMaxFile=static_cast<DWORD>(std::size(path));
    dialog.Flags=OFN_FILEMUSTEXIST|OFN_PATHMUSTEXIST|OFN_NOCHANGEDIR|OFN_EXPLORER;
    if(GetOpenFileNameW(&dialog)) {
        SetWindowTextW(state.Control(Target),path);
        if(state.draft.name.empty()) {
            std::wstring name=path;
            const auto slash=name.find_last_of(L"\\/");if(slash!=std::wstring::npos)name.erase(0,slash+1);
            const auto dot=name.find_last_of(L'.');if(dot!=std::wstring::npos)name.resize(dot);
            SetWindowTextW(state.Control(Name),name.c_str());
        }
    }
}

void Command(State& state,int id,int notification) {
    if(state.creating)return;
    // A field's frame carries its focus ring.
    if((notification==EN_SETFOCUS||notification==EN_KILLFOCUS) && state.frames.contains(id)) {
        InvalidateRect(state.window,nullptr,FALSE);return;
    }
    if(id==Search && notification==EN_CHANGE) {state.query=Text(state.Control(Search));Filter(state);return;}
    if(id==InstalledList && notification==LBN_DBLCLK) {PickInstalled(state);return;}
    if(notification==EN_CHANGE) {
        if(id==Name)state.draft.name=Text(state.Control(Name));
        else if(id==Target)state.draft.target=Text(state.Control(Target));
        else if(id==Arguments)state.draft.arguments=Text(state.Control(Arguments));
        else if(id==Directory)state.draft.workingDirectory=Text(state.Control(Directory));
        else return;
        state.dirty=true;return;
    }
    if(id==AppList && (notification==LBN_SELCHANGE||notification==LBN_DBLCLK)) {
        const auto selected=SendMessageW(state.Control(AppList),LB_GETCURSEL,0,0);
        if(selected!=LB_ERR)state.selected=static_cast<int>(selected);
        EnableWindow(state.Control(Up),state.selected>0);
        EnableWindow(state.Control(Down),state.selected+1<static_cast<int>(state.apps.size()));
        if(notification==LBN_DBLCLK)StartEdit(state,false);
        return;
    }
    if(id==LanguageChoice && notification==CBN_SELCHANGE) {
        const auto language=SendMessageW(state.Control(LanguageChoice),CB_GETCURSEL,0,0)==1?
            Language::SimplifiedChinese:Language::English;
        if(!SaveLanguage(language)) {
            MessageBoxW(state.window,Tr(L"The language preference could not be saved.",L"无法保存语言设置。"),kAppName,MB_OK|MB_ICONWARNING);
            SendMessageW(state.Control(LanguageChoice),CB_SETCURSEL,CurrentLanguage()==Language::SimplifiedChinese?1:0,0);
            return;
        }
        SetLanguage(language);PostMessageW(state.notifyOwner,kLanguageChangedMessage,0,0);
        BuildPage(state);return;
    }
    if(id==Kind && notification==CBN_SELCHANGE) {
        state.draft.kind=SendMessageW(state.Control(Kind),CB_GETCURSEL,0,0)==1?TargetKind::Aumid:TargetKind::Shell;
        EnableWindow(state.Control(Directory),state.draft.kind==TargetKind::Shell);
        state.dirty=true;return;
    }
    if(notification!=BN_CLICKED)return;
    switch(id) {
    case IDCANCEL:case Back:NavigateBack(state);break;
    case IDOK:
        if(state.page==Page::Editor)CompleteEdit(state);
        else if(state.page==Page::Advanced)ChangePage(state,Page::Editor);
        else if(state.page==Page::Picker)PickInstalled(state);
        else if(state.page==Page::Apps)StartEdit(state,false);
        break;
    case Close:SendMessageW(state.window,WM_CLOSE,0,0);break;
    case Apps:ChangePage(state,Page::Apps);break;
    case AppearancePage:ChangePage(state,Page::Appearance);break;
    case SettingsPage:ChangePage(state,Page::Settings);break;
    case About:ChangePage(state,Page::About);break;
    case Add:if(state.apps.size()<8)StartEdit(state,true);break;
    case Edit:StartEdit(state,false);break;
    case Advanced:ChangePage(state,Page::Advanced);break;
    case Done:ChangePage(state,Page::Editor);break;
    case Browse:BrowseTarget(state);break;
    case ChooseApp:state.query.clear();ChangePage(state,Page::Picker);ReadInstalledApps(state);break;
    case Pick:PickInstalled(state);break;
    case Save:CompleteEdit(state);break;
    case Remove:
        if(state.apps.size()>1 && MessageBoxW(state.window,
            (std::wstring(Tr(L"Remove ",L"移除 "))+AppDisplayName(state.apps.at(static_cast<size_t>(state.selected)))+L"?").c_str(),
            kAppName,MB_YESNO|MB_ICONQUESTION)==IDYES) {
            auto candidate=state.apps;candidate.erase(candidate.begin()+state.selected);
            if(Persist(state,candidate))BuildPage(state);
        }
        break;
    case Up:case Down: {
        const int destination=state.selected+(id==Up?-1:1);
        if(destination>=0&&destination<static_cast<int>(state.apps.size())) {
            auto candidate=state.apps;std::swap(candidate[state.selected],candidate[destination]);
            if(Persist(state,candidate)){state.selected=destination;BuildPage(state);}
        }
        break;
    }
    case WindowsAppearance:
        if(reinterpret_cast<INT_PTR>(ShellExecuteW(state.window,L"open",L"ms-settings:personalization-colors",nullptr,nullptr,SW_SHOWNORMAL))<=32)
            MessageBoxW(state.window,Tr(L"Windows appearance settings could not be opened.",L"无法打开 Windows 外观设置。"),kAppName,MB_OK|MB_ICONWARNING);
        break;
    }
}

LRESULT CALLBACK OsdProc(HWND window,UINT message,WPARAM wParam,LPARAM lParam) {
    auto* state=reinterpret_cast<State*>(GetWindowLongPtrW(window,GWLP_USERDATA));
    if(message==WM_NCCREATE) {
        auto* create=reinterpret_cast<CREATESTRUCTW*>(lParam);
        auto* owner=static_cast<std::unique_ptr<State>*>(create->lpCreateParams);
        state=owner->release();state->window=window;
        SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(state));
    }
    if(!state)return DefWindowProcW(window,message,wParam,lParam);
    try {
        switch(message) {
        case WM_CREATE:
            state->dpi=GetDpiForWindow(window);
            state->source=std::make_unique<AppearanceSource>(window);
            if(FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,state->factory.GetAddressOf())) ||
                FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,__uuidof(IDWriteFactory),
                    reinterpret_cast<IUnknown**>(state->textFactory.GetAddressOf()))))return -1;
            UpdateFont(*state);UpdateAppearance(*state);BuildPage(*state);return 0;
        case WM_PAINT:Paint(*state);return 0;
        case WM_ERASEBKGND:return 1;
        case WM_SIZE:
            if(state->target)state->target->Resize(D2D1::SizeU(LOWORD(lParam),HIWORD(lParam)));
            UpdateScrollbars(*state);
            return 0;
        case WM_HSCROLL:Scroll(*state,SB_HORZ,LOWORD(wParam));return 0;
        case WM_VSCROLL:Scroll(*state,SB_VERT,LOWORD(wParam));return 0;
        case WM_MOUSEWHEEL:
            Scroll(*state,SB_VERT,GET_WHEEL_DELTA_WPARAM(wParam)>0?SB_LINEUP:SB_LINEDOWN);return 0;
        case WM_DRAWITEM:DrawControl(*state,*reinterpret_cast<DRAWITEMSTRUCT*>(lParam));return TRUE;
        case WM_MEASUREITEM:
            reinterpret_cast<MEASUREITEMSTRUCT*>(lParam)->itemHeight=state->Px(
                reinterpret_cast<MEASUREITEMSTRUCT*>(lParam)->CtlType==ODT_COMBOBOX?32:34);return TRUE;
        // A click on a field's padding focuses its text, as on the text itself.
        case WM_LBUTTONDOWN: {
            const float x=(GET_X_LPARAM(lParam)+state->scrollX)*96.f/state->dpi,y=(GET_Y_LPARAM(lParam)+state->scrollY)*96.f/state->dpi;
            for(const auto& [id,frame]:state->frames)
                if(x>=frame.left && x<frame.right && y>=frame.top && y<frame.bottom) {
                    const HWND field=state->Control(id);
                    if(field && IsWindowEnabled(field)) {SetFocus(field);SendMessageW(field,EM_SETSEL,0,-1);}
                    return 0;
                }
            break;
        }
        case WM_CTLCOLORSTATIC:case WM_CTLCOLOREDIT:case WM_CTLCOLORLISTBOX: {
            HDC dc=reinterpret_cast<HDC>(wParam);
            // A disabled EDIT asks for static colors; it still sits on its field's fill.
            if(message==WM_CTLCOLORSTATIC && state->frames.contains(GetDlgCtrlID(reinterpret_cast<HWND>(lParam)))) {
                SetTextColor(dc,GdiColor(state->appearance.secondary));
                SetBkColor(dc,GdiColor(state->Tile()));
                return reinterpret_cast<LRESULT>(state->inputBrush);
            }
            const bool input=message!=WM_CTLCOLORSTATIC;
            const bool languageLabel=message==WM_CTLCOLORSTATIC && state->page==Page::Settings &&
                reinterpret_cast<HWND>(lParam)==state->Control(FirstLabel);
            SetTextColor(dc,GdiColor(state->appearance.foreground));
            SetBkColor(dc,GdiColor(input||languageLabel?state->Tile():state->appearance.surface));
            return reinterpret_cast<LRESULT>(input||languageLabel?state->inputBrush:state->surfaceBrush);
        }
        case WM_COMMAND:Command(*state,LOWORD(wParam),HIWORD(wParam));return 0;
        case WM_KEYDOWN:if(wParam==VK_ESCAPE){NavigateBack(*state);return 0;}break;
        case WM_SETTINGCHANGE:case WM_THEMECHANGED:case kAppearanceMessage:UpdateAppearance(*state);return 0;
        case kLanguageChangedMessage:BuildPage(*state);return 0;
        case kInstalledAppsMessage: {
            std::unique_ptr<std::vector<InstalledApp>> apps(reinterpret_cast<std::vector<InstalledApp>*>(lParam));
            state->listing=false;state->listed=true;
            state->installed=std::move(*apps);
            state->icons.assign(state->installed.size(),{});
            if(state->page==Page::Picker)Filter(*state);
            return 0;
        }
        case kInstalledIconMessage: {
            std::unique_ptr<InstalledIcon> icon(reinterpret_cast<InstalledIcon*>(lParam));
            if(icon->index<state->icons.size())state->icons[icon->index]=std::move(icon->pixels);
            // Only the arriving icon's row repaints; icons stream in while the user scrolls.
            const HWND list=state->Control(InstalledList);
            const auto row=std::find(state->visible.begin(),state->visible.end(),icon->index);
            RECT bounds{};
            if(state->page==Page::Picker && list && row!=state->visible.end() &&
                SendMessageW(list,LB_GETITEMRECT,static_cast<WPARAM>(row-state->visible.begin()),reinterpret_cast<LPARAM>(&bounds))!=LB_ERR)
                InvalidateRect(list,&bounds,FALSE);
            return 0;
        }
        case WM_DPICHANGED: {
            state->dpi=HIWORD(wParam);
            const auto* suggested=reinterpret_cast<RECT*>(lParam);
            SetWindowPos(window,nullptr,suggested->left,suggested->top,suggested->right-suggested->left,
                suggested->bottom-suggested->top,SWP_NOZORDER|SWP_NOACTIVATE);
            state->target.Reset();state->controlTarget.Reset();UpdateFont(*state);BuildPage(*state);return 0;
        }
        case WM_CLOSE:if(MayLeaveEditor(*state))DestroyWindow(window);return 0;
        case WM_NCDESTROY:
            if(gOsd==window)gOsd=nullptr;
            if(state->focusedEditor && IsWindow(state->notifyOwner))
                PostMessageW(state->notifyOwner,kTargetEditorClosedMessage,0,0);
            SetWindowLongPtrW(window,GWLP_USERDATA,0);delete state;
            return DefWindowProcW(window,message,wParam,lParam);
        }
    } catch(...) {
        if(message==WM_CREATE)return -1;
        MessageBoxW(window,Tr(L"The menu could not complete that action.",
            L"菜单无法完成此操作。"),kAppName,MB_OK|MB_ICONWARNING);
    }
    return DefWindowProcW(window,message,wParam,lParam);
}
} // namespace

void ShowOsdAt(HWND owner,int editorIndex,bool focusedEditor,const AppTarget* preset=nullptr) {
    try {
        if(gOsd&&IsWindow(gOsd)) {
            auto* state=reinterpret_cast<State*>(GetWindowLongPtrW(gOsd,GWLP_USERDATA));
            if(state) {
                if(focusedEditor) {
                    if(!MayLeaveEditor(*state))return;
                    std::wstring error;
                    auto apps=LoadTargets(error);
                    if(editorIndex < -1 || editorIndex >= static_cast<int>(apps.size()) ||
                        (editorIndex == -1 && apps.size() >= 8))return;
                    state->apps=std::move(apps);
                    state->focusedEditor=true;
                    state->selected=std::max(0,editorIndex);
                    StartEdit(*state,editorIndex == -1,preset);
                }
                state->notifyOwner=owner;
            }
            ShowWindow(gOsd,SW_RESTORE);SetForegroundWindow(gOsd);return;
        }
        WNDCLASSEXW cls{sizeof(cls)};
        cls.lpfnWndProc=OsdProc;cls.hInstance=GetModuleHandleW(nullptr);
        cls.hCursor=LoadCursorW(nullptr,IDC_ARROW);cls.hIcon=LoadIconW(cls.hInstance,MAKEINTRESOURCEW(1));
        cls.lpszClassName=kOsdClass;
        if(!RegisterClassExW(&cls)&&GetLastError()!=ERROR_CLASS_ALREADY_EXISTS) {
            MessageBoxW(owner,Tr(L"The quick menu window could not be registered.",L"无法注册快捷菜单窗口。"),kAppName,MB_OK|MB_ICONERROR);
            return;
        }
        WNDCLASSEXW viewport{sizeof(viewport)};
        viewport.lpfnWndProc=ViewportProc;viewport.hInstance=cls.hInstance;
        viewport.hCursor=LoadCursorW(nullptr,IDC_ARROW);viewport.lpszClassName=kViewportClass;
        if(!RegisterClassExW(&viewport)&&GetLastError()!=ERROR_CLASS_ALREADY_EXISTS) {
            MessageBoxW(owner,Tr(L"The quick menu window could not be registered.",L"无法注册快捷菜单窗口。"),kAppName,MB_OK|MB_ICONERROR);
            return;
        }
        auto state=std::make_unique<State>();state->notifyOwner=owner;
        std::wstring error;state->apps=LoadTargets(error);
        if(focusedEditor) {
            if(editorIndex < -1 || editorIndex >= static_cast<int>(state->apps.size()) ||
                (editorIndex == -1 && state->apps.size() >= 8))return;
            state->focusedEditor=true;
            state->adding=editorIndex == -1;
            state->selected=std::max(0,editorIndex);
            state->page=Page::Editor;
            state->draft=state->adding?AppTarget{}:state->apps.at(static_cast<size_t>(editorIndex));
            if(state->adding && preset)state->draft=*preset;
        }
        // A real caption preserves drag, taskbar, system-menu and keyboard accessibility.
        // The 560 x 360 client area holds the visual OSD and secondary task sheets.
        POINT pointer{};GetCursorPos(&pointer);
        MONITORINFO monitor{sizeof(monitor)};GetMonitorInfoW(MonitorFromPoint(pointer,MONITOR_DEFAULTTONEAREST),&monitor);
        const DWORD style=WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_CLIPCHILDREN|WS_HSCROLL|WS_VSCROLL;
        const DWORD extended=WS_EX_CONTROLPARENT;
        const UINT dpi=GetDpiForSystem();
        RECT bounds{0,0,MulDiv(kWidth,static_cast<int>(dpi),96),MulDiv(kHeight,static_cast<int>(dpi),96)};
        AdjustWindowRectExForDpi(&bounds,style,FALSE,extended,dpi);
        const int width=bounds.right-bounds.left,height=bounds.bottom-bounds.top;
        const int x=monitor.rcWork.left+(monitor.rcWork.right-monitor.rcWork.left-width)/2;
        const int y=monitor.rcWork.top+(monitor.rcWork.bottom-monitor.rcWork.top-height)/2;
        gOsd=CreateWindowExW(extended,kOsdClass,Tr(L"Copilot Reel — Quick menu",L"Copilot Reel — 快捷菜单"),
            style,x,y,width,height,nullptr,nullptr,cls.hInstance,&state);
        if(!gOsd) {
            MessageBoxW(owner,Tr(L"The quick menu could not be created.",L"无法创建快捷菜单。"),kAppName,MB_OK|MB_ICONERROR);
            return;
        }
        // WM_CREATE uses the actual monitor DPI. Recenter after its size correction.
        RECT actual{};GetWindowRect(gOsd,&actual);
        SetWindowPos(gOsd,nullptr,monitor.rcWork.left+(monitor.rcWork.right-monitor.rcWork.left-(actual.right-actual.left))/2,
            monitor.rcWork.top+(monitor.rcWork.bottom-monitor.rcWork.top-(actual.bottom-actual.top))/2,0,0,SWP_NOSIZE|SWP_NOZORDER);
        ShowWindow(gOsd,SW_SHOWNORMAL);UpdateWindow(gOsd);SetForegroundWindow(gOsd);
        if(!error.empty())MessageBoxW(gOsd,error.c_str(),kAppName,MB_OK|MB_ICONWARNING);
    } catch(...) {
        MessageBoxW(owner,Tr(L"The quick menu could not be opened.",L"无法打开快捷菜单。"),kAppName,MB_OK|MB_ICONERROR);
    }
}
void ShowOsd(HWND owner) { ShowOsdAt(owner,-1,false); }
void ShowTargetEditor(HWND owner,int index) { ShowOsdAt(owner,index,true); }
void ShowPresetTargetEditor(HWND owner,const AppTarget& preset) { ShowOsdAt(owner,-1,true,&preset); }
void RefreshOsdAppearance() {
    if(gOsd&&IsWindow(gOsd))PostMessageW(gOsd,kAppearanceMessage,0,0);
}
bool OsdOpen() {return gOsd && IsWindow(gOsd);}
bool CloseOsd() {
    const auto window=gOsd;
    if(!window||!IsWindow(window))return true;
    SendMessageW(window,WM_CLOSE,0,0);
    return !IsWindow(window);
}
} // namespace reel
