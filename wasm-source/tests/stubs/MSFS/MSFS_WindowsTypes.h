#pragma once
#include <stdint.h>
typedef uint32_t DWORD; typedef void* HANDLE; typedef long HRESULT; typedef int BOOL;
#define CALLBACK
#define FALSE 0
#define TRUE 1
#define S_OK ((HRESULT)0)
#define E_FAIL ((HRESULT)0x80004005L)
#define SUCCEEDED(h) ((h) >= 0)
#define FAILED(h) ((h) < 0)
typedef void* HWND;   // added: elevator/yaxis probe headers use SimConnect_Open HWND
