// mhgudlss_nvngx.dll: calls into nvngx_dlssnr.dll (DLSS Neural Rendering, NGX feature 18) on the
// proxy's behalf. The NR runtime only accepts calls whose return address lies in a module whose path
// contains "nvngx.dll" (GetModuleHandleEx on the caller, then wcsstr), so every call into it has to be
// made from here, as a real call: a tail call would leave the proxy's address as the caller. Built
// with -O0 -fno-optimize-sibling-calls for that reason.

#define VK_NO_PROTOTYPES
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <vulkan/vulkan.h>
#include <wchar.h>

typedef int ngx_result;
typedef void ngx_params;
typedef void ngx_handle;

typedef ngx_result (*init_ext_fn)(unsigned long long app_id, const wchar_t *data_path, VkInstance, VkPhysicalDevice,
				  VkDevice, int sdk_version, const void *feature_info);
typedef ngx_result (*create_fn)(VkDevice, VkCommandBuffer, int feature, ngx_params *, ngx_handle **);
typedef ngx_result (*eval_fn)(VkCommandBuffer, const ngx_handle *, const ngx_params *, void *progress);
typedef ngx_result (*release_fn)(ngx_handle *);

static init_ext_fn nr_init_ext;
static create_fn nr_create;
static eval_fn nr_eval;
static release_fn nr_release;

#define NR_FEATURE 18

// Loads the NR runtime from the given path. 0 when it or one of its entry points is missing.
__declspec(dllexport) int nrsat_load(const wchar_t *path)
{
	HMODULE m = LoadLibraryW(path);
	if (!m)
		return 0;
	nr_init_ext = (init_ext_fn)GetProcAddress(m, "NVSDK_NGX_VULKAN_Init_Ext");
	nr_create = (create_fn)GetProcAddress(m, "NVSDK_NGX_VULKAN_CreateFeature1");
	nr_eval = (eval_fn)GetProcAddress(m, "NVSDK_NGX_VULKAN_EvaluateFeature");
	nr_release = (release_fn)GetProcAddress(m, "NVSDK_NGX_VULKAN_ReleaseFeature");
	return nr_init_ext && nr_create && nr_eval && nr_release;
}

__declspec(dllexport) ngx_result nrsat_init(unsigned long long app_id, const wchar_t *data_path, VkInstance inst,
					    VkPhysicalDevice pd, VkDevice dev, int sdk_version)
{
	volatile ngx_result r = nr_init_ext(app_id, data_path, inst, pd, dev, sdk_version, NULL);
	return r;
}

__declspec(dllexport) ngx_result nrsat_create(VkDevice dev, VkCommandBuffer cb, ngx_params *p, ngx_handle **out)
{
	volatile ngx_result r = nr_create(dev, cb, NR_FEATURE, p, out);
	return r;
}

__declspec(dllexport) ngx_result nrsat_eval(VkCommandBuffer cb, const ngx_handle *h, const ngx_params *p)
{
	volatile ngx_result r = nr_eval(cb, h, p, NULL);
	return r;
}

__declspec(dllexport) ngx_result nrsat_release(ngx_handle *h)
{
	volatile ngx_result r = nr_release(h);
	return r;
}
