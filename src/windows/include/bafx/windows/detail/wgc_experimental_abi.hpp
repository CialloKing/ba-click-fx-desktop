#pragma once

#include <windows.h>

#include <inspectable.h>

namespace bafx::windows::detail
{

// These interfaces are experimental WinRT ABI contracts. They are declared
// locally so an older build SDK cannot remove a runtime capability simply
// because its projection omits the types. The method parameters are opaque
// pointers by design: the ABI only requires the underlying iterable/vector
// interface pointers and the WinRT projection owns their concrete types.
struct WindowIdAbi final
{
    UINT64 Value{0U};
};

MIDL_INTERFACE("BB91F61B-218A-587D-8580-2701A74C0525")
DisplayGraphicsCaptureSessionAbi : public IInspectable
{
public:
    virtual HRESULT STDMETHODCALLTYPE SetWindowExclusionList(
        void* excludedWindows,
        UINT64* result) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetWindowExclusionList(
        void** result) = 0;
};

MIDL_INTERFACE("82D1AA4D-4366-543E-A6D0-A4805E6BCF2C")
GraphicsCaptureSession7Abi : public IInspectable
{
public:
    virtual HRESULT STDMETHODCALLTYPE get_ConfigurationIteration(
        UINT64* value) = 0;
};

MIDL_INTERFACE("71616DC8-FEA5-5741-A3D8-591ACC39A9EE")
Direct3D11CaptureFrame3Abi : public IInspectable
{
public:
    virtual HRESULT STDMETHODCALLTYPE get_ConfigurationIteration(
        UINT64* value) = 0;
};

} // namespace bafx::windows::detail
