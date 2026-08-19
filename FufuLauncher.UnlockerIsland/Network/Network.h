/*
Copyright (c) FufuLauncher Dev Team. All rights reserved.
Licensed under the AGPL-3.0 License.
*/
#pragma once
#include "../Core/SharedState.h"

int WSAAPI hk_send(SOCKET s, const char* buf, int len, int flags);
int WSAAPI hk_sendto(SOCKET s, const char* buf, int len, int flags, const struct sockaddr* to, int tolen);
HRESULT __stdcall hk_Present1_Detect(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT PresentFlags, const DXGI_PRESENT_PARAMETERS* pPresentParameters);
HRESULT __stdcall hk_ResizeBuffers(IDXGISwapChain* pSwapChain, UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT NewFormat, UINT SwapChainFlags);
