// NGX without NVIDIA's static library: the driver's _nvngx.dll exports the same C entry points the
// library forwards to. NVSDK_NGX_Parameter is a C++ interface; its MSVC vtable slots are called
// directly (read off NVSDK_NGX_Parameter_Set*/Get* in nvsdk_ngx_s.lib: overloads in reverse order).
#pragma once

#define VK_NO_PROTOTYPES
#include <stdbool.h>
#include <wchar.h>
#include <vulkan/vulkan.h>
#include "nvsdk_ngx_defs.h"
#include "nvsdk_ngx_defs_vk.h"

typedef struct NVSDK_NGX_Parameter NVSDK_NGX_Parameter;

typedef struct ngx_api {
	NVSDK_NGX_Result (*RequiredExtensions)(unsigned *n_inst, const char ***inst, unsigned *n_dev, const char ***dev);
	NVSDK_NGX_Result (*Init_ProjectID_Ext)(const char *project, NVSDK_NGX_EngineType engine, const char *engine_version,
					       const wchar_t *data_path, VkInstance, VkPhysicalDevice, VkDevice,
					       PFN_vkGetInstanceProcAddr, PFN_vkGetDeviceProcAddr, NVSDK_NGX_Version,
					       const NVSDK_NGX_FeatureCommonInfo *);
	NVSDK_NGX_Result (*Shutdown1)(VkDevice);
	NVSDK_NGX_Result (*GetCapabilityParameters)(NVSDK_NGX_Parameter **);
	NVSDK_NGX_Result (*AllocateParameters)(NVSDK_NGX_Parameter **);
	NVSDK_NGX_Result (*DestroyParameters)(NVSDK_NGX_Parameter *);
	NVSDK_NGX_Result (*CreateFeature1)(VkDevice, VkCommandBuffer, NVSDK_NGX_Feature, NVSDK_NGX_Parameter *,
					   NVSDK_NGX_Handle **);
	NVSDK_NGX_Result (*ReleaseFeature)(NVSDK_NGX_Handle *);
	NVSDK_NGX_Result (*EvaluateFeature)(VkCommandBuffer, const NVSDK_NGX_Handle *, const NVSDK_NGX_Parameter *,
					    void *progress_callback);
} ngx_api;

extern ngx_api ngx;

// Loads _nvngx.dll from the path the driver registers. Returns false (and logs) when unavailable.
bool ngx_load(void (*log)(const char *fmt, ...));

void ngx_set_ull(NVSDK_NGX_Parameter *p, const char *name, unsigned long long v);
void ngx_set_f(NVSDK_NGX_Parameter *p, const char *name, float v);
void ngx_set_ui(NVSDK_NGX_Parameter *p, const char *name, unsigned v);
void ngx_set_i(NVSDK_NGX_Parameter *p, const char *name, int v);
void ngx_set_ptr(NVSDK_NGX_Parameter *p, const char *name, void *v);
NVSDK_NGX_Result ngx_get_i(NVSDK_NGX_Parameter *p, const char *name, int *v);
NVSDK_NGX_Result ngx_get_ui(NVSDK_NGX_Parameter *p, const char *name, unsigned *v);
