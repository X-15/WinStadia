// The GATT client is the WinRT class
// Microsoft.Bluetooth.Profiles.Gatt.Interface.GattClientDevice, served by
// bthserv and used by the inbox HID over GATT driver. It has no metadata. The
// interface IDs, method slots and structures below were recovered from the
// public symbols of Microsoft.Bluetooth.Service and that driver on Windows
// build 26200, and seen working on 26220. Nothing checks them at run time: a
// Windows update that moves a slot makes this call the wrong method.

#define WIN32_LEAN_AND_MEAN
#include "private-gatt.h"
#include <roapi.h>
#include <inspectable.h>
#include <asyncinfo.h>
#include <windows.storage.streams.h>
#include <robuffer.h>
#include <wrl/client.h>
#include <wrl/wrappers/corewrappers.h>
#include <new>

using ABI::Windows::Foundation::IAsyncInfo;
using ABI::Windows::Storage::Streams::IBuffer;
using ABI::Windows::Storage::Streams::IBufferFactory;
using Microsoft::WRL::ComPtr;
using Microsoft::WRL::Wrappers::HStringReference;
using Windows::Storage::Streams::IBufferByteAccess;

#define AWAIT_TIMEOUT_MS 2000
#define AWAIT_POLL_MS 2
// More than any service of a HID device holds.
#define RESULT_COUNT_MAX 128

#define UUID_HID_SERVICE 0x1812
#define UUID_REPORT 0x2a4d
#define UUID_REPORT_REFERENCE 0x2908
#define REPORT_TYPE_OUTPUT 2
#define CHARACTERISTIC_WRITE 0x08

// Vtable slots; the first six belong to IInspectable. The names describe
// what the methods do, their real ones are not known.
enum {
    // GattClientDevice
    DEVICE_FIND_SERVICES = 11,
    DEVICE_OPEN_SERVICE = 14,
    // The service
    SERVICE_GET_DISCOVERY = 11,
    SERVICE_GET_CHARACTERISTIC = 12,
    // Its discovery object
    DISCOVERY_FIND_CHARACTERISTICS = 8,
    DISCOVERY_FIND_DESCRIPTORS = 11,
    // GattClientCharacteristic
    CHARACTERISTIC_WRITE_VALUE = 8,
    CHARACTERISTIC_READ_DESCRIPTOR = 9,
    // Any asynchronous operation
    OPERATION_GET_RESULTS = 8,
    // The results of the operations
    RESULT_GET_STATUS = 6,
    RESULT_GET_VALUE = 8,
    RESULT_GET_LIST = 9,
    // The lists in the results
    LIST_GET_AT = 6,
    LIST_GET_SIZE = 7,
};

#define RESULT_STATUS_SUCCESS 0
#define RESULT_STATUS_ACCESS_DENIED 3

// Taken to mean a random address, which is what the Stadia controller has; a
// device with a public one was never tried.
#define ADDRESS_TYPE_RANDOM 1
#define WRITE_WITH_RESPONSE 1
// What the inbox driver passes when it opens the service.
#define OPEN_SERVICE_FLAGS 0x0000000100000001ULL

static const GUID DeviceStaticsIid = {0xcbaa1a5f, 0xdcef, 0x508b, {0xac, 0xc6, 0x15, 0x8f, 0x1e, 0x16, 0x0c, 0x0f}};
static const GUID DeviceIid = {0x6bbede1f, 0x797f, 0x5c17, {0x8a, 0xc7, 0x3f, 0x34, 0x20, 0x37, 0xcb, 0x59}};

struct RemoteAddress {
    UINT64 Address;
    UINT32 Type;
    UINT32 Padding;
};

struct DeviceStatics : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE CreateAsync(RemoteAddress, UINT32, HSTRING, HSTRING, UINT32, IInspectable **) = 0;
};

struct ServiceInfo {
    USHORT First;
    USHORT Last;
    GUID Uuid;
    UINT32 Flags;
    USHORT Handle;
    USHORT Padding;
};

struct CharacteristicInfo {
    USHORT First;
    USHORT Last;
    GUID Uuid;
    UINT32 Properties;
    USHORT Handle;
    USHORT ValueHandle;
    UINT32 Padding;
};

static_assert(sizeof(ServiceInfo) == 28, "service ABI");
static_assert(sizeof(CharacteristicInfo) == 32, "characteristic ABI");

// Its layout is not known; the list that holds one copies it out and the
// read takes it back.
struct DescriptorInfo {
    alignas(16) BYTE Opaque[128];
};

struct PRIVATE_GATT_SESSION {
    // The characteristic does not keep these alive.
    ComPtr<IInspectable> Device;
    ComPtr<IInspectable> Service;
    ComPtr<IInspectable> Characteristic;
};

// The calls come from whatever thread the driver has at hand.
struct Apartment {
    HRESULT Status = RoInitialize(RO_INIT_MULTITHREADED);

    ~Apartment()
    {
        if (SUCCEEDED(Status)) {
            RoUninitialize();
        }
    }

    HRESULT
    Check() const
    {
        return Status == RPC_E_CHANGED_MODE ? S_OK : Status;
    }
};

// Calls the method in that slot. The argument types are spelled out at each
// call, since they are all there is of a declaration.
template <typename... Args>
static HRESULT
Call(IInspectable *Object, size_t Slot, Args... Arguments)
{
    typedef HRESULT (STDMETHODCALLTYPE *Method)(IInspectable *, Args...);

    return reinterpret_cast<Method>((*reinterpret_cast<void ***>(Object))[Slot])(Object, Arguments...);
}

static GUID
BluetoothUuid(USHORT Value)
{
    return {Value, 0, 0x1000, {0x80, 0, 0, 0x80, 0x5f, 0x9b, 0x34, 0xfb}};
}

// Waits for the operation by polling, which needs no completion handler and
// with it no knowledge of the handler's interface.
static HRESULT
Await(IInspectable *Operation, IInspectable **Result)
{
    ComPtr<IAsyncInfo> info;
    AsyncStatus status = Started;
    ULONGLONG start = GetTickCount64();
    HRESULT hr;

    *Result = nullptr;
    if (Operation == nullptr) {
        return E_UNEXPECTED;
    }
    hr = Operation->QueryInterface(IID_PPV_ARGS(&info));
    if (FAILED(hr)) {
        return hr;
    }

    while (SUCCEEDED(hr = info->get_Status(&status)) && status == Started &&
           GetTickCount64() - start < AWAIT_TIMEOUT_MS) {
        Sleep(AWAIT_POLL_MS);
    }
    if (SUCCEEDED(hr)) {
        if (status == Completed) {
            hr = Call<IInspectable **>(Operation, OPERATION_GET_RESULTS, Result);
            if (SUCCEEDED(hr) && *Result == nullptr) {
                hr = E_UNEXPECTED;
            }
        } else if (status == Started) {
            info->Cancel();
            hr = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
        } else {
            info->get_ErrorCode(&hr);
            if (SUCCEEDED(hr)) {
                hr = E_FAIL;
            }
        }
    }
    info->Close();
    return hr;
}

// An operation that completed says nothing about the exchange with the
// device; its result does.
static HRESULT
CheckResult(IInspectable *Result)
{
    UINT32 status = 0;
    HRESULT hr = Call<UINT32 *>(Result, RESULT_GET_STATUS, &status);

    if (FAILED(hr)) {
        return hr;
    }
    switch (status) {
    case RESULT_STATUS_SUCCESS:
        return S_OK;
    case RESULT_STATUS_ACCESS_DENIED:
        return E_ACCESSDENIED;
    default:
        return HRESULT_FROM_WIN32(ERROR_GEN_FAILURE);
    }
}

// Waits for an operation that finds attributes and hands out what it found.
static HRESULT
AwaitList(IInspectable *Operation, IInspectable **List, UINT32 *Count)
{
    ComPtr<IInspectable> result;
    HRESULT hr = Await(Operation, &result);

    *List = nullptr;
    *Count = 0;
    if (SUCCEEDED(hr)) {
        hr = CheckResult(result.Get());
    }
    if (SUCCEEDED(hr)) {
        hr = Call<IInspectable **>(result.Get(), RESULT_GET_LIST, List);
    }
    if (SUCCEEDED(hr) && *List == nullptr) {
        hr = E_UNEXPECTED;
    }
    if (SUCCEEDED(hr)) {
        hr = Call<UINT32 *>(*List, LIST_GET_SIZE, Count);
    }
    if (SUCCEEDED(hr) && *Count > RESULT_COUNT_MAX) {
        hr = HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    }
    return hr;
}

static HRESULT
OpenDevice(UINT64 Address, IInspectable **Device)
{
    ComPtr<DeviceStatics> statics;
    ComPtr<IInspectable> operation;
    ComPtr<IInspectable> created;
    RemoteAddress remote = {Address, ADDRESS_TYPE_RANDOM, 0};
    HRESULT hr;

    hr = RoGetActivationFactory(
        HStringReference(L"Microsoft.Bluetooth.Profiles.Gatt.Interface.GattClientDevice").Get(), DeviceStaticsIid,
        reinterpret_cast<void **>(statics.GetAddressOf()));
    if (FAILED(hr)) {
        return hr;
    }
    hr = statics->CreateAsync(remote, 0, nullptr, nullptr, 0, &operation);
    if (FAILED(hr)) {
        return hr;
    }
    hr = Await(operation.Get(), &created);
    if (FAILED(hr)) {
        return hr;
    }
    return created->QueryInterface(DeviceIid, reinterpret_cast<void **>(Device));
}

static HRESULT
OpenHidService(IInspectable *Device, IInspectable **Service)
{
    ComPtr<IInspectable> operation;
    ComPtr<IInspectable> list;
    ServiceInfo info = {};
    GUID uuid = BluetoothUuid(UUID_HID_SERVICE);
    UINT32 count;
    HRESULT hr;

    hr = Call<const GUID *, UINT32, IInspectable **>(Device, DEVICE_FIND_SERVICES, &uuid, 0, &operation);
    if (FAILED(hr)) {
        return hr;
    }
    hr = AwaitList(operation.Get(), &list, &count);
    if (FAILED(hr)) {
        return hr;
    }
    if (count != 1) {
        return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    }
    hr = Call<UINT32, ServiceInfo *>(list.Get(), LIST_GET_AT, 0, &info);
    if (FAILED(hr)) {
        return hr;
    }
    if (!IsEqualGUID(info.Uuid, uuid)) {
        return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    }
    hr = Call<const ServiceInfo *, UINT64, UINT32, IInspectable **>(Device, DEVICE_OPEN_SERVICE, &info,
                                                                    OPEN_SERVICE_FLAGS, 0, Service);
    if (SUCCEEDED(hr) && *Service == nullptr) {
        hr = E_UNEXPECTED;
    }
    return hr;
}

// Reads the characteristic's Report Reference descriptor: the ID and type of
// the report it carries.
static HRESULT
ReadReportReference(IInspectable *Discovery, IInspectable *Characteristic, const CharacteristicInfo *Info, UCHAR *Id,
                    UCHAR *Type)
{
    ComPtr<IInspectable> operation;
    ComPtr<IInspectable> list;
    ComPtr<IInspectable> result;
    ComPtr<IBuffer> buffer;
    ComPtr<IBufferByteAccess> access;
    DescriptorInfo descriptor = {};
    GUID uuid = BluetoothUuid(UUID_REPORT_REFERENCE);
    BYTE *bytes = nullptr;
    UINT32 count;
    UINT32 length = 0;
    HRESULT hr;

    hr = Call<const CharacteristicInfo *, const GUID *, UINT32, IInspectable **>(
        Discovery, DISCOVERY_FIND_DESCRIPTORS, Info, &uuid, 0, &operation);
    if (FAILED(hr)) {
        return hr;
    }
    hr = AwaitList(operation.Get(), &list, &count);
    if (FAILED(hr)) {
        return hr;
    }
    if (count != 1) {
        return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    }
    hr = Call<UINT32, DescriptorInfo *>(list.Get(), LIST_GET_AT, 0, &descriptor);
    if (FAILED(hr)) {
        return hr;
    }

    operation.Reset();
    hr = Call<const DescriptorInfo *, UINT32, UINT32, IInspectable **>(
        Characteristic, CHARACTERISTIC_READ_DESCRIPTOR, &descriptor, 0, 0, &operation);
    if (FAILED(hr)) {
        return hr;
    }
    hr = Await(operation.Get(), &result);
    if (FAILED(hr)) {
        return hr;
    }
    hr = CheckResult(result.Get());
    if (FAILED(hr)) {
        return hr;
    }
    hr = Call<IBuffer **>(result.Get(), RESULT_GET_VALUE, &buffer);
    if (FAILED(hr)) {
        return hr;
    }
    if (buffer == nullptr) {
        return E_UNEXPECTED;
    }
    hr = buffer->get_Length(&length);
    if (FAILED(hr)) {
        return hr;
    }
    if (length != 2) {
        return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    }
    hr = buffer.As(&access);
    if (FAILED(hr)) {
        return hr;
    }
    hr = access->Buffer(&bytes);
    if (FAILED(hr)) {
        return hr;
    }
    if (bytes == nullptr) {
        return E_UNEXPECTED;
    }
    *Id = bytes[0];
    *Type = bytes[1];
    return S_OK;
}

// The service has a Report characteristic for every report. The one wanted
// is found by its Report Reference, so that no attribute handle is assumed.
static HRESULT
FindOutputReport(IInspectable *Service, UCHAR ReportId, IInspectable **Characteristic)
{
    ComPtr<IInspectable> discovery;
    ComPtr<IInspectable> operation;
    ComPtr<IInspectable> list;
    GUID uuid = BluetoothUuid(UUID_REPORT);
    UINT32 count;
    HRESULT hr;

    hr = Call<IInspectable **>(Service, SERVICE_GET_DISCOVERY, &discovery);
    if (FAILED(hr)) {
        return hr;
    }
    if (discovery == nullptr) {
        return E_UNEXPECTED;
    }
    hr = Call<UINT32, IInspectable **>(discovery.Get(), DISCOVERY_FIND_CHARACTERISTICS, 0, &operation);
    if (FAILED(hr)) {
        return hr;
    }
    hr = AwaitList(operation.Get(), &list, &count);
    if (FAILED(hr)) {
        return hr;
    }

    for (UINT32 i = 0; i < count; i++) {
        ComPtr<IInspectable> characteristic;
        CharacteristicInfo info = {};
        UCHAR id = 0;
        UCHAR type = 0;

        hr = Call<UINT32, CharacteristicInfo *>(list.Get(), LIST_GET_AT, i, &info);
        if (FAILED(hr)) {
            return hr;
        }
        if (!IsEqualGUID(info.Uuid, uuid) || !(info.Properties & CHARACTERISTIC_WRITE)) {
            continue;
        }
        hr = Call<const CharacteristicInfo *, IInspectable **>(Service, SERVICE_GET_CHARACTERISTIC, &info,
                                                               &characteristic);
        if (FAILED(hr)) {
            return hr;
        }
        if (characteristic == nullptr) {
            return E_UNEXPECTED;
        }
        hr = ReadReportReference(discovery.Get(), characteristic.Get(), &info, &id, &type);
        if (FAILED(hr)) {
            return hr;
        }
        if (id == ReportId && type == REPORT_TYPE_OUTPUT) {
            *Characteristic = characteristic.Detach();
            return S_OK;
        }
    }
    return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
}

extern "C" HRESULT
PrivateGattOpen(UINT64 Address, UCHAR ReportId, PRIVATE_GATT_SESSION **Session)
{
    Apartment apartment;
    PRIVATE_GATT_SESSION *session;
    HRESULT hr = apartment.Check();

    *Session = nullptr;
    if (Address == 0) {
        return E_INVALIDARG;
    }
    if (FAILED(hr)) {
        return hr;
    }
    session = new (std::nothrow) PRIVATE_GATT_SESSION;
    if (session == nullptr) {
        return E_OUTOFMEMORY;
    }

    hr = OpenDevice(Address, &session->Device);
    if (SUCCEEDED(hr)) {
        hr = OpenHidService(session->Device.Get(), &session->Service);
    }
    if (SUCCEEDED(hr)) {
        hr = FindOutputReport(session->Service.Get(), ReportId, &session->Characteristic);
    }
    if (FAILED(hr)) {
        delete session;
        return hr;
    }
    *Session = session;
    return S_OK;
}

extern "C" HRESULT
PrivateGattWrite(PRIVATE_GATT_SESSION *Session, const UCHAR *Value, ULONG Length)
{
    Apartment apartment;
    ComPtr<IBufferFactory> factory;
    ComPtr<IBuffer> buffer;
    ComPtr<IBufferByteAccess> access;
    ComPtr<IInspectable> operation;
    ComPtr<IInspectable> result;
    BYTE *bytes = nullptr;
    HRESULT hr = apartment.Check();

    if (FAILED(hr)) {
        return hr;
    }
    hr = RoGetActivationFactory(HStringReference(L"Windows.Storage.Streams.Buffer").Get(), IID_PPV_ARGS(&factory));
    if (FAILED(hr)) {
        return hr;
    }
    hr = factory->Create(Length, &buffer);
    if (FAILED(hr)) {
        return hr;
    }
    hr = buffer->put_Length(Length);
    if (FAILED(hr)) {
        return hr;
    }
    hr = buffer.As(&access);
    if (FAILED(hr)) {
        return hr;
    }
    hr = access->Buffer(&bytes);
    if (FAILED(hr)) {
        return hr;
    }
    if (bytes == nullptr) {
        return E_UNEXPECTED;
    }
    CopyMemory(bytes, Value, Length);

    // With response, because that is all the Stadia controller's output
    // report takes. The inbox driver writes without and fails.
    hr = Call<UINT32, UINT32, IBuffer *, IInspectable **>(Session->Characteristic.Get(), CHARACTERISTIC_WRITE_VALUE,
                                                          WRITE_WITH_RESPONSE, 0, buffer.Get(), &operation);
    if (FAILED(hr)) {
        return hr;
    }
    hr = Await(operation.Get(), &result);
    if (FAILED(hr)) {
        return hr;
    }
    return CheckResult(result.Get());
}

extern "C" VOID
PrivateGattClose(PRIVATE_GATT_SESSION *Session)
{
    Apartment apartment;

    delete Session;
}
