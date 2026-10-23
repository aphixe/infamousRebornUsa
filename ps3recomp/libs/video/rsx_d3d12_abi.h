/* rsx_d3d12_abi.h -- D3D12 C-binding calls that work with every Windows SDK.
 *
 * D3D12 methods that return a struct (descriptor-heap start handles, ID3D12Resource::GetDesc,
 * ID3D12Device::GetResourceAllocationInfo) are C++ methods: they always return through a hidden
 * pointer argument placed right after `This`.  Windows SDKs before 10.0.20348 declare the C binding
 * as if the struct came back by value (one argument fewer), newer SDKs declare the real signature.
 * Code written for one form fails to compile with the other ("too many arguments to function call,
 * expected 1, have 2" in a player's install log, 2026-10-15).  Calling through a pointer cast to
 * the real signature compiles with both and is the ABI the method actually has. Usage:
 *     RSX_D3D12_CPU_HEAP_START(heap)(heap, &handle);
 * i.e. exactly the newer SDK's call with `heap->lpVtbl->GetCPUDescriptorHandleForHeapStart` replaced. */
#pragma once
#include <d3d12.h>

#define RSX_D3D12_CPU_HEAP_START(o) \
    ((D3D12_CPU_DESCRIPTOR_HANDLE* (STDMETHODCALLTYPE*)(void*, D3D12_CPU_DESCRIPTOR_HANDLE*)) \
        (o)->lpVtbl->GetCPUDescriptorHandleForHeapStart)
#define RSX_D3D12_GPU_HEAP_START(o) \
    ((D3D12_GPU_DESCRIPTOR_HANDLE* (STDMETHODCALLTYPE*)(void*, D3D12_GPU_DESCRIPTOR_HANDLE*)) \
        (o)->lpVtbl->GetGPUDescriptorHandleForHeapStart)
#define RSX_D3D12_RESOURCE_DESC(o) \
    ((D3D12_RESOURCE_DESC* (STDMETHODCALLTYPE*)(void*, D3D12_RESOURCE_DESC*)) \
        (o)->lpVtbl->GetDesc)
#define RSX_D3D12_ALLOC_INFO(o) \
    ((D3D12_RESOURCE_ALLOCATION_INFO* (STDMETHODCALLTYPE*)(void*, D3D12_RESOURCE_ALLOCATION_INFO*, UINT, UINT, \
                                                           const D3D12_RESOURCE_DESC*)) \
        (o)->lpVtbl->GetResourceAllocationInfo)
