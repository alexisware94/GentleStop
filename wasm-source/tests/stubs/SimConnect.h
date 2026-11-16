#pragma once
#include <MSFS/MSFS_WindowsTypes.h>
#define SIMCONNECT_OBJECT_ID_USER 0
#define SIMCONNECT_GROUP_PRIORITY_HIGHEST 1
#define SIMCONNECT_EVENT_FLAG_GROUPID_IS_PRIORITY 0x10
#define SIMCONNECT_UNUSED 0xFFFFFFFF
#define SIMCONNECT_STATE_ON 1
enum SIMCONNECT_DATATYPE { SIMCONNECT_DATATYPE_FLOAT64 = 4 };
enum SIMCONNECT_PERIOD { SIMCONNECT_PERIOD_SIM_FRAME = 3 };
enum SIMCONNECT_RECV_ID { SIMCONNECT_RECV_ID_NULL, SIMCONNECT_RECV_ID_EXCEPTION, SIMCONNECT_RECV_ID_OPEN, SIMCONNECT_RECV_ID_QUIT,
  SIMCONNECT_RECV_ID_EVENT, SIMCONNECT_RECV_ID_EVENT_OBJECT_ADDREMOVE, SIMCONNECT_RECV_ID_EVENT_FILENAME, SIMCONNECT_RECV_ID_EVENT_FRAME,
  SIMCONNECT_RECV_ID_SIMOBJECT_DATA = 8,
  SIMCONNECT_RECV_ID_CONTROLLERS_LIST = 32 };
struct SIMCONNECT_RECV { DWORD dwSize, dwVersion, dwID; };
struct SIMCONNECT_RECV_EXCEPTION : SIMCONNECT_RECV { DWORD dwException, dwSendID, dwIndex; };
struct SIMCONNECT_RECV_EVENT : SIMCONNECT_RECV { DWORD uGroupID, uEventID, dwData; };
struct SIMCONNECT_RECV_SIMOBJECT_DATA : SIMCONNECT_RECV { DWORD dwRequestID, dwObjectID, dwDefineID, dwFlags, dwentrynumber, dwoutof, dwDefineCount, dwData; };
typedef void (CALLBACK *DispatchProc)(SIMCONNECT_RECV*, DWORD, void*);
HRESULT SimConnect_Open(HANDLE*, const char*, void*, DWORD, void*, DWORD);
HRESULT SimConnect_Close(HANDLE);
HRESULT SimConnect_AddToDataDefinition(HANDLE, DWORD, const char*, const char*, SIMCONNECT_DATATYPE = SIMCONNECT_DATATYPE_FLOAT64, float = 0, DWORD = 0xFFFFFFFF);
HRESULT SimConnect_MapClientEventToSimEvent(HANDLE, DWORD, const char* = "");
HRESULT SimConnect_MapInputEventToClientEvent(HANDLE, DWORD, const char*, DWORD, DWORD = 0, DWORD = SIMCONNECT_UNUSED, DWORD = 0, BOOL = FALSE);
HRESULT SimConnect_SetInputGroupState(HANDLE, DWORD, DWORD);
HRESULT SimConnect_RequestDataOnSimObject(HANDLE, DWORD, DWORD, DWORD, SIMCONNECT_PERIOD);
HRESULT SimConnect_TransmitClientEvent(HANDLE, DWORD, DWORD, DWORD, DWORD, DWORD);
HRESULT SimConnect_CallDispatch(HANDLE, DispatchProc, void*);

// ---------------------------------------------------------------------------
// Additive declarations for the GS_USE_LEARN offline test build. They mirror
// the real SimConnect.h one-to-one (values and signatures). The existing mock
// suite is unaffected: test_glue.cpp and test_input_probe.cpp never reference
// these items.
// ---------------------------------------------------------------------------
typedef DWORD SIMCONNECT_INPUT_GROUP_ID;
typedef DWORD SIMCONNECT_NOTIFICATION_GROUP_ID;
typedef DWORD SIMCONNECT_CLIENT_EVENT_ID;
static const DWORD SIMCONNECT_GROUP_PRIORITY_DEFAULT = 2000000000;
static const DWORD SIMCONNECT_GROUP_PRIORITY_HIGHEST_MASKABLE = 10000000;
struct SIMCONNECT_VERSION_BASE_TYPE { unsigned short Major, Minor, Revision, Build; };
struct SIMCONNECT_CONTROLLER_ITEM {
  char DeviceName[256];
  unsigned int DeviceId, ProductId, CompositeID;
  SIMCONNECT_VERSION_BASE_TYPE HardwareVersion;
};
struct SIMCONNECT_RECV_LIST_TEMPLATE : SIMCONNECT_RECV { DWORD dwRequestID, dwArraySize, dwEntryNumber, dwOutOf; };
struct SIMCONNECT_RECV_CONTROLLERS_LIST : SIMCONNECT_RECV_LIST_TEMPLATE { SIMCONNECT_CONTROLLER_ITEM rgData[1]; };
HRESULT SimConnect_ClearInputGroup(HANDLE, DWORD);
HRESULT SimConnect_SetInputGroupPriority(HANDLE, DWORD, DWORD);
HRESULT SimConnect_AddClientEventToNotificationGroup(HANDLE, DWORD, DWORD, BOOL = FALSE);
HRESULT SimConnect_SetNotificationGroupPriority(HANDLE, DWORD, DWORD);
HRESULT SimConnect_GetLastSentPacketID(HANDLE, DWORD*);
HRESULT SimConnect_EnumerateControllers(HANDLE);
HRESULT SimConnect_MapInputEventToClientEvent_EX1(HANDLE, DWORD, const char*, DWORD, DWORD = 0, DWORD = SIMCONNECT_UNUSED, DWORD = 0, BOOL = FALSE);
