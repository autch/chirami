#include "ImageSaver.h"

#include <utility>

namespace
{
constexpr float kJpegQuality = 0.90f;  // fixed for now; a setting later
}

ImageSaver::ImageSaver(HWND notifyWindow, UINT notifyMessage)
    : m_notifyWindow(notifyWindow),
      m_notifyMessage(notifyMessage),
      m_thread([this](std::stop_token stopToken) { WorkerProc(stopToken); })
{
}

bool ImageSaver::RequestSave(std::filesystem::path path, GUID containerFormat, LoadedImage image)
{
    {
        std::lock_guard lock(m_mutex);
        if (m_pending.has_value())
        {
            return false;  // a save is already queued or running
        }
        m_pending = Request{std::move(path), containerFormat, std::move(image)};
    }
    m_cv.notify_one();
    return true;
}

std::optional<ImageSaver::Result> ImageSaver::TakeResult()
{
    std::lock_guard lock(m_mutex);
    return std::exchange(m_completed, std::nullopt);
}

void ImageSaver::WorkerProc(std::stop_token stopToken) noexcept
try
{
    auto coInit = wil::CoInitializeEx(COINIT_MULTITHREADED);

    // Factory lives below coInit so it is released before CoUninitialize;
    // WIC objects never leave this thread.
    auto factory = wil::CoCreateInstance<IWICImagingFactory>(CLSID_WICImagingFactory);

    while (true)
    {
        Request request;
        {
            std::unique_lock lock(m_mutex);
            if (!m_cv.wait(lock, stopToken, [this] { return m_pending.has_value(); }))
            {
                return;  // stop requested
            }
            // Note: m_pending stays set while encoding so RequestSave keeps
            // rejecting; it is cleared when the result is published below.
            request = std::move(*m_pending);
        }

        Result result;
        result.path = request.path;
        result.hr = SaveImage(factory.get(), request);

        if (stopToken.stop_requested())
        {
            return;
        }
        {
            std::lock_guard lock(m_mutex);
            m_pending.reset();
            m_completed = std::move(result);
        }
        PostMessageW(m_notifyWindow, m_notifyMessage, 0, 0);
    }
}
catch (...)
{
    LOG_CAUGHT_EXCEPTION();
}

HRESULT ImageSaver::SaveImage(IWICImagingFactory* factory, const Request& request) noexcept
try
{
    const LoadedImage& image = request.image;

    wil::com_ptr<IWICBitmap> source;
    RETURN_IF_FAILED(factory->CreateBitmapFromMemory(
        image.width, image.height, GUID_WICPixelFormat32bppPBGRA, image.stride,
        static_cast<UINT>(image.pixels.size()),
        const_cast<BYTE*>(image.pixels.data()), source.put()));

    wil::com_ptr<IWICStream> stream;
    RETURN_IF_FAILED(factory->CreateStream(stream.put()));
    RETURN_IF_FAILED(stream->InitializeFromFilename(request.path.c_str(), GENERIC_WRITE));

    wil::com_ptr<IWICBitmapEncoder> encoder;
    RETURN_IF_FAILED(factory->CreateEncoder(request.containerFormat, nullptr, encoder.put()));
    RETURN_IF_FAILED(encoder->Initialize(stream.get(), WICBitmapEncoderNoCache));

    wil::com_ptr<IWICBitmapFrameEncode> frame;
    wil::com_ptr<IPropertyBag2> options;
    RETURN_IF_FAILED(encoder->CreateNewFrame(frame.put(), options.put()));

    if (request.containerFormat == GUID_ContainerFormatJpeg && options)
    {
        PROPBAG2 name{};
        WCHAR nameText[] = L"ImageQuality";
        name.pstrName = nameText;
        wil::unique_variant value;
        value.vt = VT_R4;
        value.fltVal = kJpegQuality;
        (void)options->Write(1, &name, value.addressof());  // best effort
    }
    RETURN_IF_FAILED(frame->Initialize(options.get()));
    RETURN_IF_FAILED(frame->SetSize(image.width, image.height));

    // Let the encoder pick the closest pixel format it supports (e.g. JPEG
    // has no alpha), then convert into it. Premultiplied alpha is undone by
    // the converter where the target is straight or opaque.
    WICPixelFormatGUID targetFormat = GUID_WICPixelFormat32bppPBGRA;
    RETURN_IF_FAILED(frame->SetPixelFormat(&targetFormat));
    wil::com_ptr<IWICBitmapSource> converted;
    RETURN_IF_FAILED(WICConvertBitmapSource(targetFormat, source.get(), converted.put()));

    RETURN_IF_FAILED(frame->WriteSource(converted.get(), nullptr));
    RETURN_IF_FAILED(frame->Commit());
    RETURN_IF_FAILED(encoder->Commit());
    RETURN_IF_FAILED(stream->Commit(STGC_DEFAULT));
    return S_OK;
}
CATCH_RETURN()
