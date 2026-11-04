// mhgu-dlss: vulkan-1.dll proxy for MHGU Recomp.
// Every export of the system vulkan-1.dll is forwarded (src/exports.S, written by gen_exports.py);
// the functions named on the HOOKS lines are implemented here instead.
//
// Stage 2: DLAA. The renderer's passes are told apart by their attachments (FRAME.md): the main depth
// is the first depth attachment that comes with a colour attachment of its size; the scene colour is
// the last full-size colour-only target after it; DLSS runs on those two just before the first
// smaller colour-only pass (the bloom downsample). Passes on the main depth are jittered by moving
// their viewports a fraction of a pixel. Motion vectors come from the camera: the game's 3D vertex
// shaders take the view-projection from vertex constant buffer 4 (rows 0-3), which the recomp's
// backend has bound when a scene pass begins; a compute pass reprojects the depth buffer with this
// and the previous frame's matrix.
//
// DLSS 5 Neural Rendering (NGX feature 18, nvngx_dlssnr.dll) runs after DLAA, the way NVIDIA places it
// after the upscaler: DLAA's output is shrunk to at most 2560x1440 (the runtime's limit), the network
// runs there, and only what it changed is scaled back up onto the full-resolution picture. The runtime
// takes calls only from a module named *nvngx.dll, so they go through mhgudlss_nvngx.dll.

#define VK_NO_PROTOTYPES
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <vulkan/vulkan.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "ngx.h"
#include "shaders.h"

// Hooked here, not forwarded (read by gen_exports.py):
// HOOKS: vkGetInstanceProcAddr vkGetDeviceProcAddr vkCreateInstance vkCreateDevice vkQueuePresentKHR
// HOOKS: vkCmdBeginRenderPass vkCmdSetViewport vkCmdPipelineBarrier vkCmdDraw vkCmdDrawIndexed
// HOOKS: vkCreateImage vkDestroyImage vkCreateImageView vkDestroyImageView
// HOOKS: vkCreateFramebuffer vkDestroyFramebuffer vkCreateRenderPass vkDestroyRenderPass

extern const char *proxy_names[];
extern void *proxy_reals[];

// ---- log ----

static FILE *log_file;
static CRITICAL_SECTION log_lock;

static void dlog(const char *fmt, ...)
{
	if (!log_file)
		return;
	va_list ap;
	va_start(ap, fmt);
	EnterCriticalSection(&log_lock);
	vfprintf(log_file, fmt, ap);
	fflush(log_file);
	LeaveCriticalSection(&log_lock);
	va_end(ap);
}

// ---- the real vulkan-1.dll ----

static void *real(const char *name)
{
	for (int i = 0; proxy_names[i]; i++)
		if (!strcmp(proxy_names[i], name))
			return proxy_reals[i];
	return NULL;
}

#define REALS(X)                                                                                         \
	X(GetInstanceProcAddr) X(GetDeviceProcAddr) X(CreateInstance) X(CreateDevice) X(QueuePresentKHR)  \
	X(CmdBeginRenderPass) X(CmdSetViewport) X(CmdPipelineBarrier) X(CreateImage) X(DestroyImage)      \
	X(CreateImageView) X(DestroyImageView) X(CreateFramebuffer) X(DestroyFramebuffer)                 \
	X(CreateRenderPass) X(DestroyRenderPass) X(GetImageMemoryRequirements) X(AllocateMemory)          \
	X(FreeMemory) X(BindImageMemory) X(CmdCopyImage) X(CmdClearColorImage) X(DeviceWaitIdle)          \
	X(GetPhysicalDeviceMemoryProperties) X(EnumerateInstanceExtensionProperties)                      \
	X(EnumerateDeviceExtensionProperties) X(GetPhysicalDeviceProperties) X(CreateSampler)                \
	X(CreateDescriptorSetLayout) X(CreatePipelineLayout) X(CreateShaderModule) X(DestroyShaderModule)   \
	X(CreateComputePipelines) X(CmdBindPipeline) X(CmdPushConstants) X(CmdDispatch) X(CmdDraw) X(CmdDrawIndexed)
#define DECL(n) static PFN_vk##n r_##n;
REALS(DECL)
#undef DECL

static BOOL load_real(void)
{
	wchar_t path[MAX_PATH];
	UINT n = GetSystemDirectoryW(path, MAX_PATH);
	if (!n || n > MAX_PATH - 16)
		return FALSE;
	wcscat(path, L"\\vulkan-1.dll");
	HMODULE m = LoadLibraryW(path);
	if (!m)
		return FALSE;
	for (int i = 0; proxy_names[i]; i++)
		proxy_reals[i] = (void *)GetProcAddress(m, proxy_names[i]);
#define GET(n) r_##n = (PFN_vk##n)real("vk" #n);
	REALS(GET)
#undef GET
	return TRUE;
}

// ---- settings: [DLSS] in mhgurecomp.ini (read at start, written by the Forge menu) ----

enum { MODE_OFF, MODE_DLAA };

static struct {
	int mode;
	int preset;	    // index in presets
	int toggle_key;	    // virtual key that switches DLSS on and off while playing
	float jitter_sign[2]; // diagnostics: sign of the jitter reported to DLSS
	int camera_mv;	    // motion vectors from the camera (else zero)
	int nr;		    // DLSS 5 Neural Rendering after DLAA
	int nr_key;	    // switches it on and off while playing
	float nr_intensity, nr_tone, nr_structure, nr_skin; // the network's own strengths (skin -1: its choice)
	float nr_strength;  // how much of its change is put back on the picture
	int nr_model;	    // DLSSNR.Hint.Render.Preset: 0 the runtime's default, 1 model A, 2 model B
	int nr_preset;	    // index in nr_presets
} cfg = { MODE_OFF, 0, VK_HOME, { 1, 1 }, 1, 0, VK_END, 1.0f, 0.15f, 0.70f, -1.0f, 1.0f, 0, 2 };

static const struct { const char *n; int p; } presets[] = {
	{ "Default", NVSDK_NGX_DLSS_Hint_Render_Preset_Default }, { "J", NVSDK_NGX_DLSS_Hint_Render_Preset_J },
	{ "K", NVSDK_NGX_DLSS_Hint_Render_Preset_K }, { "L", NVSDK_NGX_DLSS_Hint_Render_Preset_L },
	{ "M", NVSDK_NGX_DLSS_Hint_Render_Preset_M },
};
#define N_PRESETS (int)(sizeof presets / sizeof *presets)

// Lightest to strongest, as the menu lists them.
static const struct { const char *n; float i, t, st, sk; } nr_presets[] = {
	{ "Light", 0.45f, 0.55f, 0.25f, 0.15f }, { "Moderate", 0.70f, 0.80f, 0.55f, 0.40f },
	{ "Default", 1.00f, 0.15f, 0.70f, -1.0f }, { "Reference", 1.00f, 1.00f, 1.00f, -1.0f },
	{ "Overdrive", 1.30f, 1.25f, 1.45f, 1.20f },
};
#define N_NR_PRESETS (int)(sizeof nr_presets / sizeof *nr_presets)

static const char *const nr_models[] = { "Default", "A", "B" };

static void set_nr_preset(int i)
{
	cfg.nr_preset = i;
	cfg.nr_intensity = nr_presets[i].i;
	cfg.nr_tone = nr_presets[i].t;
	cfg.nr_structure = nr_presets[i].st;
	cfg.nr_skin = nr_presets[i].sk;
}

static int key_by_name(const char *s)
{
	static const struct { const char *n; int vk; } keys[] = {
		{ "Home", VK_HOME }, { "End", VK_END }, { "PageUp", VK_PRIOR }, { "PageDown", VK_NEXT },
		{ "Pause", VK_PAUSE }, { "ScrollLock", VK_SCROLL }, { "None", 0 },
	};
	for (size_t i = 0; i < sizeof keys / sizeof *keys; i++)
		if (!_stricmp(s, keys[i].n))
			return keys[i].vk;
	if ((s[0] == 'F' || s[0] == 'f') && atoi(s + 1) >= 1 && atoi(s + 1) <= 24)
		return VK_F1 + atoi(s + 1) - 1;
	return -1;
}

static char ini_path[MAX_PATH];

static void load_config(void)
{
	char v[64];
	DWORD n = GetModuleFileNameA(NULL, ini_path, MAX_PATH);
	char *slash = strrchr(ini_path, '\\');
	if (!n || !slash) {
		ini_path[0] = 0;
		return;
	}
	strcpy(slash + 1, "mhgurecomp.ini");
	const char *ini = ini_path;

	GetPrivateProfileStringA("DLSS", "Mode", "Off", v, sizeof v, ini);
	cfg.mode = !_stricmp(v, "DLAA") ? MODE_DLAA : MODE_OFF;
	if (_stricmp(v, "Off") && _stricmp(v, "DLAA"))
		dlog("[config] Mode = %s: only Off and DLAA exist so far, using Off\n", v);

	GetPrivateProfileStringA("DLSS", "Preset", "Default", v, sizeof v, ini);
	for (int i = 0; i < N_PRESETS; i++)
		if (!_stricmp(v, presets[i].n))
			cfg.preset = i;

	GetPrivateProfileStringA("DLSS", "ToggleKey", "Home", v, sizeof v, ini);
	int k = key_by_name(v);
	if (k >= 0)
		cfg.toggle_key = k;
	else
		dlog("[config] ToggleKey = %s not known, using Home\n", v);

	GetPrivateProfileStringA("DLSS", "MotionVectors", "Camera", v, sizeof v, ini);
	cfg.camera_mv = _stricmp(v, "Off") != 0;

	GetPrivateProfileStringA("DLSS", "NeuralRendering", "Off", v, sizeof v, ini);
	cfg.nr = !_stricmp(v, "On");
	GetPrivateProfileStringA("DLSS", "NRToggleKey", "End", v, sizeof v, ini);
	k = key_by_name(v);
	if (k >= 0)
		cfg.nr_key = k;
	GetPrivateProfileStringA("DLSS", "NRPreset", "Default", v, sizeof v, ini);
	for (int i = 0; i < N_NR_PRESETS; i++)
		if (!_stricmp(v, nr_presets[i].n))
			set_nr_preset(i);
	GetPrivateProfileStringA("DLSS", "NRModel", "Default", v, sizeof v, ini);
	cfg.nr_model = !_stricmp(v, "A") ? 1 : !_stricmp(v, "B") ? 2 : 0;
	GetPrivateProfileStringA("DLSS", "NRStrength", "1.0", v, sizeof v, ini);
	cfg.nr_strength = (float)atof(v);
	if (cfg.nr_strength < 0)
		cfg.nr_strength = 0;
	if (cfg.nr_strength > 2)
		cfg.nr_strength = 2;

	GetPrivateProfileStringA("DLSS", "JitterSignX", "1", v, sizeof v, ini);
	cfg.jitter_sign[0] = atof(v) < 0 ? -1.0f : 1.0f;
	GetPrivateProfileStringA("DLSS", "JitterSignY", "1", v, sizeof v, ini);
	cfg.jitter_sign[1] = atof(v) < 0 ? -1.0f : 1.0f;

	dlog("[config] %s: Mode %s, Preset %s, ToggleKey %#x, motion vectors %s, jitter signs %+.0f %+.0f\n", ini,
	     cfg.mode == MODE_OFF ? "Off" : "DLAA", presets[cfg.preset].n, cfg.toggle_key,
	     cfg.camera_mv ? "camera" : "off", cfg.jitter_sign[0], cfg.jitter_sign[1]);
	dlog("[config] neural rendering %s (key %#x), preset %s, model %s: intensity %.2f, local tone %.2f, "
	     "structure %.2f, skin %.2f, strength %.2f\n", cfg.nr ? "on" : "off", cfg.nr_key,
	     nr_presets[cfg.nr_preset].n, nr_models[cfg.nr_model], cfg.nr_intensity, cfg.nr_tone, cfg.nr_structure,
	     cfg.nr_skin, cfg.nr_strength);
}

// Writes the settings the Forge menu changes; the comments and the other keys in the file stay as they are.
static void save_config(void)
{
	if (!ini_path[0])
		return;
	char v[32];
	int ok = WritePrivateProfileStringA("DLSS", "Mode", cfg.mode == MODE_OFF ? "Off" : "DLAA", ini_path);
	ok &= WritePrivateProfileStringA("DLSS", "Preset", presets[cfg.preset].n, ini_path);
	ok &= WritePrivateProfileStringA("DLSS", "MotionVectors", cfg.camera_mv ? "Camera" : "Off", ini_path);
	ok &= WritePrivateProfileStringA("DLSS", "NeuralRendering", cfg.nr ? "On" : "Off", ini_path);
	ok &= WritePrivateProfileStringA("DLSS", "NRPreset", nr_presets[cfg.nr_preset].n, ini_path);
	ok &= WritePrivateProfileStringA("DLSS", "NRModel", nr_models[cfg.nr_model], ini_path);
	snprintf(v, sizeof v, "%.2f", cfg.nr_strength);
	ok &= WritePrivateProfileStringA("DLSS", "NRStrength", v, ini_path);
	dlog("[config] %s %s\n", ok ? "saved to" : "could not be saved to", ini_path);
}

// ---- the recomp's own FXAA switch (g_config+0x20, read by draw() for every draw) ----

#define RECOMP_TIMESTAMP 0x6adc9f04u // the mhgurecomp.exe these offsets were read from
#define RECOMP_FXAA_RVA 0x2cc020u
#define RECOMP_ST_RVA 0x76c970u	    // the Vulkan backend's state ("st")
#define ST_UBO 0x3a8		    // st: bound constant buffers {u64 guest address, u64 size}[6 stages][18]
#define GUEST_BASE 0x10000000000ull // guest address 0 in host memory

static volatile int *fxaa_flag;
static int fxaa_user;
static char *recomp_st;

static void find_fxaa_flag(void)
{
	char *base = (char *)GetModuleHandleW(NULL);
	IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
	if (nt->FileHeader.TimeDateStamp != RECOMP_TIMESTAMP) {
		dlog("[fxaa] mhgurecomp.exe is another build (%08lx): FXAA is left as mhgurecomp.ini sets it\n",
		     nt->FileHeader.TimeDateStamp);
		return;
	}
	fxaa_flag = (volatile int *)(base + RECOMP_FXAA_RVA);
	recomp_st = base + RECOMP_ST_RVA;
}

// The view-projection in the bound vertex constant buffer the shaders call cb4 (NVN binding 1: the
// translated shaders number them binding + 3), row-vector convention: clip = pos * M. Only if it looks
// like a perspective camera; draws that do not use the camera (full-screen passes) fail this.
static int read_camera(float m[16])
{
	if (!recomp_st)
		return 0;
	const uint64_t *ubo = (const uint64_t *)(recomp_st + ST_UBO + 1 * 16); // stage 0 (vertex), binding 1
	uint64_t addr = ubo[0], size = ubo[1];
	if (!addr || addr >= 0x100000000ull || size < 64)
		return 0;
	memcpy(m, (const void *)(uintptr_t)(GUEST_BASE + addr), 64);
	// w = -(view z): the column that makes w is a unit vector (a rotation row) for a perspective camera,
	// and depth (column 2) follows it; 2D matrices fail one or the other.
	float len = sqrtf(m[3] * m[3] + m[7] * m[7] + m[11] * m[11]);
	float zlen = sqrtf(m[2] * m[2] + m[6] * m[6] + m[10] * m[10]);
	return len > 0.98f && len < 1.02f && zlen > 0.5f && fabsf(m[15]) + fabsf(m[14]) > 0;
}

static int invert4(const double *m, double *inv)
{
	inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
	inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
	inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
	inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
	inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
	inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
	inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
	inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
	inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
	inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
	inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
	inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
	inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
	inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
	inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
	inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
	double det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
	if (det == 0)
		return 0;
	for (int i = 0; i < 16; i++)
		inv[i] /= det;
	return 1;
}

// out = inverse(cur) * prev, row-major (row-vector convention: clip_prev = clip_cur * out). Read as a
// column-major GLSL mat4 that is the transpose, so in the shader clip_prev = mat * clip_cur.
static int cur_to_prev(const float *cur, const float *prev, float *out)
{
	double c[16], ci[16];
	for (int i = 0; i < 16; i++)
		c[i] = cur[i];
	if (!invert4(c, ci))
		return 0;
	for (int r = 0; r < 4; r++)
		for (int k = 0; k < 4; k++) {
			double v = 0;
			for (int j = 0; j < 4; j++)
				v += ci[r * 4 + j] * prev[j * 4 + k];
			out[r * 4 + k] = (float)v;
		}
	return 1;
}

static void set_game_fxaa(int dlss_on)
{
	if (!fxaa_flag)
		return;
	*fxaa_flag = dlss_on ? 0 : fxaa_user;
}

// ---- handle maps ----

typedef struct map {
	uint64_t *keys;
	void **vals;
	size_t cap, used; // used counts live entries and tombstones
} map;

#define TOMB ((void *)1)
static SRWLOCK map_lock = SRWLOCK_INIT;

static size_t hash64(uint64_t k) { return (size_t)((k * 0x9e3779b97f4a7c15ull) >> 17); }

static void map_put(map *m, uint64_t k, void *v);

static void map_grow(map *m)
{
	map old = *m;
	m->cap = old.cap ? old.cap * 2 : 4096;
	m->keys = calloc(m->cap, sizeof *m->keys);
	m->vals = calloc(m->cap, sizeof *m->vals);
	m->used = 0;
	for (size_t i = 0; i < old.cap; i++)
		if (old.vals[i] && old.vals[i] != TOMB)
			map_put(m, old.keys[i], old.vals[i]);
	free(old.keys);
	free(old.vals);
}

static void map_put(map *m, uint64_t k, void *v)
{
	if ((m->used + 1) * 4 >= m->cap * 3)
		map_grow(m);
	size_t i = hash64(k) & (m->cap - 1);
	for (;; i = (i + 1) & (m->cap - 1)) {
		if (!m->vals[i] || (m->vals[i] != TOMB && m->keys[i] == k)) {
			if (!m->vals[i])
				m->used++;
			m->keys[i] = k;
			m->vals[i] = v;
			return;
		}
	}
}

static void *map_get(map *m, uint64_t k)
{
	if (!m->cap)
		return NULL;
	for (size_t i = hash64(k) & (m->cap - 1);; i = (i + 1) & (m->cap - 1)) {
		if (!m->vals[i])
			return NULL;
		if (m->vals[i] != TOMB && m->keys[i] == k)
			return m->vals[i];
	}
}

static void *map_take(map *m, uint64_t k)
{
	if (!m->cap)
		return NULL;
	for (size_t i = hash64(k) & (m->cap - 1);; i = (i + 1) & (m->cap - 1)) {
		if (!m->vals[i])
			return NULL;
		if (m->vals[i] != TOMB && m->keys[i] == k) {
			void *v = m->vals[i];
			m->vals[i] = TOMB;
			return v;
		}
	}
}

#define H(x) ((uint64_t)(uintptr_t)(x))

typedef struct image_info {
	VkImage image;
	VkFormat format;
	VkExtent3D extent;
	VkImageUsageFlags usage;
	VkSampleCountFlagBits samples;
	VkImageLayout layout; // last layout seen in recording order
	VkImageView own_view; // our sampled view (depth: depth aspect only), made on first use
} image_info;

typedef struct view_info {
	VkImage image;
} view_info;

typedef struct fb_info {
	uint32_t n, w, h;
	VkImage images[8];
} fb_info;

typedef struct rp_info {
	uint32_t n;
	VkFormat formats[8];
	VkImageLayout final_layouts[8];
} rp_info;

static map images, views, fbs, rps, cbs;

static image_info *image_of(VkImage im)
{
	AcquireSRWLockShared(&map_lock);
	image_info *ii = map_get(&images, H(im));
	ReleaseSRWLockShared(&map_lock);
	return ii;
}

static int is_depth_format(VkFormat f)
{
	return f == VK_FORMAT_D16_UNORM || f == VK_FORMAT_X8_D24_UNORM_PACK32 || f == VK_FORMAT_D32_SFLOAT ||
	       f == VK_FORMAT_D16_UNORM_S8_UINT || f == VK_FORMAT_D24_UNORM_S8_UINT || f == VK_FORMAT_D32_SFLOAT_S8_UINT ||
	       f == VK_FORMAT_S8_UINT;
}

static int has_stencil(VkFormat f)
{
	return f == VK_FORMAT_D16_UNORM_S8_UINT || f == VK_FORMAT_D24_UNORM_S8_UINT || f == VK_FORMAT_D32_SFLOAT_S8_UINT;
}

// ---- device state ----

static VkInstance g_instance;
static VkPhysicalDevice g_pd;
static VkDevice g_device;
static int ngx_ok;	// NGX initialised and DLSS available
static int ngx_tried;
static NVSDK_NGX_Parameter *ngx_params;

// ---- resource tracking hooks ----

VKAPI_ATTR VkResult VKAPI_CALL vkCreateImage(VkDevice dev, const VkImageCreateInfo *ci, const VkAllocationCallbacks *a,
					     VkImage *out)
{
	VkImageCreateInfo c = *ci;
	// Render targets must be readable by DLSS and writable by a copy.
	if (c.usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)
		c.usage |= VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	if (c.usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)
		c.usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
	VkResult r = r_CreateImage(dev, &c, a, out);
	if (r == VK_SUCCESS && c.imageType == VK_IMAGE_TYPE_2D &&
	    (c.usage & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT))) {
		image_info *ii = calloc(1, sizeof *ii);
		ii->image = *out;
		ii->format = c.format;
		ii->extent = c.extent;
		ii->usage = c.usage;
		ii->samples = c.samples;
		ii->layout = c.initialLayout;
		AcquireSRWLockExclusive(&map_lock);
		map_put(&images, H(*out), ii);
		ReleaseSRWLockExclusive(&map_lock);
	}
	return r;
}

VKAPI_ATTR void VKAPI_CALL vkDestroyImage(VkDevice dev, VkImage im, const VkAllocationCallbacks *a)
{
	AcquireSRWLockExclusive(&map_lock);
	image_info *ii = map_take(&images, H(im));
	ReleaseSRWLockExclusive(&map_lock);
	if (ii) {
		if (ii->own_view)
			r_DestroyImageView(dev, ii->own_view, NULL);
		free(ii);
	}
	r_DestroyImage(dev, im, a);
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateImageView(VkDevice dev, const VkImageViewCreateInfo *ci,
						 const VkAllocationCallbacks *a, VkImageView *out)
{
	VkResult r = r_CreateImageView(dev, ci, a, out);
	if (r == VK_SUCCESS && image_of(ci->image)) {
		view_info *vi = calloc(1, sizeof *vi);
		vi->image = ci->image;
		AcquireSRWLockExclusive(&map_lock);
		map_put(&views, H(*out), vi);
		ReleaseSRWLockExclusive(&map_lock);
	}
	return r;
}

VKAPI_ATTR void VKAPI_CALL vkDestroyImageView(VkDevice dev, VkImageView v, const VkAllocationCallbacks *a)
{
	AcquireSRWLockExclusive(&map_lock);
	free(map_take(&views, H(v)));
	ReleaseSRWLockExclusive(&map_lock);
	r_DestroyImageView(dev, v, a);
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateFramebuffer(VkDevice dev, const VkFramebufferCreateInfo *ci,
						   const VkAllocationCallbacks *a, VkFramebuffer *out)
{
	VkResult r = r_CreateFramebuffer(dev, ci, a, out);
	if (r != VK_SUCCESS || (ci->flags & VK_FRAMEBUFFER_CREATE_IMAGELESS_BIT) || ci->attachmentCount > 8)
		return r;
	fb_info *fi = calloc(1, sizeof *fi);
	fi->n = ci->attachmentCount;
	fi->w = ci->width;
	fi->h = ci->height;
	AcquireSRWLockExclusive(&map_lock);
	for (uint32_t i = 0; i < fi->n; i++) {
		view_info *vi = map_get(&views, H(ci->pAttachments[i]));
		fi->images[i] = vi ? vi->image : VK_NULL_HANDLE;
	}
	map_put(&fbs, H(*out), fi);
	ReleaseSRWLockExclusive(&map_lock);
	return r;
}

VKAPI_ATTR void VKAPI_CALL vkDestroyFramebuffer(VkDevice dev, VkFramebuffer fb, const VkAllocationCallbacks *a)
{
	AcquireSRWLockExclusive(&map_lock);
	free(map_take(&fbs, H(fb)));
	ReleaseSRWLockExclusive(&map_lock);
	r_DestroyFramebuffer(dev, fb, a);
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateRenderPass(VkDevice dev, const VkRenderPassCreateInfo *ci,
						  const VkAllocationCallbacks *a, VkRenderPass *out)
{
	VkResult r = r_CreateRenderPass(dev, ci, a, out);
	if (r != VK_SUCCESS || ci->attachmentCount > 8)
		return r;
	rp_info *ri = calloc(1, sizeof *ri);
	ri->n = ci->attachmentCount;
	for (uint32_t i = 0; i < ri->n; i++) {
		ri->formats[i] = ci->pAttachments[i].format;
		ri->final_layouts[i] = ci->pAttachments[i].finalLayout;
	}
	AcquireSRWLockExclusive(&map_lock);
	map_put(&rps, H(*out), ri);
	ReleaseSRWLockExclusive(&map_lock);
	return r;
}

VKAPI_ATTR void VKAPI_CALL vkDestroyRenderPass(VkDevice dev, VkRenderPass rp, const VkAllocationCallbacks *a)
{
	AcquireSRWLockExclusive(&map_lock);
	free(map_take(&rps, H(rp)));
	ReleaseSRWLockExclusive(&map_lock);
	r_DestroyRenderPass(dev, rp, a);
}

VKAPI_ATTR void VKAPI_CALL vkCmdPipelineBarrier(VkCommandBuffer cb, VkPipelineStageFlags src, VkPipelineStageFlags dst,
						VkDependencyFlags dep, uint32_t nm, const VkMemoryBarrier *m,
						uint32_t nb, const VkBufferMemoryBarrier *b, uint32_t ni,
						const VkImageMemoryBarrier *ib)
{
	for (uint32_t i = 0; i < ni; i++) {
		image_info *ii = image_of(ib[i].image);
		if (ii)
			ii->layout = ib[i].newLayout;
	}
	r_CmdPipelineBarrier(cb, src, dst, dep, nm, m, nb, b, ni, ib);
}

// ---- frame state ----

static volatile LONG64 frame_no;
static int dlss_on;	     // mode on and NGX ready, and not switched off with the toggle key
static int dlss_reset = 1;   // tell DLSS the history is invalid (first frame, after toggling)
static float jitter[2];	     // this frame's sub-pixel offset in pixels (+x right, +y down)

static struct {
	image_info *main_depth;	 // this frame
	float cam[16];		 // view-projection, read when the first scene pass begins
	int have_cam;
	image_info *scene_color; // last full-size colour-only target after the main depth
	int done, passes;
} fr;
static VkImage jitter_depth; // passes on this depth image are jittered (the previous frame's main depth)
static float prev_cam[16];   // view-projection of the last frame DLSS ran on
static int have_prev_cam;
static int logged_frames;

typedef struct cb_state {
	int scene;	    // inside a pass on the main depth
	int want_cam;	    // read the camera at the next draw
	int have_vp;
	VkViewport vp;	    // last viewport the game set, unjittered
} cb_state;

static cb_state *cb_of(VkCommandBuffer cb)
{
	AcquireSRWLockShared(&map_lock);
	cb_state *s = map_get(&cbs, H(cb));
	ReleaseSRWLockShared(&map_lock);
	if (!s) {
		s = calloc(1, sizeof *s);
		AcquireSRWLockExclusive(&map_lock);
		map_put(&cbs, H(cb), s);
		ReleaseSRWLockExclusive(&map_lock);
	}
	return s;
}

static float halton(int i, int b)
{
	float f = 1, r = 0;
	for (; i > 0; i /= b) {
		f /= b;
		r += f * (i % b);
	}
	return r;
}

// ---- DLSS ----

static struct {
	uint32_t w, h;
	NVSDK_NGX_Handle *feature;
	VkImage mv, out;
	VkImageView mv_view, out_view;
	VkDeviceMemory mv_mem, out_mem;
	int fresh; // images just made: still UNDEFINED
} dl;

static uint32_t memory_type(uint32_t bits, VkMemoryPropertyFlags want)
{
	VkPhysicalDeviceMemoryProperties mp;
	r_GetPhysicalDeviceMemoryProperties(g_pd, &mp);
	for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
		if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want)
			return i;
	return 0;
}

static int make_image_sized(uint32_t w, uint32_t h, VkFormat fmt, VkImageUsageFlags usage, VkImage *im,
			    VkDeviceMemory *mem, VkImageView *view)
{
	VkImageCreateInfo ci = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
	ci.imageType = VK_IMAGE_TYPE_2D;
	ci.format = fmt;
	ci.extent = (VkExtent3D){ w, h, 1 };
	ci.mipLevels = 1;
	ci.arrayLayers = 1;
	ci.samples = VK_SAMPLE_COUNT_1_BIT;
	ci.tiling = VK_IMAGE_TILING_OPTIMAL;
	ci.usage = usage;
	if (r_CreateImage(g_device, &ci, NULL, im) != VK_SUCCESS)
		return 0;
	VkMemoryRequirements req;
	r_GetImageMemoryRequirements(g_device, *im, &req);
	VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
	ai.allocationSize = req.size;
	ai.memoryTypeIndex = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	if (r_AllocateMemory(g_device, &ai, NULL, mem) != VK_SUCCESS)
		return 0;
	r_BindImageMemory(g_device, *im, *mem, 0);
	VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
	vi.image = *im;
	vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
	vi.format = fmt;
	vi.subresourceRange = (VkImageSubresourceRange){ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
	return r_CreateImageView(g_device, &vi, NULL, view) == VK_SUCCESS;
}

static int make_image(VkFormat fmt, VkImageUsageFlags usage, VkImage *im, VkDeviceMemory *mem, VkImageView *view)
{
	return make_image_sized(dl.w, dl.h, fmt, usage, im, mem, view);
}

static void free_nr(void);

static void free_dlss(void)
{
	r_DeviceWaitIdle(g_device);
	if (dl.feature)
		ngx.ReleaseFeature(dl.feature);
	if (dl.mv_view) r_DestroyImageView(g_device, dl.mv_view, NULL);
	if (dl.out_view) r_DestroyImageView(g_device, dl.out_view, NULL);
	if (dl.mv) r_DestroyImage(g_device, dl.mv, NULL);
	if (dl.out) r_DestroyImage(g_device, dl.out, NULL);
	if (dl.mv_mem) r_FreeMemory(g_device, dl.mv_mem, NULL);
	if (dl.out_mem) r_FreeMemory(g_device, dl.out_mem, NULL);
	memset(&dl, 0, sizeof dl);
	free_nr();
}

// (Re)creates the DLSS feature and our images for a w x h render size. Records into cb.
static volatile int dl_rebuild, nr_rebuild; // set by the menu: a new DLAA preset / network model

static int ensure_dlss(VkCommandBuffer cb, uint32_t w, uint32_t h, VkFormat color_fmt)
{
	if (dl_rebuild) {
		dl_rebuild = 0;
		if (dl.w || dl.feature)
			free_dlss();
	}
	if (dl.feature && dl.w == w && dl.h == h)
		return 1;
	if (dl.w || dl.feature)
		free_dlss();
	dl.w = w;
	dl.h = h;
	if (!make_image(VK_FORMAT_R16G16_SFLOAT,
			VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, &dl.mv,
			&dl.mv_mem, &dl.mv_view) ||
	    !make_image(color_fmt,
			VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, &dl.out,
			&dl.out_mem, &dl.out_view)) {
		dlog("[dlss] creating the %ux%u motion vector / output images failed\n", w, h);
		free_dlss();
		return 0;
	}
	dl.fresh = 1;

	NVSDK_NGX_Parameter *p = ngx_params;
	ngx_set_ui(p, NVSDK_NGX_Parameter_CreationNodeMask, 1);
	ngx_set_ui(p, NVSDK_NGX_Parameter_VisibilityNodeMask, 1);
	ngx_set_ui(p, NVSDK_NGX_Parameter_Width, w);
	ngx_set_ui(p, NVSDK_NGX_Parameter_Height, h);
	ngx_set_ui(p, NVSDK_NGX_Parameter_OutWidth, w);
	ngx_set_ui(p, NVSDK_NGX_Parameter_OutHeight, h);
	ngx_set_i(p, NVSDK_NGX_Parameter_PerfQualityValue, NVSDK_NGX_PerfQuality_Value_DLAA);
	ngx_set_i(p, NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, NVSDK_NGX_DLSS_Feature_Flags_MVLowRes);
	ngx_set_i(p, NVSDK_NGX_Parameter_DLSS_Enable_Output_Subrects, 0);
	ngx_set_ui(p, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA, (unsigned)presets[cfg.preset].p);
	NVSDK_NGX_Result r = ngx.CreateFeature1(g_device, cb, NVSDK_NGX_Feature_SuperSampling, p, &dl.feature);
	dlog("[dlss] DLAA feature %ux%u (format %d, preset %s): %#x\n", w, h, color_fmt, presets[cfg.preset].n, r);
	if (NVSDK_NGX_FAILED(r)) {
		dl.feature = NULL;
		free_dlss();
		return 0;
	}
	return 1;
}

static VkImageMemoryBarrier barrier(VkImage im, VkImageAspectFlags aspect, VkImageLayout from, VkImageLayout to);

// ---- compute passes ----

typedef struct pipe {
	const uint32_t *spv;
	size_t spv_size;
	uint32_t push_size;
	int nbind;
	VkDescriptorType types[8];
	VkDescriptorSetLayout set_layout;
	VkPipelineLayout layout;
	VkPipeline pipeline;
	int failed;
	const char *name;
} pipe;

static PFN_vkCmdPushDescriptorSetKHR push_set;
static VkSampler nearest_sampler, linear_sampler;

static int make_pipe(pipe *pp)
{
	if (pp->pipeline || pp->failed)
		return pp->pipeline != VK_NULL_HANDLE;
	pp->failed = 1;
	if (!push_set)
		push_set = (PFN_vkCmdPushDescriptorSetKHR)r_GetDeviceProcAddr(g_device, "vkCmdPushDescriptorSetKHR");
	if (!push_set) {
		dlog("[%s] no vkCmdPushDescriptorSetKHR\n", pp->name);
		return 0;
	}
	if (!nearest_sampler) {
		VkSamplerCreateInfo si = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
		si.magFilter = si.minFilter = VK_FILTER_NEAREST;
		si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		r_CreateSampler(g_device, &si, NULL, &nearest_sampler);
		si.magFilter = si.minFilter = VK_FILTER_LINEAR;
		r_CreateSampler(g_device, &si, NULL, &linear_sampler);
	}
	VkDescriptorSetLayoutBinding b[8];
	for (int i = 0; i < pp->nbind; i++)
		b[i] = (VkDescriptorSetLayoutBinding){ (uint32_t)i, pp->types[i], 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL };
	VkDescriptorSetLayoutCreateInfo dsl = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
	dsl.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
	dsl.bindingCount = (uint32_t)pp->nbind;
	dsl.pBindings = b;
	VkPushConstantRange pcr = { VK_SHADER_STAGE_COMPUTE_BIT, 0, pp->push_size };
	VkPipelineLayoutCreateInfo pl = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
	pl.setLayoutCount = 1;
	pl.pSetLayouts = &pp->set_layout;
	pl.pushConstantRangeCount = 1;
	pl.pPushConstantRanges = &pcr;
	VkShaderModuleCreateInfo sm = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
	sm.codeSize = pp->spv_size;
	sm.pCode = pp->spv;
	VkShaderModule mod;
	if (!nearest_sampler || !linear_sampler ||
	    r_CreateDescriptorSetLayout(g_device, &dsl, NULL, &pp->set_layout) != VK_SUCCESS ||
	    r_CreatePipelineLayout(g_device, &pl, NULL, &pp->layout) != VK_SUCCESS ||
	    r_CreateShaderModule(g_device, &sm, NULL, &mod) != VK_SUCCESS) {
		dlog("[%s] creating the pipeline objects failed\n", pp->name);
		return 0;
	}
	VkComputePipelineCreateInfo cp = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
	cp.stage = (VkPipelineShaderStageCreateInfo){ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
	cp.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
	cp.stage.module = mod;
	cp.stage.pName = "main";
	cp.layout = pp->layout;
	VkResult r = r_CreateComputePipelines(g_device, VK_NULL_HANDLE, 1, &cp, NULL, &pp->pipeline);
	r_DestroyShaderModule(g_device, mod, NULL);
	if (r != VK_SUCCESS) {
		dlog("[%s] compute pipeline: %d\n", pp->name, r);
		pp->pipeline = VK_NULL_HANDLE;
		return 0;
	}
	pp->failed = 0;
	dlog("[%s] pipeline ready\n", pp->name);
	return 1;
}

// A descriptor for binding slot i: a sampled image (with the given sampler) or a storage image.
typedef struct bind {
	VkImageView view;
	VkSampler sampler; // VK_NULL_HANDLE for storage images
} bind;

static void dispatch(VkCommandBuffer cb, pipe *pp, const bind *binds, const void *pc, uint32_t gx, uint32_t gy)
{
	VkDescriptorImageInfo ii[8];
	VkWriteDescriptorSet w[8];
	for (int i = 0; i < pp->nbind; i++) {
		ii[i] = (VkDescriptorImageInfo){ binds[i].sampler, binds[i].view, VK_IMAGE_LAYOUT_GENERAL };
		w[i] = (VkWriteDescriptorSet){ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, VK_NULL_HANDLE, (uint32_t)i, 0, 1,
					      pp->types[i], &ii[i], NULL, NULL };
	}
	r_CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pp->pipeline);
	push_set(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pp->layout, 0, (uint32_t)pp->nbind, w);
	r_CmdPushConstants(cb, pp->layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, pp->push_size, pc);
	r_CmdDispatch(cb, (gx + 7) / 8, (gy + 7) / 8, 1);
}

// Everything before is done (and visible) before anything after starts.
static void full_barrier(VkCommandBuffer cb)
{
	VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
	mb.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
	mb.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
	r_CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0,
			     NULL, 0, NULL);
}

#define SAMPLED VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
#define STORAGE VK_DESCRIPTOR_TYPE_STORAGE_IMAGE

// ---- camera motion vectors ----

typedef struct mv_push {
	float cur_to_prev[16];
	float size[2], jitter[2];
} mv_push;

static pipe mv_pipe = { spv_camera_mv, sizeof spv_camera_mv, sizeof(mv_push), 2, { SAMPLED, STORAGE }, .name = "mv" };

// Writes this frame's motion vectors into dl.mv (GENERAL): zero without a usable camera pair.
static int write_motion_vectors(VkCommandBuffer cb, VkImageView depth_view)
{
	mv_push pc;
	int ok = cfg.camera_mv && fr.have_cam && have_prev_cam && make_pipe(&mv_pipe) &&
		 cur_to_prev(fr.cam, prev_cam, pc.cur_to_prev);
	if (ok) {
		pc.size[0] = (float)dl.w;
		pc.size[1] = (float)dl.h;
		pc.jitter[0] = jitter[0];
		pc.jitter[1] = jitter[1];
		bind b[2] = { { depth_view, nearest_sampler }, { dl.mv_view, VK_NULL_HANDLE } };
		dispatch(cb, &mv_pipe, b, &pc, dl.w, dl.h);
	} else {
		VkClearColorValue zero = { { 0, 0, 0, 0 } };
		VkImageSubresourceRange all = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
		r_CmdClearColorImage(cb, dl.mv, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &all);
	}
	full_barrier(cb);
	return ok;
}

// ---- DLSS 5 Neural Rendering ----

#define NR_MAX_W 2560
#define NR_MAX_H 1440

typedef int (*nrsat_load_fn)(const wchar_t *);
typedef NVSDK_NGX_Result (*nrsat_init_fn)(unsigned long long, const wchar_t *, VkInstance, VkPhysicalDevice, VkDevice, int);
typedef NVSDK_NGX_Result (*nrsat_create_fn)(VkDevice, VkCommandBuffer, NVSDK_NGX_Parameter *, NVSDK_NGX_Handle **);
typedef NVSDK_NGX_Result (*nrsat_eval_fn)(VkCommandBuffer, const NVSDK_NGX_Handle *, const NVSDK_NGX_Parameter *);
typedef NVSDK_NGX_Result (*nrsat_release_fn)(NVSDK_NGX_Handle *);

static struct {
	int tried, usable; // satellite and runtime loaded, runtime initialised
	int on;		   // configured on and not switched off with the key
	int reset;
	nrsat_create_fn create;
	nrsat_eval_fn eval;
	nrsat_release_fn release;
	NVSDK_NGX_Parameter *params;
	NVSDK_NGX_Handle *handle;
	uint32_t w, h, factor; // network resolution, full = network * factor
	VkImage img[5];
	VkDeviceMemory mem[5];
	VkImageView view[5];
	int fresh;
} nr;
enum { NR_IN, NR_DEPTH, NR_MV, NR_OUT, NR_FULL };

typedef struct nr_prepare_push {
	int32_t size[2];
	int32_t factor;
} nr_prepare_push;

typedef struct nr_composite_push {
	float size[2];
	float strength;
} nr_composite_push;

static pipe nr_prepare_pipe = { spv_nr_prepare, sizeof spv_nr_prepare, sizeof(nr_prepare_push), 6,
				{ SAMPLED, SAMPLED, SAMPLED, STORAGE, STORAGE, STORAGE }, .name = "nr-prepare" };
static pipe nr_composite_pipe = { spv_nr_composite, sizeof spv_nr_composite, sizeof(nr_composite_push), 4,
				  { SAMPLED, SAMPLED, SAMPLED, STORAGE }, .name = "nr-composite" };

// Loads mhgudlss_nvngx.dll and through it nvngx_dlssnr.dll, and initialises the NR runtime once.
static unsigned nr_ver[4]; // the DLSS 5 runtime's version, once it is loaded

static int nr_setup(void)
{
	if (nr.tried)
		return nr.usable;
	nr.tried = 1;
	wchar_t dir[MAX_PATH], path[MAX_PATH + 32];
	DWORD n = GetModuleFileNameW(NULL, dir, MAX_PATH);
	wchar_t *slash = wcsrchr(dir, L'\\');
	if (!n || !slash)
		return 0;
	*slash = 0;
	_snwprintf(path, MAX_PATH + 32, L"%ls\\mhgudlss_nvngx.dll", dir);
	HMODULE sat = LoadLibraryW(path);
	if (!sat) {
		dlog("[nr] %ls is missing (%lu): neural rendering off\n", path, GetLastError());
		return 0;
	}
	nrsat_load_fn load = (nrsat_load_fn)GetProcAddress(sat, "nrsat_load");
	nrsat_init_fn init = (nrsat_init_fn)GetProcAddress(sat, "nrsat_init");
	nr.create = (nrsat_create_fn)GetProcAddress(sat, "nrsat_create");
	nr.eval = (nrsat_eval_fn)GetProcAddress(sat, "nrsat_eval");
	nr.release = (nrsat_release_fn)GetProcAddress(sat, "nrsat_release");
	if (!load || !init || !nr.create || !nr.eval || !nr.release)
		return 0;
	_snwprintf(path, MAX_PATH + 32, L"%ls\\nvngx_dlssnr.dll", dir);
	if (!load(path)) {
		dlog("[nr] %ls missing or without the Vulkan entry points: neural rendering off\n", path);
		return 0;
	}
	// 310.8 has one model and ignores the model hint; 310.9.16 has A (older) and B (smaller, its default).
	unsigned ver[4] = { 0 };
	DWORD vsize = GetFileVersionInfoSizeW(path, NULL);
	void *vdata = vsize ? malloc(vsize) : NULL;
	VS_FIXEDFILEINFO *fi = NULL;
	UINT fl = 0;
	if (vdata && GetFileVersionInfoW(path, 0, vsize, vdata) && VerQueryValueW(vdata, L"\\", (void **)&fi, &fl) && fi) {
		ver[0] = HIWORD(fi->dwFileVersionMS), ver[1] = LOWORD(fi->dwFileVersionMS);
		ver[2] = HIWORD(fi->dwFileVersionLS), ver[3] = LOWORD(fi->dwFileVersionLS);
	}
	free(vdata);
	NVSDK_NGX_Result r = init(0x6d686775ull, dir, g_instance, g_pd, g_device, NVSDK_NGX_Version_API);
	memcpy(nr_ver, ver, sizeof ver);
	dlog("[nr] runtime %u.%u.%u.%u, model hint %d (%s): init %#x\n", ver[0], ver[1], ver[2], ver[3], cfg.nr_model,
	     cfg.nr_model == 1 ? "A" : cfg.nr_model == 2 ? "B" : "default", r);
	if (NVSDK_NGX_FAILED(r))
		return 0;
	if (NVSDK_NGX_FAILED(ngx.AllocateParameters(&nr.params)) || !nr.params)
		return 0;
	nr.usable = 1;
	return 1;
}

static void free_nr(void)
{
	if (nr.handle)
		nr.release(nr.handle);
	nr.handle = NULL;
	for (int i = 0; i < 5; i++) {
		if (nr.view[i]) r_DestroyImageView(g_device, nr.view[i], NULL);
		if (nr.img[i]) r_DestroyImage(g_device, nr.img[i], NULL);
		if (nr.mem[i]) r_FreeMemory(g_device, nr.mem[i], NULL);
		nr.view[i] = VK_NULL_HANDLE;
		nr.img[i] = VK_NULL_HANDLE;
		nr.mem[i] = VK_NULL_HANDLE;
	}
	nr.w = nr.h = 0;
}

// The network's images and feature for a w x h picture (the caller has waited for the GPU if sizes change).
static int ensure_nr(VkCommandBuffer cb, uint32_t w, uint32_t h)
{
	uint32_t f = 1;
	while (w / f > NR_MAX_W || h / f > NR_MAX_H)
		f++;
	if (nr.handle && nr.w == w / f && nr.h == h / f && !nr_rebuild)
		return 1;
	nr_rebuild = 0;
	if (nr.w || nr.handle) {
		r_DeviceWaitIdle(g_device);
		free_nr();
	}
	nr.factor = f;
	nr.w = w / f;
	nr.h = h / f;
	const VkImageUsageFlags u = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
	if (!make_image_sized(nr.w, nr.h, VK_FORMAT_R8G8B8A8_UNORM, u, &nr.img[NR_IN], &nr.mem[NR_IN], &nr.view[NR_IN]) ||
	    !make_image_sized(nr.w, nr.h, VK_FORMAT_R32_SFLOAT, u, &nr.img[NR_DEPTH], &nr.mem[NR_DEPTH], &nr.view[NR_DEPTH]) ||
	    !make_image_sized(nr.w, nr.h, VK_FORMAT_R16G16_SFLOAT, u, &nr.img[NR_MV], &nr.mem[NR_MV], &nr.view[NR_MV]) ||
	    !make_image_sized(nr.w, nr.h, VK_FORMAT_R16G16B16A16_SFLOAT, u, &nr.img[NR_OUT], &nr.mem[NR_OUT], &nr.view[NR_OUT]) ||
	    !make_image_sized(w, h, VK_FORMAT_R8G8B8A8_UNORM, u | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, &nr.img[NR_FULL],
			      &nr.mem[NR_FULL], &nr.view[NR_FULL])) {
		dlog("[nr] creating the %ux%u images failed\n", nr.w, nr.h);
		free_nr();
		return 0;
	}
	VkImageMemoryBarrier b[5];
	for (int i = 0; i < 5; i++)
		b[i] = barrier(nr.img[i], VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
	r_CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0,
			     NULL, 5, b);

	NVSDK_NGX_Parameter *p = nr.params;
	ngx_set_ui(p, "DLSSNR.Width", nr.w);
	ngx_set_ui(p, "DLSSNR.Height", nr.h);
	ngx_set_ui(p, "DLSSNR.Enabled", 1);
	ngx_set_ui(p, "DLSSNR.Hint.Render.Preset", (unsigned)cfg.nr_model);
	ngx_set_ui(p, "DLSSNR.DepthInverted", 0);
	ngx_set_f(p, "DLSSNR.ScalingRatio", 1.0f);
	ngx_set_ui(p, "DLSSNR.UseAutoMask", 1);
	ngx_set_ui(p, "DLSSNR.Style", 0);
	ngx_set_ui(p, NVSDK_NGX_Parameter_CreationNodeMask, 1);
	ngx_set_ui(p, NVSDK_NGX_Parameter_VisibilityNodeMask, 1);
	NVSDK_NGX_Result r = nr.create(g_device, cb, p, &nr.handle);
	dlog("[nr] feature %ux%u (1/%u of %ux%u): %#x\n", nr.w, nr.h, f, w, h, r);
	if (NVSDK_NGX_FAILED(r) || !nr.handle) {
		nr.handle = NULL;
		free_nr();
		nr.usable = 0; // a creation that failed once does not succeed later
		return 0;
	}
	nr.reset = 1;
	return 1;
}

static NVSDK_NGX_Resource_VK res(VkImageView view, VkImage im, VkFormat fmt, VkImageAspectFlags aspect, int rw,
				 uint32_t w, uint32_t h);

// Runs the network on DLAA's output (dl.out) and composites the result into nr.img[NR_FULL].
static int run_nr(VkCommandBuffer cb, VkImageView depth_view, uint32_t w, uint32_t h)
{
	if (!nr_setup() || !make_pipe(&nr_prepare_pipe) || !make_pipe(&nr_composite_pipe) || !ensure_nr(cb, w, h))
		return 0;

	nr_prepare_push pp = { { (int32_t)nr.w, (int32_t)nr.h }, (int32_t)nr.factor };
	bind pb[6] = {
		{ dl.out_view, nearest_sampler }, { depth_view, nearest_sampler }, { dl.mv_view, nearest_sampler },
		{ nr.view[NR_IN], VK_NULL_HANDLE }, { nr.view[NR_DEPTH], VK_NULL_HANDLE }, { nr.view[NR_MV], VK_NULL_HANDLE },
	};
	dispatch(cb, &nr_prepare_pipe, pb, &pp, nr.w, nr.h);
	full_barrier(cb);

	NVSDK_NGX_Resource_VK in_color = res(nr.view[NR_IN], nr.img[NR_IN], VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT, 0, nr.w, nr.h);
	NVSDK_NGX_Resource_VK in_depth = res(nr.view[NR_DEPTH], nr.img[NR_DEPTH], VK_FORMAT_R32_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT, 0, nr.w, nr.h);
	NVSDK_NGX_Resource_VK in_mv = res(nr.view[NR_MV], nr.img[NR_MV], VK_FORMAT_R16G16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT, 0, nr.w, nr.h);
	NVSDK_NGX_Resource_VK out = res(nr.view[NR_OUT], nr.img[NR_OUT], VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT, 1, nr.w, nr.h);
	NVSDK_NGX_Parameter *p = nr.params;
	ngx_set_ptr(p, "DLSSNR.Color", &in_color);
	ngx_set_ptr(p, "DLSSNR.Depth", &in_depth);
	ngx_set_ptr(p, "DLSSNR.MVec", &in_mv);
	ngx_set_ptr(p, "DLSSNR.Output", &out);
	ngx_set_f(p, "DLSSNR.MVecScaleX", 1.0f);
	ngx_set_f(p, "DLSSNR.MVecScaleY", 1.0f);
	ngx_set_ui(p, "DLSSNR.Reset", nr.reset ? 1u : 0u);
	ngx_set_i(p, "DLSSNR.Reset", nr.reset);
	ngx_set_ui(p, "DLSSNR.DepthInverted", 0);
	ngx_set_ui(p, "DLSSNR.UseAutoMask", 1);
	ngx_set_ui(p, "DLSSNR.Style", 0);
	ngx_set_f(p, "DLSSNR.Intensity", cfg.nr_intensity);
	ngx_set_f(p, "DLSSNR.LocalToneStrength", cfg.nr_tone);
	ngx_set_f(p, "DLSSNR.LocalStructureStrength", cfg.nr_structure);
	ngx_set_f(p, "DLSSNR.SkinStructureStrength", cfg.nr_skin);
	NVSDK_NGX_Result r = nr.eval(cb, nr.handle, p);
	nr.reset = 0;
	if (NVSDK_NGX_FAILED(r)) {
		static int failures;
		if (failures++ < 5)
			dlog("[nr] evaluate failed: %#x\n", r);
		return 0;
	}
	full_barrier(cb);

	nr_composite_push cp = { { (float)w, (float)h }, cfg.nr_strength };
	bind cbind[4] = {
		{ dl.out_view, nearest_sampler }, { nr.view[NR_IN], linear_sampler }, { nr.view[NR_OUT], linear_sampler },
		{ nr.view[NR_FULL], VK_NULL_HANDLE },
	};
	dispatch(cb, &nr_composite_pipe, cbind, &cp, w, h);
	full_barrier(cb);
	return 1;
}

static VkImageView own_view(image_info *ii)
{
	if (ii->own_view)
		return ii->own_view;
	VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
	vi.image = ii->image;
	vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
	vi.format = ii->format;
	vi.subresourceRange = (VkImageSubresourceRange){
		is_depth_format(ii->format) ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1
	};
	if (r_CreateImageView(g_device, &vi, NULL, &ii->own_view) != VK_SUCCESS)
		ii->own_view = VK_NULL_HANDLE;
	return ii->own_view;
}

static NVSDK_NGX_Resource_VK res(VkImageView view, VkImage im, VkFormat fmt, VkImageAspectFlags aspect, int rw,
				 uint32_t w, uint32_t h)
{
	NVSDK_NGX_Resource_VK r;
	memset(&r, 0, sizeof r);
	r.Type = NVSDK_NGX_RESOURCE_VK_TYPE_VK_IMAGEVIEW;
	r.Resource.ImageViewInfo.ImageView = view;
	r.Resource.ImageViewInfo.Image = im;
	r.Resource.ImageViewInfo.SubresourceRange = (VkImageSubresourceRange){ aspect, 0, 1, 0, 1 };
	r.Resource.ImageViewInfo.Format = fmt;
	r.Resource.ImageViewInfo.Width = w;
	r.Resource.ImageViewInfo.Height = h;
	r.ReadWrite = rw != 0;
	return r;
}

static VkImageMemoryBarrier barrier(VkImage im, VkImageAspectFlags aspect, VkImageLayout from, VkImageLayout to)
{
	VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
	b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
	b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
	b.oldLayout = from;
	b.newLayout = to;
	b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	b.image = im;
	b.subresourceRange = (VkImageSubresourceRange){ aspect, 0, 1, 0, 1 };
	return b;
}

static VkImageLayout keep(VkImageLayout l)
{
	return l == VK_IMAGE_LAYOUT_UNDEFINED || l == VK_IMAGE_LAYOUT_PREINITIALIZED ? VK_IMAGE_LAYOUT_GENERAL : l;
}

// Runs DLAA on the scene colour (in place, through our output image) with the main depth.
static void run_dlss(VkCommandBuffer cb, image_info *color, image_info *depth)
{
	uint32_t w = color->extent.width, h = color->extent.height;
	if (!ensure_dlss(cb, w, h, color->format))
		return;
	VkImageView cv = own_view(color), dv = own_view(depth);
	if (!cv || !dv)
		return;
	VkImageAspectFlags da = VK_IMAGE_ASPECT_DEPTH_BIT | (has_stencil(depth->format) ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
	VkImageLayout cl = color->layout, dlay = depth->layout;

	VkImageMemoryBarrier pre[4];
	uint32_t n = 0;
	pre[n++] = barrier(color->image, VK_IMAGE_ASPECT_COLOR_BIT, cl, VK_IMAGE_LAYOUT_GENERAL);
	pre[n++] = barrier(depth->image, da, dlay, VK_IMAGE_LAYOUT_GENERAL);
	pre[n++] = barrier(dl.out, VK_IMAGE_ASPECT_COLOR_BIT, dl.fresh ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_GENERAL,
			   VK_IMAGE_LAYOUT_GENERAL);
	if (dl.fresh)
		pre[n++] = barrier(dl.mv, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
	r_CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0,
			     NULL, n, pre);
	dl.fresh = 0;
	int moving = write_motion_vectors(cb, dv);
	static int mv_logged = -1;
	if (moving != mv_logged) {
		dlog("[mv] frame %lld: motion vectors %s\n", frame_no, moving ? "from the camera" : "zero (no camera pair)");
		mv_logged = moving;
	}

	NVSDK_NGX_Resource_VK in_color = res(cv, color->image, color->format, VK_IMAGE_ASPECT_COLOR_BIT, 0, w, h);
	NVSDK_NGX_Resource_VK in_depth = res(dv, depth->image, depth->format, VK_IMAGE_ASPECT_DEPTH_BIT, 0, w, h);
	NVSDK_NGX_Resource_VK in_mv = res(dl.mv_view, dl.mv, VK_FORMAT_R16G16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT, 0, w, h);
	NVSDK_NGX_Resource_VK out = res(dl.out_view, dl.out, color->format, VK_IMAGE_ASPECT_COLOR_BIT, 1, w, h);
	NVSDK_NGX_Parameter *p = ngx_params;
	ngx_set_ptr(p, NVSDK_NGX_Parameter_Color, &in_color);
	ngx_set_ptr(p, NVSDK_NGX_Parameter_Output, &out);
	ngx_set_ptr(p, NVSDK_NGX_Parameter_Depth, &in_depth);
	ngx_set_ptr(p, NVSDK_NGX_Parameter_MotionVectors, &in_mv);
	ngx_set_f(p, NVSDK_NGX_Parameter_Jitter_Offset_X, jitter[0] * cfg.jitter_sign[0]);
	ngx_set_f(p, NVSDK_NGX_Parameter_Jitter_Offset_Y, jitter[1] * cfg.jitter_sign[1]);
	ngx_set_f(p, NVSDK_NGX_Parameter_Sharpness, 0);
	ngx_set_i(p, NVSDK_NGX_Parameter_Reset, dlss_reset);
	ngx_set_f(p, NVSDK_NGX_Parameter_MV_Scale_X, 1);
	ngx_set_f(p, NVSDK_NGX_Parameter_MV_Scale_Y, 1);
	ngx_set_ui(p, NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, w);
	ngx_set_ui(p, NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, h);
	ngx_set_f(p, NVSDK_NGX_Parameter_DLSS_Pre_Exposure, 1);
	ngx_set_f(p, NVSDK_NGX_Parameter_DLSS_Exposure_Scale, 1);
	NVSDK_NGX_Result r = ngx.EvaluateFeature(cb, dl.feature, p, NULL);
	if (NVSDK_NGX_FAILED(r)) {
		static int failures;
		if (failures++ < 5)
			dlog("[dlss] evaluate failed: %#x\n", r);
	}
	dlss_reset = 0;
	have_prev_cam = fr.have_cam;
	if (fr.have_cam)
		memcpy(prev_cam, fr.cam, sizeof prev_cam);

	VkImageMemoryBarrier mid = barrier(dl.out, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
	r_CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0,
			     NULL, 1, &mid);
	if (NVSDK_NGX_SUCCEED(r)) {
		int neural = nr.on && color->format == VK_FORMAT_R8G8B8A8_UNORM && run_nr(cb, dv, w, h);
		static int nr_logged = -1;
		if (nr.on && neural != nr_logged) {
			dlog("[nr] frame %lld: neural rendering %s\n", frame_no, neural ? "running" : "not running");
			nr_logged = neural;
		}
		VkImageCopy copy = { { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, { 0, 0, 0 }, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
				     { 0, 0, 0 }, { w, h, 1 } };
		r_CmdCopyImage(cb, neural ? nr.img[NR_FULL] : dl.out, VK_IMAGE_LAYOUT_GENERAL, color->image,
			       VK_IMAGE_LAYOUT_GENERAL, 1, &copy);
	}
	VkImageMemoryBarrier post[2] = {
		barrier(color->image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL, keep(cl)),
		barrier(depth->image, da, VK_IMAGE_LAYOUT_GENERAL, keep(dlay)),
	};
	r_CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0,
			     NULL, 2, post);
	color->layout = keep(cl);
	depth->layout = keep(dlay);
}

// ---- command hooks ----

static void set_viewport(VkCommandBuffer cb, cb_state *s)
{
	VkViewport v = s->vp;
	if (s->scene) {
		v.x += jitter[0];
		v.y += jitter[1];
	}
	r_CmdSetViewport(cb, 0, 1, &v);
}

VKAPI_ATTR void VKAPI_CALL vkCmdSetViewport(VkCommandBuffer cb, uint32_t first, uint32_t n, const VkViewport *vps)
{
	cb_state *s = cb_of(cb);
	if (first == 0 && n >= 1) {
		s->vp = vps[0];
		s->have_vp = 1;
		if (s->scene && n == 1) {
			set_viewport(cb, s);
			return;
		}
	}
	r_CmdSetViewport(cb, first, n, vps);
}

static void draw_hook(VkCommandBuffer cb)
{
	cb_state *s = cb_of(cb);
	// Tried at every draw of the pass until one uses the camera.
	if (s->want_cam && !fr.have_cam && read_camera(fr.cam)) {
		fr.have_cam = 1;
		s->want_cam = 0;
		static int logged;
		if (!logged++)
			dlog("[cam] frame %lld: camera [%.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f]\n",
			     frame_no, fr.cam[0], fr.cam[1], fr.cam[2], fr.cam[3], fr.cam[4], fr.cam[5], fr.cam[6], fr.cam[7],
			     fr.cam[8], fr.cam[9], fr.cam[10], fr.cam[11], fr.cam[12], fr.cam[13], fr.cam[14], fr.cam[15]);
	}
}

VKAPI_ATTR void VKAPI_CALL vkCmdDraw(VkCommandBuffer cb, uint32_t nv, uint32_t ni, uint32_t fv, uint32_t fi)
{
	draw_hook(cb);
	r_CmdDraw(cb, nv, ni, fv, fi);
}

VKAPI_ATTR void VKAPI_CALL vkCmdDrawIndexed(VkCommandBuffer cb, uint32_t n, uint32_t ni, uint32_t fi, int32_t vo,
					     uint32_t fin)
{
	draw_hook(cb);
	r_CmdDrawIndexed(cb, n, ni, fi, vo, fin);
}

VKAPI_ATTR void VKAPI_CALL vkCmdBeginRenderPass(VkCommandBuffer cb, const VkRenderPassBeginInfo *info,
						 VkSubpassContents contents)
{
	AcquireSRWLockShared(&map_lock);
	fb_info *fi = map_get(&fbs, H(info->framebuffer));
	rp_info *ri = map_get(&rps, H(info->renderPass));
	ReleaseSRWLockShared(&map_lock);

	image_info *color = NULL, *depth = NULL;
	int ncolor = 0;
	if (fi && ri && fi->n == ri->n) {
		for (uint32_t i = 0; i < fi->n; i++) {
			image_info *ii = image_of(fi->images[i]);
			if (!ii)
				continue;
			if (is_depth_format(ri->formats[i]))
				depth = ii;
			else if (!ncolor++)
				color = ii;
		}
	}
	fr.passes++;

	if (dlss_on && color && color->samples == VK_SAMPLE_COUNT_1_BIT) {
		uint32_t w = color->extent.width, h = color->extent.height;
		if (!fr.main_depth) {
			if (depth && depth->extent.width == w && depth->extent.height == h && w >= 640)
				fr.main_depth = depth;

		} else if (!fr.done && !depth) {
			image_info *md = fr.main_depth;
			if (w == md->extent.width && h == md->extent.height) {
				fr.scene_color = color;
			} else if (w < md->extent.width && fr.scene_color) {
				run_dlss(cb, fr.scene_color, md);
				fr.done = fr.passes;
			}
		}
	}

	cb_state *s = cb_of(cb);
	int scene = dlss_on && depth && depth->image == jitter_depth;
	// The backend binds a draw's constant buffers just before the draw, so the camera is read there.
	s->want_cam = dlss_on && cfg.camera_mv && depth && fr.main_depth && depth == fr.main_depth && !fr.have_cam;
	int was = s->scene;
	s->scene = scene;

	r_CmdBeginRenderPass(cb, info, contents);

	if (s->have_vp && scene != was)
		set_viewport(cb, s);
	// The attachments end the pass in their final layouts.
	if (fi && ri && fi->n == ri->n)
		for (uint32_t i = 0; i < fi->n; i++) {
			image_info *ii = image_of(fi->images[i]);
			if (ii)
				ii->layout = ri->final_layouts[i];
		}
}

VKAPI_ATTR VkResult VKAPI_CALL vkQueuePresentKHR(VkQueue queue, const VkPresentInfoKHR *info)
{
	LONG64 f = InterlockedIncrement64(&frame_no);

	if (ngx_ok && cfg.mode != MODE_OFF && cfg.toggle_key && (GetAsyncKeyState(cfg.toggle_key) & 1)) {
		DWORD pid = 0;
		GetWindowThreadProcessId(GetForegroundWindow(), &pid);
		if (pid == GetCurrentProcessId()) {
			dlss_on = !dlss_on;
			dlss_reset = 1;
			set_game_fxaa(dlss_on);
			dlog("[dlss] frame %lld: switched %s\n", f, dlss_on ? "on" : "off (game FXAA as configured)");
		}
	}

	if (ngx_ok && dlss_on && cfg.nr && cfg.nr_key && (GetAsyncKeyState(cfg.nr_key) & 1)) {
		DWORD pid = 0;
		GetWindowThreadProcessId(GetForegroundWindow(), &pid);
		if (pid == GetCurrentProcessId()) {
			nr.on = !nr.on;
			nr.reset = 1;
			dlog("[nr] frame %lld: switched %s\n", f, nr.on ? "on" : "off");
		}
	}

	static int was_done = -1;
	if (dlss_on && (logged_frames < 3 || f % 3600 == 0 || (fr.done != 0) != was_done)) {
		logged_frames++;
		if (fr.done)
			dlog("[dlss] frame %lld: %d passes, DLAA on %ux%u before pass %d, camera %s"
			     " [%.3f %.3f %.3f %.3f / %.3f %.3f %.3f %.3f]\n", f, fr.passes, fr.main_depth->extent.width,
			     fr.main_depth->extent.height, fr.done, fr.have_cam ? "found" : "not found", fr.cam[0], fr.cam[1],
			     fr.cam[2], fr.cam[3], fr.cam[12], fr.cam[13], fr.cam[14], fr.cam[15]);
		else
			dlog("[dlss] frame %lld: %d passes, no DLAA point (main depth %s, scene colour %s)\n", f,
			     fr.passes, fr.main_depth ? "found" : "not found", fr.scene_color ? "found" : "not found");
	}
	was_done = fr.done != 0;
	// Jitter only frames whose scene DLAA resolves.
	jitter_depth = fr.done ? fr.main_depth->image : VK_NULL_HANDLE;
	memset(&fr, 0, sizeof fr);

	// Next frame's jitter: Halton(2,3), 32 phases.
	int i = (int)(f % 32) + 1;
	jitter[0] = halton(i, 2) - 0.5f;
	jitter[1] = halton(i, 3) - 0.5f;
	return r_QueuePresentKHR(queue, info);
}

// ---- creation: add NGX's extensions, initialise NGX ----

static int has_ext(const char *name, const char *const *list, uint32_t n)
{
	for (uint32_t i = 0; i < n; i++)
		if (!strcmp(list[i], name))
			return 1;
	return 0;
}

static unsigned ngx_n_inst, ngx_n_dev;
static const char **ngx_inst_exts, **ngx_dev_exts;

VKAPI_ATTR VkResult VKAPI_CALL vkCreateInstance(const VkInstanceCreateInfo *ci, const VkAllocationCallbacks *a,
						VkInstance *out)
{
	uint32_t api = ci->pApplicationInfo ? ci->pApplicationInfo->apiVersion : 0;
	dlog("vkCreateInstance: api %u.%u, %u extension(s)\n", VK_API_VERSION_MAJOR(api), VK_API_VERSION_MINOR(api),
	     ci->enabledExtensionCount);

	VkInstanceCreateInfo c = *ci;
	const char *exts[64];
	uint32_t n = 0;
	for (uint32_t i = 0; i < ci->enabledExtensionCount && n < 64; i++)
		exts[n++] = ci->ppEnabledExtensionNames[i];

	if (!ngx_tried) { // with Mode Off too, so the Forge menu can switch DLAA on while playing
		ngx_tried = 1;
		if (ngx_load(dlog)) {
			NVSDK_NGX_Result r = ngx.RequiredExtensions(&ngx_n_inst, &ngx_inst_exts, &ngx_n_dev, &ngx_dev_exts);
			dlog("[ngx] required extensions: %#x, %u instance, %u device\n", r, ngx_n_inst, ngx_n_dev);
			if (NVSDK_NGX_FAILED(r))
				ngx_n_inst = ngx_n_dev = 0;
		}
	}
	if (ngx_n_inst) {
		uint32_t avail_n = 0;
		r_EnumerateInstanceExtensionProperties(NULL, &avail_n, NULL);
		VkExtensionProperties *avail = calloc(avail_n, sizeof *avail);
		r_EnumerateInstanceExtensionProperties(NULL, &avail_n, avail);
		for (unsigned i = 0; i < ngx_n_inst && n < 64; i++) {
			int ok = 0;
			for (uint32_t k = 0; k < avail_n; k++)
				ok |= !strcmp(avail[k].extensionName, ngx_inst_exts[i]);
			if (ok && !has_ext(ngx_inst_exts[i], exts, n))
				exts[n++] = ngx_inst_exts[i];
			dlog("  + %s%s\n", ngx_inst_exts[i], ok ? "" : " (not available)");
		}
		free(avail);
	}
	c.enabledExtensionCount = n;
	c.ppEnabledExtensionNames = exts;
	VkResult r = r_CreateInstance(&c, a, out);
	if (r != VK_SUCCESS && n != ci->enabledExtensionCount) {
		dlog("  -> %d with NGX's extensions; again without them\n", r);
		r = r_CreateInstance(ci, a, out);
		ngx_n_dev = 0;
	}
	if (r == VK_SUCCESS)
		g_instance = *out;
	dlog("  -> %d\n", r);
	return r;
}

static void init_ngx(void)
{
	wchar_t dir[MAX_PATH];
	DWORD n = GetModuleFileNameW(NULL, dir, MAX_PATH);
	wchar_t *slash = wcsrchr(dir, L'\\');
	if (!n || !slash)
		return;
	*slash = 0;
	NVSDK_NGX_Result r = ngx.Init_ProjectID_Ext("6c1b1a8e-3f0d-4d8e-9b51-4d48475532a1", NVSDK_NGX_ENGINE_TYPE_CUSTOM,
						    "1.0", dir, g_instance, g_pd, g_device, r_GetInstanceProcAddr,
						    r_GetDeviceProcAddr, NVSDK_NGX_Version_API, NULL);
	dlog("[ngx] init: %#x\n", r);
	if (NVSDK_NGX_FAILED(r))
		return;
	NVSDK_NGX_Parameter *caps = NULL;
	r = ngx.GetCapabilityParameters(&caps);
	int avail = 0, needs_driver = 0;
	if (NVSDK_NGX_SUCCEED(r) && caps) {
		ngx_get_i(caps, NVSDK_NGX_Parameter_SuperSampling_Available, &avail);
		ngx_get_i(caps, NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver, &needs_driver);
	}
	dlog("[ngx] capabilities %#x: DLSS available %d, needs a newer driver %d\n", r, avail, needs_driver);
	if (!avail)
		return;
	if (NVSDK_NGX_FAILED(ngx.AllocateParameters(&ngx_params)) || !ngx_params) {
		dlog("[ngx] AllocateParameters failed\n");
		return;
	}
	ngx_ok = 1;
	nr.on = cfg.nr;
	if (fxaa_flag)
		fxaa_user = *fxaa_flag; // mhgurecomp.ini has been read by now
	dlss_on = cfg.mode != MODE_OFF;
	set_game_fxaa(dlss_on);
	dlog("[dlss] ready: DLAA %s, %s switches it while playing\n", dlss_on ? "on" : "off",
	     cfg.toggle_key ? "the toggle key" : "no key");
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateDevice(VkPhysicalDevice pd, const VkDeviceCreateInfo *ci,
					      const VkAllocationCallbacks *a, VkDevice *out)
{
	VkPhysicalDeviceProperties props;
	r_GetPhysicalDeviceProperties(pd, &props);
	dlog("vkCreateDevice on %s: %u extension(s)\n", props.deviceName, ci->enabledExtensionCount);

	VkDeviceCreateInfo c = *ci;
	const char *exts[64];
	uint32_t n = 0;
	for (uint32_t i = 0; i < ci->enabledExtensionCount && n < 64; i++)
		exts[n++] = ci->ppEnabledExtensionNames[i];
	int want_bda = 0;
	if (ngx_n_dev && props.vendorID == 0x10de) {
		uint32_t avail_n = 0;
		r_EnumerateDeviceExtensionProperties(pd, NULL, &avail_n, NULL);
		VkExtensionProperties *avail = calloc(avail_n, sizeof *avail);
		r_EnumerateDeviceExtensionProperties(pd, NULL, &avail_n, avail);
		for (unsigned i = 0; i < ngx_n_dev && n < 64; i++) {
			const char *e = ngx_dev_exts[i];
			int ok = 0;
			for (uint32_t k = 0; k < avail_n; k++)
				ok |= !strcmp(avail[k].extensionName, e);
			// Core in Vulkan 1.2, and the EXT version must not be enabled together with the feature.
			if (!strcmp(e, VK_EXT_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME) ||
			    !strcmp(e, VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME)) {
				want_bda = 1;
				ok = 0;
			}
			if (ok && !has_ext(e, exts, n))
				exts[n++] = e;
			dlog("  + %s%s\n", e, ok ? "" : " (skipped)");
		}
		free(avail);
	}
	c.enabledExtensionCount = n;
	c.ppEnabledExtensionNames = exts;
	if (want_bda)
		for (VkBaseOutStructure *s = (VkBaseOutStructure *)ci->pNext; s; s = s->pNext)
			if (s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES) {
				((VkPhysicalDeviceVulkan12Features *)s)->bufferDeviceAddress = VK_TRUE;
				dlog("  + bufferDeviceAddress (Vulkan 1.2 feature)\n");
			}

	VkResult r = r_CreateDevice(pd, &c, a, out);
	dlog("  -> %d\n", r);
	if (r == VK_SUCCESS && ngx_n_dev && props.vendorID == 0x10de) {
		g_pd = pd;
		g_device = *out;
		init_ngx();
	}
	return r;
}

// ---- proc addresses: hand out our hooks ----

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance inst, const char *name);
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice dev, const char *name);

static PFN_vkVoidFunction own_hook(const char *name)
{
#define HOOK(n) { "vk" #n, (PFN_vkVoidFunction)vk##n },
	static const struct { const char *name; PFN_vkVoidFunction fn; } hooks[] = {
		HOOK(GetInstanceProcAddr) HOOK(GetDeviceProcAddr) HOOK(CreateInstance) HOOK(CreateDevice)
		HOOK(QueuePresentKHR) HOOK(CmdBeginRenderPass) HOOK(CmdSetViewport) HOOK(CmdPipelineBarrier)
		HOOK(CmdDraw) HOOK(CmdDrawIndexed)
		HOOK(CreateImage) HOOK(DestroyImage) HOOK(CreateImageView) HOOK(DestroyImageView)
		HOOK(CreateFramebuffer) HOOK(DestroyFramebuffer) HOOK(CreateRenderPass) HOOK(DestroyRenderPass)
	};
#undef HOOK
	for (size_t i = 0; i < sizeof hooks / sizeof *hooks; i++)
		if (!strcmp(hooks[i].name, name))
			return hooks[i].fn;
	return NULL;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance inst, const char *name)
{
	if (!name)
		return NULL;
	PFN_vkVoidFunction own = own_hook(name);
	PFN_vkVoidFunction r = r_GetInstanceProcAddr(inst, name);
	return own && r ? own : r;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice dev, const char *name)
{
	if (!name)
		return NULL;
	PFN_vkVoidFunction own = own_hook(name);
	PFN_vkVoidFunction r = r_GetDeviceProcAddr(dev, name);
	return own && r ? own : r;
}

// ---- Forge menu: a "DLSS" entry under the overlay's "Plugin UI", after the plugins' own ----
// mhgurecomp.exe exports its ImGui (cimgui names) and forge_imgui_run, which draws the plugins' entries. The
// overlay calls it once inside "Plugin UI" (edx = 1, the menu; edx = 2 is the plugins' free drawing); that one
// call is pointed at a stub near the exe that runs it and then this menu.

typedef _Bool (*ig_tree_fn)(const char *label);
typedef void (*ig_void_fn)(void);
typedef _Bool (*ig_bool_fn)(void);
typedef _Bool (*ig_check_fn)(const char *label, _Bool *v);
typedef _Bool (*ig_combo_fn)(const char *label, int *cur, const char *const items[], int n, int max_height);
typedef _Bool (*ig_slider_fn)(const char *label, float *v, float lo, float hi, const char *fmt, int flags);
typedef void (*ig_text_fn)(const char *text, const char *end);
typedef void (*forge_run_fn)(void *arg, int kind);

static struct {
	ig_tree_fn tree;
	ig_void_fn pop, separator;
	ig_bool_fn edited;
	ig_check_fn check;
	ig_combo_fn combo;
	ig_slider_fn slider;
	ig_text_fn text;
} ig;
static forge_run_fn forge_run;
static const char *preset_names[N_PRESETS], *nr_preset_names[N_NR_PRESETS];

static void menu_text(const char *s)
{
	ig.text(s, NULL);
}

static void draw_menu(void)
{
	if (!ig.tree("DLSS###mhgu-dlss"))
		return;
	char line[200];
	int changed = 0;
	if (!ngx_ok)
		menu_text("DLSS isn't available: it needs an NVIDIA RTX card, a current driver and nvngx_dlss.dll next to "
			  "mhgurecomp.exe (mhgu-dlss.log says what failed).");

	_Bool b = cfg.mode != MODE_OFF;
	if (ig.check("DLAA (DLSS anti-aliasing; the game's FXAA goes off)", &b)) {
		cfg.mode = b ? MODE_DLAA : MODE_OFF;
		dlss_on = b && ngx_ok;
		dlss_reset = 1;
		set_game_fxaa(dlss_on);
		changed = 1;
	}
	int i = cfg.preset;
	if (ig.combo("DLAA model###dlaa-model", &i, preset_names, N_PRESETS, -1) && i != cfg.preset) {
		cfg.preset = i;
		dl_rebuild = 1;
		dlss_reset = 1;
		changed = 1;
	}
	b = cfg.camera_mv != 0;
	if (ig.check("Camera motion vectors (clean scenery and fog in motion)", &b)) {
		cfg.camera_mv = b;
		changed = 1;
	}

	if (ig.separator)
		ig.separator();
	b = cfg.nr != 0;
	if (ig.check("DLSS 5 neural rendering (runs after DLAA)", &b)) {
		cfg.nr = b;
		nr.on = b;
		nr.reset = 1;
		changed = 1;
	}
	i = cfg.nr_model;
	if (ig.combo("Neural rendering model###nr-model", &i, nr_models, 3, -1) && i != cfg.nr_model) {
		cfg.nr_model = i;
		nr_rebuild = 1;
		nr.reset = 1;
		changed = 1;
	}
	if (nr_ver[0])
		snprintf(line, sizeof line, "Runtime %u.%u.%u.%u%s", nr_ver[0], nr_ver[1], nr_ver[2], nr_ver[3],
			 nr_ver[0] == 310 && nr_ver[1] == 8 ? ": it has one model and ignores this" :
			 ": Default is B, the newer and faster one; A is the previous");
	else
		snprintf(line, sizeof line, "Runtime not loaded yet (it loads the first time neural rendering runs)");
	menu_text(line);
	i = cfg.nr_preset;
	if (ig.combo("Neural rendering preset###nr-preset", &i, nr_preset_names, N_NR_PRESETS, -1) && i != cfg.nr_preset) {
		set_nr_preset(i);
		changed = 1;
	}
	ig.slider("Neural rendering strength###nr-strength", &cfg.nr_strength, 0.0f, 2.0f, "%.2f", 0);
	if (ig.edited())
		changed = 1;

	snprintf(line, sizeof line, "Now: DLAA %s, neural rendering %s",
		 dlss_on ? "on" : "off", dlss_on && nr.on ? (nr.usable || !nr_ver[0] ? "on" : "failed (see log)") : "off");
	menu_text(line);
	if (changed)
		save_config();
	ig.pop();
}

static void forge_run_then_menu(void *arg, int kind)
{
	forge_run(arg, kind);
	draw_menu();
}

static void hook_forge_menu(void)
{
	HMODULE exe = GetModuleHandleW(NULL);
#define IG(field, type, name) ig.field = (type)(void *)GetProcAddress(exe, name);
	IG(tree, ig_tree_fn, "igTreeNode_Str") IG(pop, ig_void_fn, "igTreePop") IG(separator, ig_void_fn, "igSeparator")
	IG(edited, ig_bool_fn, "igIsItemDeactivatedAfterEdit") IG(check, ig_check_fn, "igCheckbox")
	IG(combo, ig_combo_fn, "igCombo_Str_arr") IG(slider, ig_slider_fn, "igSliderFloat")
	IG(text, ig_text_fn, "igTextUnformatted")
#undef IG
	forge_run = (forge_run_fn)(void *)GetProcAddress(exe, "forge_imgui_run");
	if (!ig.tree || !ig.pop || !ig.edited || !ig.check || !ig.combo || !ig.slider || !ig.text || !forge_run) {
		dlog("[menu] mhgurecomp.exe doesn't export the ImGui/Forge functions: no DLSS entry in the Forge menu\n");
		return;
	}
	for (int k = 0; k < N_PRESETS; k++)
		preset_names[k] = presets[k].n;
	for (int k = 0; k < N_NR_PRESETS; k++)
		nr_preset_names[k] = nr_presets[k].n;

	// The call to forge_imgui_run with edx = 1: ba 01 00 00 00, e8 rel32.
	unsigned char *base = (unsigned char *)exe, *site = NULL;
	IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
	IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
	int found = 0;
	for (int k = 0; k < nt->FileHeader.NumberOfSections; k++, sec++) {
		if (!(sec->Characteristics & IMAGE_SCN_MEM_EXECUTE))
			continue;
		unsigned char *p = base + sec->VirtualAddress, *end = p + sec->Misc.VirtualSize - 5;
		for (p += 5; p < end; p++)
			if (p[0] == 0xe8 && p + 5 + *(int32_t *)(p + 1) == (unsigned char *)(void *)forge_run &&
			    !memcmp(p - 5, "\xba\x01\x00\x00\x00", 5)) {
				site = p;
				found++;
			}
	}
	if (found != 1) {
		dlog("[menu] %d places call the Forge menu (1 expected): no DLSS entry in the Forge menu\n", found);
		return;
	}
	// A stub within 2 GB of the call: mov rax, forge_run_then_menu; jmp rax.
	unsigned char *stub = NULL;
	for (uintptr_t a = ((uintptr_t)base & ~(uintptr_t)0xffff) - 0x10000; !stub && a > (uintptr_t)base - 0x7f000000u;
	     a -= 0x10000)
		stub = VirtualAlloc((void *)a, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
	if (!stub) {
		dlog("[menu] no memory near mhgurecomp.exe for the menu hook\n");
		return;
	}
	void *target = (void *)forge_run_then_menu;
	stub[0] = 0x48, stub[1] = 0xb8;
	memcpy(stub + 2, &target, 8);
	stub[10] = 0xff, stub[11] = 0xe0;
	DWORD old;
	if (!VirtualProtect(site + 1, 4, PAGE_EXECUTE_READWRITE, &old)) {
		dlog("[menu] couldn't patch the Forge menu call\n");
		return;
	}
	int32_t rel = (int32_t)(stub - (site + 5));
	memcpy(site + 1, &rel, 4);
	VirtualProtect(site + 1, 4, old, &old);
	FlushInstructionCache(GetCurrentProcess(), site, 5);
	dlog("[menu] DLSS entry in the Forge menu (call at exe+%#llx)\n", (unsigned long long)(site - base));
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
	(void)inst;
	(void)reserved;
	if (reason == DLL_PROCESS_ATTACH) {
		InitializeCriticalSection(&log_lock);
		log_file = fopen("mhgu-dlss.log", "w");
		dlog("mhgu-dlss stage 2 (DLAA)\n");
		if (!load_real()) {
			dlog("could not load the system vulkan-1.dll\n");
			return FALSE;
		}
		load_config();
		find_fxaa_flag();
		hook_forge_menu();
	}
	return TRUE;
}
