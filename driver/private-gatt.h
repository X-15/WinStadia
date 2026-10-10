// Writes HID output reports of a Bluetooth LE device through the GATT client
// of bthserv, which Windows keeps to its own drivers.

#pragma once

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct PRIVATE_GATT_SESSION PRIVATE_GATT_SESSION;

// Finds the HID service's output report with that ID on the device.
HRESULT PrivateGattOpen(UINT64 Address, UCHAR ReportId, PRIVATE_GATT_SESSION **Session);
// Takes the report without its ID and waits for the device's answer.
HRESULT PrivateGattWrite(PRIVATE_GATT_SESSION *Session, const UCHAR *Value, ULONG Length);
VOID PrivateGattClose(PRIVATE_GATT_SESSION *Session);

#ifdef __cplusplus
}
#endif
