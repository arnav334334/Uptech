#pragma once

#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

class ScreenCapture
{
public:
    bool Initialize(ID3D11Device* device);

    bool CaptureFrame();

    ID3D11ShaderResourceView* GetCapturedImage();

    // Acquires the next desktop frame and copies ONLY the requested region
    // straight into 'destination' (no full-desktop copy). Returns true when a
    // new image was copied.
    bool CaptureRegion(
        ID3D11Texture2D* destination,
        UINT sourceX,
        UINT sourceY,
        UINT width,
        UINT height);

    // True once at least one image has been copied by CaptureRegion().
    bool HasImage() const { return haveImage; }

    // Drops the current duplication so the next CaptureRegion() call gets a
    // fresh frame immediately (used after the destination texture changes).
    void Restart();

    // Position and size of the captured monitor in virtual-desktop
    // coordinates (needed to convert window positions to capture coordinates).
    LONG DesktopLeft() const { return desktopLeft; }
    LONG DesktopTop() const { return desktopTop; }
    UINT DesktopWidth() const { return desktopWidth; }
    UINT DesktopHeight() const { return desktopHeight; }

    // Diagnostics, printed by the stats line in main().
    unsigned int timeoutCount = 0;
    unsigned int accessLostCount = 0;
    unsigned int errorCount = 0;
    HRESULT lastError = S_OK;

    bool CopyRegion(
    ID3D11DeviceContext* context,
    ID3D11Texture2D* destination,
    UINT sourceX,
    UINT sourceY,
    UINT width,
    UINT height
);

private:
    Microsoft::WRL::ComPtr<IDXGIOutputDuplication>
        duplication;

    Microsoft::WRL::ComPtr<ID3D11Texture2D>
        capturedTexture;

    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>
        capturedSRV;

    UINT textureWidth = 0;
    UINT textureHeight = 0;

    // Kept so desktop duplication can be recreated after it is lost.
    Microsoft::WRL::ComPtr<ID3D11Device> ownerDevice;
    ULONGLONG lastRetryTick = 0;
    void TryRecover();
    bool haveImage = false;

    LONG desktopLeft = 0;
    LONG desktopTop = 0;
    UINT desktopWidth = 0;
    UINT desktopHeight = 0;
};