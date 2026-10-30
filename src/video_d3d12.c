/*
 * Direct3D 12 video backend (Windows; see src/video.h).
 *
 * Each frame the CPU-drawn picture goes into an upload buffer, is copied into
 * a texture, and a full-screen triangle scales it into a flip-model swap chain
 * (nearest-neighbour, letterboxed like SDL_RenderSetLogicalSize). The swap
 * chain uses a frame latency waitable object so input-to-screen latency stays
 * at one frame, and with --no-vsync it presents with tearing allowed, which
 * lets variable refresh rate displays follow the game's frame rate.
 *
 * d3d12.dll, dxgi.dll and d3dcompiler_47.dll are loaded at run time, so the
 * game still starts (on the SDL backend) where Direct3D 12 is missing.
 */
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <SDL_syswm.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "video.h"

#define FRAMES 2  /* frames in flight (command allocator + upload buffer each) */
#define BUFFERS 3 /* swap chain back buffers */

#define RELEASE(p) do { if (p) { IUnknown_Release((IUnknown *)(p)); (p) = NULL; } } while (0)

typedef HRESULT(WINAPI *PFN_CreateDXGIFactory2)(UINT, REFIID, void **);
typedef HRESULT(WINAPI *PFN_D3DCompile)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO *, ID3DInclude *, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob **, ID3DBlob **);

static HMODULE dll_d3d12, dll_dxgi, dll_compiler;
static PFN_D3D12_CREATE_DEVICE p_D3D12CreateDevice;
static PFN_D3D12_GET_DEBUG_INTERFACE p_D3D12GetDebugInterface;
static PFN_D3D12_SERIALIZE_ROOT_SIGNATURE p_D3D12SerializeRootSignature;
static PFN_CreateDXGIFactory2 p_CreateDXGIFactory2;
static PFN_D3DCompile p_D3DCompile;

static SDL_Window *sdl_window;
static HWND hwnd;
static int use_vsync, allow_tearing;

static IDXGIFactory4 *factory;
static ID3D12Device *device;
static ID3D12CommandQueue *queue;
static IDXGISwapChain3 *swapchain;
static HANDLE latency_wait;
static UINT swap_flags;
static int sc_w, sc_h;
static ID3D12Resource *backbuf[BUFFERS];
static ID3D12DescriptorHeap *rtv_heap, *srv_heap;
static UINT rtv_step;
static ID3D12RootSignature *root_sig;
static ID3D12PipelineState *pso;
static ID3D12CommandAllocator *alloc[FRAMES];
static ID3D12GraphicsCommandList *cmd;
static ID3D12Fence *fence;
static HANDLE fence_event;
static UINT64 fence_next = 1, fence_done[FRAMES];
static int frame; /* current frame-in-flight slot */

/* the frame buffer the CPU draws into */
static ID3D12Resource *src_tex, *upload[FRAMES];
static uint8_t *upload_ptr[FRAMES];
static D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
static int src_w, src_h, logical_w, logical_h;
static int src_written; /* upload[frame] holds a new picture */
static int src_in_srv;  /* src_tex is in PIXEL_SHADER_RESOURCE state */
static int waited;      /* waited on the latency object for this frame */

/* Some displays (virtual and remote-desktop ones) don't hold flip-model
 * presents to the refresh rate. When vsync is on and presents come back much
 * faster than the display refreshes, wait for the vertical blank instead. */
static int vblank_fallback;
static Uint64 pace_start;
static int pace_frames;

static const char shader_src[] =
    "Texture2D t : register(t0);\n"
    "SamplerState s : register(s0);\n"
    "struct V { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
    "V vs(uint id : SV_VertexID) {\n"
    "    V o;\n"
    "    o.uv = float2((id << 1) & 2, id & 2);\n"
    "    o.pos = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);\n"
    "    return o;\n"
    "}\n"
    "float4 ps(V i) : SV_Target { return float4(t.Sample(s, i.uv).rgb, 1); }\n";

static int failed(HRESULT hr, const char *what) {
    if (SUCCEEDED(hr)) return 0;
    fprintf(stderr, "d3d12: %s failed (0x%08lx)\n", what, (unsigned long)hr);
    return 1;
}

static int load_dlls(void) {
    dll_d3d12 = LoadLibraryA("d3d12.dll");
    dll_dxgi = LoadLibraryA("dxgi.dll");
    dll_compiler = LoadLibraryA("d3dcompiler_47.dll");
    if (!dll_d3d12 || !dll_dxgi || !dll_compiler) return -1;
    p_D3D12CreateDevice = (PFN_D3D12_CREATE_DEVICE)(void *)GetProcAddress(dll_d3d12, "D3D12CreateDevice");
    p_D3D12GetDebugInterface = (PFN_D3D12_GET_DEBUG_INTERFACE)(void *)GetProcAddress(dll_d3d12, "D3D12GetDebugInterface");
    p_D3D12SerializeRootSignature = (PFN_D3D12_SERIALIZE_ROOT_SIGNATURE)(void *)GetProcAddress(dll_d3d12, "D3D12SerializeRootSignature");
    p_CreateDXGIFactory2 = (PFN_CreateDXGIFactory2)(void *)GetProcAddress(dll_dxgi, "CreateDXGIFactory2");
    p_D3DCompile = (PFN_D3DCompile)(void *)GetProcAddress(dll_compiler, "D3DCompile");
    return p_D3D12CreateDevice && p_D3D12SerializeRootSignature && p_CreateDXGIFactory2 && p_D3DCompile ? 0 : -1;
}

/* First hardware adapter that does feature level 11_0, preferring the fast GPU. */
static int create_device(void) {
    IDXGIFactory6 *f6 = NULL;
    IDXGIAdapter1 *ad = NULL;
    IDXGIFactory4_QueryInterface(factory, &IID_IDXGIFactory6, (void **)&f6);
    for (UINT i = 0;; i++) {
        HRESULT hr = f6 ? IDXGIFactory6_EnumAdapterByGpuPreference(f6, i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, &IID_IDXGIAdapter1, (void **)&ad)
                        : IDXGIFactory4_EnumAdapters1(factory, i, &ad);
        if (FAILED(hr)) break;
        DXGI_ADAPTER_DESC1 desc;
        IDXGIAdapter1_GetDesc1(ad, &desc);
        if (!(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) &&
            SUCCEEDED(p_D3D12CreateDevice((IUnknown *)ad, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, (void **)&device))) {
            if (getenv("N3D_DEBUG")) fprintf(stderr, "d3d12: adapter %ls\n", desc.Description);
            RELEASE(ad);
            break;
        }
        RELEASE(ad);
    }
    RELEASE(f6);
    if (!device) { /* no usable GPU: the WARP software rasterizer */
        if (SUCCEEDED(IDXGIFactory4_EnumWarpAdapter(factory, &IID_IDXGIAdapter, (void **)&ad))) {
            p_D3D12CreateDevice((IUnknown *)ad, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, (void **)&device);
            RELEASE(ad);
            if (device && getenv("N3D_DEBUG")) fprintf(stderr, "d3d12: adapter WARP\n");
        }
    }
    return device ? 0 : -1;
}

static void wait_gpu(UINT64 value) {
    if (ID3D12Fence_GetCompletedValue(fence) >= value) return;
    ID3D12Fence_SetEventOnCompletion(fence, value, fence_event);
    WaitForSingleObject(fence_event, INFINITE);
}

static void flush_gpu(void) {
    if (!queue || !fence) return;
    UINT64 v = fence_next++;
    ID3D12CommandQueue_Signal(queue, fence, v);
    wait_gpu(v);
}

/* Through the vtable: the Windows SDK and MinGW headers disagree on the C
 * macros for these (struct-returning) methods, but not on the vtable. */
static void cpu_start(ID3D12DescriptorHeap *heap, D3D12_CPU_DESCRIPTOR_HANDLE *h) { heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(heap, h); }
static void gpu_start(ID3D12DescriptorHeap *heap, D3D12_GPU_DESCRIPTOR_HANDLE *h) { heap->lpVtbl->GetGPUDescriptorHandleForHeapStart(heap, h); }

static void release_backbuffers(void) {
    for (int i = 0; i < BUFFERS; i++) RELEASE(backbuf[i]);
}

static int get_backbuffers(void) {
    D3D12_CPU_DESCRIPTOR_HANDLE h;
    cpu_start(rtv_heap, &h);
    for (int i = 0; i < BUFFERS; i++) {
        if (failed(IDXGISwapChain3_GetBuffer(swapchain, i, &IID_ID3D12Resource, (void **)&backbuf[i]), "GetBuffer")) return -1;
        ID3D12Device_CreateRenderTargetView(device, backbuf[i], NULL, h);
        h.ptr += rtv_step;
    }
    return 0;
}

static void client_size(int *w, int *h) {
    RECT r;
    GetClientRect(hwnd, &r);
    *w = r.right - r.left;
    *h = r.bottom - r.top;
}

/* Follow the window size; 0 while minimised (nothing to draw into). */
static int fit_swapchain(void) {
    int w, h;
    client_size(&w, &h);
    if (w <= 0 || h <= 0) return 0;
    if (w == sc_w && h == sc_h) return 1;
    flush_gpu();
    release_backbuffers();
    if (failed(IDXGISwapChain3_ResizeBuffers(swapchain, BUFFERS, w, h, DXGI_FORMAT_UNKNOWN, swap_flags), "ResizeBuffers")) return 0;
    sc_w = w;
    sc_h = h;
    return get_backbuffers() == 0;
}

static int create_pipeline(void) {
    D3D12_DESCRIPTOR_RANGE range = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND};
    D3D12_ROOT_PARAMETER param = {0};
    param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    param.DescriptorTable.NumDescriptorRanges = 1;
    param.DescriptorTable.pDescriptorRanges = &range;
    param.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_STATIC_SAMPLER_DESC samp = {0};
    samp.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT; /* sharp pixels, like SDL's "nearest" scale quality */
    samp.AddressU = samp.AddressV = samp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samp.MaxLOD = D3D12_FLOAT32_MAX;
    samp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC rs = {1, &param, 1, &samp, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ID3DBlob *blob = NULL, *err = NULL, *vs = NULL, *ps = NULL;
    HRESULT hr = p_D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err);
    if (failed(hr, "D3D12SerializeRootSignature")) goto out;
    hr = ID3D12Device_CreateRootSignature(device, 0, ID3D10Blob_GetBufferPointer(blob), ID3D10Blob_GetBufferSize(blob), &IID_ID3D12RootSignature, (void **)&root_sig);
    if (failed(hr, "CreateRootSignature")) goto out;

    hr = p_D3DCompile(shader_src, sizeof shader_src - 1, "present.hlsl", NULL, NULL, "vs", "vs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &vs, &err);
    if (SUCCEEDED(hr)) hr = p_D3DCompile(shader_src, sizeof shader_src - 1, "present.hlsl", NULL, NULL, "ps", "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &ps, &err);
    if (FAILED(hr)) {
        if (err) fprintf(stderr, "d3d12: %s\n", (const char *)ID3D10Blob_GetBufferPointer(err));
        failed(hr, "D3DCompile");
        goto out;
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {0};
    pd.pRootSignature = root_sig;
    pd.VS.pShaderBytecode = ID3D10Blob_GetBufferPointer(vs);
    pd.VS.BytecodeLength = ID3D10Blob_GetBufferSize(vs);
    pd.PS.pShaderBytecode = ID3D10Blob_GetBufferPointer(ps);
    pd.PS.BytecodeLength = ID3D10Blob_GetBufferSize(ps);
    pd.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_ONE;
    pd.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_ZERO;
    pd.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
    pd.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
    pd.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ZERO;
    pd.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
    pd.BlendState.RenderTarget[0].LogicOp = D3D12_LOGIC_OP_NOOP;
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.SampleMask = UINT_MAX;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.RasterizerState.DepthClipEnable = TRUE;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1;
    pd.RTVFormats[0] = DXGI_FORMAT_B8G8R8A8_UNORM;
    pd.SampleDesc.Count = 1;
    hr = ID3D12Device_CreateGraphicsPipelineState(device, &pd, &IID_ID3D12PipelineState, (void **)&pso);
    failed(hr, "CreateGraphicsPipelineState");
out:
    RELEASE(blob);
    RELEASE(err);
    RELEASE(vs);
    RELEASE(ps);
    return SUCCEEDED(hr) ? 0 : -1;
}

static void d3d12_shutdown(void);

static int d3d12_init(SDL_Window *window, int vsync) {
    SDL_SysWMinfo wm;
    SDL_VERSION(&wm.version);
    if (!SDL_GetWindowWMInfo(window, &wm) || wm.subsystem != SDL_SYSWM_WINDOWS) return -1;
    sdl_window = window;
    hwnd = wm.info.win.window;
    use_vsync = vsync;
    if (load_dlls() != 0) {
        fprintf(stderr, "d3d12: Direct3D 12 is not available\n");
        return -1;
    }
    UINT fflags = 0;
    if (getenv("N3D_D3D12_DEBUG") && p_D3D12GetDebugInterface) {
        ID3D12Debug *dbg;
        if (SUCCEEDED(p_D3D12GetDebugInterface(&IID_ID3D12Debug, (void **)&dbg))) {
            ID3D12Debug_EnableDebugLayer(dbg);
            RELEASE(dbg);
            fflags = DXGI_CREATE_FACTORY_DEBUG;
        }
    }
    if (failed(p_CreateDXGIFactory2(fflags, &IID_IDXGIFactory4, (void **)&factory), "CreateDXGIFactory2")) goto fail;
    if (create_device() != 0) {
        fprintf(stderr, "d3d12: no Direct3D 12 device\n");
        goto fail;
    }
    {
        IDXGIFactory5 *f5;
        BOOL tear = FALSE;
        if (SUCCEEDED(IDXGIFactory4_QueryInterface(factory, &IID_IDXGIFactory5, (void **)&f5))) {
            if (FAILED(IDXGIFactory5_CheckFeatureSupport(f5, DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tear, sizeof tear))) tear = FALSE;
            RELEASE(f5);
        }
        allow_tearing = tear;
    }

    D3D12_COMMAND_QUEUE_DESC qd = {D3D12_COMMAND_LIST_TYPE_DIRECT, 0, D3D12_COMMAND_QUEUE_FLAG_NONE, 0};
    if (failed(ID3D12Device_CreateCommandQueue(device, &qd, &IID_ID3D12CommandQueue, (void **)&queue), "CreateCommandQueue")) goto fail;

    client_size(&sc_w, &sc_h);
    if (sc_w < 1) sc_w = 1;
    if (sc_h < 1) sc_h = 1;
    swap_flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT | (allow_tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0);
    DXGI_SWAP_CHAIN_DESC1 sd = {0};
    sd.Width = sc_w;
    sd.Height = sc_h;
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = BUFFERS;
    sd.Scaling = DXGI_SCALING_STRETCH;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    sd.Flags = swap_flags;
    IDXGISwapChain1 *sc1;
    if (failed(IDXGIFactory4_CreateSwapChainForHwnd(factory, (IUnknown *)queue, hwnd, &sd, NULL, NULL, &sc1), "CreateSwapChainForHwnd")) goto fail;
    HRESULT hr = IDXGISwapChain1_QueryInterface(sc1, &IID_IDXGISwapChain3, (void **)&swapchain);
    RELEASE(sc1);
    if (failed(hr, "IDXGISwapChain3")) goto fail;
    IDXGIFactory4_MakeWindowAssociation(factory, hwnd, DXGI_MWA_NO_ALT_ENTER); /* Alt+Enter is SDL's borderless toggle */
    IDXGISwapChain3_SetMaximumFrameLatency(swapchain, 1);
    latency_wait = IDXGISwapChain3_GetFrameLatencyWaitableObject(swapchain);

    D3D12_DESCRIPTOR_HEAP_DESC rh = {D3D12_DESCRIPTOR_HEAP_TYPE_RTV, BUFFERS, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
    if (failed(ID3D12Device_CreateDescriptorHeap(device, &rh, &IID_ID3D12DescriptorHeap, (void **)&rtv_heap), "CreateDescriptorHeap")) goto fail;
    rtv_step = ID3D12Device_GetDescriptorHandleIncrementSize(device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    D3D12_DESCRIPTOR_HEAP_DESC sh = {D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
    if (failed(ID3D12Device_CreateDescriptorHeap(device, &sh, &IID_ID3D12DescriptorHeap, (void **)&srv_heap), "CreateDescriptorHeap")) goto fail;
    if (get_backbuffers() != 0) goto fail;

    for (int i = 0; i < FRAMES; i++)
        if (failed(ID3D12Device_CreateCommandAllocator(device, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator, (void **)&alloc[i]), "CreateCommandAllocator")) goto fail;
    if (create_pipeline() != 0) goto fail;
    if (failed(ID3D12Device_CreateCommandList(device, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc[0], pso, &IID_ID3D12GraphicsCommandList, (void **)&cmd), "CreateCommandList")) goto fail;
    ID3D12GraphicsCommandList_Close(cmd);
    if (failed(ID3D12Device_CreateFence(device, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&fence), "CreateFence")) goto fail;
    fence_event = CreateEventA(NULL, FALSE, FALSE, NULL);
    if (getenv("N3D_DEBUG")) fprintf(stderr, "renderer: d3d12 vsync=%d tearing=%d\n", use_vsync, allow_tearing);
    return 0;
fail:
    d3d12_shutdown();
    return -1;
}

static void release_source(void) {
    for (int i = 0; i < FRAMES; i++) {
        if (upload[i]) ID3D12Resource_Unmap(upload[i], 0, NULL);
        upload_ptr[i] = NULL;
        RELEASE(upload[i]);
    }
    RELEASE(src_tex);
    src_w = src_h = 0;
}

static void d3d12_shutdown(void) {
    flush_gpu();
    release_source();
    release_backbuffers();
    RELEASE(cmd);
    for (int i = 0; i < FRAMES; i++) RELEASE(alloc[i]);
    RELEASE(pso);
    RELEASE(root_sig);
    RELEASE(srv_heap);
    RELEASE(rtv_heap);
    if (latency_wait) CloseHandle(latency_wait);
    latency_wait = NULL;
    RELEASE(swapchain);
    RELEASE(queue);
    RELEASE(fence);
    if (fence_event) CloseHandle(fence_event);
    fence_event = NULL;
    RELEASE(device);
    RELEASE(factory);
}

static int d3d12_output_size(int *w, int *h) {
    if (!fit_swapchain()) return -1;
    *w = sc_w;
    *h = sc_h;
    return 0;
}

static int d3d12_set_source(int w, int h, int lw, int lh) {
    logical_w = lw;
    logical_h = lh;
    if (w == src_w && h == src_h && src_tex) return 0;
    flush_gpu();
    release_source();

    D3D12_HEAP_PROPERTIES heap = {0};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC td = {0};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = w;
    td.Height = h;
    td.DepthOrArraySize = 1;
    td.MipLevels = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; /* XRGB8888 in memory order */
    td.SampleDesc.Count = 1;
    if (failed(ID3D12Device_CreateCommittedResource(device, &heap, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, NULL, &IID_ID3D12Resource, (void **)&src_tex), "CreateCommittedResource")) return -1;
    src_in_srv = 0;
    D3D12_CPU_DESCRIPTOR_HANDLE srv;
    cpu_start(srv_heap, &srv);
    ID3D12Device_CreateShaderResourceView(device, src_tex, NULL, srv);

    UINT64 total = 0;
    ID3D12Device_GetCopyableFootprints(device, &td, 0, 1, 0, &footprint, NULL, NULL, &total);
    D3D12_HEAP_PROPERTIES up = {0};
    up.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC bd = {0};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = total;
    bd.Height = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    for (int i = 0; i < FRAMES; i++) {
        if (failed(ID3D12Device_CreateCommittedResource(device, &up, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, NULL, &IID_ID3D12Resource, (void **)&upload[i]), "CreateCommittedResource")) return -1;
        D3D12_RANGE none = {0, 0}; /* the CPU never reads it */
        if (failed(ID3D12Resource_Map(upload[i], 0, &none, (void **)&upload_ptr[i]), "Map")) return -1;
        memset(upload_ptr[i], 0, (size_t)total);
    }
    src_w = w;
    src_h = h;
    src_written = 0;
    return 0;
}

static void wait_latency(void) {
    if (waited) return;
    waited = 1;
    if (latency_wait) WaitForSingleObjectEx(latency_wait, 1000, TRUE);
}

static uint32_t *d3d12_lock(int *pitch) {
    if (!src_tex) return NULL;
    wait_latency();
    wait_gpu(fence_done[frame]); /* the GPU is done reading this slot's upload buffer */
    *pitch = (int)(footprint.Footprint.RowPitch / 4);
    return (uint32_t *)upload_ptr[frame];
}

static void d3d12_unlock(void) { src_written = 1; }

static void barrier(ID3D12Resource *res, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER b = {0};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = res;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    ID3D12GraphicsCommandList_ResourceBarrier(cmd, 1, &b);
}

/* Where the logical area lands in the back buffer (SDL_RenderSetLogicalSize's rule). */
static void letterbox(float *x, float *y, float *w, float *h, float *scale) {
    float want = (float)logical_w / logical_h, real = (float)sc_w / sc_h;
    if (want > real) {
        *scale = (float)sc_w / logical_w;
        *w = (float)sc_w;
        *h = (float)(int)(logical_h * *scale);
    } else {
        *scale = (float)sc_h / logical_h;
        *w = (float)(int)(logical_w * *scale);
        *h = (float)sc_h;
    }
    *x = (float)(int)((sc_w - *w) / 2);
    *y = (float)(int)((sc_h - *h) / 2);
}

static void check_pacing(void) {
    if (vblank_fallback) {
        IDXGIOutput *out = NULL;
        if (SUCCEEDED(IDXGISwapChain3_GetContainingOutput(swapchain, &out))) {
            IDXGIOutput_WaitForVBlank(out);
            RELEASE(out);
        }
        return;
    }
    /* measure 120 presents, skipping the first ones while the window settles */
    if (++pace_frames == 30) pace_start = SDL_GetPerformanceCounter();
    if (pace_frames < 150) return;
    double secs = (SDL_GetPerformanceCounter() - pace_start) / (double)SDL_GetPerformanceFrequency();
    SDL_DisplayMode dm;
    int hz = SDL_GetWindowDisplayMode(sdl_window, &dm) == 0 && dm.refresh_rate > 0 ? dm.refresh_rate : 60;
    if (120 / secs > hz * 1.5) {
        vblank_fallback = 1;
        if (getenv("N3D_DEBUG")) fprintf(stderr, "d3d12: presents ran at %.0f fps on a %d Hz display; pacing with WaitForVBlank\n", 120 / secs, hz);
    }
    pace_frames = 0; /* check again later (the window may move to another display) */
}

static void d3d12_present(void) {
    wait_latency();
    waited = 0;
    if (!src_tex || !fit_swapchain()) { src_written = 0; return; }
    wait_gpu(fence_done[frame]);
    ID3D12CommandAllocator_Reset(alloc[frame]);
    ID3D12GraphicsCommandList_Reset(cmd, alloc[frame], pso);

    if (src_written) {
        if (src_in_srv) barrier(src_tex, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_TEXTURE_COPY_LOCATION dst = {0}, src = {0};
        dst.pResource = src_tex;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.pResource = upload[frame];
        src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint = footprint;
        ID3D12GraphicsCommandList_CopyTextureRegion(cmd, &dst, 0, 0, 0, &src, NULL);
        barrier(src_tex, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        src_in_srv = 1;
        src_written = 0;
    } else if (!src_in_srv) {
        barrier(src_tex, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        src_in_srv = 1;
    }

    UINT bb = IDXGISwapChain3_GetCurrentBackBufferIndex(swapchain);
    barrier(backbuf[bb], D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
    D3D12_CPU_DESCRIPTOR_HANDLE rtv;
    cpu_start(rtv_heap, &rtv);
    rtv.ptr += (SIZE_T)bb * rtv_step;
    static const float black[4] = {0, 0, 0, 1};
    ID3D12GraphicsCommandList_ClearRenderTargetView(cmd, rtv, black, 0, NULL);
    ID3D12GraphicsCommandList_OMSetRenderTargets(cmd, 1, &rtv, FALSE, NULL);

    float x, y, w, h, scale;
    letterbox(&x, &y, &w, &h, &scale);
    D3D12_VIEWPORT vp = {x, y, w, h, 0, 1};
    D3D12_RECT sr = {0, 0, sc_w, sc_h};
    ID3D12GraphicsCommandList_RSSetViewports(cmd, 1, &vp);
    ID3D12GraphicsCommandList_RSSetScissorRects(cmd, 1, &sr);
    ID3D12GraphicsCommandList_SetGraphicsRootSignature(cmd, root_sig);
    ID3D12GraphicsCommandList_SetDescriptorHeaps(cmd, 1, &srv_heap);
    D3D12_GPU_DESCRIPTOR_HANDLE srv;
    gpu_start(srv_heap, &srv);
    ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(cmd, 0, srv);
    ID3D12GraphicsCommandList_IASetPrimitiveTopology(cmd, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D12GraphicsCommandList_DrawInstanced(cmd, 3, 1, 0, 0);
    barrier(backbuf[bb], D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
    ID3D12GraphicsCommandList_Close(cmd);

    ID3D12CommandList *lists[1] = {(ID3D12CommandList *)cmd};
    ID3D12CommandQueue_ExecuteCommandLists(queue, 1, lists);
    HRESULT hr = use_vsync ? IDXGISwapChain3_Present(swapchain, 1, 0)
                           : IDXGISwapChain3_Present(swapchain, 0, allow_tearing ? DXGI_PRESENT_ALLOW_TEARING : 0);
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET)
        fprintf(stderr, "d3d12: device lost (0x%08lx)\n", (unsigned long)ID3D12Device_GetDeviceRemovedReason(device));
    if (use_vsync) check_pacing();
    fence_done[frame] = fence_next++;
    ID3D12CommandQueue_Signal(queue, fence, fence_done[frame]);
    frame = (frame + 1) % FRAMES;
}

static void d3d12_window_to_logical(int wx, int wy, float *lx, float *ly) {
    int ww, wh;
    SDL_GetWindowSize(sdl_window, &ww, &wh);
    float x, y, w, h, scale;
    letterbox(&x, &y, &w, &h, &scale);
    float px = ww > 0 ? (float)wx * sc_w / ww : (float)wx; /* window units to pixels (HiDPI) */
    float py = wh > 0 ? (float)wy * sc_h / wh : (float)wy;
    *lx = (px - x) / scale;
    *ly = (py - y) / scale;
}

const video_backend video_d3d12 = {
    "d3d12", d3d12_init, d3d12_shutdown, d3d12_output_size, d3d12_set_source, d3d12_lock, d3d12_unlock, d3d12_present, d3d12_window_to_logical,
};
