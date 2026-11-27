#include "screen_capture.h"

#include <windows.h>

#include <iostream>

using Microsoft::WRL::ComPtr;

bool ScreenCapture::Initialize(ID3D11Device* device)
{
    if (!device)
        return false;

    ownerDevice = device;

    ComPtr<IDXGIDevice> dxgiDevice;

    HRESULT hr = device->QueryInterface(
        IID_PPV_ARGS(&dxgiDevice)
    );

    if (FAILED(hr))
        return false;

    ComPtr<IDXGIAdapter> adapter;

    hr = dxgiDevice->GetAdapter(&adapter);

    if (FAILED(hr))
        return false;

    ComPtr<IDXGIOutput> output;

    hr = adapter->EnumOutputs(0, &output);

    if (FAILED(hr))
        return false;

    // Remember where this monitor sits on the virtual desktop.
    DXGI_OUTPUT_DESC outputDesc = {};

    if (SUCCEEDED(output->GetDesc(&outputDesc)))
    {
        desktopLeft = outputDesc.DesktopCoordinates.left;
        desktopTop = outputDesc.DesktopCoordinates.top;
        desktopWidth = (UINT)(outputDesc.DesktopCoordinates.right -
                              outputDesc.DesktopCoordinates.left);
        desktopHeight = (UINT)(outputDesc.DesktopCoordinates.bottom -
                               outputDesc.DesktopCoordinates.top);
    }
    else
    {
        desktopLeft = 0;
        desktopTop = 0;
        desktopWidth = (UINT)GetSystemMetrics(SM_CXSCREEN);
        desktopHeight = (UINT)GetSystemMetrics(SM_CYSCREEN);
    }

    ComPtr<IDXGIOutput1> output1;

    hr = output.As(&output1);

    if (FAILED(hr))
        return false;

    hr = output1->DuplicateOutput(
        device,
        &duplication
    );

    if (FAILED(hr))
    {
        std::cout << "Desktop duplication failed: 0x"
                  << std::hex << hr << std::endl;

        return false;
    }

    std::cout << "Desktop capture initialized!\n";

    return true;
}


bool ScreenCapture::CaptureFrame()
{
    // Duplication was lost earlier (or never created): try to bring it back.
    if (!duplication)
    {
        TryRecover();
        return false;
    }

    DXGI_OUTDUPL_FRAME_INFO frameInfo = {};

    ComPtr<IDXGIResource> desktopResource;

    HRESULT hr = duplication->AcquireNextFrame(
        0,
        &frameInfo,
        &desktopResource
    );

    if (hr == DXGI_ERROR_WAIT_TIMEOUT)
    {
        timeoutCount++;
        return false;
    }

    if (hr == DXGI_ERROR_ACCESS_LOST)
    {
        // Display mode change, UAC prompt, fullscreen switch, ...
        // The duplication object is dead and must be recreated.
        accessLostCount++;
        lastError = hr;
        duplication.Reset();
        return false;
    }

    if (FAILED(hr))
    {
        errorCount++;
        lastError = hr;
        return false;
    }

    // Pointer-only update (LastPresentTime == 0): the desktop image did not
    // change, so skip the full-desktop copy and the GPU work after it.
    if (frameInfo.LastPresentTime.QuadPart == 0 && capturedTexture)
    {
        duplication->ReleaseFrame();
        return false;
    }

    bool success = false;

    ComPtr<ID3D11Texture2D> desktopTexture;

    hr = desktopResource.As(&desktopTexture);

    if (SUCCEEDED(hr))
    {
        D3D11_TEXTURE2D_DESC desc = {};
        desktopTexture->GetDesc(&desc);

        if (desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM)
        {
            // Create or recreate our GPU texture if needed.
            if (!capturedTexture ||
                textureWidth != desc.Width ||
                textureHeight != desc.Height)
            {
                D3D11_TEXTURE2D_DESC textureDesc = {};

                textureDesc.Width = desc.Width;
                textureDesc.Height = desc.Height;
                textureDesc.MipLevels = 1;
                textureDesc.ArraySize = 1;
                textureDesc.Format = desc.Format;
                textureDesc.SampleDesc.Count = 1;
                textureDesc.Usage = D3D11_USAGE_DEFAULT;
                textureDesc.BindFlags =
                    D3D11_BIND_SHADER_RESOURCE;

                ComPtr<ID3D11Texture2D> newTexture;

                // Get the device from the duplication resource.
                ComPtr<ID3D11Device> device;
                desktopTexture->GetDevice(&device);

                hr = device->CreateTexture2D(
                    &textureDesc,
                    nullptr,
                    &newTexture
                );

                if (SUCCEEDED(hr))
                {
                    ComPtr<ID3D11ShaderResourceView> newSRV;

                    hr = device->CreateShaderResourceView(
                        newTexture.Get(),
                        nullptr,
                        &newSRV
                    );

                    if (SUCCEEDED(hr))
                    {
                        capturedTexture = newTexture;
                        capturedSRV = newSRV;

                        textureWidth = desc.Width;
                        textureHeight = desc.Height;
                    }
                }
            }

            if (capturedTexture && capturedSRV)
            {
                ComPtr<ID3D11Device> device;
                desktopTexture->GetDevice(&device);

                ComPtr<ID3D11DeviceContext> context;
                device->GetImmediateContext(&context);

                context->CopyResource(
                    capturedTexture.Get(),
                    desktopTexture.Get()
                );

                success = true;
            }
        }
        else
        {
            std::cout << "Unsupported desktop texture format: "
                      << desc.Format << std::endl;
        }
    }

    duplication->ReleaseFrame();

    return success;
}


ID3D11ShaderResourceView* ScreenCapture::GetCapturedImage()
{
    return capturedSRV.Get();
}

bool ScreenCapture::CopyRegion(
    ID3D11DeviceContext* context,
    ID3D11Texture2D* destination,
    UINT sourceX,
    UINT sourceY,
    UINT width,
    UINT height
)
{
    if (!context || !destination || !capturedTexture)
        return false;

    if (sourceX + width > textureWidth ||
        sourceY + height > textureHeight)
    {
        std::cout << "Crop region is outside the captured desktop!\n";
        return false;
    }

    D3D11_BOX sourceBox = {};

    sourceBox.left = sourceX;
    sourceBox.top = sourceY;
    sourceBox.front = 0;

    sourceBox.right = sourceX + width;
    sourceBox.bottom = sourceY + height;
    sourceBox.back = 1;

    context->CopySubresourceRegion(
        destination,
        0,
        0,
        0,
        0,
        capturedTexture.Get(),
        0,
        &sourceBox
    );

    return true;
}

void ScreenCapture::TryRecover()
{
    // Retry at most twice a second.
    ULONGLONG now = GetTickCount64();

    if (!ownerDevice || now - lastRetryTick < 500)
        return;

    lastRetryTick = now;

    Initialize(ownerDevice.Get());
}


bool ScreenCapture::CaptureRegion(
    ID3D11Texture2D* destination,
    UINT sourceX,
    UINT sourceY,
    UINT width,
    UINT height)
{
    if (!destination || !ownerDevice)
        return false;

    if (!duplication)
    {
        TryRecover();
        return false;
    }

    DXGI_OUTDUPL_FRAME_INFO frameInfo = {};

    ComPtr<IDXGIResource> desktopResource;

    HRESULT hr = duplication->AcquireNextFrame(
        0,
        &frameInfo,
        &desktopResource
    );

    if (hr == DXGI_ERROR_WAIT_TIMEOUT)
    {
        timeoutCount++;
        return false;
    }

    if (hr == DXGI_ERROR_ACCESS_LOST)
    {
        accessLostCount++;
        lastError = hr;
        duplication.Reset();
        return false;
    }

    if (FAILED(hr))
    {
        errorCount++;
        lastError = hr;
        return false;
    }

    // Pointer-only update: the desktop image did not change.
    if (frameInfo.LastPresentTime.QuadPart == 0 && haveImage)
    {
        duplication->ReleaseFrame();
        return false;
    }

    bool success = false;

    ComPtr<ID3D11Texture2D> desktopTexture;

    if (SUCCEEDED(desktopResource.As(&desktopTexture)))
    {
        D3D11_TEXTURE2D_DESC desc = {};
        desktopTexture->GetDesc(&desc);

        if (desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM)
        {
            // For example an HDR desktop (FP16).
            errorCount++;
            lastError = DXGI_ERROR_UNSUPPORTED;
        }
        else if (sourceX + width > desc.Width ||
                 sourceY + height > desc.Height)
        {
            errorCount++;
            lastError = E_INVALIDARG;
        }
        else
        {
            ComPtr<ID3D11DeviceContext> context;
            ownerDevice->GetImmediateContext(&context);

            D3D11_BOX box = {};

            box.left = sourceX;
            box.top = sourceY;
            box.front = 0;

            box.right = sourceX + width;
            box.bottom = sourceY + height;
            box.back = 1;

            context->CopySubresourceRegion(
                destination,
                0,
                0,
                0,
                0,
                desktopTexture.Get(),
                0,
                &box
            );

            success = true;
            haveImage = true;
        }
    }

    duplication->ReleaseFrame();

    return success;
}


void ScreenCapture::Restart()
{
    duplication.Reset();
    haveImage = false;
    lastRetryTick = 0;
}
