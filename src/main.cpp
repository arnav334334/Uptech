#define WIN32_LEAN_AND_MEAN
#define NOMINMAX


#include <windows.h>
#include<cstring>
#include "screen_capture.h"
#include <d3dcompiler.h>
#include <chrono>
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>
#include <cstdint>
#include <cstdio>




#define A_CPU 1
#include "../external/FidelityFX-FSR/ffx-fsr/ffx_a.h"
#include "../external/FidelityFX-FSR/ffx-fsr/ffx_fsr1.h"
#include <iostream>
#include <chrono>

using Microsoft::WRL::ComPtr;



// RCAS sharpness is in "stops": 0.0 = sharpest, every +1.0 halves the effect.
// AMD's recommended default is 0.2 (0.8 is noticeably soft).
float g_rcasSharpness = 0.2f;

// Output size: swap chain, EASU output texture and viewports all use this.
constexpr UINT kOutputWidth  = 1280;
constexpr UINT kOutputHeight = 720;

// Top-left corner of the captured desktop region. In Auto mode this follows the
// game window's client area; the manual presets use the desktop's top-left.
UINT g_sourceX = 0;
UINT g_sourceY = 0;

// Auto window capture: the region follows the window you are playing in (the
// foreground window, other than UpTech itself). F8 locks / unlocks the window.
bool g_autoMode = true;
HWND g_targetWindow = nullptr;
bool g_targetLocked = false;

// Present() timing, reported once per second by the stats line.
double g_presentMsTotal = 0.0;
double g_presentMsMax = 0.0;
unsigned int g_presentCount = 0;

// Fixed-size window so DXGI never stretches the back buffer.
constexpr DWORD kWindowStyle =
    WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;

ScreenCapture g_screenCapture;

ComPtr<ID3D11Device> g_device;
ComPtr<ID3D11DeviceContext> g_context;
ComPtr<IDXGISwapChain> g_swapChain;
ComPtr<ID3D11RenderTargetView> g_renderTarget;
ComPtr<ID3D11VertexShader> g_vertexShader;
ComPtr<ID3D11PixelShader> g_pixelShader;
ComPtr<ID3D11SamplerState> g_sampler;
ComPtr<ID3D11PixelShader> g_easuPixelShader;
ComPtr<ID3D11Buffer> g_easuConstantBuffer;
// RCAS shader resources
ComPtr<ID3D11PixelShader> g_rcasPixelShader;
ComPtr<ID3D11Buffer> g_rcasConstantBuffer;

HWND g_window = nullptr;
HWND g_resolutionCombo = nullptr;


void UpdateEASUConstants(UINT outputWidth, UINT outputHeight);

// Resolution scaling resources
UINT g_internalWidth = 640;
UINT g_internalHeight = 360;

ComPtr<ID3D11Texture2D> g_scaledTexture;
ComPtr<ID3D11RenderTargetView> g_scaledRenderTarget;
ComPtr<ID3D11ShaderResourceView> g_scaledSRV;
// EASU output resources (input for RCAS)
ComPtr<ID3D11Texture2D> g_easuOutputTexture;
ComPtr<ID3D11RenderTargetView> g_easuOutputRenderTarget;
ComPtr<ID3D11ShaderResourceView> g_easuOutputSRV;

struct EASUConstants
{
    uint32_t Const0[4];
    uint32_t Const1[4];
    uint32_t Const2[4];
    uint32_t Const3[4];
};

struct RCASConstants
{
    uint32_t Const0[4];
};

bool SetInternalResolution(UINT width, UINT height);


LRESULT CALLBACK WindowProc(
    HWND hwnd,
    UINT message,
    WPARAM wParam,
    LPARAM lParam)
{
    switch (message)
    {
    case WM_COMMAND:

        if (LOWORD(wParam) == 1001 &&
            HIWORD(wParam) == CBN_SELCHANGE)
        {
            int selectedIndex = (int)SendMessageA(
                g_resolutionCombo,
                CB_GETCURSEL,
                0,
                0
            );

            if (selectedIndex == 0)
            {
                // Auto: follow the game window.
                g_autoMode = true;
                g_targetWindow = nullptr;
                g_targetLocked = false;

                std::cout << "Auto mode: following the game window "
                          << "(F8 locks / unlocks the current window)"
                          << std::endl;

                return 0;
            }

            // Manual preset: crop the top-left corner of the desktop.
            g_autoMode = false;
            g_sourceX = 0;
            g_sourceY = 0;

            UINT newWidth = 640;
            UINT newHeight = 360;

            switch (selectedIndex - 1)
            {
            case 0:
                newWidth = 1280;
                newHeight = 720;
                break;

            case 1:
                newWidth = 960;
                newHeight = 540;
                break;

            case 2:
                newWidth = 854;
                newHeight = 480;
                break;

            case 3:
                newWidth = 640;
                newHeight = 360;
                break;

            case 4:
                newWidth = 426;
                newHeight = 240;
                break;
            }

            if (!SetInternalResolution(newWidth, newHeight))
            {
                std::cout << "Failed to change internal resolution!"
                          << std::endl;
            }

            return 0;
        }

        break;

    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;

    default:
        return DefWindowProc(hwnd, message, wParam, lParam);
    }

    return DefWindowProc(hwnd, message, wParam, lParam);
}




bool InitializeEASUConstantBuffer()
{
    D3D11_BUFFER_DESC bufferDesc = {};

    bufferDesc.ByteWidth = sizeof(EASUConstants);
    bufferDesc.Usage = D3D11_USAGE_DEFAULT;
    bufferDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;

    HRESULT hr = g_device->CreateBuffer(
        &bufferDesc,
        nullptr,
        &g_easuConstantBuffer
    );

    if (FAILED(hr))
    {
        std::cout
            << "Failed to create EASU constant buffer: 0x"
            << std::hex << hr
            << std::endl;

        return false;
    }

    std::cout << "EASU constant buffer initialized!\n";

    return true;
}

bool InitializeRCASConstantBuffer()
{
    D3D11_BUFFER_DESC bufferDesc = {};

    bufferDesc.ByteWidth = sizeof(RCASConstants);
    bufferDesc.Usage = D3D11_USAGE_DEFAULT;
    bufferDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;

    HRESULT hr = g_device->CreateBuffer(
        &bufferDesc,
        nullptr,
        &g_rcasConstantBuffer
    );

    if (FAILED(hr))
    {
        std::cout
            << "Failed to create RCAS constant buffer: 0x"
            << std::hex << hr
            << std::endl;

        return false;
    }

    std::cout << "RCAS constant buffer initialized!\n";

    return true;
}

void UpdateEASUConstants(UINT outputWidth, UINT outputHeight)
{
    EASUConstants constants = {};

    FsrEasuCon(
        constants.Const0,
        constants.Const1,
        constants.Const2,
        constants.Const3,
        static_cast<AF1>(g_internalWidth),
        static_cast<AF1>(g_internalHeight),
        static_cast<AF1>(g_internalWidth),
        static_cast<AF1>(g_internalHeight),
        static_cast<AF1>(outputWidth),
        static_cast<AF1>(outputHeight)
    );

    g_context->UpdateSubresource(
        g_easuConstantBuffer.Get(),
        0,
        nullptr,
        &constants,
        0,
        0
    );

    std::cout << "EASU constants generated and uploaded!\n";
}

void UpdateRCASConstants()
{
    RCASConstants constants = {};

    FsrRcasCon(
        constants.Const0,
        g_rcasSharpness
    );

    g_context->UpdateSubresource(
        g_rcasConstantBuffer.Get(),
        0,
        nullptr,
        &constants,
        0,
        0
    );

    std::cout << "RCAS constants generated and uploaded!\n";
}


bool CreateGUIControls(HINSTANCE instance)
{
    // Group box for the settings panel.
    HWND settingsGroup = CreateWindowExA(
        0,
        "BUTTON",
        "UpTech Settings",
        WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
        15, 15, 250, 120,
        g_window,
        nullptr,
        instance,
        nullptr
    );

    if (!settingsGroup)
        return false;

    // Resolution label.
    HWND resolutionLabel = CreateWindowExA(
        0,
        "STATIC",
        "Internal Resolution:",
        WS_CHILD | WS_VISIBLE,
        30, 45, 210, 22,
        g_window,
        nullptr,
        instance,
        nullptr
    );

    if (!resolutionLabel)
        return false;

    // Resolution dropdown.
    g_resolutionCombo = CreateWindowExA(
        0,
        "COMBOBOX",
        nullptr,
        WS_CHILD | WS_VISIBLE |
        WS_VSCROLL |
        WS_TABSTOP |
        CBS_DROPDOWNLIST,
        30, 70, 215, 180,
        g_window,
        (HMENU)1001,
        instance,
        nullptr
    );

    if (!g_resolutionCombo)
        return false;

    // Auto mode first, then the manual resolution presets.
    SendMessageA(
        g_resolutionCombo,
        CB_ADDSTRING,
        0,
        (LPARAM)"Auto - follow game window"
    );

    // Add resolution presets.
    SendMessageA(
        g_resolutionCombo,
        CB_ADDSTRING,
        0,
        (LPARAM)"1280 x 720 - Native"
    );

    SendMessageA(
        g_resolutionCombo,
        CB_ADDSTRING,
        0,
        (LPARAM)"960 x 540 - Quality"
    );

    SendMessageA(
        g_resolutionCombo,
        CB_ADDSTRING,
        0,
        (LPARAM)"854 x 480 - Balanced"
    );

    SendMessageA(
        g_resolutionCombo,
        CB_ADDSTRING,
        0,
        (LPARAM)"640 x 360 - Performance"
    );

    SendMessageA(
        g_resolutionCombo,
        CB_ADDSTRING,
        0,
        (LPARAM)"426 x 240 - Ultra Performance"
    );

    // Start in Auto mode.
    SendMessageA(
        g_resolutionCombo,
        CB_SETCURSEL,
        0,
        0
    );

    // Use the standard Windows GUI font.
    HFONT guiFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

    SendMessageA(
        resolutionLabel,
        WM_SETFONT,
        (WPARAM)guiFont,
        TRUE
    );

    SendMessageA(
        g_resolutionCombo,
        WM_SETFONT,
        (WPARAM)guiFont,
        TRUE
    );

    SendMessageA(
        settingsGroup,
        WM_SETFONT,
        (WPARAM)guiFont,
        TRUE
    );

    return true;
}

bool InitializeWindow(HINSTANCE instance)
{
    const char CLASS_NAME[] = "UpTechWindow";

    WNDCLASSA wc = {};
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = instance;
    wc.lpszClassName = CLASS_NAME;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);

    if (!RegisterClassA(&wc))
        return false;

    RECT rect = { 0, 0, (LONG)kOutputWidth, (LONG)kOutputHeight };

    AdjustWindowRect(
        &rect,
        kWindowStyle,
        FALSE
    );

    g_window = CreateWindowExA(
        0,
        CLASS_NAME,
        "UpTech - Screen Capture",
        kWindowStyle | WS_CLIPCHILDREN,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        rect.right - rect.left,
        rect.bottom - rect.top,
        nullptr,
        nullptr,
        instance,
        nullptr
    );

    if (!g_window)
        return false;
    
    if (!CreateGUIControls(instance))
    return false;

    

    ShowWindow(g_window, SW_SHOW);
    SetWindowDisplayAffinity(
    g_window,
    WDA_EXCLUDEFROMCAPTURE
    );

    return true;
}


bool InitializeDirectX()
{
    DXGI_SWAP_CHAIN_DESC swapDesc = {};

    swapDesc.BufferCount = 2;
    swapDesc.BufferDesc.Width = kOutputWidth;
    swapDesc.BufferDesc.Height = kOutputHeight;
    swapDesc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swapDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapDesc.OutputWindow = g_window;
    swapDesc.SampleDesc.Count = 1;
    swapDesc.Windowed = TRUE;
    swapDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

    D3D_FEATURE_LEVEL featureLevel;

    const D3D_FEATURE_LEVEL requestedLevel =
        D3D_FEATURE_LEVEL_11_0;

    HRESULT result = D3D11CreateDeviceAndSwapChain(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        0,
        &requestedLevel,
        1,
        D3D11_SDK_VERSION,
        &swapDesc,
        &g_swapChain,
        &g_device,
        &featureLevel,
        &g_context
    );

    if (FAILED(result))
    {
        std::cout << "DirectX initialization failed: 0x"
                  << std::hex << result << std::endl;

        return false;
    }

    ComPtr<ID3D11Texture2D> backBuffer;

    result = g_swapChain->GetBuffer(
        0,
        IID_PPV_ARGS(&backBuffer)
    );

    if (FAILED(result))
        return false;

    result = g_device->CreateRenderTargetView(
        backBuffer.Get(),
        nullptr,
        &g_renderTarget
    );

    if (FAILED(result))
        return false;

    std::cout << "DirectX 11 initialized successfully!"
              << std::endl;

    return true;
}

bool SetInternalResolution(UINT width, UINT height)
{
    D3D11_TEXTURE2D_DESC textureDesc = {};

    textureDesc.Width = width;
    textureDesc.Height = height;
    textureDesc.MipLevels = 1;
    textureDesc.ArraySize = 1;
    // Must match the captured desktop format (BGRA). CopySubresourceRegion
    // cannot copy between BGRA and RGBA textures.
    textureDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    textureDesc.SampleDesc.Count = 1;
    textureDesc.Usage = D3D11_USAGE_DEFAULT;

    textureDesc.BindFlags =
        D3D11_BIND_RENDER_TARGET |
        D3D11_BIND_SHADER_RESOURCE;

    ComPtr<ID3D11Texture2D> newTexture;

    HRESULT hr = g_device->CreateTexture2D(
        &textureDesc,
        nullptr,
        &newTexture
    );

    if (FAILED(hr))
        return false;

    ComPtr<ID3D11RenderTargetView> newRenderTarget;

    hr = g_device->CreateRenderTargetView(
        newTexture.Get(),
        nullptr,
        &newRenderTarget
    );

    if (FAILED(hr))
        return false;

    ComPtr<ID3D11ShaderResourceView> newSRV;

    hr = g_device->CreateShaderResourceView(
        newTexture.Get(),
        nullptr,
        &newSRV
    );

    if (FAILED(hr))
        return false;

    // Replace the previous GPU resources.
    g_scaledTexture = newTexture;
    g_scaledRenderTarget = newRenderTarget;
    g_scaledSRV = newSRV;

    g_internalWidth = width;
    g_internalHeight = height;

    UpdateEASUConstants(kOutputWidth, kOutputHeight);

    // The destination texture is new and empty: force a fresh capture.
    g_screenCapture.Restart();

    std::cout << "Internal resolution changed to "
              << width << " x " << height << std::endl;

    return true;
}

bool InitializeEASUOutputResources()
{
    D3D11_TEXTURE2D_DESC textureDesc = {};

    textureDesc.Width = kOutputWidth;
    textureDesc.Height = kOutputHeight;
    textureDesc.MipLevels = 1;
    textureDesc.ArraySize = 1;
    textureDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    textureDesc.SampleDesc.Count = 1;
    textureDesc.Usage = D3D11_USAGE_DEFAULT;

    textureDesc.BindFlags =
        D3D11_BIND_RENDER_TARGET |
        D3D11_BIND_SHADER_RESOURCE;

    HRESULT hr = g_device->CreateTexture2D(
        &textureDesc,
        nullptr,
        &g_easuOutputTexture
    );

    if (FAILED(hr))
    {
        std::cout << "Failed to create EASU output texture: 0x"
                  << std::hex << hr << std::endl;
        return false;
    }

    hr = g_device->CreateRenderTargetView(
        g_easuOutputTexture.Get(),
        nullptr,
        &g_easuOutputRenderTarget
    );

    if (FAILED(hr))
        return false;

    hr = g_device->CreateShaderResourceView(
        g_easuOutputTexture.Get(),
        nullptr,
        &g_easuOutputSRV
    );

    if (FAILED(hr))
        return false;

    std::cout << "EASU output resources initialized!\n";

    return true;
}

bool InitializeScalingResources()
{
    return SetInternalResolution(640, 360);
}



void Render()
{
    if (g_screenCapture.HasImage())
    {
        ID3D11ShaderResourceView* nullSRV = nullptr;
        ID3D11Buffer* nullBuffer = nullptr;

        ID3D11SamplerState* sampler = g_sampler.Get();

        // ============================================
        // PASS 1: Bind the fullscreen-triangle vertex shader (the game region is
        // already copied into g_scaledTexture by CaptureRegion)
        // ============================================


        // Fullscreen-triangle vertex shader (uses SV_VertexID, no vertex buffer).
        g_context->IASetInputLayout(nullptr);
        g_context->IASetPrimitiveTopology(
            D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        g_context->VSSetShader(g_vertexShader.Get(), nullptr, 0);


        // ============================================
        // PASS 2: AMD FSR 1 EASU upscaling
        // ============================================

        g_context->OMSetRenderTargets(
            1,
            g_easuOutputRenderTarget.GetAddressOf(),
            nullptr
        );

        D3D11_VIEWPORT easuViewport = {};

        easuViewport.Width = (float)kOutputWidth;
        easuViewport.Height = (float)kOutputHeight;
        easuViewport.MinDepth = 0.0f;
        easuViewport.MaxDepth = 1.0f;

        g_context->RSSetViewports(
            1,
            &easuViewport
        );

        g_context->PSSetShader(
            g_easuPixelShader.Get(),
            nullptr,
            0
        );

        ID3D11ShaderResourceView* scaledImage =
            g_scaledSRV.Get();

        g_context->PSSetShaderResources(
            0,
            1,
            &scaledImage
        );

        ID3D11Buffer* easuBuffer =
            g_easuConstantBuffer.Get();

        g_context->PSSetConstantBuffers(
            0,
            1,
            &easuBuffer
        );

        g_context->PSSetSamplers(
            0,
            1,
            &sampler
        );

        g_context->Draw(3, 0);

        // Unbind EASU resources.
        g_context->PSSetShaderResources(
            0,
            1,
            &nullSRV
        );

        g_context->PSSetConstantBuffers(
            0,
            1,
            &nullBuffer
        );


        // ============================================
        // PASS 3: AMD FSR 1 RCAS sharpening
        // ============================================

        g_context->OMSetRenderTargets(
            1,
            g_renderTarget.GetAddressOf(),
            nullptr
        );

        D3D11_VIEWPORT rcasViewport = {};

        rcasViewport.Width = (float)kOutputWidth;
        rcasViewport.Height = (float)kOutputHeight;
        rcasViewport.MinDepth = 0.0f;
        rcasViewport.MaxDepth = 1.0f;

        g_context->RSSetViewports(
            1,
            &rcasViewport
        );

        g_context->PSSetShader(
            g_rcasPixelShader.Get(),
            nullptr,
            0
        );

        ID3D11ShaderResourceView* easuImage =
            g_easuOutputSRV.Get();

        g_context->PSSetShaderResources(
            0,
            1,
            &easuImage
        );

        ID3D11Buffer* rcasBuffer =
            g_rcasConstantBuffer.Get();

        g_context->PSSetConstantBuffers(
            0,
            1,
            &rcasBuffer
        );

        g_context->Draw(3, 0);

        // Unbind RCAS resources.
        g_context->PSSetShaderResources(
            0,
            1,
            &nullSRV
        );

        g_context->PSSetConstantBuffers(
            0,
            1,
            &nullBuffer
        );
    }
    else
    {
        const float clearColor[] =
        {
            0.02f,
            0.02f,
            0.02f,
            1.0f
        };

        g_context->OMSetRenderTargets(
            1,
            g_renderTarget.GetAddressOf(),
            nullptr
        );

        g_context->ClearRenderTargetView(
            g_renderTarget.Get(),
            clearColor
        );
    }

    auto presentStart = std::chrono::steady_clock::now();

    g_swapChain->Present(1, 0);

    double presentMs = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - presentStart).count();

    g_presentMsTotal += presentMs;
    g_presentCount++;

    if (presentMs > g_presentMsMax)
        g_presentMsMax = presentMs;
}


bool InitializeEASUShader()
{
    ComPtr<ID3DBlob> shaderBlob;
    ComPtr<ID3DBlob> errorBlob;

    HRESULT hr = D3DCompileFromFile(
        L"C:\\UpTech\\shaders\\fsr_easu.hlsl",
        nullptr,
        D3D_COMPILE_STANDARD_FILE_INCLUDE,
        "PSMain",
        "ps_5_0",
        D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3,
        0,
        &shaderBlob,
        &errorBlob
    );

    if (FAILED(hr))
    {
        if (errorBlob)
        {
            std::cout
                << "EASU shader compilation error:\n"
                << (char*)errorBlob->GetBufferPointer()
                << std::endl;
        }
        else
        {
            std::cout
                << "EASU shader compilation failed: 0x"
                << std::hex << hr
                << std::endl;
        }

        return false;
    }

    hr = g_device->CreatePixelShader(
        shaderBlob->GetBufferPointer(),
        shaderBlob->GetBufferSize(),
        nullptr,
        &g_easuPixelShader
    );

    if (FAILED(hr))
    {
        std::cout
            << "Failed to create EASU pixel shader: 0x"
            << std::hex << hr
            << std::endl;

        return false;
    }

    std::cout << "EASU shader initialized successfully!\n";

    return true;
}

bool InitializeRCASShader()
{
    ComPtr<ID3DBlob> shaderBlob;
    ComPtr<ID3DBlob> errorBlob;

    HRESULT hr = D3DCompileFromFile(
        L"C:\\UpTech\\shaders\\fsr_rcas.hlsl",
        nullptr,
        D3D_COMPILE_STANDARD_FILE_INCLUDE,
        "PSMain",
        "ps_5_0",
        D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3,
        0,
        &shaderBlob,
        &errorBlob
    );

    if (FAILED(hr))
    {
        if (errorBlob)
        {
            std::cout
                << "RCAS shader compilation error:\n"
                << (char*)errorBlob->GetBufferPointer()
                << std::endl;
        }
        else
        {
            std::cout
                << "RCAS shader compilation failed: 0x"
                << std::hex << hr
                << std::endl;
        }

        return false;
    }

    hr = g_device->CreatePixelShader(
        shaderBlob->GetBufferPointer(),
        shaderBlob->GetBufferSize(),
        nullptr,
        &g_rcasPixelShader
    );

    if (FAILED(hr))
    {
        std::cout
            << "Failed to create RCAS pixel shader: 0x"
            << std::hex << hr
            << std::endl;

        return false;
    }

    std::cout << "RCAS shader initialized successfully!\n";

    return true;
}

bool InitializeDisplayPipeline()
{
    const char* shaderCode = R"(
        Texture2D desktopTexture : register(t0);
        SamplerState desktopSampler : register(s0);

        struct VSOutput
        {
            float4 position : SV_POSITION;
            float2 uv : TEXCOORD;
        };

        VSOutput VSMain(uint vertexID : SV_VertexID)
        {
            VSOutput output;

            float2 positions[3] =
            {
                float2(-1.0, -1.0),
                float2(-1.0,  3.0),
                float2( 3.0, -1.0)
            };

            float2 position = positions[vertexID];

            output.position = float4(position, 0.0, 1.0);

            output.uv = float2(
                (position.x + 1.0) * 0.5,
                (1.0 - position.y) * 0.5
            );

            return output;
        }

        float4 PSMain(VSOutput input) : SV_TARGET
        {
            return desktopTexture.Sample(
                desktopSampler,
                input.uv
            );
        }
    )";

    ComPtr<ID3DBlob> vertexCode;
    ComPtr<ID3DBlob> pixelCode;
    ComPtr<ID3DBlob> errors;

    HRESULT hr = D3DCompile(
        shaderCode,
        strlen(shaderCode),
        nullptr,
        nullptr,
        nullptr,
        "VSMain",
        "vs_4_0",
        0,
        0,
        &vertexCode,
        &errors
    );

    if (FAILED(hr))
    {
        if (errors)
            std::cout << (char*)errors->GetBufferPointer();

        return false;
    }

    hr = D3DCompile(
        shaderCode,
        strlen(shaderCode),
        nullptr,
        nullptr,
        nullptr,
        "PSMain",
        "ps_4_0",
        0,
        0,
        &pixelCode,
        &errors
    );

    if (FAILED(hr))
    {
        if (errors)
            std::cout << (char*)errors->GetBufferPointer();

        return false;
    }

    hr = g_device->CreateVertexShader(
        vertexCode->GetBufferPointer(),
        vertexCode->GetBufferSize(),
        nullptr,
        &g_vertexShader
    );

    if (FAILED(hr))
        return false;

    hr = g_device->CreatePixelShader(
        pixelCode->GetBufferPointer(),
        pixelCode->GetBufferSize(),
        nullptr,
        &g_pixelShader
    );

    if (FAILED(hr))
        return false;

    D3D11_SAMPLER_DESC samplerDesc = {};

    samplerDesc.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;

    hr = g_device->CreateSamplerState(
        &samplerDesc,
        &g_sampler
    );

    if (FAILED(hr))
        return false;

    std::cout << "Display shader initialized!\n";

    return true;
}


// ---------------------------------------------------------------------------
// Automatic window capture
// ---------------------------------------------------------------------------

// Windows that should never become the capture target.
bool IsCaptureCandidate(HWND hwnd)
{
    if (!hwnd || !IsWindow(hwnd) || !IsWindowVisible(hwnd) || IsIconic(hwnd))
        return false;

    // Never capture ourselves.
    DWORD processId = 0;
    GetWindowThreadProcessId(hwnd, &processId);

    if (processId == GetCurrentProcessId())
        return false;

    char className[128] = {};
    GetClassNameA(hwnd, className, (int)sizeof(className));

    // Desktop, taskbar, consoles/terminals and the Alt-Tab switcher.
    static const char* const ignoredClasses[] =
    {
        "Progman",
        "WorkerW",
        "Shell_TrayWnd",
        "Shell_SecondaryTrayWnd",
        "ConsoleWindowClass",
        "CASCADIA_HOSTING_WINDOW_CLASS",
        "XamlExplorerHostIslandWindow",
        "MultitaskingViewFrame",
        "TaskSwitcherWnd",
        "ForegroundStaging"
    };

    for (const char* ignored : ignoredClasses)
    {
        if (std::strcmp(className, ignored) == 0)
            return false;
    }

    return true;
}

// Called every loop iteration in Auto mode: picks the window to capture and
// keeps the capture region on its client area.
void UpdateCaptureTarget()
{
    if (!g_autoMode)
        return;

    ULONGLONG now = GetTickCount64();

    // F8 locks / unlocks the current window, so Alt-Tabbing to a browser does
    // not switch the upscaler over to it.
    static bool f8WasDown = false;
    bool f8Down = (GetAsyncKeyState(VK_F8) & 0x8000) != 0;

    if (f8Down && !f8WasDown && g_targetWindow)
    {
        g_targetLocked = !g_targetLocked;

        std::cout << (g_targetLocked
                          ? "Locked to this window."
                          : "Unlocked - following the foreground window.")
                  << std::endl;
    }

    f8WasDown = f8Down;

    // Choose the window (checked every 100 ms). A new foreground window must
    // stay in front for 400 ms before it is adopted, which filters out
    // pop-ups and the Alt-Tab switcher.
    static ULONGLONG lastCheck = 0;
    static HWND pending = nullptr;
    static ULONGLONG pendingSince = 0;

    if (now - lastCheck >= 100)
    {
        lastCheck = now;

        if (g_targetWindow && !IsWindow(g_targetWindow))
        {
            g_targetWindow = nullptr;
            g_targetLocked = false;
        }

        if (!g_targetLocked)
        {
            HWND foreground = GetForegroundWindow();

            if (IsCaptureCandidate(foreground) &&
                foreground != g_targetWindow)
            {
                if (foreground != pending)
                {
                    pending = foreground;
                    pendingSince = now;
                }
                else if (now - pendingSince >= 400)
                {
                    g_targetWindow = foreground;
                    pending = nullptr;

                    char title[128] = {};
                    GetWindowTextA(g_targetWindow, title, (int)sizeof(title));

                    std::cout << "Capturing window: \"" << title
                              << "\"  (F8 locks / unlocks it)" << std::endl;
                }
            }
            else
            {
                pending = nullptr;
            }
        }
    }

    // Follow the window's client area (position every loop, size throttled).
    if (!g_targetWindow || IsIconic(g_targetWindow))
        return;

    RECT client = {};

    if (!GetClientRect(g_targetWindow, &client))
        return;

    POINT origin = { 0, 0 };

    if (!ClientToScreen(g_targetWindow, &origin))
        return;

    // Convert to the captured monitor's coordinates and clip to it.
    LONG left = origin.x - g_screenCapture.DesktopLeft();
    LONG top = origin.y - g_screenCapture.DesktopTop();
    LONG right = left + (client.right - client.left);
    LONG bottom = top + (client.bottom - client.top);

    if (left < 0)
        left = 0;

    if (top < 0)
        top = 0;

    if (right > (LONG)g_screenCapture.DesktopWidth())
        right = (LONG)g_screenCapture.DesktopWidth();

    if (bottom > (LONG)g_screenCapture.DesktopHeight())
        bottom = (LONG)g_screenCapture.DesktopHeight();

    LONG width = right - left;
    LONG height = bottom - top;

    // Too small, minimised or on another monitor: keep the last region.
    if (width < 64 || height < 64)
        return;

    g_sourceX = (UINT)left;
    g_sourceY = (UINT)top;

    // A size change recreates textures, so apply it at most 4 times a second.
    static ULONGLONG lastResize = 0;

    if (((UINT)width != g_internalWidth ||
         (UINT)height != g_internalHeight) &&
        now - lastResize >= 250)
    {
        lastResize = now;
        SetInternalResolution((UINT)width, (UINT)height);
    }
}

// Same mechanism capture tools such as OBS use so a game that saturates the GPU
// cannot starve them. gdi32 exports it; the header is only in the WDK, so it is
// loaded dynamically. 4 = D3DKMT_SCHEDULINGPRIORITYCLASS_HIGH (IDLE=0, BELOW=1,
// NORMAL=2, ABOVE=3, HIGH=4, REALTIME=5). May need administrator rights.
typedef LONG(WINAPI* PFN_SetProcessSchedulingPriorityClass)(HANDLE, int);

void RaiseGpuSchedulingPriority()
{
    HMODULE gdi32 = LoadLibraryA("gdi32.dll");

    PFN_SetProcessSchedulingPriorityClass setClass = gdi32
        ? reinterpret_cast<PFN_SetProcessSchedulingPriorityClass>(
              GetProcAddress(gdi32, "D3DKMTSetProcessSchedulingPriorityClass"))
        : nullptr;

    if (!setClass)
    {
        std::cout << "GPU scheduling class: function not available"
                  << std::endl;
        return;
    }

    LONG status = setClass(GetCurrentProcess(), 4);

    std::cout << "GPU scheduling class HIGH -> 0x" << std::hex
              << (unsigned int)status << std::dec
              << (status == 0 ? " (ok)" : " (not applied)") << std::endl;
}

int main()
{
    // Without this, Windows bitmap-stretches the window on scaled displays
    // (125% / 150%), which blurs the upscaled output.
    if (!SetProcessDpiAwarenessContext(
            DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2))
    {
        SetProcessDPIAware();
    }

    // Keep the CPU side of this process from being outcompeted by the game.
    SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS);

    std::cout << "Starting UpTech..." << std::endl;

    if (!InitializeWindow(GetModuleHandle(nullptr)))
    {
        std::cout << "Window initialization failed!"
                  << std::endl;
        return 1;
    }

    if (!InitializeDirectX())
    {
        std::cout << "DirectX initialization failed!"
                  << std::endl;
        return 1;
    }

    RaiseGpuSchedulingPriority();

    // Ask the GPU scheduler to favour this process: a game that saturates the
    // GPU can otherwise starve this (tiny) upscaling workload. Priorities above
    // 0 may need administrator rights, and not every driver honours them.
    {
        ComPtr<IDXGIDevice> dxgiDevice;

        if (SUCCEEDED(g_device.As(&dxgiDevice)))
        {
            HRESULT hrPriority = dxgiDevice->SetGPUThreadPriority(7);

            std::cout << "SetGPUThreadPriority(7) -> 0x" << std::hex
                      << (unsigned int)hrPriority << std::dec << std::endl;

            ComPtr<IDXGIDevice1> dxgiDevice1;

            if (SUCCEEDED(dxgiDevice.As(&dxgiDevice1)))
                // Allow a few frames in flight (DXGI default is 3). With a busy GPU,
                // waiting for each frame to finish before starting the next
                // makes throughput collapse to 1 / latency.
                dxgiDevice1->SetMaximumFrameLatency(3);
        }
    }

    if (!g_screenCapture.Initialize(g_device.Get()))
    {
        std::cout << "Screen capture initialization failed!"
                  << std::endl;
        return 1;
    }

    if (!InitializeDisplayPipeline())
    {
        std::cout << "Display pipeline initialization failed!"
                << std::endl;
        return 1;
    }

    if (!InitializeEASUShader())
    {
        std::cout << "EASU shader initialization failed!" << std::endl;
        return -1;
    }

    if (!InitializeRCASShader())
    {
        std::cout << "RCAS shader initialization failed!" << std::endl;
        return -1;
    }

    if (!InitializeEASUConstantBuffer())
    {
        std::cout << "EASU constant buffer initialization failed!" << std::endl;
        return -1;
    }

    if (!InitializeRCASConstantBuffer())
    {
        std::cout << "RCAS constant buffer initialization failed!" << std::endl;
        return -1;
    }

    UpdateRCASConstants();

    if (!InitializeScalingResources())
    {
        std::cout << "Scaling resources initialization failed!"
                << std::endl;
        return -1;
    }

    if (!InitializeEASUOutputResources())
    {
        std::cout << "EASU output resources initialization failed!"
                << std::endl;
        return -1;
    }

    std::cout << "\nContinuous capture started!"
              << std::endl;

    std::cout << "Move your mouse or move windows around "
              << "to generate desktop updates."
              << std::endl;

    std::cout << "Close the UpTech window to stop."
              << std::endl;

    MSG message = {};

    using Clock = std::chrono::steady_clock;

    auto lastReport = Clock::now();

    unsigned int capturedFrames = 0;
    unsigned int renderedFrames = 0;
    unsigned int loops = 0;
    double captureMsTotal = 0.0;
    double renderMsTotal = 0.0;

    while (message.message != WM_QUIT)
    {
        if (PeekMessage(
            &message,
            nullptr,
            0,
            0,
            PM_REMOVE))
        {
            TranslateMessage(&message);
            DispatchMessage(&message);
        }
        else
        {
            UpdateCaptureTarget();

            auto captureStart = Clock::now();
            bool newFrame = g_screenCapture.CaptureRegion(
                g_scaledTexture.Get(),
                g_sourceX,
                g_sourceY,
                g_internalWidth,
                g_internalHeight);
            auto captureEnd = Clock::now();

            // Only redo the GPU work when the desktop actually changed.
            if (newFrame || !g_screenCapture.HasImage())
            {
                Render();
                renderedFrames++;
            }
            else
            {
                Sleep(1);
            }

            auto renderEnd = Clock::now();

            if (newFrame)
                capturedFrames++;

            loops++;

            captureMsTotal += std::chrono::duration<double, std::milli>(
                captureEnd - captureStart).count();
            renderMsTotal += std::chrono::duration<double, std::milli>(
                renderEnd - captureEnd).count();

            if (renderEnd - lastReport >= std::chrono::seconds(1))
            {
                double secs = std::chrono::duration<double>(
                    renderEnd - lastReport).count();

                unsigned int l = loops ? loops : 1;
                unsigned int r = renderedFrames ? renderedFrames : 1;
                unsigned int p = g_presentCount ? g_presentCount : 1;

                char line[512];

                std::snprintf(
                    line, sizeof(line),
                    "[stats] loops/s=%.0f newFrames/s=%.0f rendered/s=%.0f"
                    " | capture avg=%.2fms render avg=%.2fms"
                    " present avg=%.2fms max=%.2fms"
                    " | timeouts=%u accessLost=%u errors=%u lastErr=0x%08X\n",
                    loops / secs, capturedFrames / secs, renderedFrames / secs,
                    captureMsTotal / l, renderMsTotal / r,
                    g_presentMsTotal / p, g_presentMsMax,
                    g_screenCapture.timeoutCount,
                    g_screenCapture.accessLostCount,
                    g_screenCapture.errorCount,
                    (unsigned int)g_screenCapture.lastError);

                std::cout << line << std::flush;

                lastReport = renderEnd;
                loops = 0;
                capturedFrames = 0;
                renderedFrames = 0;
                captureMsTotal = 0.0;
                renderMsTotal = 0.0;
                g_presentMsTotal = 0.0;
                g_presentMsMax = 0.0;
                g_presentCount = 0;
            }
        }
    }

    std::cout << "UpTech closed." << std::endl;

    return 0;
}