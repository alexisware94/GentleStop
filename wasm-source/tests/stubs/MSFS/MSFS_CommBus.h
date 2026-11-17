#pragma once

typedef void (*fsCommBusWasmCallback)(const char* buf, unsigned int bufSize, void* ctx);

enum FsCommBusBroadcastFlags : unsigned char {
    FsCommBusBroadcast_JS = 1 << 0,
    FsCommBusBroadcast_Wasm = 1 << 1,
    FsCommBusBroadcast_WasmSelfCall = 1 << 2,
    FsCommBusBroadcast_SimConnect = 1 << 3,
    FsCommBusBroadcast_Default = FsCommBusBroadcast_JS | FsCommBusBroadcast_Wasm | FsCommBusBroadcast_SimConnect
};

extern "C" {
bool fsCommBusCall(const char* eventName, const char* buf, unsigned int bufSize, FsCommBusBroadcastFlags broadcastTo = FsCommBusBroadcast_Default);
bool fsCommBusRegister(const char* eventName, fsCommBusWasmCallback callback, void* context = nullptr);
int fsCommBusUnregister(const char* eventName, fsCommBusWasmCallback callback = nullptr);
}
