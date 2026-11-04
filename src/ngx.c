#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "ngx.h"

ngx_api ngx;

bool ngx_load(void (*log)(const char *fmt, ...))
{
	wchar_t dir[MAX_PATH];
	DWORD size = sizeof dir;
	LSTATUS st = RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\NVIDIA Corporation\\Global\\NGXCore", L"FullPath",
				  RRF_RT_REG_SZ, NULL, dir, &size);
	if (st != ERROR_SUCCESS) {
		log("[ngx] no NGXCore path in the registry (%ld): not an NVIDIA driver with NGX\n", (long)st);
		return false;
	}
	wchar_t path[MAX_PATH + 16];
	_snwprintf(path, MAX_PATH + 16, L"%ls\\_nvngx.dll", dir);
	HMODULE m = LoadLibraryW(path);
	if (!m) {
		log("[ngx] loading %ls failed (%lu)\n", path, GetLastError());
		return false;
	}
#define GET(field, name)                                                       \
	if (!(*(FARPROC *)&ngx.field = GetProcAddress(m, name))) {             \
		log("[ngx] _nvngx.dll has no %s\n", name);                     \
		return false;                                                  \
	}
	GET(RequiredExtensions, "NVSDK_NGX_VULKAN_RequiredExtensions");
	GET(Init_ProjectID_Ext, "NVSDK_NGX_VULKAN_Init_ProjectID_Ext");
	GET(Shutdown1, "NVSDK_NGX_VULKAN_Shutdown1");
	GET(GetCapabilityParameters, "NVSDK_NGX_VULKAN_GetCapabilityParameters");
	GET(AllocateParameters, "NVSDK_NGX_VULKAN_AllocateParameters");
	GET(DestroyParameters, "NVSDK_NGX_VULKAN_DestroyParameters");
	GET(CreateFeature1, "NVSDK_NGX_VULKAN_CreateFeature1");
	GET(ReleaseFeature, "NVSDK_NGX_VULKAN_ReleaseFeature");
	GET(EvaluateFeature, "NVSDK_NGX_VULKAN_EvaluateFeature");
#undef GET
	log("[ngx] %ls loaded\n", path);
	return true;
}

// vtable slots (x64 MSVC): Set(void*) 0, Set(ID3D12Resource*) 1, Set(ID3D11Resource*) 2, Set(int) 3,
// Set(unsigned) 4, Set(double) 5, Set(float) 6, Set(unsigned long long) 7, then Get in the same
// reverse order at 8..15, Reset 16.
typedef void (*set_ull_fn)(NVSDK_NGX_Parameter *, const char *, unsigned long long);
typedef void (*set_f_fn)(NVSDK_NGX_Parameter *, const char *, float);
typedef void (*set_ui_fn)(NVSDK_NGX_Parameter *, const char *, unsigned);
typedef void (*set_i_fn)(NVSDK_NGX_Parameter *, const char *, int);
typedef void (*set_ptr_fn)(NVSDK_NGX_Parameter *, const char *, void *);
typedef NVSDK_NGX_Result (*get_i_fn)(NVSDK_NGX_Parameter *, const char *, int *);
typedef NVSDK_NGX_Result (*get_ui_fn)(NVSDK_NGX_Parameter *, const char *, unsigned *);

static void **vtbl(NVSDK_NGX_Parameter *p) { return *(void ***)p; }

void ngx_set_ptr(NVSDK_NGX_Parameter *p, const char *n, void *v) { ((set_ptr_fn)vtbl(p)[0])(p, n, v); }
void ngx_set_i(NVSDK_NGX_Parameter *p, const char *n, int v) { ((set_i_fn)vtbl(p)[3])(p, n, v); }
void ngx_set_ui(NVSDK_NGX_Parameter *p, const char *n, unsigned v) { ((set_ui_fn)vtbl(p)[4])(p, n, v); }
void ngx_set_f(NVSDK_NGX_Parameter *p, const char *n, float v) { ((set_f_fn)vtbl(p)[6])(p, n, v); }
void ngx_set_ull(NVSDK_NGX_Parameter *p, const char *n, unsigned long long v) { ((set_ull_fn)vtbl(p)[7])(p, n, v); }
NVSDK_NGX_Result ngx_get_i(NVSDK_NGX_Parameter *p, const char *n, int *v) { return ((get_i_fn)vtbl(p)[11])(p, n, v); }
NVSDK_NGX_Result ngx_get_ui(NVSDK_NGX_Parameter *p, const char *n, unsigned *v) { return ((get_ui_fn)vtbl(p)[12])(p, n, v); }
