#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <windows.h>

#include <atlbase.h>
#include <atltypes.h>
#include <atlapp.h>

extern CAppModule _Module;

#include <atlwin.h>
#include <atlgdi.h>
#include <atluser.h>
#include <atlcrack.h>

#include <d2d1_1.h>
#include <d3d11.h>
#include <dxgi1_4.h>
#include <dwrite.h>
#include <wincodec.h>

#include <wil/com.h>
#include <wil/resource.h>
#include <wil/result.h>
