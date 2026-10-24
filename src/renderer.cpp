#include "renderer.h"
#include "acrylic.h"
#include "artwork.h"
#include "display_names.h"
#include "vi_tokens.h"

#include <d2d1_1.h>
#include <d2d1_1helper.h>
#include <d2d1effects.h>
#include <d3d11.h>
#include <dwrite_2.h>
#include <dwmapi.h>
#include <dxgi.h>
#include <DispatcherQueue.h>
#include <windows.ui.composition.interop.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Numerics.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.Composition.h>
#include <winrt/Windows.UI.Composition.Desktop.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <initializer_list>
#include <map>
#include <memory>
#include <numbers>
#include <optional>
#include <string>
#include <vector>

namespace reel {
namespace {
namespace wc = winrt::Windows::UI::Composition;
namespace wn = winrt::Windows::Foundation::Numerics;
namespace ac = ABI::Windows::UI::Composition;
using winrt::com_ptr;

constexpr float kPi = std::numbers::pi_v<float>;
constexpr float kCenter = vi::kCanvas / 2;
constexpr wchar_t kLanguageTarget[] = L"reel:language";
constexpr wchar_t kSizeTarget[] = L"reel:size";

D2D1_COLOR_F Color(const Rgba& c) { return {c.r, c.g, c.b, c.a}; }
bool Same(const Rgba& a, const Rgba& b) { return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a; }
bool SameAppearance(const Appearance& a, const Appearance& b) {
    return a.dark == b.dark && a.transparency == b.transparency && a.animations == b.animations &&
        a.highContrast == b.highContrast && a.shadowOpacity == b.shadowOpacity &&
        Same(a.surface, b.surface) && Same(a.foreground, b.foreground) && Same(a.secondary, b.secondary) &&
        Same(a.outline, b.outline) && Same(a.tile, b.tile) && Same(a.tileStroke, b.tileStroke) &&
        Same(a.plateStroke, b.plateStroke) && Same(a.ringIdle, b.ringIdle) && Same(a.accent, b.accent) &&
        Same(a.accentText, b.accentText) && Same(a.textOnAccent, b.textOnAccent) &&
        Same(a.selection, b.selection) && Same(a.selectionText, b.selectionText) && Same(a.critical, b.critical) &&
        Same(a.criticalFill, b.criticalFill) && Same(a.textOnCritical, b.textOnCritical);
}
bool DeviceLost(HRESULT hr) {
    return hr == D2DERR_RECREATE_TARGET || hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET;
}
float SlotAngle(size_t index, size_t count) {
    return -kPi / 2 + 2 * kPi * static_cast<float>(index) / static_cast<float>(count);
}
void Detach(const wc::ContainerVisual& parent, const wc::Visual& child) {
    if (!parent || !child) return;
    try { parent.Children().Remove(child); } catch (...) {}
}
} // namespace

struct OrbitRenderer::Impl {
    explicit Impl(HWND hwnd) : window(hwnd) {}

    struct Item {
        PageItem entry;
        ArtworkClass artClass = ArtworkClass::Generic;
        bool plated = false, preferred = false;
        float angle = 0, cx = 0, cy = 0;     // exact orbit center (DIP); hit testing uses the same
        // Where a drag last sent the item and at what scale: a Reflow page starts its items here.
        D2D1_POINT_2F at{};
        float scale = 1;
        bool falling = false;  // dropped into the trash: no Reflow tile starts from it
        std::wstring artKey;
        wc::ContainerVisual root{nullptr};
        wc::SpriteVisual tile{nullptr}, art{nullptr}, artSelected{nullptr};
    };
    // One orbit's contents. The plate, ring and label never belong to a page, so
    // page transitions move only these visuals.
    struct Page {
        PageModel model;
        wc::ContainerVisual root{nullptr};
        wc::SpriteVisual seat{nullptr};
        // Slot-0 adjust chevrons, one sprite per side: 0 = earlier (left), 1 = later (right).
        wc::SpriteVisual chevrons[2]{wc::SpriteVisual{nullptr}, wc::SpriteVisual{nullptr}},
            chevronsActive[2]{wc::SpriteVisual{nullptr}, wc::SpriteVisual{nullptr}};
        wc::CompositionPropertySet seatProps{nullptr};
        std::vector<Item> items;
        int seatIndex = -1;      // item under the seat; -1 when the seat is hidden
        float seatAngle = 0;     // last target angle (unbounded; travel takes the shortest arc)
        unsigned seatTravel = 0; // invalidates the completion of an interrupted travel
        // The lifted item while a tile is dragged (-1 otherwise), the slot opened for it, and
        // whether it is over the armed trash; `trash` is false when nothing may be removed.
        int drag = -1, dragSlot = -1;
        bool overTrash = false, trash = true;
    };
    struct Canvas { wc::CompositionDrawingSurface surface{nullptr}; wn::float2 size{}; };
    struct CenterState { float ringIdle = -1, ringGated = -1, dot = -1, back = -1, trash = -1, armed = -1; };

    HWND window;
    UINT dpi = 96;
    // `closing` covers the exit animation: the tree is still attached, but nothing may
    // re-show content until Hide or the next Show.
    bool ready = false, visible = false, closing = false, backdrop = false, softwareDevice = false;
    bool contentDirty = true, appearanceSet = false, awaitCenter = false;
    unsigned surfaceDraws = 0;
    Appearance appearance;
    Language language = Language::English;
    std::wstring sizeLabel = L"100%";  // the Reel size tile's monogram
    std::wstring lastError, lastTransition = L"none", textFamily;
    std::wstring backdropStatus = L"solid (transparency disabled)";
    // Composition completion handlers outlive nothing: they check this token first.
    std::shared_ptr<int> life = std::make_shared<int>(0);

    winrt::Windows::System::DispatcherQueueController queueController{nullptr};
    wc::Compositor compositor{nullptr};
    wc::Desktop::DesktopWindowTarget target{nullptr};
    wc::CompositionGraphicsDevice graphicsDevice{nullptr};
    wc::CompositionCapabilities capabilities{nullptr};
    winrt::event_token capabilitiesChanged{};
    com_ptr<ID3D11Device> d3d;
    com_ptr<ID2D1Factory1> d2dFactory;
    com_ptr<ID2D1Device> d2d;
    com_ptr<IDWriteFactory2> dwrite;
    com_ptr<IDWriteFontFallback> fallback;
    com_ptr<IDWriteTextFormat> labelFormat, monogramFormat, sizeMonogramFormat;
    float capRatio = .7f;

    wc::CompositionEasingFunction linear{nullptr}, decelerate{nullptr}, accelerate{nullptr}, drill{nullptr};
    wc::ContainerVisual pixelRoot{nullptr}, dpiRoot{nullptr}, root{nullptr}, pagesLayer{nullptr}, handoffLayer{nullptr};
    wc::SpriteVisual backdropVisual{nullptr}, base{nullptr}, ringIdle{nullptr}, ringGated{nullptr}, gateDot{nullptr}, back{nullptr},
        trash{nullptr}, trashArmed{nullptr};
    wc::SpriteVisual labels[2]{wc::SpriteVisual{nullptr}, wc::SpriteVisual{nullptr}};
    wc::CompositionDrawingSurface labelSurfaces[2]{wc::CompositionDrawingSurface{nullptr}, wc::CompositionDrawingSurface{nullptr}};
    int activeLabel = 0;
    std::wstring labelText;
    wc::CompositionSurfaceBrush tileBrush{nullptr}, seatBrush{nullptr}, chevronBrush{nullptr},
        chevronActiveBrush{nullptr}, noiseBrush{nullptr};
    wn::float2 itemSize{}, seatSize{}, chevronSize{};
    std::map<std::wstring, wc::CompositionSurfaceBrush> art;  // per-item artwork, by ArtKey
    CenterState center;
    std::shared_ptr<Page> page;
    std::vector<std::shared_ptr<Page>> leaving;

    // ---- Units, timing and animation helpers.

    float Scale() const { return static_cast<float>(dpi) / 96.f; }
    // Device-pixel grid for drawing and visual offsets; hit testing keeps exact geometry.
    float SnapDip(float value) const { return std::round(value * Scale()) / Scale(); }
    float StrokeDip(float width) const { return (std::max)(1.f, std::round(width * Scale())) / Scale(); }
    bool Animated() const { return appearance.animations && !appearance.highContrast; }
    // Reduced motion and high contrast run the same animations at 1 ms.
    std::chrono::milliseconds Ms(unsigned value) const {
        return std::chrono::milliseconds(Animated() ? value : vi::kReducedMotionMs);
    }

    wc::CompositionEasingFunction Bezier(const vi::Bezier& curve) {
        return compositor.CreateCubicBezierEasingFunction({curve.x1, curve.y1}, {curve.x2, curve.y2});
    }
    // From the current presentation value unless `from` is given (then held through the delay).
    wc::ScalarKeyFrameAnimation ScalarAnimation(std::optional<float> from, float to, unsigned ms,
        const wc::CompositionEasingFunction& ease, unsigned delay = 0) {
        auto animation = compositor.CreateScalarKeyFrameAnimation();
        if (from) {
            animation.InsertKeyFrame(0, *from);
            animation.DelayBehavior(wc::AnimationDelayBehavior::SetInitialValueBeforeDelay);
        } else {
            animation.InsertExpressionKeyFrame(0, L"this.StartingValue");
        }
        animation.InsertKeyFrame(1, to, ease);
        animation.Duration(Ms(ms));
        if (delay && Animated()) animation.DelayTime(std::chrono::milliseconds(delay));
        return animation;
    }
    wc::Vector3KeyFrameAnimation ScaleAnimation(std::optional<float> from, float to, unsigned ms,
        const wc::CompositionEasingFunction& ease, unsigned delay = 0) {
        auto animation = compositor.CreateVector3KeyFrameAnimation();
        if (from) {
            animation.InsertKeyFrame(0, {*from, *from, 1});
            animation.DelayBehavior(wc::AnimationDelayBehavior::SetInitialValueBeforeDelay);
        } else {
            animation.InsertExpressionKeyFrame(0, L"this.StartingValue");
        }
        animation.InsertKeyFrame(1, {to, to, 1}, ease);
        animation.Duration(Ms(ms));
        if (delay && Animated()) animation.DelayTime(std::chrono::milliseconds(delay));
        return animation;
    }
    // Opacity to `value`: animated from the presentation value, or set at once.
    void Opacity(const wc::Visual& visual, float value, unsigned ms, bool animate,
                 const wc::CompositionEasingFunction& ease, unsigned delay = 0) {
        if (!visual) return;
        if (animate) visual.StartAnimation(L"Opacity", ScalarAnimation(std::nullopt, value, ms, ease, delay));
        else { visual.StopAnimation(L"Opacity"); visual.Opacity(value); }
    }
    void Opacity(const wc::Visual& visual, float value, unsigned ms, bool animate) {
        Opacity(visual, value, ms, animate, linear);
    }
    void Settle(const wc::Visual& visual, float opacity) {
        if (!visual) return;
        visual.StopAnimation(L"Opacity");
        visual.StopAnimation(L"Scale");
        visual.Opacity(opacity);
        visual.Scale({1, 1, 1});
    }
    template <class Done> void OnCompleted(const wc::CompositionScopedBatch& batch, Done done) {
        std::weak_ptr<int> alive = life;
        batch.Completed([alive, done](auto&&, auto&&) mutable {
            if (!alive.lock()) return;
            try { done(); } catch (...) {}
        });
    }

    // ---- Devices, text and material.

    void Initialize() {
        // An existing queue is borrowed. Otherwise its controller remains alive
        // for this HWND's UI-thread lifetime.
        if (!winrt::Windows::System::DispatcherQueue::GetForCurrentThread()) {
            DispatcherQueueOptions options{sizeof(DispatcherQueueOptions), DQTYPE_THREAD_CURRENT, DQTAT_COM_NONE};
            winrt::check_hresult(CreateDispatcherQueueController(options,
                reinterpret_cast<ABI::Windows::System::IDispatcherQueueController**>(winrt::put_abi(queueController))));
        }
        compositor = wc::Compositor();
        winrt::check_hresult(compositor.as<ac::Desktop::ICompositorDesktopInterop>()->CreateDesktopWindowTarget(window, FALSE,
            reinterpret_cast<ac::Desktop::IDesktopWindowTarget**>(winrt::put_abi(target))));
        linear = compositor.CreateLinearEasingFunction();
        decelerate = Bezier(vi::kDecelerate);
        accelerate = Bezier(vi::kAccelerate);
        drill = Bezier(vi::kDrillCurve);

        pixelRoot = compositor.CreateContainerVisual();
        dpiRoot = compositor.CreateContainerVisual();
        root = compositor.CreateContainerVisual();
        root.Size({vi::kCanvas, vi::kCanvas});
        root.CenterPoint({kCenter, kCenter, 0});
        dpiRoot.Children().InsertAtTop(root);
        pixelRoot.Children().InsertAtTop(dpiRoot);
        backdropVisual = compositor.CreateSpriteVisual();
        backdropVisual.Size({vi::kPlateRadius * 2, vi::kPlateRadius * 2});
        backdropVisual.Offset({kCenter - vi::kPlateRadius, kCenter - vi::kPlateRadius, 0});
        auto disk = compositor.CreateEllipseGeometry();
        disk.Center({vi::kPlateRadius, vi::kPlateRadius});
        disk.Radius({vi::kPlateRadius, vi::kPlateRadius});
        backdropVisual.Clip(compositor.CreateGeometricClip(disk));
        base = compositor.CreateSpriteVisual();
        ringIdle = compositor.CreateSpriteVisual();
        ringGated = compositor.CreateSpriteVisual();
        gateDot = compositor.CreateSpriteVisual();
        pagesLayer = compositor.CreateContainerVisual();
        handoffLayer = compositor.CreateContainerVisual();
        back = compositor.CreateSpriteVisual();
        trash = compositor.CreateSpriteVisual();
        trashArmed = compositor.CreateSpriteVisual();
        for (auto& label : labels) label = compositor.CreateSpriteVisual();
        // Back to front: material, plate (shadow, fill, rim), center ring, orbit pages, the
        // handoff clone, then the center's back chevron, drag trash (over the shrunken tile) and name.
        for (const wc::Visual& visual : std::initializer_list<wc::Visual>{backdropVisual, base, ringIdle, ringGated,
                 gateDot, pagesLayer, handoffLayer, back, trash, trashArmed, labels[0], labels[1]})
            root.Children().InsertAtTop(visual);

        CreateDevices();
        winrt::check_hresult(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory2),
            reinterpret_cast<IUnknown**>(dwrite.put())));
        CreateTextFormats();
        SubscribeCapabilities();
        ready = true;
        ResizeRoot();
        ApplyMaterial();
        RebuildAll();
        // Root is attached by Show. Hidden state has no active composition tree.
    }

    void CreateDevices() {
        d3d = nullptr;
        d2d = nullptr;
        softwareDevice = false;
        UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
        if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, nullptr, 0,
            D3D11_SDK_VERSION, d3d.put(), nullptr, nullptr))) {
            d3d = nullptr;
            softwareDevice = true;
            winrt::check_hresult(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags, nullptr, 0,
                D3D11_SDK_VERSION, d3d.put(), nullptr, nullptr));
        }
        if (!d2dFactory) {
            D2D1_FACTORY_OPTIONS options{};
            winrt::check_hresult(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                __uuidof(ID2D1Factory1), &options, d2dFactory.put_void()));
        }
        winrt::check_hresult(d2dFactory->CreateDevice(d3d.as<IDXGIDevice>().get(), d2d.put()));
        if (!graphicsDevice) {
            winrt::check_hresult(compositor.as<ac::ICompositorInterop>()->CreateGraphicsDevice(d2d.get(),
                reinterpret_cast<ac::ICompositionGraphicsDevice**>(winrt::put_abi(graphicsDevice))));
        } else {
            winrt::check_hresult(graphicsDevice.as<ac::ICompositionGraphicsDeviceInterop>()->SetRenderingDevice(d2d.get()));
        }
    }

    // Recreates the rendering device in place; every surface is drawn again.
    void RecoverDevice() {
        ReleaseArtwork(nullptr);
        art.clear();
        noiseBrush = nullptr;
        CreateDevices();
        ApplyMaterial();
        contentDirty = true;
        RebuildAll();
    }

    void CreateTextFormats() {
        if (!fallback) {
            // Han and CJK punctuation use Microsoft YaHei UI instead of per-character
            // JhengHei UI / Yu Gothic UI; the system fallback follows for everything else.
            com_ptr<IDWriteFontFallbackBuilder> builder;
            com_ptr<IDWriteFontFallback> system;
            DWRITE_UNICODE_RANGE ranges[std::size(vi::kCjkRanges)]{};
            for (size_t i = 0; i != std::size(ranges); ++i) ranges[i] = {vi::kCjkRanges[i].first, vi::kCjkRanges[i].last};
            const wchar_t* family[] = {vi::kCjkFamily};
            if (SUCCEEDED(dwrite->CreateFontFallbackBuilder(builder.put())) &&
                SUCCEEDED(dwrite->GetSystemFontFallback(system.put())) &&
                SUCCEEDED(builder->AddMapping(ranges, static_cast<UINT32>(std::size(ranges)), family, 1)) &&
                SUCCEEDED(builder->AddMappings(system.get())))
                builder->CreateFontFallback(fallback.put());
        }
        // The WWS family of the Windows 11 variable face; static Segoe UI elsewhere.
        textFamily = vi::kTextFallbackFamily;
        com_ptr<IDWriteFontCollection> fonts;
        com_ptr<IDWriteFontFamily> family;
        com_ptr<IDWriteFont> font;
        UINT32 index = 0;
        BOOL exists = FALSE;
        if (SUCCEEDED(dwrite->GetSystemFontCollection(fonts.put())) &&
            SUCCEEDED(fonts->FindFamilyName(vi::kTextFamily, &index, &exists)) && exists) textFamily = vi::kTextFamily;
        else if (fonts) fonts->FindFamilyName(vi::kTextFallbackFamily, &index, &exists);
        if (exists && SUCCEEDED(fonts->GetFontFamily(index, family.put())) &&
            SUCCEEDED(family->GetFirstMatchingFont(static_cast<DWRITE_FONT_WEIGHT>(vi::kTextWeight),
                DWRITE_FONT_STRETCH_NORMAL, DWRITE_FONT_STYLE_NORMAL, font.put()))) {
            DWRITE_FONT_METRICS metrics{};
            font->GetMetrics(&metrics);
            if (metrics.designUnitsPerEm) capRatio = static_cast<float>(metrics.capHeight) / metrics.designUnitsPerEm;
        }
        labelFormat = TextFormat(vi::kLabelSize);
        monogramFormat = TextFormat(vi::kMonogramSize);
        sizeMonogramFormat = TextFormat(vi::kSizeMonogramSize);
    }

    com_ptr<IDWriteTextFormat> TextFormat(float size) {
        com_ptr<IDWriteTextFormat> result;
        winrt::check_hresult(dwrite->CreateTextFormat(textFamily.c_str(), nullptr,
            static_cast<DWRITE_FONT_WEIGHT>(vi::kTextWeight), DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
            size, LocaleName(language), result.put()));
        winrt::check_hresult(result->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER));
        winrt::check_hresult(result->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR));
        winrt::check_hresult(result->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP));
        DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
        com_ptr<IDWriteInlineObject> ellipsis;
        winrt::check_hresult(dwrite->CreateEllipsisTrimmingSign(result.get(), ellipsis.put()));
        winrt::check_hresult(result->SetTrimming(&trimming, ellipsis.get()));
        if (fallback) result.as<IDWriteTextFormat1>()->SetFontFallback(fallback.get());
        return result;
    }

    void SubscribeCapabilities() {
        try {
            auto factory = winrt::get_activation_factory<wc::CompositionCapabilities,
                ac::Desktop::ICompositionCapabilitiesInteropFactory>();
            winrt::check_hresult(factory->GetForWindow(window,
                reinterpret_cast<ac::ICompositionCapabilities**>(winrt::put_abi(capabilities))));
            // The event's thread is unspecified (WinUI marshals it too): all renderer
            // state belongs to the UI thread, so the work is queued there.
            std::weak_ptr<int> alive = life;
            auto queue = winrt::Windows::System::DispatcherQueue::GetForCurrentThread();
            capabilitiesChanged = capabilities.Changed([this, alive, queue](auto&&, auto&&) {
                if (!queue || !alive.lock()) return;
                queue.TryEnqueue([this, alive] {
                    if (!alive.lock()) return;
                    Run([&] {
                        ApplyMaterial();
                        if (visible && !closing && contentDirty) RebuildAll();
                    });
                });
            });
        } catch (...) { capabilities = nullptr; }
    }

    wc::CompositionSurfaceBrush NoiseBrush() {
        if (noiseBrush) return noiseBrush;
        constexpr float edge = static_cast<float>(vi::kAcrylicNoiseSize);
        auto surface = graphicsDevice.CreateDrawingSurface({edge, edge},
            winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized,
            winrt::Windows::Graphics::DirectX::DirectXAlphaMode::Premultiplied);
        const auto pixels = AcrylicNoisePixels();
        auto interop = surface.as<ac::ICompositionDrawingSurfaceInterop>();
        POINT offset{};
        com_ptr<ID2D1DeviceContext> context;
        winrt::check_hresult(interop->BeginDraw(nullptr, __uuidof(ID2D1DeviceContext), context.put_void(), &offset));
        HRESULT drawResult = S_OK;
        try {
            com_ptr<ID2D1Bitmap1> bitmap;
            winrt::check_hresult(context->CreateBitmap(D2D1::SizeU(vi::kAcrylicNoiseSize, vi::kAcrylicNoiseSize),
                pixels.data(), vi::kAcrylicNoiseSize * 4, D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE,
                    D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)), bitmap.put()));
            context->SetDpi(96, 96);
            context->SetTransform(D2D1::Matrix3x2F::Identity());
            context->Clear(D2D1::ColorF(0, 0.f));
            context->DrawBitmap(bitmap.get(), D2D1::RectF(static_cast<float>(offset.x), static_cast<float>(offset.y),
                offset.x + edge, offset.y + edge), 1.f, D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
        } catch (...) { drawResult = winrt::to_hresult(); }
        const HRESULT endResult = interop->EndDraw();
        winrt::check_hresult(drawResult);
        winrt::check_hresult(endResult);
        noiseBrush = compositor.CreateSurfaceBrush(surface);
        noiseBrush.Stretch(wc::CompositionStretch::None);
        return noiseBrush;
    }

    // Frosted only when Windows transparency is on (or forced for captures), contrast
    // is off and effects are supported and fast; the solid plate otherwise.
    void ApplyMaterial() {
        const bool previous = backdrop;
        backdrop = false;
        backdropVisual.Brush(nullptr);
        BOOL enable = FALSE;
        DwmSetWindowAttribute(window, DWMWA_USE_HOSTBACKDROPBRUSH, &enable, sizeof(enable));
        if (!appearance.transparency || appearance.highContrast) {
            backdropStatus = appearance.highContrast ? L"solid (high contrast)" : L"solid (transparency disabled)";
        } else if (softwareDevice) {
            backdropStatus = L"solid (software rendering fallback)";
        } else {
            try {
                if (capabilities && (!capabilities.AreEffectsSupported() || !capabilities.AreEffectsFast()))
                    winrt::throw_hresult(E_NOTIMPL);
                enable = TRUE;
                winrt::check_hresult(DwmSetWindowAttribute(window, DWMWA_USE_HOSTBACKDROPBRUSH, &enable, sizeof(enable)));
                backdropVisual.Brush(CreateAcrylicBrush(compositor, appearance.dark, NoiseBrush()));
                backdrop = true;
                backdropStatus = L"acrylic (host backdrop + luminosity + tint + noise)";
            } catch (const winrt::hresult_error& e) {
                backdropVisual.Brush(nullptr);
                enable = FALSE;
                DwmSetWindowAttribute(window, DWMWA_USE_HOSTBACKDROPBRUSH, &enable, sizeof(enable));
                backdropStatus = L"solid (backdrop unavailable: " + std::wstring(e.message().c_str()) + L")";
            }
        }
        if (backdrop != previous) contentDirty = true;
    }

    void ResizeRoot() {
        const float scale = Scale();
        const float edge = std::ceil(vi::kCanvas * scale);
        pixelRoot.Size({edge, edge});
        dpiRoot.Size({vi::kCanvas, vi::kCanvas});
        dpiRoot.Scale({scale, scale, 1.0f});
    }

    // ---- Surfaces and drawing.

    // Surfaces have an even number of device pixels on each axis, so their center is a
    // pixel boundary and a snapped center places the whole surface on the grid.
    Canvas MakeCanvas(float halfWidth, float halfHeight) {
        const float scale = Scale();
        const float width = 2 * std::ceil(halfWidth * scale), height = 2 * std::ceil(halfHeight * scale);
        Canvas canvas;
        canvas.surface = graphicsDevice.CreateDrawingSurface({width, height},
            winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized,
            winrt::Windows::Graphics::DirectX::DirectXAlphaMode::Premultiplied);
        canvas.size = {width / scale, height / scale};
        return canvas;
    }

    template <class Draw> void Paint(const wc::CompositionDrawingSurface& surface, Draw&& draw) {
        auto interop = surface.as<ac::ICompositionDrawingSurfaceInterop>();
        POINT offset{};
        com_ptr<ID2D1DeviceContext> context;
        winrt::check_hresult(interop->BeginDraw(nullptr, __uuidof(ID2D1DeviceContext), context.put_void(), &offset));
        // EndDraw is required after every successful BeginDraw, including errors.
        HRESULT drawResult = S_OK;
        try {
            context->SetDpi(static_cast<float>(dpi), static_cast<float>(dpi));
            const float toDip = 96.0f / static_cast<float>(dpi);
            context->SetTransform(D2D1::Matrix3x2F::Translation(offset.x * toDip, offset.y * toDip));
            context->Clear(D2D1::ColorF(0, 0.0f));
            context->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
            draw(context.get());
        } catch (...) { drawResult = winrt::to_hresult(); }
        const HRESULT endResult = interop->EndDraw();
        winrt::check_hresult(drawResult);
        winrt::check_hresult(endResult);
        ++surfaceDraws;
    }

    com_ptr<ID2D1SolidColorBrush> SolidBrush(ID2D1DeviceContext* context, const D2D1_COLOR_F& color) {
        com_ptr<ID2D1SolidColorBrush> brush;
        winrt::check_hresult(context->CreateSolidColorBrush(color, brush.put()));
        return brush;
    }

    wc::SpriteVisual MakeSprite(const wc::CompositionBrush& brush, wn::float2 size, D2D1_POINT_2F centerDip) {
        auto sprite = compositor.CreateSpriteVisual();
        sprite.Brush(brush);
        sprite.Size(size);
        sprite.Offset({SnapDip(centerDip.x - size.x / 2), SnapDip(centerDip.y - size.y / 2), 0});
        sprite.CenterPoint({size.x / 2, size.y / 2, 0});
        return sprite;
    }
    void Place(const wc::SpriteVisual& sprite, const Canvas& canvas, D2D1_POINT_2F centerDip) {
        sprite.Brush(compositor.CreateSurfaceBrush(canvas.surface));
        sprite.Size(canvas.size);
        sprite.Offset({SnapDip(centerDip.x - canvas.size.x / 2), SnapDip(centerDip.y - canvas.size.y / 2), 0});
        sprite.CenterPoint({canvas.size.x / 2, canvas.size.y / 2, 0});
    }

    // One line centered on the cap height (not the line box), ellipsis beyond `width`.
    void DrawCentered(ID2D1DeviceContext* context, const std::wstring& text, IDWriteTextFormat* format,
                      float size, D2D1_POINT_2F centerDip, float width, const D2D1_COLOR_F& color) {
        if (text.empty()) return;
        com_ptr<IDWriteTextLayout> layout;
        winrt::check_hresult(dwrite->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), format,
            width, size * 4, layout.put()));
        DWRITE_LINE_METRICS line{};
        UINT32 lines = 0;
        layout->GetLineMetrics(&line, 1, &lines);
        const float baseline = SnapDip(centerDip.y + capRatio * size / 2);
        context->DrawTextLayout({centerDip.x - width / 2, baseline - line.baseline}, layout.get(),
            SolidBrush(context, color).get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
    }

    void DrawBase(ID2D1DeviceContext* context, wn::float2 size) {
        const D2D1_POINT_2F c{size.x / 2, size.y / 2};
        const float radius = vi::kPlateRadius;
        if (appearance.shadowOpacity > 0) {
            // One soft directional flyout shadow, baked once per DPI and theme.
            com_ptr<ID2D1CommandList> disk;
            winrt::check_hresult(context->CreateCommandList(disk.put()));
            {
                // Restores the surface target and transform even if recording throws.
                struct Restore {
                    ID2D1DeviceContext* context;
                    com_ptr<ID2D1Image> target;
                    D2D1_MATRIX_3X2_F transform{};
                    ~Restore() { context->SetTarget(target.get()); context->SetTransform(transform); }
                } restore{context};
                context->GetTarget(restore.target.put());
                context->GetTransform(&restore.transform);
                context->SetTarget(disk.get());
                context->SetTransform(D2D1::Matrix3x2F::Identity());
                context->FillEllipse(D2D1::Ellipse(c, radius, radius), SolidBrush(context, D2D1::ColorF(0, 1.f)).get());
            }
            winrt::check_hresult(disk->Close());
            com_ptr<ID2D1Effect> shadow;
            winrt::check_hresult(context->CreateEffect(CLSID_D2D1Shadow, shadow.put()));
            shadow->SetInput(0, disk.get());
            winrt::check_hresult(shadow->SetValue(D2D1_SHADOW_PROP_BLUR_STANDARD_DEVIATION, vi::kShadowBlur / 2));
            winrt::check_hresult(shadow->SetValue(D2D1_SHADOW_PROP_COLOR, D2D1::Vector4F(0, 0, 0, appearance.shadowOpacity)));
            const auto shadowOffset = D2D1::Point2F(0, vi::kShadowOffsetY);
            context->DrawImage(shadow.get(), &shadowOffset);
            // Knock the disk out: nothing of the shadow remains under a translucent plate.
            context->SetPrimitiveBlend(D2D1_PRIMITIVE_BLEND_COPY);
            context->FillEllipse(D2D1::Ellipse(c, radius - 1, radius - 1), SolidBrush(context, D2D1::ColorF(0, 0.f)).get());
            context->SetPrimitiveBlend(D2D1_PRIMITIVE_BLEND_SOURCE_OVER);
        }
        // Frosted plates take their fill from the acrylic visual beneath.
        if (!backdrop) context->FillEllipse(D2D1::Ellipse(c, radius, radius), SolidBrush(context, Color(appearance.surface)).get());
        const float rim = StrokeDip(appearance.highContrast ? vi::kPlateStrokeHighContrast : vi::kPlateStroke);
        context->DrawEllipse(D2D1::Ellipse(c, radius - rim / 2, radius - rim / 2),
            SolidBrush(context, Color(appearance.plateStroke)).get(), rim);
    }

    // Tile and seat edges sit on device pixels at every reel DPI (the canvas center is a pixel boundary).
    void DrawTile(ID2D1DeviceContext* context, wn::float2 size, bool fill) {
        const float h = size.x / 2, t = SnapDip(vi::kTile / 2);
        const auto tile = D2D1::RectF(h - t, h - t, h + t, h + t);
        if (fill) context->FillRoundedRectangle(D2D1::RoundedRect(tile, vi::kTileRadius, vi::kTileRadius),
            SolidBrush(context, Color(appearance.tile)).get());
        const float w = StrokeDip(vi::kTileStroke);
        context->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(tile.left + w / 2, tile.top + w / 2,
            tile.right - w / 2, tile.bottom - w / 2), vi::kTileRadius - w / 2, vi::kTileRadius - w / 2),
            SolidBrush(context, Color(appearance.tileStroke)).get(), w);
    }

    void DrawSeat(ID2D1DeviceContext* context, wn::float2 size) {
        const float h = size.x / 2, s = SnapDip(vi::kSeat / 2);
        const auto seat = D2D1::RectF(h - s, h - s, h + s, h + s);
        context->FillRoundedRectangle(D2D1::RoundedRect(seat, vi::kSeatRadius, vi::kSeatRadius),
            SolidBrush(context, Color(appearance.selection)).get());
        const float w = StrokeDip(appearance.highContrast ? vi::kSeatStrokeHighContrast : vi::kSeatStroke);
        context->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(seat.left + w / 2, seat.top + w / 2,
            seat.right - w / 2, seat.bottom - w / 2), vi::kSeatRadius - w / 2, vi::kSeatRadius - w / 2),
            SolidBrush(context, Color(appearance.highContrast ? appearance.selectionText : appearance.accent)).get(), w);
    }

    // Vendor light/dark variants follow the actual plate: high-contrast themes keep the
    // app's light/dark mode but paint the plate with their own window color.
    bool DarkPlate() const {
        const auto& plate = appearance.surface;
        return appearance.highContrast ? plate.r * .2126f + plate.g * .7152f + plate.b * .0722f < .5f : appearance.dark;
    }
    void DrawItemArt(ID2D1DeviceContext* context, wn::float2 size, const AppTarget& visual, bool preferred, bool selected) {
        const float h = size.x / 2, t = SnapDip(vi::kTile / 2);
        const auto tile = D2D1::RectF(h - t, h - t, h + t, h + t);
        const auto glyph = Color(selected && appearance.highContrast ? appearance.selectionText : appearance.foreground);
        const bool darkPlate = DarkPlate();
        if (visual.target == kLanguageTarget) {
            DrawCentered(context, language == Language::English ? L"EN" : L"ZH", monogramFormat.get(),
                vi::kMonogramSize, {h, h}, vi::kTile, glyph);
        } else if (visual.target == kSizeTarget) {
            DrawCentered(context, sizeLabel.c_str(), sizeMonogramFormat.get(), vi::kSizeMonogramSize, {h, h}, vi::kTile, glyph);
        } else {
            DrawArtwork(context, visual, tile, ArtworkStyle{static_cast<float>(dpi), darkPlate,
                appearance.highContrast, glyph});
            // Plated marks replace the neutral tile, so they carry its hairline themselves.
            if (ClassifyArtwork(visual) == ArtworkClass::Plated) DrawTile(context, size, false);
        }
        if (preferred) {
            // Accent disc with a plate-colored ring that reads as a cutout.
            const D2D1_POINT_2F badge{h + vi::kBadgeOffset, h - vi::kBadgeOffset};
            context->FillEllipse(D2D1::Ellipse(badge, vi::kBadgeRingRadius, vi::kBadgeRingRadius),
                SolidBrush(context, Color(selected ? appearance.selection : appearance.surface)).get());
            context->FillEllipse(D2D1::Ellipse(badge, vi::kBadgeRadius, vi::kBadgeRadius),
                SolidBrush(context, Color(appearance.accent)).get());
            DrawGlyph(context, Glyph::Check, badge, 2 * vi::kBadgeRadius, Color(appearance.textOnAccent),
                static_cast<float>(dpi));
        }
    }

    std::wstring ArtKey(const AppTarget& visual, bool preferred, bool selected) const {
        std::wstring key = selected ? L"S" : L"N";
        key += preferred ? L"P|" : L"-|";
        // The artwork source, not the target: catalog entries without a target share an empty one.
        // A Shell icon that appears later (a drive or package becoming available) changes it.
        key += ArtworkIdentity(visual, DarkPlate());
        key += L"|" + std::to_wstring(static_cast<int>(ClassifyArtwork(visual)));
        if (visual.target == kLanguageTarget) key += language == Language::English ? L"|EN" : L"|ZH";
        if (visual.target == kSizeTarget) key += L"|" + sizeLabel;
        return key;
    }
    // Item artwork is rasterized once per identity, theme and DPI; selection never redraws it.
    wc::CompositionSurfaceBrush ArtBrush(const AppTarget& visual, bool preferred, bool selected) {
        const auto key = ArtKey(visual, preferred, selected);
        if (const auto found = art.find(key); found != art.end()) return found->second;
        const auto canvas = MakeCanvas(itemSize.x / 2, itemSize.y / 2);
        Paint(canvas.surface, [&](ID2D1DeviceContext* context) { DrawItemArt(context, canvas.size, visual, preferred, selected); });
        auto brush = compositor.CreateSurfaceBrush(canvas.surface);
        art.emplace(key, brush);
        return brush;
    }
    // The selected variant differs only in the badge ring and in high-contrast glyph color.
    bool HasSelectedVariant(const Item& item) const { return item.preferred || appearance.highContrast; }

    void BuildStatic() {
        const D2D1_POINT_2F c{kCenter, kCenter};
        auto canvas = MakeCanvas(kCenter, kCenter);
        Paint(canvas.surface, [&](ID2D1DeviceContext* context) { DrawBase(context, canvas.size); });
        Place(base, canvas, c);
        const float ringHalf = vi::kCenterZoneRadius + 4;
        canvas = MakeCanvas(ringHalf, ringHalf);
        Paint(canvas.surface, [&](ID2D1DeviceContext* context) {
            const D2D1_POINT_2F rc{canvas.size.x / 2, canvas.size.y / 2};
            context->DrawEllipse(D2D1::Ellipse(rc, vi::kCenterZoneRadius, vi::kCenterZoneRadius),
                SolidBrush(context, Color(appearance.ringIdle)).get(), StrokeDip(vi::kRingStroke));
        });
        Place(ringIdle, canvas, c);
        canvas = MakeCanvas(ringHalf, ringHalf);
        Paint(canvas.surface, [&](ID2D1DeviceContext* context) {
            const D2D1_POINT_2F rc{canvas.size.x / 2, canvas.size.y / 2};
            context->DrawEllipse(D2D1::Ellipse(rc, vi::kCenterZoneRadius, vi::kCenterZoneRadius),
                SolidBrush(context, Color(appearance.secondary)).get(), StrokeDip(vi::kRingGatedStroke));
        });
        Place(ringGated, canvas, c);
        canvas = MakeCanvas(vi::kGateDotRadius + 2, vi::kGateDotRadius + 2);
        Paint(canvas.surface, [&](ID2D1DeviceContext* context) {
            context->FillEllipse(D2D1::Ellipse({canvas.size.x / 2, canvas.size.y / 2}, vi::kGateDotRadius, vi::kGateDotRadius),
                SolidBrush(context, Color(appearance.secondary)).get());
        });
        Place(gateDot, canvas, c);
        canvas = MakeCanvas(vi::kBackChevronEm, vi::kBackChevronEm);
        Paint(canvas.surface, [&](ID2D1DeviceContext* context) {
            DrawGlyph(context, Glyph::BackChevron, {canvas.size.x / 2, canvas.size.y / 2}, vi::kBackChevronEm,
                Color(appearance.secondary), static_cast<float>(dpi));
        });
        Place(back, canvas, c);
        // The drag trash: a critical glyph inside a critical ring at the center-zone edge; armed,
        // a filled disc with the glyph in textOnCritical covers the idle glyph and keeps the ring.
        const float trashHalf = vi::kCenterZoneRadius + 2;
        for (const bool armed : {false, true}) {
            canvas = MakeCanvas(trashHalf, trashHalf);
            Paint(canvas.surface, [&](ID2D1DeviceContext* context) {
                const D2D1_POINT_2F tc{canvas.size.x / 2, canvas.size.y / 2};
                if (armed) {
                    context->FillEllipse(D2D1::Ellipse(tc, vi::kTrashArmedRadius, vi::kTrashArmedRadius),
                        SolidBrush(context, Color(appearance.criticalFill)).get());
                    DrawGlyph(context, Glyph::Remove, tc, vi::kTrashEm, Color(appearance.textOnCritical), static_cast<float>(dpi));
                    return;
                }
                const float w = StrokeDip(vi::kTrashRingStroke), r = vi::kCenterZoneRadius - w / 2;
                context->DrawEllipse(D2D1::Ellipse(tc, r, r), SolidBrush(context, Color(appearance.critical)).get(), w);
                DrawGlyph(context, Glyph::Remove, tc, vi::kTrashEm, Color(appearance.critical), static_cast<float>(dpi));
            });
            Place(armed ? trashArmed : trash, canvas, c);
        }

        const float itemHalf = vi::kBadgeOffset + vi::kBadgeRingRadius + 1.5f;
        canvas = MakeCanvas(itemHalf, itemHalf);
        itemSize = canvas.size;
        Paint(canvas.surface, [&](ID2D1DeviceContext* context) { DrawTile(context, canvas.size, true); });
        tileBrush = compositor.CreateSurfaceBrush(canvas.surface);
        canvas = MakeCanvas(vi::kSeat / 2 + 2, vi::kSeat / 2 + 2);
        seatSize = canvas.size;
        Paint(canvas.surface, [&](ID2D1DeviceContext* context) { DrawSeat(context, canvas.size); });
        seatBrush = compositor.CreateSurfaceBrush(canvas.surface);
        for (const bool active : {false, true}) {
            canvas = MakeCanvas(vi::kChevronOffset + vi::kChevronEm, vi::kChevronEm);
            chevronSize = canvas.size;
            Paint(canvas.surface, [&](ID2D1DeviceContext* context) {
                const auto color = Color(active ? appearance.foreground : appearance.secondary);
                const float x = canvas.size.x / 2, y = canvas.size.y / 2;
                DrawGlyph(context, Glyph::ChevronLeftSmall, {x - vi::kChevronOffset, y}, vi::kChevronEm, color, static_cast<float>(dpi));
                DrawGlyph(context, Glyph::ChevronRightSmall, {x + vi::kChevronOffset, y}, vi::kChevronEm, color, static_cast<float>(dpi));
            });
            (active ? chevronActiveBrush : chevronBrush) = compositor.CreateSurfaceBrush(canvas.surface);
        }
        for (int i = 0; i != 2; ++i) {
            canvas = MakeCanvas(vi::kLabelWidth / 2 + 4, vi::kLabelHeight / 2 + 4);
            labelSurfaces[i] = canvas.surface;
            Place(labels[i], canvas, c);
        }
        labelText.clear();
    }

    // ---- Pages.

    std::shared_ptr<Page> BuildPage(const PageModel& model) {
        auto result = std::make_shared<Page>();
        auto& p = *result;
        p.model = model;
        p.root = compositor.CreateContainerVisual();
        p.root.Size({vi::kCanvas, vi::kCanvas});
        p.root.CenterPoint({kCenter, kCenter, 0});
        p.seatProps = compositor.CreatePropertySet();
        p.seatProps.InsertScalar(L"Angle", 0);
        p.seat = MakeSprite(seatBrush, seatSize, {kCenter, kCenter});
        p.seat.Opacity(0);
        p.root.Children().InsertAtTop(p.seat);
        const size_t count = model.items.size();
        for (size_t i = 0; i != count; ++i) {
            Item item;
            item.entry = model.items[i];
            item.angle = SlotAngle(i, count);
            item.cx = kCenter + vi::kOrbitRadius * std::cos(item.angle);
            item.cy = kCenter + vi::kOrbitRadius * std::sin(item.angle);
            item.at = {item.cx, item.cy};
            item.preferred = static_cast<int>(i) == model.preferredIndex;
            item.artClass = ClassifyArtwork(item.entry.visual);
            item.plated = item.artClass == ArtworkClass::Plated;
            item.artKey = ArtKey(item.entry.visual, item.preferred, false);
            item.root = compositor.CreateContainerVisual();
            item.root.Size(itemSize);
            item.root.Offset({SnapDip(item.cx - itemSize.x / 2), SnapDip(item.cy - itemSize.y / 2), 0});
            item.root.CenterPoint({itemSize.x / 2, itemSize.y / 2, 0});
            const D2D1_POINT_2F local{itemSize.x / 2, itemSize.y / 2};
            if (!item.plated) {
                item.tile = MakeSprite(tileBrush, itemSize, local);
                item.root.Children().InsertAtTop(item.tile);
            }
            item.art = MakeSprite(ArtBrush(item.entry.visual, item.preferred, false), itemSize, local);
            item.root.Children().InsertAtTop(item.art);
            if (HasSelectedVariant(item)) {
                item.artSelected = MakeSprite(ArtBrush(item.entry.visual, item.preferred, true), itemSize, local);
                item.artSelected.Opacity(0);
                item.root.Children().InsertAtTop(item.artSelected);
            }
            item.root.Opacity(item.entry.enabled ? 1 : vi::kDisabledOpacity);
            p.root.Children().InsertAtTop(item.root);
            p.items.push_back(std::move(item));
        }
        if (model.adjustControl && count) {
            // Both glyphs stay in one raster at their usual device pixels; each side shows
            // its half of it through its own clipped sprite, so the halves fade independently.
            // The cut runs through the transparent middle, on a device-pixel boundary.
            const D2D1_POINT_2F top{kCenter, kCenter - vi::kOrbitRadius};
            const float half = chevronSize.x / 2;
            for (int side = 0; side != 2; ++side) {
                for (const bool active : {false, true}) {
                    auto sprite = MakeSprite(active ? chevronActiveBrush : chevronBrush, chevronSize, top);
                    sprite.Clip(compositor.CreateInsetClip(side ? half : 0, 0, side ? 0 : half, 0));
                    sprite.Opacity(active ? 0 : SideIdle(model, side));
                    p.root.Children().InsertAtTop(sprite);
                    (active ? p.chevronsActive : p.chevrons)[side] = sprite;
                }
            }
        }
        return result;
    }
    static bool SideEnabled(const PageModel& model, int side) { return side ? model.adjustLater : model.adjustEarlier; }
    static float SideIdle(const PageModel& model, int side) { return SideEnabled(model, side) ? 1.f : vi::kDisabledOpacity; }
    // A side shows the foreground variant only while slot 0 is selected and that side can move.
    bool SideActive(const PageModel& model, int side, int selected) const {
        return model.adjustControl && selected == 0 && !awaitCenter && SideEnabled(model, side);
    }

    // Same slots and artwork classes: tile visuals can stay, only state changes.
    static bool SameStructure(const Page& p, const PageModel& model) {
        if (p.items.size() != model.items.size() || p.model.adjustControl != model.adjustControl) return false;
        for (size_t i = 0; i != p.items.size(); ++i) {
            const auto& a = p.items[i].entry.visual;
            const auto& b = model.items[i].visual;
            if (a.kind != b.kind || a.target != b.target || ClassifyArtwork(b) != p.items[i].artClass) return false;
        }
        return true;
    }

    // Same slots, new state (language, preferred, enabled, movable sides): only what changed crossfades.
    void UpdateInPlace(const PageModel& model, bool animate) {
        auto& p = *page;
        for (size_t i = 0; i != p.items.size(); ++i) {
            auto& item = p.items[i];
            item.entry = model.items[i];
            item.preferred = static_cast<int>(i) == model.preferredIndex;
            const auto key = ArtKey(item.entry.visual, item.preferred, false);
            const D2D1_POINT_2F local{itemSize.x / 2, itemSize.y / 2};
            if (key != item.artKey) {
                auto previous = item.art;
                item.art = MakeSprite(ArtBrush(item.entry.visual, item.preferred, false), itemSize, local);
                item.root.Children().InsertAbove(item.art, previous);
                item.artKey = key;
                if (animate) {
                    item.art.Opacity(0);
                    auto batch = compositor.CreateScopedBatch(wc::CompositionBatchTypes::Animation);
                    Opacity(item.art, 1, vi::kMonogramFadeMs, true);
                    Opacity(previous, 0, vi::kMonogramFadeMs, true);
                    batch.End();
                    auto parent = item.root;
                    OnCompleted(batch, [parent, previous] { Detach(parent, previous); });
                } else {
                    Detach(item.root, previous);
                }
            }
            if (HasSelectedVariant(item)) {
                auto brush = ArtBrush(item.entry.visual, item.preferred, true);
                if (!item.artSelected) {
                    item.artSelected = MakeSprite(brush, itemSize, local);
                    item.artSelected.Opacity(0);
                    item.root.Children().InsertAtTop(item.artSelected);
                } else item.artSelected.Brush(brush);
            } else if (item.artSelected) {
                Detach(item.root, item.artSelected);
                item.artSelected = nullptr;
            }
            Opacity(item.root, item.entry.enabled ? 1 : vi::kDisabledOpacity, vi::kLabelFadeMs, animate);
        }
        // The seat follows the new state below; force its re-evaluation.
        const int seat = p.seatIndex;
        if (seat >= 0) ItemSelected(p.items[seat], true, false);
        // A move that reaches or leaves an end dims or restores that side; the active
        // variant follows in SetSelectionState.
        for (int side = 0; side != 2; ++side)
            if (SideEnabled(model, side) != SideEnabled(p.model, side))
                Opacity(p.chevrons[side], SideIdle(model, side), vi::kLabelFadeMs, animate);
        p.model = model;
    }

    void FinishLeaving() {
        for (auto& p : leaving) if (p) Detach(pagesLayer, p->root);
        leaving.clear();
        handoffLayer.Children().RemoveAll();
    }

    void SetPage(const PageModel& model, PageTransition transition, int origin) {
        awaitCenter = model.awaitCenter;
        std::vector<AppTarget> targets;
        for (const auto& item : model.items) targets.push_back(item.visual);
        PrepareArtwork(targets, static_cast<float>(dpi));
        const bool rebuilt = contentDirty;
        if (contentDirty) { art.clear(); BuildStatic(); contentDirty = false; }
        if (!visible || !page || rebuilt) transition = PageTransition::None;
        lastTransition = TransitionName(transition);
        if (transition == PageTransition::Reflow) { Reflow(model); return; }
        if (transition == PageTransition::None && page && !rebuilt && SameStructure(*page, model)) {
            UpdateInPlace(model, visible);
            SetSelectionState(model.selection, visible);
            return;
        }
        auto next = BuildPage(model);
        FinishLeaving();
        auto previous = page;
        page = next;
        pagesLayer.Children().InsertAtTop(next->root);
        if (!previous || transition == PageTransition::None) {
            if (previous) Detach(pagesLayer, previous->root);
            SetSelectionState(model.selection, visible && previous);
            return;
        }
        unsigned backDelay = vi::kDrillEnterDelayMs, labelMs = vi::kLabelFadeMs;
        auto batch = compositor.CreateScopedBatch(wc::CompositionBatchTypes::Animation);
        const auto& out = previous->root;
        const auto& in = next->root;
        switch (transition) {
        case PageTransition::DrillIn:
        case PageTransition::DrillOut: {
            const bool deeper = transition == PageTransition::DrillIn;
            out.StartAnimation(L"Scale", ScaleAnimation(std::nullopt,
                deeper ? vi::kDrillInExitScale : vi::kDrillOutExitScale, vi::kDrillExitMs, decelerate));
            out.StartAnimation(L"Opacity", ScalarAnimation(std::nullopt, 0, vi::kDrillExitMs, linear));
            in.StartAnimation(L"Scale", ScaleAnimation(deeper ? vi::kDrillInEnterScale : vi::kDrillOutEnterScale,
                1, vi::kDrillEnterMs, drill));
            in.StartAnimation(L"Opacity", ScalarAnimation(0.f, 1, vi::kDrillEnterFadeMs, linear, vi::kDrillEnterDelayMs));
            if (deeper && origin >= 0 && static_cast<size_t>(origin) < previous->items.size()) {
                StartHandoff(*previous, origin);
                backDelay = vi::kHandoffMs - vi::kHandoffFadeMs;
                // The outgoing name clears before the travelling tile reaches the center.
                labelMs = vi::kHandoffLabelFadeMs;
            }
            break;
        }
        default:
            out.StartAnimation(L"Opacity", ScalarAnimation(std::nullopt, 0, vi::kSwapExitMs, linear));
            in.StartAnimation(L"Opacity", ScalarAnimation(0.f, 1, vi::kSwapMs, decelerate));
            in.StartAnimation(L"Scale", ScaleAnimation(vi::kSwapScaleFrom, 1, vi::kSwapMs, decelerate));
            backDelay = 0;
            break;
        }
        batch.End();
        leaving.push_back(previous);
        std::weak_ptr<Page> weak = previous;
        OnCompleted(batch, [this, weak] {
            auto done = weak.lock();
            if (!done) return;
            const auto found = std::find(leaving.begin(), leaving.end(), done);
            if (found == leaving.end()) return;
            Detach(pagesLayer, done->root);
            leaving.erase(found);
        });
        SetSelectionState(model.selection, true, backDelay, labelMs);
    }

    // The tapped tile travels to the center and hands over to the back chevron.
    void StartHandoff(Page& from, int index) {
        auto& item = from.items[index];
        const bool seated = from.seatIndex == index;
        auto clone = compositor.CreateContainerVisual();
        clone.Size(itemSize);
        clone.CenterPoint({itemSize.x / 2, itemSize.y / 2, 0});
        clone.Offset(ItemOffset(item.at));
        const D2D1_POINT_2F local{itemSize.x / 2, itemSize.y / 2};
        if (seated) clone.Children().InsertAtTop(MakeSprite(seatBrush, seatSize, local));
        else if (item.tile) clone.Children().InsertAtTop(MakeSprite(tileBrush, itemSize, local));
        clone.Children().InsertAtTop(MakeSprite((seated && item.artSelected ? item.artSelected : item.art).Brush(),
            itemSize, local));
        handoffLayer.Children().InsertAtTop(clone);
        Settle(item.root, 0);
        if (seated) Settle(from.seat, 0);
        auto batch = compositor.CreateScopedBatch(wc::CompositionBatchTypes::Animation);
        // From where the tile is drawn: a Reflow may still be moving it.
        auto offset = compositor.CreateVector3KeyFrameAnimation();
        offset.InsertExpressionKeyFrame(0, L"source.Offset");
        offset.InsertKeyFrame(1, {kCenter - itemSize.x / 2, kCenter - itemSize.y / 2, 0}, drill);
        offset.SetReferenceParameter(L"source", item.root);
        offset.Duration(Ms(vi::kHandoffMs));
        clone.StartAnimation(L"Offset", offset);
        clone.StartAnimation(L"Scale", ScaleAnimation(1.f, vi::kHandoffScaleTo, vi::kHandoffMs, drill));
        auto fade = compositor.CreateScalarKeyFrameAnimation();
        fade.InsertKeyFrame(0, 1);
        fade.InsertKeyFrame(static_cast<float>(vi::kHandoffMs - vi::kHandoffFadeMs) / vi::kHandoffMs, 1);
        fade.InsertKeyFrame(1, 0, linear);
        fade.Duration(Ms(vi::kHandoffMs));
        clone.StartAnimation(L"Opacity", fade);
        batch.End();
        auto layer = handoffLayer;
        OnCompleted(batch, [layer, clone] { Detach(layer, clone); });
    }

    // ---- Dragging on Manage apps and the reflow that follows a drop.

    wn::float3 ItemOffset(D2D1_POINT_2F at) const {
        return {SnapDip(at.x - itemSize.x / 2), SnapDip(at.y - itemSize.y / 2), 0};
    }
    D2D1_POINT_2F SlotCenter(size_t index, size_t count) const {
        const float angle = SlotAngle(index, count);
        return {kCenter + vi::kOrbitRadius * std::cos(angle), kCenter + vi::kOrbitRadius * std::sin(angle)};
    }
    // Sends an item to `to` from wherever it is now.
    void MoveItem(Item& item, D2D1_POINT_2F to, unsigned ms, const wc::CompositionEasingFunction& ease) {
        item.at = to;
        auto offset = compositor.CreateVector3KeyFrameAnimation();
        offset.InsertExpressionKeyFrame(0, L"this.StartingValue");
        offset.InsertKeyFrame(1, ItemOffset(to), ease);
        offset.Duration(Ms(ms));
        item.root.StartAnimation(L"Offset", offset);
    }
    void ScaleItem(Item& item, float scale, unsigned ms, const wc::CompositionEasingFunction& ease) {
        item.scale = scale;
        item.root.StartAnimation(L"Scale", ScaleAnimation(std::nullopt, scale, ms, ease));
    }

    void BeginDrag(int index, bool trashEnabled) {
        if (!page || !visible || closing || index < 0 || static_cast<size_t>(index) >= page->items.size()) return;
        auto& p = *page;
        if (p.drag >= 0) return;
        p.drag = p.dragSlot = index;
        p.overTrash = false;
        p.trash = trashEnabled;
        // The seat and the name leave; the center shows the trash.
        SetSelectionState(-1, true);
        auto& item = p.items[index];
        // Lifted above the other items.
        p.root.Children().Remove(item.root);
        p.root.Children().InsertAtTop(item.root);
        ScaleItem(item, vi::kDragLiftScale, vi::kDragLiftMs, decelerate);
    }

    void DragTo(D2D1_POINT_2F point, int slot, bool overTrash) {
        if (!page || page->drag < 0) return;
        auto& p = *page;
        const size_t count = p.items.size();
        const int from = p.drag;
        auto& lifted = p.items[from];
        // The lifted item follows the pointer without easing.
        lifted.at = point;
        lifted.root.StopAnimation(L"Offset");
        lifted.root.Offset(ItemOffset(point));
        if (slot < 0 || static_cast<size_t>(slot) >= count) slot = from;
        if (slot != p.dragSlot) {
            // The items between the lifted one's home and its new slot step one place toward the home.
            p.dragSlot = slot;
            for (int i = 0; i != static_cast<int>(count); ++i) {
                if (i == from) continue;
                int at = i;
                if (from < slot && i > from && i <= slot) at = i - 1;
                else if (slot < from && i >= slot && i < from) at = i + 1;
                MoveItem(p.items[i], SlotCenter(at, count), vi::kDragReflowMs, decelerate);
            }
        }
        overTrash = overTrash && p.trash;
        if (overTrash != p.overTrash) {
            p.overTrash = overTrash;
            ScaleItem(lifted, overTrash ? vi::kDragOverTrashScale : vi::kDragLiftScale, vi::kDragArmMs, decelerate);
            Opacity(lifted.root, overTrash ? vi::kDragOverTrashOpacity : 1.f, vi::kDragArmMs, true);
            UpdateCenter(true);
        }
    }

    void EndDrag(DragEnd end) {
        if (!page || page->drag < 0) return;
        auto& p = *page;
        const size_t count = p.items.size();
        auto& lifted = p.items[p.drag];
        p.drag = p.dragSlot = -1;
        p.overTrash = false;
        switch (end) {
        case DragEnd::Cancel:
            // Everything returns to its own slot.
            for (size_t i = 0; i != count; ++i) {
                auto& item = p.items[i];
                MoveItem(item, SlotCenter(i, count), vi::kDragSettleMs, drill);
                if (item.scale != 1) ScaleItem(item, 1, vi::kDragSettleMs, drill);
            }
            Opacity(lifted.root, lifted.entry.enabled ? 1.f : vi::kDisabledOpacity, vi::kDragArmMs, true);
            break;
        case DragEnd::Remove:
            // The lifted item falls into the trash; the Reflow page that follows keeps it until it is gone.
            lifted.falling = true;
            MoveItem(lifted, {kCenter, kCenter}, vi::kDragRemoveMs, accelerate);
            ScaleItem(lifted, vi::kDragRemoveScaleTo, vi::kDragRemoveMs, accelerate);
            Opacity(lifted.root, 0, vi::kDragRemoveMs, true, accelerate);
            break;
        default:
            break;
        }
        UpdateCenter(true);
    }

    static bool SameVisual(const AppTarget& a, const AppTarget& b) {
        return a.name == b.name && a.kind == b.kind && a.target == b.target && a.arguments == b.arguments &&
            a.workingDirectory == b.workingDirectory;
    }

    // The same page in a new order: every item travels from where it is (`at`) to its new slot, an
    // item that is new fades in, and an item that is gone shrinks away with the outgoing page.
    void Reflow(const PageModel& model) {
        auto next = BuildPage(model);
        FinishLeaving();
        auto previous = page;
        page = next;
        pagesLayer.Children().InsertAtTop(next->root);
        if (!previous) { SetSelectionState(model.selection, false); return; }
        const size_t count = previous->items.size();
        std::vector<bool> used(count);
        auto batch = compositor.CreateScopedBatch(wc::CompositionBatchTypes::Animation);
        for (size_t i = 0; i != next->items.size(); ++i) {
            auto& item = next->items[i];
            // The controller names each item's previous slot; otherwise the first unused tile of
            // the same app that is not falling into the trash.
            size_t match = count;
            if (model.from.size() == next->items.size()) {
                const int from = model.from[i];
                if (from >= 0 && static_cast<size_t>(from) < count && !used[from] &&
                    SameVisual(previous->items[from].entry.visual, item.entry.visual)) match = from;
            } else {
                for (size_t j = 0; j != count; ++j)
                    if (!used[j] && !previous->items[j].falling &&
                        SameVisual(previous->items[j].entry.visual, item.entry.visual)) { match = j; break; }
            }
            const float rest = item.entry.enabled ? 1.f : vi::kDisabledOpacity;
            if (match == count) {
                item.root.Opacity(0);
                Opacity(item.root, rest, vi::kDragSettleMs, true, decelerate);
                continue;
            }
            used[match] = true;
            auto& old = previous->items[match];
            // Starts where the old tile is drawn, even while its own travel is still running.
            const D2D1_POINT_2F home = item.at;
            item.at = home;
            auto offset = compositor.CreateVector3KeyFrameAnimation();
            offset.InsertExpressionKeyFrame(0, L"old.Offset");
            offset.InsertKeyFrame(1, ItemOffset(home), drill);
            offset.SetReferenceParameter(L"old", old.root);
            offset.Duration(Ms(vi::kDragSettleMs));
            item.root.StartAnimation(L"Offset", offset);
            if (old.scale != 1) {
                auto scale = compositor.CreateVector3KeyFrameAnimation();
                scale.InsertExpressionKeyFrame(0, L"old.Scale");
                scale.InsertKeyFrame(1, {1, 1, 1}, drill);
                scale.SetReferenceParameter(L"old", old.root);
                scale.Duration(Ms(vi::kDragSettleMs));
                item.root.StartAnimation(L"Scale", scale);
            }
            // Hidden, but its travel keeps running for the expressions above.
            old.root.StopAnimation(L"Opacity");
            old.root.Opacity(0);
        }
        Settle(previous->seat, 0);
        for (size_t j = 0; j != previous->items.size(); ++j) {
            if (used[j]) continue;
            auto& gone = previous->items[j];
            gone.root.StartAnimation(L"Scale", ScaleAnimation(std::nullopt, vi::kDragRemoveScaleTo, vi::kDragRemoveMs, accelerate));
            Opacity(gone.root, 0, vi::kDragRemoveMs, true, accelerate);
        }
        batch.End();
        leaving.push_back(previous);
        std::weak_ptr<Page> weak = previous;
        OnCompleted(batch, [this, weak] {
            auto done = weak.lock();
            if (!done) return;
            const auto found = std::find(leaving.begin(), leaving.end(), done);
            if (found == leaving.end()) return;
            Detach(pagesLayer, done->root);
            leaving.erase(found);
        });
        SetSelectionState(model.selection, true);
    }

    // ---- Selection and center.

    // Which chevron sides the page draws as movable: none, both, earlier, later or blocked.
    const wchar_t* AdjustState() const {
        if (!page || !page->model.adjustControl || page->items.empty()) return L"none";
        const auto& m = page->model;
        return m.adjustEarlier ? (m.adjustLater ? L"both" : L"earlier") : (m.adjustLater ? L"later" : L"blocked");
    }

    // The center trash: hidden, idle, armed, or disabled while one app remains.
    const wchar_t* TrashState() const {
        if (!page || page->drag < 0) return L"hidden";
        return !page->trash ? L"disabled" : page->overTrash ? L"armed" : L"idle";
    }

    std::wstring CenterText() const {
        if (!page || awaitCenter || page->drag >= 0) return {};
        const int selected = page->model.selection;
        if (selected < 0 || static_cast<size_t>(selected) >= page->items.size()) return {};
        return AppDisplayName(page->items[selected].entry.visual);
    }

    // The seat replaces the selected item's neutral tile fill. A selected art variant
    // replaces the normal one: it fades in over it, and the normal art leaves once it is
    // covered, so at rest the artwork is never composited twice (no hardened edges).
    void ItemSelected(Item& item, bool selected, bool animate) {
        Opacity(item.tile, selected ? 0.f : 1.f, vi::kTileFillFadeMs, animate);
        if (!item.artSelected) return;
        Opacity(item.artSelected, selected ? 1.f : 0.f, vi::kTileFillFadeMs, animate);
        if (selected) Opacity(item.art, 0, vi::kReducedMotionMs, animate, linear, vi::kTileFillFadeMs);
        else Opacity(item.art, 1, vi::kReducedMotionMs, animate);
    }
    // Ends a travel: the expression stops wherever the seat is.
    void StopSeat(Page& p) {
        ++p.seatTravel;
        p.seat.StopAnimation(L"Offset");
        p.seatProps.StopAnimation(L"Angle");
    }

    wn::float3 SeatOffset(const Item& item) const {
        return {SnapDip(item.cx - seatSize.x / 2), SnapDip(item.cy - seatSize.y / 2), 0};
    }
    void PlaceSeat(Page& p, int index) {
        StopSeat(p);
        p.seatAngle = p.items[index].angle;
        p.seatProps.InsertScalar(L"Angle", p.seatAngle);
        p.seat.Offset(SeatOffset(p.items[index]));
    }
    // The seat travels along the orbit (shortest arc) on a lightly underdamped spring.
    void TravelSeat(Page& p, int index, bool animate) {
        if (!animate || !Animated()) { PlaceSeat(p, index); return; }
        p.seatAngle += std::remainder(p.items[index].angle - p.seatAngle, 2 * kPi);
        auto follow = compositor.CreateExpressionAnimation(
            L"Vector3(c + r * Cos(props.Angle) - h, c + r * Sin(props.Angle) - h, 0)");
        follow.SetReferenceParameter(L"props", p.seatProps);
        follow.SetScalarParameter(L"c", kCenter);
        follow.SetScalarParameter(L"r", vi::kOrbitRadius);
        follow.SetScalarParameter(L"h", seatSize.x / 2);
        p.seat.StartAnimation(L"Offset", follow);
        auto spring = compositor.CreateSpringScalarAnimation();
        spring.DampingRatio(vi::kSeatSpringDamping);
        spring.Period(std::chrono::milliseconds(vi::kSeatSpringPeriodMs));
        spring.FinalValue(p.seatAngle);
        auto batch = compositor.CreateScopedBatch(wc::CompositionBatchTypes::Animation);
        p.seatProps.StartAnimation(L"Angle", spring);
        batch.End();
        const unsigned travel = ++p.seatTravel;
        std::weak_ptr<Page> weak = page;
        OnCompleted(batch, [this, weak, travel] {
            // At rest the expression stops, so an idle reel produces no frames.
            auto settled = weak.lock();
            if (!settled || settled->seatTravel != travel || settled->seatIndex < 0) return;
            PlaceSeat(*settled, settled->seatIndex);
        });
    }

    void SetSelectionState(int selected, bool animate, unsigned backDelay = 0, unsigned labelMs = vi::kLabelFadeMs) {
        if (page) {
            auto& p = *page;
            if (selected < -1 || static_cast<size_t>(selected + 1) > p.items.size()) selected = -1;
            p.model.selection = selected;
            p.model.awaitCenter = awaitCenter;
            const int seat = awaitCenter || selected < 0 || !p.items[selected].entry.enabled ? -1 : selected;
            const int previous = p.seatIndex;
            if (seat != previous) {
                if (previous >= 0) ItemSelected(p.items[previous], false, animate);
                if (seat >= 0) ItemSelected(p.items[seat], true, animate);
                if (seat < 0) {
                    // A travel in flight stops where it is and fades there.
                    StopSeat(p);
                    Opacity(p.seat, 0, vi::kSeatFadeMs, animate);
                } else if (previous < 0) {
                    PlaceSeat(p, seat);
                    if (animate) {
                        p.seat.StartAnimation(L"Opacity", ScalarAnimation(0.f, 1, vi::kSeatFadeMs, linear));
                        p.seat.StartAnimation(L"Scale", ScaleAnimation(vi::kSeatAppearScaleFrom, 1, vi::kSeatAppearMs, decelerate));
                    } else Settle(p.seat, 1);
                } else {
                    TravelSeat(p, seat, animate);
                }
                p.seatIndex = seat;
            }
            for (int side = 0; side != 2; ++side)
                Opacity(p.chevronsActive[side], SideActive(p.model, side, selected) ? 1.f : 0.f, vi::kLabelFadeMs, animate);
        }
        UpdateLabel(animate, labelMs);
        UpdateCenter(animate, backDelay);
    }

    void UpdateLabel(bool animate, unsigned ms = vi::kLabelFadeMs) {
        const auto text = CenterText();
        if (text == labelText) return;
        labelText = text;
        auto& current = labels[activeLabel];
        if (!text.empty()) {
            const int next = 1 - activeLabel;
            auto& surface = labelSurfaces[next];
            const auto size = labels[next].Size();
            Paint(surface, [&](ID2D1DeviceContext* context) {
                DrawCentered(context, text, labelFormat.get(), vi::kLabelSize, {size.x / 2, size.y / 2},
                    vi::kLabelWidth, Color(appearance.foreground));
            });
            Opacity(labels[next], 1, ms, animate);
            activeLabel = next;
        }
        Opacity(current, 0, ms, animate);
    }

    // The name owns the center: the ring and back chevron give way while it shows. While a tile
    // is dragged the trash owns it alone.
    void UpdateCenter(bool animate, unsigned backDelay = 0) {
        const bool named = !labelText.empty();
        const bool gated = awaitCenter && page;
        const bool dragging = page && page->drag >= 0;
        const bool quiet = !named && !gated && !dragging;
        const CenterState next{quiet ? 1.f : 0.f, gated ? 1.f : 0.f, gated ? 1.f : 0.f,
            page && page->model.isSubmenu && quiet ? 1.f : 0.f,
            dragging ? (page->trash ? 1.f : vi::kDisabledOpacity) : 0.f,
            dragging && page->overTrash ? 1.f : 0.f};
        if (next.trash != center.trash) Opacity(trash, next.trash, vi::kDragArmMs, animate);
        if (next.armed != center.armed) {
            Opacity(trashArmed, next.armed, vi::kDragArmMs, animate);
            if (next.armed > 0 && animate && Animated())
                trashArmed.StartAnimation(L"Scale", ScaleAnimation(vi::kDragArmScaleFrom, 1, vi::kDragArmMs, decelerate));
        }
        const bool arming = center.ringGated > 0 && !gated;
        if (next.ringIdle != center.ringIdle)
            Opacity(ringIdle, next.ringIdle, arming ? vi::kGateArmMs : vi::kRingHideMs, animate, arming ? decelerate : linear);
        if (next.ringGated != center.ringGated) Opacity(ringGated, next.ringGated, vi::kGateArmMs, animate, decelerate);
        if (next.dot != center.dot) Opacity(gateDot, next.dot, vi::kGateDotFadeMs, animate);
        if (next.back != center.back || (backDelay && next.back > 0))
            Opacity(back, next.back, vi::kLabelFadeMs, animate, linear, next.back > 0 ? backDelay : 0);
        center = next;
    }

    // ---- Lifetime of a gesture.

    // Every visual returns to its resting state for the current page; no animation runs.
    void ResetPresentation() {
        FinishLeaving();
        for (const wc::Visual& visual : std::initializer_list<wc::Visual>{root, base, backdropVisual}) Settle(visual, 1);
        if (page) {
            auto& p = *page;
            Settle(p.root, 1);
            p.drag = p.dragSlot = -1;
            p.overTrash = false;
            for (size_t i = 0; i != p.items.size(); ++i) {
                auto& item = p.items[i];
                Settle(item.root, item.entry.enabled ? 1 : vi::kDisabledOpacity);
                item.root.StopAnimation(L"Offset");
                item.at = {item.cx, item.cy};
                item.scale = 1;
                item.root.Offset(ItemOffset(item.at));
                ItemSelected(item, static_cast<int>(i) == p.seatIndex, false);
            }
            Settle(p.seat, p.seatIndex >= 0 ? 1.f : 0.f);
            if (p.seatIndex >= 0) PlaceSeat(p, p.seatIndex);
            else StopSeat(p);
            for (int side = 0; side != 2; ++side) {
                Settle(p.chevrons[side], SideIdle(p.model, side));
                Settle(p.chevronsActive[side], SideActive(p.model, side, p.model.selection) ? 1.f : 0.f);
            }
        }
        Settle(labels[activeLabel], labelText.empty() ? 0.f : 1.f);
        Settle(labels[1 - activeLabel], 0);
        for (const wc::Visual& visual : std::initializer_list<wc::Visual>{ringIdle, ringGated, gateDot, back})
            visual.StopAnimation(L"Scale");
        Settle(trash, 0);
        Settle(trashArmed, 0);
        center = {};
        UpdateCenter(false);
    }

    void RebuildAll() {
        art.clear();
        BuildStatic();
        contentDirty = false;
        if (page) {
            const auto model = page->model;
            FinishLeaving();
            Detach(pagesLayer, page->root);
            page = BuildPage(model);
            pagesLayer.Children().InsertAtTop(page->root);
            SetSelectionState(model.selection, false);
        } else {
            UpdateLabel(false);
            center = {};
            UpdateCenter(false);
        }
    }

    void Show() {
        if (FAILED(d3d->GetDeviceRemovedReason())) RecoverDevice();
        if (contentDirty) RebuildAll();
        ResetPresentation();
        target.Root(pixelRoot);
        visible = true;
        closing = false;
        if (!Animated()) return;
        // Transform-led arrival: a short linear fade and a decelerating settle at the final center.
        root.StartAnimation(L"Opacity", ScalarAnimation(0.f, 1, vi::kOpenFadeMs, linear));
        root.StartAnimation(L"Scale", ScaleAnimation(vi::kOpenScaleFrom, 1, vi::kOpenScaleMs, decelerate));
    }

    unsigned BeginHide(HideStyle style) {
        if (!visible || !Animated()) { Hide(); return 0; }
        closing = true;
        if (style == HideStyle::Commit && page && page->seatIndex >= 0) {
            // The chosen item holds for one beat with a light press while the rest leaves.
            auto& p = *page;
            auto& chosen = p.items[p.seatIndex];
            for (const wc::Visual& visual : std::initializer_list<wc::Visual>{base, backdropVisual, ringIdle, ringGated,
                     gateDot, back, trash, trashArmed, labels[0], labels[1], p.chevrons[0], p.chevrons[1], p.chevronsActive[0], p.chevronsActive[1]})
                Opacity(visual, 0, vi::kCommitFadeMs, true);
            for (auto& item : p.items) if (&item != &chosen) Opacity(item.root, 0, vi::kCommitFadeMs, true);
            for (const wc::Visual& visual : std::initializer_list<wc::Visual>{chosen.root, p.seat}) {
                visual.StartAnimation(L"Opacity", ScalarAnimation(std::nullopt, 0, vi::kCommitFadeMs, linear, vi::kCommitHoldMs));
                auto press = compositor.CreateVector3KeyFrameAnimation();
                press.InsertKeyFrame(0, {1, 1, 1});
                press.InsertKeyFrame(.5f, {vi::kCommitPressScale, vi::kCommitPressScale, 1}, decelerate);
                press.InsertKeyFrame(1, {1, 1, 1}, decelerate);
                press.Duration(Ms(vi::kCommitPressMs));
                visual.StartAnimation(L"Scale", press);
            }
            return vi::kCommitTotalMs;
        }
        root.StartAnimation(L"Opacity", ScalarAnimation(std::nullopt, 0, vi::kCloseFadeMs, linear));
        root.StartAnimation(L"Scale", ScaleAnimation(std::nullopt, vi::kCloseScaleTo, vi::kCloseScaleMs, accelerate));
        return vi::kCloseFadeMs;
    }

    void Hide() {
        visible = false;
        closing = false;
        if (target) target.Root(nullptr);
        ResetPresentation();
    }

    // After device loss the device is replaced, everything is redrawn, and `retry` runs once.
    template <class Operation, class Retry> void Run(Operation&& operation, Retry&& retry) noexcept {
        try {
            try { operation(); }
            catch (const winrt::hresult_error& e) {
                if (!DeviceLost(e.code())) throw;
                RecoverDevice();
                retry();
            }
        }
        catch (const winrt::hresult_error& e) { lastError = e.message().c_str(); }
        catch (const std::exception&) { lastError = L"Renderer resource allocation failed."; }
        catch (...) { lastError = L"Unknown renderer failure."; }
    }
    template <class Operation> void Run(Operation&& operation) noexcept { Run(operation, operation); }
};

OrbitRenderer::OrbitRenderer(HWND window) : impl_(std::make_unique<Impl>(window)) {}
OrbitRenderer::~OrbitRenderer() {
    if (!impl_) return;
    impl_->life.reset();
    impl_->Run([&] {
        if (impl_->capabilities) impl_->capabilities.Changed(impl_->capabilitiesChanged);
        if (impl_->target) impl_->target.Root(nullptr);
        ReleaseArtwork(nullptr);
        BOOL disabled = FALSE;
        DwmSetWindowAttribute(impl_->window, DWMWA_USE_HOSTBACKDROPBRUSH, &disabled, sizeof(disabled));
    });
}
bool OrbitRenderer::Initialize(std::wstring& error) {
    impl_->Run([&] { impl_->Initialize(); });
    error = impl_->lastError;
    return impl_->ready && error.empty();
}
void OrbitRenderer::Resize(UINT dpi) {
    if (!impl_->ready || impl_->dpi == dpi || !dpi) return;
    impl_->Run([&] {
        impl_->dpi = dpi;
        impl_->ResizeRoot();
        impl_->contentDirty = true;
        if (impl_->visible && !impl_->closing) impl_->RebuildAll();
    });
}
void OrbitRenderer::SetAppearance(const Appearance& appearance) {
    auto& self = *impl_;
    if (self.appearanceSet && SameAppearance(appearance, self.appearance)) return;
    const bool material = !self.appearanceSet || appearance.dark != self.appearance.dark ||
        appearance.transparency != self.appearance.transparency || appearance.highContrast != self.appearance.highContrast;
    self.appearance = appearance;
    self.appearanceSet = true;
    if (!self.ready) return;
    self.Run([&] {
        if (material) self.ApplyMaterial();
        // A leaving reel keeps its exit; Show redraws dirty content first.
        self.contentDirty = true;
        if (self.visible && !self.closing) self.RebuildAll();
    });
}
void OrbitRenderer::SetLanguage(Language language) {
    if (impl_->labelFormat && impl_->language == language) return;
    impl_->language = language;
    if (impl_->ready) impl_->Run([&] { impl_->CreateTextFormats(); });
}
void OrbitRenderer::SetReelSize(unsigned percent) {
    auto label = std::to_wstring(percent) + L"%";
    if (impl_->ready) impl_->Run([&] { impl_->sizeLabel = label; });
    else impl_->sizeLabel = label;
}
void OrbitRenderer::SetPage(const PageModel& page, PageTransition transition, int originIndex) {
    // A page change during the exit would re-show items; the next Start sets the page again.
    if (!impl_->ready || impl_->closing) return;
    // After device loss the rebuilt page cannot continue a half-applied transition.
    impl_->Run([&] { impl_->SetPage(page, transition, originIndex); },
               [&] { impl_->SetPage(page, PageTransition::None, -1); });
}
std::pair<D2D1_RECT_F, D2D1_RECT_F> OrbitRenderer::AdjustArrowBounds() const {
    // The adjust control lives on the top orbit tile, not in the cancel zone.
    constexpr float y = kCenter - vi::kOrbitRadius, h = vi::kChevronHit / 2;
    constexpr float left = kCenter - vi::kChevronOffset, right = kCenter + vi::kChevronOffset;
    return {D2D1::RectF(left - h, y - h, left + h, y + h), D2D1::RectF(right - h, y - h, right + h, y + h)};
}
void OrbitRenderer::SetAwaitCenter(bool awaitCenter) {
    if (impl_->awaitCenter == awaitCenter) return;
    impl_->awaitCenter = awaitCenter;
    if (impl_->ready && impl_->page)
        impl_->Run([&] { impl_->SetSelectionState(impl_->page->model.selection, impl_->visible); });
}
void OrbitRenderer::SetSelection(int index) {
    if (!impl_->ready || !impl_->page) return;
    if (index < -1 || index >= static_cast<int>(impl_->page->items.size())) index = -1;
    if (impl_->page->model.selection == index) return;
    impl_->Run([&] { impl_->SetSelectionState(index, impl_->visible); });
}
void OrbitRenderer::BeginDrag(int index, bool trash) {
    if (impl_->ready && !impl_->closing) impl_->Run([&] { impl_->BeginDrag(index, trash); });
}
void OrbitRenderer::DragTo(D2D1_POINT_2F point, int slot, bool overTrash) {
    if (impl_->ready && !impl_->closing) impl_->Run([&] { impl_->DragTo(point, slot, overTrash); });
}
void OrbitRenderer::EndDrag(DragEnd end) {
    if (impl_->ready && !impl_->closing) impl_->Run([&] { impl_->EndDrag(end); });
}
bool OrbitRenderer::Dragging() const {
    return impl_->ready && impl_->visible && !impl_->closing && impl_->page && impl_->page->drag >= 0;
}
void OrbitRenderer::Show() {
    if (impl_->ready) impl_->Run([&] { impl_->Show(); });
}
unsigned OrbitRenderer::BeginHide(HideStyle style) {
    unsigned duration = 0;
    if (impl_->ready) impl_->Run([&] { duration = impl_->BeginHide(style); });
    return duration;
}
void OrbitRenderer::Hide() {
    if (impl_->ready) impl_->Run([&] { impl_->Hide(); });
}
bool OrbitRenderer::UsesBackdrop() const { return impl_->backdrop; }
std::wstring OrbitRenderer::Diagnostics() const {
    const auto& self = *impl_;
    const auto& page = self.page;
    return L"centerLabel=" + (self.visible ? self.CenterText() : L"") +
        L"\r\nWindows Composition + Direct2D/DirectWrite; icons=official-artwork-unified-tiles+segoe-fluent-icons; material=" +
        self.backdropStatus + L"; center gate=" + (self.awaitCenter ? L"waiting" : L"armed") +
        L"; menu=" + (page ? page->model.title : L"") + L"; submenu=" + (page && page->model.isSubmenu ? L"yes" : L"no") +
        L"; adjust=" + self.AdjustState() + L"; transition=" + self.lastTransition +
        L"; drag=" + std::to_wstring(page ? page->drag : -1) + L"; trash=" + self.TrashState() +
        L"; animation=open-fade83-scale167/close83/commit133/seat-spring/drill250/handoff200; center=quiet; perimeter=neutral" +
        L"; text=" + self.textFamily + L"; DPI=" + std::to_wstring(self.dpi) +
        L"; surface draws=" + std::to_wstring(self.surfaceDraws) +
        L"; device=" + (self.softwareDevice ? L"WARP" : L"hardware") +
        (self.lastError.empty() ? L"" : L"; last error=" + self.lastError);
}
} // namespace reel
