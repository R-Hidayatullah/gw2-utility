#include "wic_image.h"

#include <windows.h>

#include <wincodec.h>
#include <wrl/client.h>

namespace gw2wic {

namespace {

using Microsoft::WRL::ComPtr;

void ensure_com_initialized() {
    static bool initialized = false;
    if (!initialized) {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        initialized = true;
    }
}

} // namespace

std::optional<WicImage> decode(const uint8_t* data, size_t size) {
    if (data == nullptr || size == 0) {
        return std::nullopt;
    }

    ensure_com_initialized();

    ComPtr<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)))) {
        return std::nullopt;
    }

    ComPtr<IWICStream> stream;
    if (FAILED(factory->CreateStream(&stream))) {
        return std::nullopt;
    }
    if (FAILED(stream->InitializeFromMemory(const_cast<BYTE*>(data), static_cast<DWORD>(size)))) {
        return std::nullopt;
    }

    ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnLoad, &decoder))) {
        return std::nullopt;
    }

    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, &frame))) {
        return std::nullopt;
    }

    ComPtr<IWICFormatConverter> converter;
    if (FAILED(factory->CreateFormatConverter(&converter))) {
        return std::nullopt;
    }
    if (FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0.0,
                                      WICBitmapPaletteTypeCustom))) {
        return std::nullopt;
    }

    UINT width = 0;
    UINT height = 0;
    if (FAILED(converter->GetSize(&width, &height)) || width == 0 || height == 0) {
        return std::nullopt;
    }

    WicImage image;
    image.width = width;
    image.height = height;
    image.rgba.resize(static_cast<size_t>(width) * height * 4);

    const UINT stride = width * 4;
    if (FAILED(converter->CopyPixels(nullptr, stride, static_cast<UINT>(image.rgba.size()), image.rgba.data()))) {
        return std::nullopt;
    }

    return image;
}

} // namespace gw2wic
