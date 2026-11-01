// SPDX-License-Identifier: GPL-3.0-or-later
#include "velin/platform.hpp"
#if defined(__MINGW32__)
#define ____FIReference_1_boolean_INTERFACE_DEFINED__
#endif
#include <windows.graphics.imaging.h>
#include <windows.storage.streams.h>
#include <asyncinfo.h>
#include <roapi.h>
#include <winstring.h>
#include <algorithm>
#include <cstring>

namespace velin {
namespace {
namespace Imaging = ABI::Windows::Graphics::Imaging;
namespace Streams = ABI::Windows::Storage::Streams;
template<class T> struct ComPtr { T* value{}; ~ComPtr() { if (value) value->Release(); } T** put() { return &value; } T* operator->() const { return value; } };
// The public Windows OCR ABI is declared locally because MinGW-w64 ships the
// imaging headers but does not yet ship windows.media.ocr.h. These vtables are
// the documented Windows Runtime interfaces, not an emulated OCR backend.
struct OcrResultAbi : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_Lines(IInspectable**) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_TextAngle(IInspectable**) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Text(HSTRING*) = 0;
};
struct OcrOperationAbi : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE put_Completed(IUnknown*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Completed(IUnknown**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetResults(OcrResultAbi**) = 0;
};
struct OcrEngineAbi : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE RecognizeAsync(Imaging::ISoftwareBitmap*, OcrOperationAbi**) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_RecognizerLanguage(IInspectable**) = 0;
};
struct OcrStaticsAbi : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_MaxImageDimension(UINT32*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_AvailableRecognizerLanguages(IInspectable**) = 0;
    virtual HRESULT STDMETHODCALLTYPE IsLanguageSupported(IInspectable*, boolean*) = 0;
    virtual HRESULT STDMETHODCALLTYPE TryCreateFromLanguage(IInspectable*, OcrEngineAbi**) = 0;
    virtual HRESULT STDMETHODCALLTYPE TryCreateFromUserProfileLanguages(OcrEngineAbi**) = 0;
};
struct BufferBytes : IUnknown { virtual HRESULT STDMETHODCALLTYPE Buffer(BYTE**) = 0; };
constexpr GUID ocrStaticsId{0x5bffa85a,0x3384,0x3540,{0x99,0x40,0x69,0x91,0x20,0xd4,0x28,0xa8}};
constexpr GUID bufferBytesId{0x905a0fef,0xbc53,0x11df,{0x8c,0x49,0x00,0x1e,0x4f,0xc6,0x86,0xda}};
class HString {
public:
    explicit HString(const wchar_t* text) { WindowsCreateString(text, static_cast<UINT32>(wcslen(text)), &value); }
    ~HString() { if (value) WindowsDeleteString(value); }
    operator HSTRING() const { return value; }
private: HSTRING value{};
};
struct RuntimeApartment {
    HRESULT status = RoInitialize(RO_INIT_MULTITHREADED);
    ~RuntimeApartment() { if (SUCCEEDED(status)) RoUninitialize(); }
};
} // namespace

TextRecognitionResult recognizeTextBgra(unsigned width, unsigned height, const std::vector<std::uint8_t>& pixels) {
    if (!width || !height || width > 32768 || height > 32768 || static_cast<std::uint64_t>(width) * height * 4 != pixels.size())
        return {{}, L"The captured frame has invalid dimensions or pixel data."};
    RuntimeApartment apartment;
    if (FAILED(apartment.status) && apartment.status != RPC_E_CHANGED_MODE) return {{}, L"Cannot initialize Windows Runtime for text recognition."};
    ComPtr<OcrStaticsAbi> factory;
    HRESULT status = RoGetActivationFactory(HString(L"Windows.Media.Ocr.OcrEngine"), ocrStaticsId, reinterpret_cast<void**>(factory.put()));
    if (FAILED(status)) return {{}, L"Windows OCR is unavailable. Install a Windows language pack with optical character recognition support."};
    ComPtr<OcrEngineAbi> engine;
    status = factory->TryCreateFromUserProfileLanguages(engine.put());
    if (FAILED(status) || !engine.value) return {{}, L"No OCR recognizer is installed for your Windows profile languages. Add a language with OCR support in Windows Settings."};
    UINT32 maximum = 1600;
    factory->get_MaxImageDimension(&maximum);
    maximum = std::clamp<UINT32>(maximum, 256, 4096);
    const double scale = std::min(1.0, static_cast<double>(maximum) / std::max(width, height));
    const auto scaledWidth = std::max(1U, static_cast<unsigned>(width * scale));
    const auto scaledHeight = std::max(1U, static_cast<unsigned>(height * scale));
    const UINT32 byteCount = scaledWidth * scaledHeight * 4;
    ComPtr<Streams::IBufferFactory> bufferFactory;
    status = RoGetActivationFactory(HString(L"Windows.Storage.Streams.Buffer"), __uuidof(Streams::IBufferFactory), reinterpret_cast<void**>(bufferFactory.put()));
    ComPtr<Streams::IBuffer> buffer;
    if (FAILED(status) || FAILED(bufferFactory->Create(byteCount, buffer.put()))) return {{}, L"Cannot allocate a Windows image buffer."};
    buffer->put_Length(byteCount);
    ComPtr<BufferBytes> bytes;
    BYTE* output{};
    if (FAILED(buffer->QueryInterface(bufferBytesId, reinterpret_cast<void**>(bytes.put()))) || FAILED(bytes->Buffer(&output)))
        return {{}, L"Cannot access the Windows image buffer."};
    if (width == scaledWidth && height == scaledHeight) std::memcpy(output, pixels.data(), pixels.size());
    else for (unsigned y = 0; y < scaledHeight; ++y) for (unsigned x = 0; x < scaledWidth; ++x) {
        const auto source = (static_cast<size_t>(static_cast<unsigned>(y / scale)) * width + static_cast<unsigned>(x / scale)) * 4;
        std::memcpy(output + (static_cast<size_t>(y) * scaledWidth + x) * 4, pixels.data() + source, 4);
    }
    ComPtr<Imaging::ISoftwareBitmapStatics> bitmapFactory;
    status = RoGetActivationFactory(HString(L"Windows.Graphics.Imaging.SoftwareBitmap"), __uuidof(Imaging::ISoftwareBitmapStatics), reinterpret_cast<void**>(bitmapFactory.put()));
    ComPtr<Imaging::ISoftwareBitmap> bitmap;
    if (FAILED(status) || FAILED(bitmapFactory->CreateCopyWithAlphaFromBuffer(buffer.value, Imaging::BitmapPixelFormat_Bgra8,
        static_cast<INT32>(scaledWidth), static_cast<INT32>(scaledHeight), Imaging::BitmapAlphaMode_Premultiplied, bitmap.put())))
        return {{}, L"Cannot create the Windows OCR image."};
    ComPtr<OcrOperationAbi> operation;
    if (FAILED(engine->RecognizeAsync(bitmap.value, operation.put()))) return {{}, L"Windows OCR could not start image recognition."};
    ComPtr<IAsyncInfo> info;
    if (FAILED(operation->QueryInterface(__uuidof(IAsyncInfo), reinterpret_cast<void**>(info.put())))) return {{}, L"Windows OCR returned an invalid asynchronous operation."};
    AsyncStatus state = Started;
    const auto deadline = GetTickCount64() + 15000;
    while (SUCCEEDED(info->get_Status(&state)) && state == Started && GetTickCount64() < deadline) Sleep(10);
    if (state != Completed) { info->Cancel(); return {{}, state == Started ? L"Windows OCR timed out." : L"Windows OCR could not recognize this frame."}; }
    ComPtr<OcrResultAbi> result;
    HSTRING text{};
    if (FAILED(operation->GetResults(result.put())) || FAILED(result->get_Text(&text))) return {{}, L"Windows OCR did not return text."};
    UINT32 length{};
    const auto* value = WindowsGetStringRawBuffer(text, &length);
    std::wstring recognized(value ? value : L"", length);
    if (text) WindowsDeleteString(text);
    info->Close();
    return {std::move(recognized), {}};
}
} // namespace velin
