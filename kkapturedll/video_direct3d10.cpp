/* kkapture: intrusive demo video capturing.
 * Copyright (c) 2005-2010 Fabian "ryg/farbrausch" Giesen.
 *
 * This program is free software; you can redistribute and/or modify it under
 * the terms of the Artistic License, Version 2.0beta5, or (at your opinion)
 * any later version; all distributions of this program should contain this
 * license in a file named "LICENSE.txt".
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT UNLESS REQUIRED BY
 * LAW OR AGREED TO IN WRITING WILL ANY COPYRIGHT HOLDER OR CONTRIBUTOR
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY,
 * OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT
 * OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
 * OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE,
 * EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "stdafx.h"
#include "video.h"
#include "videoencoder.h"

#include <InitGuid.h>
#include <dxgi.h>
#include <dxgi1_3.h>
#include <d3d11.h>
#include <d3d10.h>

static HRESULT (__stdcall *Real_CreateDXGIFactory)(REFIID riid,void **ppFactory) = 0;
static HRESULT (__stdcall *Real_CreateDXGIFactory1)(REFIID riid,void **ppFactory) = 0;
static HRESULT(__stdcall* Real_CreateDXGIFactory2)(REFIID riid, void** ppFactory) = 0;
static HRESULT (__stdcall *Real_Factory_CreateSwapChain)(IUnknown *me,IUnknown *dev,DXGI_SWAP_CHAIN_DESC *desc,IDXGISwapChain **chain) = 0;
static HRESULT (__stdcall *Real_D3D11CreateDeviceAndSwapChain)(IDXGIAdapter *pAdapter, D3D_DRIVER_TYPE DriverType, HMODULE Software, UINT Flags, const D3D_FEATURE_LEVEL *pFeatureLevels, UINT FeatureLevels, UINT SDKVersion, const DXGI_SWAP_CHAIN_DESC *pSwapChainDesc, IDXGISwapChain **ppSwapChain, ID3D11Device **ppDevice, D3D_FEATURE_LEVEL *pFeatureLevel, ID3D11DeviceContext **ppImmediateContext);
static HRESULT (__stdcall *Real_SwapChain_Present)(IDXGISwapChain *me,UINT SyncInterval,UINT Flags) = 0;

static bool grabFrameD3D10(IDXGISwapChain *swap)
{
  ID3D10Device *device = 0;
  ID3D10Texture2D *tex = 0, *captureTex = 0;

  if (FAILED(swap->GetBuffer(0, IID_ID3D10Texture2D, (void**)&tex)))
    return false;

  D3D10_TEXTURE2D_DESC desc;
  tex->GetDevice(&device);
  if (!device)
    return false;  // not D3D10
  tex->GetDesc(&desc);

  // re-creating the capture staging texture each frame is definitely not the most efficient
  // way to handle things, but it frees me of all kind of resource management trouble, so
  // here goes...
  desc.MipLevels = 1;
  desc.ArraySize = 1;
  desc.SampleDesc.Count = 1;
  desc.SampleDesc.Quality = 0;
  desc.Usage = D3D10_USAGE_STAGING;
  desc.BindFlags = 0;
  desc.CPUAccessFlags = D3D10_CPU_ACCESS_READ;
  desc.MiscFlags = 0;

  if(FAILED(device->CreateTexture2D(&desc,0,&captureTex)))
    printLog("video/d3d10: couldn't create staging texture for gpu->cpu download!\n");
  else
    setCaptureResolution(desc.Width,desc.Height);

  device->CopySubresourceRegion(captureTex,0,0,0,0,tex,0,0);

  D3D10_MAPPED_TEXTURE2D mapped;
  bool grabOk = false;

  if(captureTex && SUCCEEDED(captureTex->Map(0,D3D10_MAP_READ,0,&mapped)))
  {
    switch(desc.Format)
    {
    case DXGI_FORMAT_R8G8B8A8_UNORM:
      blitAndFlipRGBAToCaptureData((unsigned char *) mapped.pData,mapped.RowPitch);
      grabOk = true;
      break;

    default:
      printLog("video/d3d10: unsupported backbuffer format, can't grab pixels!\n");
      break;
    }

    captureTex->Unmap(0);
  }

  tex->Release();
  if(captureTex) captureTex->Release();
  if(device) device->Release();

  return grabOk;
}

static bool grabFrameD3D11(IDXGISwapChain *swap)
{
  ID3D11Device *device = 0;
  ID3D11DeviceContext *context = 0;
  ID3D11Texture2D *tex = 0, *captureTex = 0;

  if (FAILED(swap->GetBuffer(0, IID_ID3D11Texture2D, (void**)&tex)))
    return false;

  D3D11_TEXTURE2D_DESC desc;
  tex->GetDevice(&device);
  if (!device)
    return false;  // not D3D11
  tex->GetDesc(&desc);

  // re-creating the capture staging texture each frame is definitely not the most efficient
  // way to handle things, but it frees me of all kind of resource management trouble, so
  // here goes...
  desc.MipLevels = 1;
  desc.ArraySize = 1;
  desc.SampleDesc.Count = 1;
  desc.SampleDesc.Quality = 0;
  desc.Usage = D3D11_USAGE_STAGING;
  desc.BindFlags = 0;
  desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  desc.MiscFlags = 0;

  if(FAILED(device->CreateTexture2D(&desc,0,&captureTex)))
    printLog("video/d3d11: couldn't create staging texture for gpu->cpu download!\n");
  else
    setCaptureResolution(desc.Width,desc.Height);

  device->GetImmediateContext(&context);
  context->CopySubresourceRegion(captureTex,0,0,0,0,tex,0,0);

  D3D11_MAPPED_SUBRESOURCE mapped;
  bool grabOk = false;

  if(captureTex && SUCCEEDED(context->Map(captureTex,0,D3D11_MAP_READ,0,&mapped)))
  {
    switch(desc.Format)
    {
    case DXGI_FORMAT_R8G8B8A8_UNORM:
      blitAndFlipRGBAToCaptureData((unsigned char *) mapped.pData,mapped.RowPitch);
      grabOk = true;
      break;

    default:
      printLog("video/d3d11: unsupported backbuffer format, can't grab pixels!\n");
      break;
    }

    context->Unmap(captureTex,0);
  }

  tex->Release();
  if(captureTex) captureTex->Release();
  context->Release();
  device->Release();

  return grabOk;
}

static HRESULT __stdcall Mine_SwapChain_Present(IDXGISwapChain *me,UINT SyncInterval,UINT Flags)
{

  if(params.CaptureVideo)
  {
    if (grabFrameD3D10(me) || grabFrameD3D11(me))
      encoder->WriteFrame(captureData);
  }

  HRESULT hr = Real_SwapChain_Present(me,0,Flags);

  nextFrame();
  return hr;
}

static HRESULT __stdcall Mine_Factory_CreateSwapChain(IUnknown *me,IUnknown *dev,DXGI_SWAP_CHAIN_DESC *desc,IDXGISwapChain **chain)
{
  HRESULT hr = Real_Factory_CreateSwapChain(me,dev,desc,chain);
  if(SUCCEEDED(hr))
  {
    printLog("video/dxgi: factory @ %p created swap chain @ %p\n", me, *chain);
    HookCOMOnce(&Real_SwapChain_Present,*chain,8,Mine_SwapChain_Present);
  }

  return hr;
}

void HookDXGIFactory(REFIID riid, void* pFactory)
{
    if (riid == IID_IDXGIFactory || riid == IID_IDXGIFactory1 || riid == IID_IDXGIFactory2) 
    {
        HookCOMOnce(&Real_Factory_CreateSwapChain, (IUnknown*)pFactory, 10, Mine_Factory_CreateSwapChain);
    }
}

static HRESULT __stdcall Mine_CreateDXGIFactory(REFIID riid,void **ppFactory)
{
  HRESULT hr = Real_CreateDXGIFactory(riid,ppFactory);
  if (SUCCEEDED(hr)) 
  {
      printLog("video/dxgi: DXGIFactory created @ %p\n", *ppFactory);
      MEMORY_BASIC_INFORMATION mbi;
      if (VirtualQuery(*ppFactory, &mbi, sizeof(MEMORY_BASIC_INFORMATION)))
      {
          printLog("video/debug: AllocationProtect %x @ %p\n", mbi.AllocationProtect, *ppFactory);
          if (mbi.AllocationProtect == PAGE_READWRITE)
          {
              HookDXGIFactory(riid, *ppFactory);
          }
      }
      else
      {
          printLog("video/debug: Failed to query %p", *ppFactory);
          DWORD lastErr = GetLastError();
          printLog(", Last Error %#X\n", lastErr);
      }
  }
  return hr;
}

static HRESULT __stdcall Mine_CreateDXGIFactory1(REFIID riid,void **ppFactory)
{
  HRESULT hr = Real_CreateDXGIFactory1(riid,ppFactory);
  if (SUCCEEDED(hr)) 
  {
      printLog("video/dxgi: DXGIFactory1 created @ %p\n", *ppFactory);
      MEMORY_BASIC_INFORMATION mbi;
      if(VirtualQuery(*ppFactory, &mbi, sizeof(MEMORY_BASIC_INFORMATION)))
      {
          printLog("video/debug: AllocationProtect %x @ %p\n", mbi.AllocationProtect, *ppFactory);
          if (mbi.AllocationProtect == PAGE_READWRITE)
          {
              HookDXGIFactory(riid, *ppFactory);
          }
      } 
      else
      {
          printLog("video/debug: Failed to query %p", *ppFactory);
          DWORD lastErr = GetLastError();
          printLog(", Last Error %#X\n", lastErr);
      }
  }
  return hr;
}

static HRESULT __stdcall Mine_CreateDXGIFactory2(REFIID riid, void** ppFactory)
{
    HRESULT hr = Real_CreateDXGIFactory2(riid, ppFactory);
    if (SUCCEEDED(hr)) 
    {
        printLog("video/dxgi: DXGIFactory2 created @ %p\n", *ppFactory);
        MEMORY_BASIC_INFORMATION mbi;
        if (VirtualQuery(*ppFactory, &mbi, sizeof(MEMORY_BASIC_INFORMATION)))
        {
            printLog("video/debug: AllocationProtect %x @ %p\n", mbi.AllocationProtect, *ppFactory);
            if (mbi.AllocationProtect == PAGE_READWRITE) 
            {
                HookDXGIFactory(riid, *ppFactory);
            }
        }
        else
        {
            printLog("video/debug: Failed to query %p", *ppFactory);
            DWORD lastErr = GetLastError();
            printLog(", Last Error %#X\n", lastErr);
        }
    }
    return hr;
}

static HRESULT __stdcall Mine_D3D11CreateDeviceAndSwapChain(IDXGIAdapter *pAdapter, D3D_DRIVER_TYPE DriverType, HMODULE Software, UINT Flags, const D3D_FEATURE_LEVEL *pFeatureLevels, UINT FeatureLevels, UINT SDKVersion, const DXGI_SWAP_CHAIN_DESC *pSwapChainDesc, IDXGISwapChain **ppSwapChain, ID3D11Device **ppDevice, D3D_FEATURE_LEVEL *pFeatureLevel, ID3D11DeviceContext **ppImmediateContext)
{
  HRESULT hr = Real_D3D11CreateDeviceAndSwapChain(pAdapter, DriverType, Software, Flags, pFeatureLevels, FeatureLevels, SDKVersion, pSwapChainDesc, ppSwapChain, ppDevice, pFeatureLevel, ppImmediateContext);
  if(SUCCEEDED(hr) && ppSwapChain)
  {
    printLog("video/d3d11: device and swap chain created.\n");
    HookCOMOnce(&Real_SwapChain_Present, *ppSwapChain, 8, Mine_SwapChain_Present);
  }
  return hr;
}


void initVideo_Direct3D10()
{
  HMODULE dxgi = LoadLibraryA("dxgi.dll");
  if(dxgi)
  {
    HookDLLFunction(&Real_CreateDXGIFactory,  dxgi, "CreateDXGIFactory",  Mine_CreateDXGIFactory);
    HookDLLFunction(&Real_CreateDXGIFactory1, dxgi, "CreateDXGIFactory1", Mine_CreateDXGIFactory1);
    HookDLLFunction(&Real_CreateDXGIFactory2, dxgi, "CreateDXGIFactory2", Mine_CreateDXGIFactory2);
  }
  HMODULE d3d11 = LoadLibraryA("d3d11.dll");
  if(d3d11)
    HookDLLFunction(&Real_D3D11CreateDeviceAndSwapChain, d3d11, "D3D11CreateDeviceAndSwapChain", Mine_D3D11CreateDeviceAndSwapChain);
}
