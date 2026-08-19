/*
Copyright (c) FufuLauncher Dev Team. All rights reserved.
Licensed under the AGPL-3.0 License.
*/
#include "Network.h"
#include "../Config/Config.h"

int WSAAPI hk_send(SOCKET s, const char* buf, int len, int flags) {
    if (Config::Get().enable_network_toggle && Config::Get().is_currently_blocking) {
        return len;
    }
    return ((tSend)o_send.load())(s, buf, len, flags);
}

int WSAAPI hk_sendto(SOCKET s, const char* buf, int len, int flags, const struct sockaddr* to, int tolen) {
    if (Config::Get().enable_network_toggle && Config::Get().is_currently_blocking) {
        return len;
    }
    return ((tSendTo)o_sendto.load())(s, buf, len, flags, to, tolen);
}

HRESULT __stdcall hk_Present1_Detect(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT PresentFlags, const DXGI_PRESENT_PARAMETERS* pPresentParameters) {
    static bool s_Warned = false;
    if (!s_Warned) {
        s_Warned = true;
        MessageBoxA(NULL,
                    "\xE6\xA3\x80\xE6\xB5\x8B\xE5\x88\xB0\xE4\xBD\xA0\xE5\xB7\xB2\xE5\xBC\x80\xE5\x90\xAF NVIDIA AI\xE6\x8F\x92\xE5\xB8\xA7\n\n"
                    "\xE6\xAD\xA4\xE5\x8A\x9F\xE8\x83\xBD\xE4\xB8\x8E\xE8\xBE\x85\xE5\x8A\xA9\xE8\x8F\x9C\xE5\x8D\x95\xE5\x86\xB2\xE7\xAA\x81\xEF\xBC\x8C\xE4\xBC\x9A\xE5\xAF\xBC\xE8\x87\xB4\xE9\xBB\x91\xE5\xB1\x8F\xE6\x88\x96\xE6\x97\xA0\xE6\xB3\x95\xE6\x98\xBE\xE7\xA4\xBA\xE7\x94\xBB\xE9\x9D\xA2\n"
                    "\xE8\xAF\xB7\xE8\xBF\x9B\xE5\x85\xA5NVIDIA\xE8\xAE\xBE\xE7\xBD\xAE\xE5\x85\xB3\xE9\x97\xAD [AI\xE6\x8F\x92\xE5\xB8\xA7] \xE9\x80\x89\xE9\xA1\xB9\xE5\x8D\xB3\xE5\x8F\xAF\xE6\x81\xA2\xE5\xA4\x8D\xE6\xAD\xA3\xE5\xB8\xB8",
                    "\xE8\xAD\xA6\xE5\x91\x8A", MB_ICONWARNING | MB_OK | MB_TOPMOST);
    }

    return o_Present1(pSwapChain, SyncInterval, PresentFlags, pPresentParameters);
}

HRESULT __stdcall hk_ResizeBuffers(IDXGISwapChain* pSwapChain, UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT NewFormat, UINT SwapChainFlags) {
    if (g_mainRenderTargetView) {
        g_pd3dContext->OMSetRenderTargets(0, 0, 0);
        g_mainRenderTargetView->Release();
        g_mainRenderTargetView = nullptr;
    }

    HRESULT hr = o_ResizeBuffers(pSwapChain, BufferCount, Width, Height, NewFormat, SwapChainFlags);

    if (g_hGameWindow_ImGui) {
        RECT rect;
        GetClientRect(g_hGameWindow_ImGui, &rect);
    }

    return hr;
}
