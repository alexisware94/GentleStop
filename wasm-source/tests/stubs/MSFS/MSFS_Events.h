#pragma once

typedef int FsEventId;
#define FS_EVENT_INVALID_ID -1

struct FsVarParamArray { unsigned int size; void* array; };
typedef void (*FsEventsKeyEventHandler)(FsEventId eventId, FsVarParamArray* param, void* pUserParam);

extern "C" {
void fsEventsRegisterKeyEventHandler(FsEventsKeyEventHandler handler, void* pUserParam);
void fsEventsUnregisterKeyEventHandler(FsEventsKeyEventHandler handler, void* pUserParam);
}
