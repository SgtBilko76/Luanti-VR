// Luanti
// SPDX-License-Identifier: LGPL-2.1-or-later

#include "vr.h"

#if USE_OPENXR

#include "constants.h"
#include "log.h"
#include "settings.h"
#include "util/numeric.h"
#include <IrrlichtDevice.h>
#include <IEventReceiver.h>
#include <Keycodes.h>
#include "client/keycode.h"
#include "client/fontengine.h"
#include "vr_keyboard.h"
#include <IGUIEnvironment.h>
#include <IGUIElement.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <jni.h>
#define SDL_MAIN_HANDLED 1
#include <SDL.h>

#define XR_USE_PLATFORM_ANDROID
#define XR_USE_GRAPHICS_API_OPENGL_ES
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

VrSystem *g_vr = nullptr;

/* Math */

VrQuat VrQuat::fromAxisAngle(v3f axis, f32 radians)
{
	axis.normalize();
	f32 s = std::sin(radians * 0.5f);
	return {axis.X * s, axis.Y * s, axis.Z * s, std::cos(radians * 0.5f)};
}

VrQuat VrQuat::operator*(const VrQuat &o) const
{
	return {
		w * o.x + x * o.w + y * o.z - z * o.y,
		w * o.y - x * o.z + y * o.w + z * o.x,
		w * o.z + x * o.y - y * o.x + z * o.w,
		w * o.w - x * o.x - y * o.y - z * o.z,
	};
}

v3f VrQuat::rotate(v3f v) const
{
	// v' = v + 2w(q x v) + 2(q x (q x v))
	v3f q(x, y, z);
	v3f t = q.crossProduct(v) * 2.0f;
	return v + t * w + q.crossProduct(t);
}

static VrQuat conjugate(const VrQuat &q)
{
	return {-q.x, -q.y, -q.z, q.w};
}

// OpenXR is right-handed with -Z forward, Irrlicht is left-handed with +Z
// forward. Mirroring the Z axis converts between them.
static v3f fromXr(const XrVector3f &v)
{
	return v3f(v.x, v.y, -v.z);
}

static VrQuat fromXr(const XrQuaternionf &q)
{
	return {-q.x, -q.y, q.z, q.w};
}

static VrPose fromXr(const XrPosef &p, bool valid)
{
	VrPose ret;
	ret.position = fromXr(p.position);
	ret.orientation = fromXr(p.orientation);
	ret.valid = valid;
	return ret;
}

static XrPosef toXr(const VrPose &p)
{
	XrPosef ret;
	ret.position = {p.position.X, p.position.Y, -p.position.Z};
	ret.orientation = {-p.orientation.x, -p.orientation.y, p.orientation.z, p.orientation.w};
	return ret;
}

/* Helpers */

#define XR_CHECK(call) xrCheck((call), #call)

static XrInstance s_log_instance = XR_NULL_HANDLE;

static bool xrCheck(XrResult res, const char *what)
{
	if (XR_SUCCEEDED(res))
		return true;
	char buf[XR_MAX_RESULT_STRING_SIZE] = "?";
	if (s_log_instance != XR_NULL_HANDLE)
		xrResultToString(s_log_instance, res, buf);
	errorstream << "VR: " << what << " failed: " << buf << " (" << res << ")" << std::endl;
	return false;
}

namespace {

struct Swapchain
{
	XrSwapchain handle = XR_NULL_HANDLE;
	core::dimension2du size;
	std::vector<GLuint> fbos;
	GLuint depth = 0;
	u32 acquired = 0;
	bool is_acquired = false;
};

// Remembers the GL framebuffer binding and restores it on destruction,
// so that Irrlicht's state cache stays valid.
struct FramebufferGuard
{
	GLint prev = 0;
	FramebufferGuard() { glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev); }
	~FramebufferGuard() { glBindFramebuffer(GL_FRAMEBUFFER, prev); }
};

enum Hand { LEFT = 0, RIGHT = 1 };

class OpenXrSystem final : public VrSystem
{
public:
	OpenXrSystem(IrrlichtDevice *device) : m_device(device),
		m_driver(device->getVideoDriver())
	{}

	~OpenXrSystem() override;

	bool init();

	void onBeginScene() override;
	bool onEndScene() override;

	void update() override;

	bool isSessionRunning() const override { return m_running; }
	bool shouldQuit() const override { return m_should_quit; }

	bool isRenderingEyes() const override { return m_frame_active && m_should_render; }
	const VrEyeView &getEyeView(u32 eye) const override { return m_eye_views[eye]; }
	bool beginEye(u32 eye) override;
	void endEye(u32 eye) override;
	void beginUi() override;

	void setUiMode(VrUiMode mode) override;
	VrUiMode getUiMode() const override { return m_ui_mode; }

	v2f getCombinedFov() const override;

	VrPose getHeadPose() const override { return m_head; }
	VrPose getAimPose() const override { return m_aim[RIGHT]; }

	void setTextInputRequested(bool requested) override { m_text_input_requested = requested; }

	v3f toWorldOffset(v3f tracking_pos) const override;
	VrQuat toWorldOrientation(const VrQuat &tracking_rot) const override;
	v2f getLookYawPitch() const override;

private:
	bool createInstance();
	bool createSession();
	bool createActions();
	bool createSwapchain(Swapchain &sc, core::dimension2du size, bool with_depth);
	void destroySwapchain(Swapchain &sc);
	bool acquire(Swapchain &sc);
	void release(Swapchain &sc);

	void pollEvents();
	void handleSessionState(XrSessionState state);
	void locate(XrTime time);
	void recenter(bool full);
	void placeMenuPanel();

	void processInput();
	void processHudInput();
	void processMenuInput();
	void releaseAllInput();
	void updatePointer();
	void drawPointer();
	bool hitQuad(const XrPosef &pose, v2f size, v2f &uv, f32 &dist) const;
	void haptic(f32 amplitude, f32 seconds);

	void updateKeyboard();
	void placeKeyboard();
	void drawKeyboard();
	void pressKey(int index);

	void postGamepadButton(GamepadButton button, bool down);
	void postGamepadAxis(GamepadAxis axis, f32 value);
	void postMouse(EMOUSE_INPUT_EVENT type, s32 wheel = 0);
	void postEscape();

	IrrlichtDevice *m_device;
	video::IVideoDriver *m_driver;

	XrInstance m_instance = XR_NULL_HANDLE;
	XrSystemId m_system = XR_NULL_SYSTEM_ID;
	XrSession m_session = XR_NULL_HANDLE;
	XrSpace m_app_space = XR_NULL_HANDLE;
	XrSpace m_view_space = XR_NULL_HANDLE;
	XrSessionState m_state = XR_SESSION_STATE_UNKNOWN;
	bool m_running = false;
	bool m_should_quit = false;

	int64_t m_color_format = 0;
	std::array<Swapchain, 2> m_eye_sc;
	Swapchain m_ui_sc;
	GLuint m_dummy_fbo = 0, m_dummy_tex = 0, m_dummy_depth = 0;
	u32 m_msaa = 0;
	PFNGLFRAMEBUFFERTEXTURE2DMULTISAMPLEEXTPROC m_fbTex2DMs = nullptr;
	PFNGLRENDERBUFFERSTORAGEMULTISAMPLEEXTPROC m_rbStorageMs = nullptr;

	// Frame state
	bool m_frame_active = false;
	bool m_should_render = false;
	XrTime m_display_time = 0;
	u8 m_eyes_done = 0;
	std::array<XrView, 2> m_xr_views;
	std::array<VrEyeView, 2> m_eye_views;

	// Tracking (Irrlicht coordinates, meters)
	VrPose m_head;
	std::array<VrPose, 2> m_aim;
	std::array<XrPosef, 2> m_aim_xr;
	XrPosef m_head_xr;
	bool m_centered = false;
	v3f m_center;
	f32 m_turn_deg = 0.0f;

	// UI panel
	VrUiMode m_ui_mode = VrUiMode::Menu;
	bool m_menu_panel_placed = false;
	XrPosef m_menu_pose;
	f32 m_menu_width = 1.6f;
	f32 m_hud_width = 1.5f;
	f32 m_hud_distance = 1.4f;

	// Actions
	XrActionSet m_action_set = XR_NULL_HANDLE;
	XrPath m_hand_paths[2] = {XR_NULL_PATH, XR_NULL_PATH};
	XrAction m_act_aim = XR_NULL_HANDLE;
	XrAction m_act_trigger = XR_NULL_HANDLE;
	XrAction m_act_grip = XR_NULL_HANDLE;
	XrAction m_act_stick = XR_NULL_HANDLE;
	XrAction m_act_stick_click = XR_NULL_HANDLE;
	XrAction m_act_primary = XR_NULL_HANDLE; // X / A
	XrAction m_act_secondary = XR_NULL_HANDLE; // Y / B
	XrAction m_act_menu = XR_NULL_HANDLE;
	XrAction m_act_haptic = XR_NULL_HANDLE;
	std::array<XrSpace, 2> m_aim_spaces = {XR_NULL_HANDLE, XR_NULL_HANDLE};

	// Input state
	struct HandInput {
		f32 trigger = 0, grip = 0;
		v2f stick;
		bool stick_click = false, primary = false, secondary = false;
	};
	std::array<HandInput, 2> m_in, m_prev_in;
	bool m_menu_btn = false, m_prev_menu_btn = false;
	std::array<bool, (size_t)GamepadButton::COUNT> m_pad_buttons{};
	std::array<f32, (size_t)GamepadAxis::COUNT> m_pad_axes{};
	bool m_turn_armed = true;
	f32 m_hotbar_repeat = 0.0f;
	f32 m_scroll_timer = 0.0f;
	u64 m_last_update_ms = 0;
	f32 m_last_dtime = 0.0f;

	// Pointer
	bool m_pointer_valid = false;
	v2s32 m_pointer_pos;
	bool m_lmb_down = false, m_rmb_down = false;
	bool m_pointer_on_kb = false;

	// Virtual keyboard, shown below the menu panel while text is edited
	Swapchain m_kb_sc;
	VrKeyboard m_kb;
	bool m_kb_visible = false;
	bool m_text_input_requested = false;
	XrPosef m_kb_pose;
	v2f m_kb_size;
	int m_kb_hover = -1, m_kb_pressed = -1;
	f32 m_kb_repeat = 0.0f;
	// Only compared, never dereferenced
	const void *m_kb_dismissed_focus = nullptr;
};

OpenXrSystem::~OpenXrSystem()
{
	if (m_driver) {
		m_driver->setFrameHook(nullptr);
		m_driver->setScreenOverride(0, {});
	}
	{
		FramebufferGuard guard;
		for (auto &sc : m_eye_sc)
			destroySwapchain(sc);
		destroySwapchain(m_ui_sc);
		destroySwapchain(m_kb_sc);
		if (m_dummy_fbo)
			glDeleteFramebuffers(1, &m_dummy_fbo);
	}
	if (m_dummy_tex)
		glDeleteTextures(1, &m_dummy_tex);
	if (m_dummy_depth)
		glDeleteRenderbuffers(1, &m_dummy_depth);
	for (auto space : m_aim_spaces)
		if (space != XR_NULL_HANDLE)
			xrDestroySpace(space);
	if (m_action_set != XR_NULL_HANDLE)
		xrDestroyActionSet(m_action_set);
	if (m_view_space != XR_NULL_HANDLE)
		xrDestroySpace(m_view_space);
	if (m_app_space != XR_NULL_HANDLE)
		xrDestroySpace(m_app_space);
	if (m_session != XR_NULL_HANDLE) {
		if (m_running)
			xrEndSession(m_session);
		xrDestroySession(m_session);
	}
	if (m_instance != XR_NULL_HANDLE)
		xrDestroyInstance(m_instance);
	s_log_instance = XR_NULL_HANDLE;
}

bool OpenXrSystem::init()
{
	if (!createInstance() || !createSession() || !createActions())
		return false;

	// Views and eye swapchains
	u32 view_count = 0;
	if (!XR_CHECK(xrEnumerateViewConfigurationViews(m_instance, m_system,
			XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &view_count, nullptr)))
		return false;
	std::vector<XrViewConfigurationView> views(view_count, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
	if (view_count != 2 || !XR_CHECK(xrEnumerateViewConfigurationViews(m_instance, m_system,
			XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, view_count, &view_count, views.data()))) {
		errorstream << "VR: unexpected view configuration" << std::endl;
		return false;
	}

	u32 format_count = 0;
	xrEnumerateSwapchainFormats(m_session, 0, &format_count, nullptr);
	std::vector<int64_t> formats(format_count);
	xrEnumerateSwapchainFormats(m_session, format_count, &format_count, formats.data());
	// Luanti does not do linear lighting, so prefer a non-sRGB format
	m_color_format = formats.empty() ? GL_RGBA8 : formats[0];
	for (int64_t f : formats) {
		if (f == GL_RGBA8) {
			m_color_format = f;
			break;
		}
	}

	m_fbTex2DMs = (PFNGLFRAMEBUFFERTEXTURE2DMULTISAMPLEEXTPROC)
			eglGetProcAddress("glFramebufferTexture2DMultisampleEXT");
	m_rbStorageMs = (PFNGLRENDERBUFFERSTORAGEMULTISAMPLEEXTPROC)
			eglGetProcAddress("glRenderbufferStorageMultisampleEXT");
	if (m_fbTex2DMs && m_rbStorageMs)
		m_msaa = rangelim(g_settings->getU16("vr_msaa"), 0, 4);

	f32 scale = rangelim(g_settings->getFloat("vr_resolution_scale"), 0.25f, 2.0f);
	for (u32 i = 0; i < 2; i++) {
		core::dimension2du size(
				views[i].recommendedImageRectWidth * scale,
				views[i].recommendedImageRectHeight * scale);
		size.Width = std::min(size.Width, views[i].maxImageRectWidth);
		size.Height = std::min(size.Height, views[i].maxImageRectHeight);
		if (!createSwapchain(m_eye_sc[i], size, true))
			return false;
	}
	infostream << "VR: eye resolution " << m_eye_sc[0].size.Width << "x"
			<< m_eye_sc[0].size.Height << ", MSAA " << m_msaa << std::endl;

	core::dimension2du ui_size(g_settings->getU16("vr_ui_width"),
			g_settings->getU16("vr_ui_height"));
	u32 msaa = m_msaa;
	m_msaa = 0;
	bool ok = createSwapchain(m_ui_sc, ui_size, true);
	if (ok) {
		core::dimension2du kb_size(ui_size.Width, ui_size.Width / m_kb.getAspect());
		ok = createSwapchain(m_kb_sc, kb_size, false);
	}
	m_msaa = msaa;
	if (!ok)
		return false;

	// Target for frames the runtime doesn't want to be displayed
	{
		FramebufferGuard guard;
		GLint prev_tex = 0;
		glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev_tex);
		glGenTextures(1, &m_dummy_tex);
		glBindTexture(GL_TEXTURE_2D, m_dummy_tex);
		glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, ui_size.Width, ui_size.Height);
		glBindTexture(GL_TEXTURE_2D, prev_tex);
		glGenRenderbuffers(1, &m_dummy_depth);
		glBindRenderbuffer(GL_RENDERBUFFER, m_dummy_depth);
		glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, ui_size.Width, ui_size.Height);
		glBindRenderbuffer(GL_RENDERBUFFER, 0);
		glGenFramebuffers(1, &m_dummy_fbo);
		glBindFramebuffer(GL_FRAMEBUFFER, m_dummy_fbo);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_dummy_tex, 0);
		glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, m_dummy_depth);
	}
	// Make GUI layout use the panel size from the start
	m_driver->setScreenOverride(m_dummy_fbo, m_ui_sc.size);

	m_menu_width = g_settings->getFloat("vr_menu_width");
	m_hud_width = g_settings->getFloat("vr_hud_width");
	m_hud_distance = g_settings->getFloat("vr_hud_distance");

	m_driver->setFrameHook(this);
	return true;
}

bool OpenXrSystem::createInstance()
{
	JNIEnv *env = (JNIEnv *)SDL_AndroidGetJNIEnv();
	jobject activity = (jobject)SDL_AndroidGetActivity();
	JavaVM *vm = nullptr;
	env->GetJavaVM(&vm);

	PFN_xrInitializeLoaderKHR initLoader = nullptr;
	if (XR_SUCCEEDED(xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrInitializeLoaderKHR",
			(PFN_xrVoidFunction *)&initLoader)) && initLoader) {
		XrLoaderInitInfoAndroidKHR loader_info{XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR};
		loader_info.applicationVM = vm;
		loader_info.applicationContext = activity;
		if (!XR_CHECK(initLoader((const XrLoaderInitInfoBaseHeaderKHR *)&loader_info)))
			return false;
	}

	const char *extensions[] = {
		XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME,
		XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME,
	};

	XrInstanceCreateInfoAndroidKHR android_info{XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR};
	android_info.applicationVM = vm;
	android_info.applicationActivity = activity;

	XrInstanceCreateInfo info{XR_TYPE_INSTANCE_CREATE_INFO};
	info.next = &android_info;
	strncpy(info.applicationInfo.applicationName, "Luanti", XR_MAX_APPLICATION_NAME_SIZE - 1);
	info.applicationInfo.applicationVersion = 1;
	strncpy(info.applicationInfo.engineName, "Luanti", XR_MAX_ENGINE_NAME_SIZE - 1);
	info.applicationInfo.engineVersion = 1;
	info.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
	info.enabledExtensionCount = ARRLEN(extensions);
	info.enabledExtensionNames = extensions;

	if (!XR_CHECK(xrCreateInstance(&info, &m_instance)))
		return false;
	s_log_instance = m_instance;

	XrInstanceProperties props{XR_TYPE_INSTANCE_PROPERTIES};
	if (XR_SUCCEEDED(xrGetInstanceProperties(m_instance, &props)))
		infostream << "VR: OpenXR runtime " << props.runtimeName << std::endl;

	XrSystemGetInfo sys_info{XR_TYPE_SYSTEM_GET_INFO};
	sys_info.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
	return XR_CHECK(xrGetSystem(m_instance, &sys_info, &m_system));
}

bool OpenXrSystem::createSession()
{
	PFN_xrGetOpenGLESGraphicsRequirementsKHR getReqs = nullptr;
	if (!XR_CHECK(xrGetInstanceProcAddr(m_instance, "xrGetOpenGLESGraphicsRequirementsKHR",
			(PFN_xrVoidFunction *)&getReqs)))
		return false;
	XrGraphicsRequirementsOpenGLESKHR reqs{XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR};
	if (!XR_CHECK(getReqs(m_instance, m_system, &reqs)))
		return false;

	GLint major = 0, minor = 0;
	glGetIntegerv(GL_MAJOR_VERSION, &major);
	glGetIntegerv(GL_MINOR_VERSION, &minor);
	if (XR_MAKE_VERSION(major, minor, 0) < reqs.minApiVersionSupported) {
		errorstream << "VR: OpenGL ES " << major << "." << minor
				<< " is too old for the OpenXR runtime" << std::endl;
		return false;
	}

	XrGraphicsBindingOpenGLESAndroidKHR binding{XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR};
	binding.display = eglGetCurrentDisplay();
	binding.context = eglGetCurrentContext();
	EGLint config_id = 0, num_configs = 0;
	eglQueryContext(binding.display, binding.context, EGL_CONFIG_ID, &config_id);
	EGLint attribs[] = {EGL_CONFIG_ID, config_id, EGL_NONE};
	eglChooseConfig(binding.display, attribs, &binding.config, 1, &num_configs);
	if (binding.context == EGL_NO_CONTEXT || num_configs != 1) {
		errorstream << "VR: no current EGL context" << std::endl;
		return false;
	}

	XrSessionCreateInfo info{XR_TYPE_SESSION_CREATE_INFO};
	info.next = &binding;
	info.systemId = m_system;
	if (!XR_CHECK(xrCreateSession(m_instance, &info, &m_session)))
		return false;

	XrReferenceSpaceCreateInfo space_info{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
	space_info.poseInReferenceSpace.orientation.w = 1.0f;
	space_info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
	if (!XR_CHECK(xrCreateReferenceSpace(m_session, &space_info, &m_app_space)))
		return false;
	space_info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
	return XR_CHECK(xrCreateReferenceSpace(m_session, &space_info, &m_view_space));
}

bool OpenXrSystem::createActions()
{
	XrActionSetCreateInfo set_info{XR_TYPE_ACTION_SET_CREATE_INFO};
	strcpy(set_info.actionSetName, "gameplay");
	strcpy(set_info.localizedActionSetName, "Gameplay");
	if (!XR_CHECK(xrCreateActionSet(m_instance, &set_info, &m_action_set)))
		return false;

	xrStringToPath(m_instance, "/user/hand/left", &m_hand_paths[LEFT]);
	xrStringToPath(m_instance, "/user/hand/right", &m_hand_paths[RIGHT]);

	auto make = [&](XrAction &action, const char *name, XrActionType type) {
		XrActionCreateInfo info{XR_TYPE_ACTION_CREATE_INFO};
		info.actionType = type;
		strcpy(info.actionName, name);
		strcpy(info.localizedActionName, name);
		info.countSubactionPaths = 2;
		info.subactionPaths = m_hand_paths;
		return XR_CHECK(xrCreateAction(m_action_set, &info, &action));
	};
	if (!make(m_act_aim, "aim", XR_ACTION_TYPE_POSE_INPUT) ||
			!make(m_act_trigger, "trigger", XR_ACTION_TYPE_FLOAT_INPUT) ||
			!make(m_act_grip, "grip", XR_ACTION_TYPE_FLOAT_INPUT) ||
			!make(m_act_stick, "thumbstick", XR_ACTION_TYPE_VECTOR2F_INPUT) ||
			!make(m_act_stick_click, "thumbstick_click", XR_ACTION_TYPE_BOOLEAN_INPUT) ||
			!make(m_act_primary, "primary", XR_ACTION_TYPE_BOOLEAN_INPUT) ||
			!make(m_act_secondary, "secondary", XR_ACTION_TYPE_BOOLEAN_INPUT) ||
			!make(m_act_menu, "menu", XR_ACTION_TYPE_BOOLEAN_INPUT) ||
			!make(m_act_haptic, "haptic", XR_ACTION_TYPE_VIBRATION_OUTPUT))
		return false;

	std::vector<XrActionSuggestedBinding> bindings;
	auto bind = [&](XrAction action, const char *path) {
		XrPath p;
		if (XR_SUCCEEDED(xrStringToPath(m_instance, path, &p)))
			bindings.push_back({action, p});
	};
	bind(m_act_aim, "/user/hand/left/input/aim/pose");
	bind(m_act_aim, "/user/hand/right/input/aim/pose");
	bind(m_act_trigger, "/user/hand/left/input/trigger/value");
	bind(m_act_trigger, "/user/hand/right/input/trigger/value");
	bind(m_act_grip, "/user/hand/left/input/squeeze/value");
	bind(m_act_grip, "/user/hand/right/input/squeeze/value");
	bind(m_act_stick, "/user/hand/left/input/thumbstick");
	bind(m_act_stick, "/user/hand/right/input/thumbstick");
	bind(m_act_stick_click, "/user/hand/left/input/thumbstick/click");
	bind(m_act_stick_click, "/user/hand/right/input/thumbstick/click");
	bind(m_act_primary, "/user/hand/left/input/x/click");
	bind(m_act_primary, "/user/hand/right/input/a/click");
	bind(m_act_secondary, "/user/hand/left/input/y/click");
	bind(m_act_secondary, "/user/hand/right/input/b/click");
	bind(m_act_menu, "/user/hand/left/input/menu/click");
	bind(m_act_haptic, "/user/hand/left/output/haptic");
	bind(m_act_haptic, "/user/hand/right/output/haptic");

	XrPath profile;
	xrStringToPath(m_instance, "/interaction_profiles/oculus/touch_controller", &profile);
	XrInteractionProfileSuggestedBinding suggested{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
	suggested.interactionProfile = profile;
	suggested.countSuggestedBindings = bindings.size();
	suggested.suggestedBindings = bindings.data();
	if (!XR_CHECK(xrSuggestInteractionProfileBindings(m_instance, &suggested)))
		return false;

	for (int hand : {LEFT, RIGHT}) {
		XrActionSpaceCreateInfo info{XR_TYPE_ACTION_SPACE_CREATE_INFO};
		info.action = m_act_aim;
		info.subactionPath = m_hand_paths[hand];
		info.poseInActionSpace.orientation.w = 1.0f;
		if (!XR_CHECK(xrCreateActionSpace(m_session, &info, &m_aim_spaces[hand])))
			return false;
	}

	XrSessionActionSetsAttachInfo attach{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
	attach.countActionSets = 1;
	attach.actionSets = &m_action_set;
	return XR_CHECK(xrAttachSessionActionSets(m_session, &attach));
}

bool OpenXrSystem::createSwapchain(Swapchain &sc, core::dimension2du size, bool with_depth)
{
	XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
	info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
	info.format = m_color_format;
	info.sampleCount = 1;
	info.width = size.Width;
	info.height = size.Height;
	info.faceCount = 1;
	info.arraySize = 1;
	info.mipCount = 1;
	if (!XR_CHECK(xrCreateSwapchain(m_session, &info, &sc.handle)))
		return false;
	sc.size = size;

	u32 count = 0;
	xrEnumerateSwapchainImages(sc.handle, 0, &count, nullptr);
	std::vector<XrSwapchainImageOpenGLESKHR> images(count, {XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR});
	if (!XR_CHECK(xrEnumerateSwapchainImages(sc.handle, count, &count,
			(XrSwapchainImageBaseHeader *)images.data())))
		return false;

	FramebufferGuard guard;
	if (with_depth) {
		glGenRenderbuffers(1, &sc.depth);
		glBindRenderbuffer(GL_RENDERBUFFER, sc.depth);
		if (m_msaa > 1)
			m_rbStorageMs(GL_RENDERBUFFER, m_msaa, GL_DEPTH_COMPONENT24, size.Width, size.Height);
		else
			glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, size.Width, size.Height);
		glBindRenderbuffer(GL_RENDERBUFFER, 0);
	}

	sc.fbos.resize(count);
	glGenFramebuffers(count, sc.fbos.data());
	for (u32 i = 0; i < count; i++) {
		glBindFramebuffer(GL_FRAMEBUFFER, sc.fbos[i]);
		if (m_msaa > 1)
			m_fbTex2DMs(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
					images[i].image, 0, m_msaa);
		else
			glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
					images[i].image, 0);
		if (sc.depth)
			glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, sc.depth);
		GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
		if (status != GL_FRAMEBUFFER_COMPLETE) {
			errorstream << "VR: incomplete framebuffer 0x" << std::hex << status
					<< std::dec << std::endl;
			return false;
		}
	}
	return true;
}

void OpenXrSystem::destroySwapchain(Swapchain &sc)
{
	if (!sc.fbos.empty())
		glDeleteFramebuffers(sc.fbos.size(), sc.fbos.data());
	sc.fbos.clear();
	if (sc.depth)
		glDeleteRenderbuffers(1, &sc.depth);
	sc.depth = 0;
	if (sc.handle != XR_NULL_HANDLE)
		xrDestroySwapchain(sc.handle);
	sc.handle = XR_NULL_HANDLE;
}

bool OpenXrSystem::acquire(Swapchain &sc)
{
	XrSwapchainImageAcquireInfo acquire_info{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
	if (!XR_CHECK(xrAcquireSwapchainImage(sc.handle, &acquire_info, &sc.acquired)))
		return false;
	XrSwapchainImageWaitInfo wait_info{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
	wait_info.timeout = XR_INFINITE_DURATION;
	sc.is_acquired = true;
	if (!XR_CHECK(xrWaitSwapchainImage(sc.handle, &wait_info))) {
		release(sc);
		return false;
	}
	return true;
}

void OpenXrSystem::release(Swapchain &sc)
{
	if (!sc.is_acquired)
		return;
	XrSwapchainImageReleaseInfo info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
	XR_CHECK(xrReleaseSwapchainImage(sc.handle, &info));
	sc.is_acquired = false;
}

/* Session lifecycle */

void OpenXrSystem::pollEvents()
{
	XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
	while (xrPollEvent(m_instance, &event) == XR_SUCCESS) {
		switch (event.type) {
		case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
			auto *e = (XrEventDataSessionStateChanged *)&event;
			handleSessionState(e->state);
			break;
		}
		case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
			m_should_quit = true;
			break;
		case XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING:
			// The user recentered the view
			m_centered = false;
			m_menu_panel_placed = false;
			break;
		default:
			break;
		}
		event = {XR_TYPE_EVENT_DATA_BUFFER};
	}
}

void OpenXrSystem::handleSessionState(XrSessionState state)
{
	infostream << "VR: session state " << state << std::endl;
	m_state = state;
	switch (state) {
	case XR_SESSION_STATE_READY: {
		XrSessionBeginInfo info{XR_TYPE_SESSION_BEGIN_INFO};
		info.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
		if (XR_CHECK(xrBeginSession(m_session, &info)))
			m_running = true;
		break;
	}
	case XR_SESSION_STATE_STOPPING:
		releaseAllInput();
		XR_CHECK(xrEndSession(m_session));
		m_running = false;
		break;
	case XR_SESSION_STATE_EXITING:
	case XR_SESSION_STATE_LOSS_PENDING:
		m_running = false;
		m_should_quit = true;
		break;
	default:
		break;
	}
}

void OpenXrSystem::locate(XrTime time)
{
	XrViewLocateInfo info{XR_TYPE_VIEW_LOCATE_INFO};
	info.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
	info.displayTime = time;
	info.space = m_app_space;
	XrViewState state{XR_TYPE_VIEW_STATE};
	u32 count = 0;
	for (auto &v : m_xr_views)
		v = {XR_TYPE_VIEW};
	if (XR_SUCCEEDED(xrLocateViews(m_session, &info, &state, 2, &count, m_xr_views.data()))) {
		bool valid = (state.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) &&
				(state.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT);
		for (u32 i = 0; i < 2; i++) {
			const XrView &v = m_xr_views[i];
			m_eye_views[i].pose = fromXr(v.pose, valid);
			// Mirroring Z does not change the left/right/up/down angles
			m_eye_views[i].tan_left = std::tan(v.fov.angleLeft);
			m_eye_views[i].tan_right = std::tan(v.fov.angleRight);
			m_eye_views[i].tan_down = std::tan(v.fov.angleDown);
			m_eye_views[i].tan_up = std::tan(v.fov.angleUp);
		}
	}

	XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
	if (XR_SUCCEEDED(xrLocateSpace(m_view_space, m_app_space, time, &loc))) {
		bool valid = loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
		if (valid) {
			m_head = fromXr(loc.pose, true);
			m_head_xr = loc.pose;
		}
	}

	for (int hand : {LEFT, RIGHT}) {
		loc = {XR_TYPE_SPACE_LOCATION};
		m_aim[hand].valid = false;
		if (XR_SUCCEEDED(xrLocateSpace(m_aim_spaces[hand], m_app_space, time, &loc))) {
			bool valid = (loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) &&
					(loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT);
			m_aim[hand] = fromXr(loc.pose, valid);
			m_aim_xr[hand] = loc.pose;
		}
	}

	if (!m_centered && m_head.valid)
		recenter(true);

	// Keep the player's body under the head when walking around physically
	if (m_centered && m_head.valid) {
		v3f d = m_head.position - m_center;
		d.Y = 0;
		const f32 max_dist = 0.5f;
		f32 len = d.getLength();
		if (len > max_dist) {
			d *= (len - max_dist) / len;
			m_center += d;
		}
	}
}

void OpenXrSystem::recenter(bool full)
{
	m_center.X = m_head.position.X;
	m_center.Z = m_head.position.Z;
	if (full) {
		m_center.Y = m_head.position.Y;
		m_centered = true;
	}
}

/* Rendering */

void OpenXrSystem::onBeginScene()
{
	pollEvents();

	m_frame_active = false;
	m_should_render = false;
	m_eyes_done = 0;
	if (!m_running) {
		m_driver->setScreenOverride(m_dummy_fbo, m_ui_sc.size);
		return;
	}

	XrFrameWaitInfo wait_info{XR_TYPE_FRAME_WAIT_INFO};
	XrFrameState frame_state{XR_TYPE_FRAME_STATE};
	if (!XR_CHECK(xrWaitFrame(m_session, &wait_info, &frame_state)))
		return;
	XrFrameBeginInfo begin_info{XR_TYPE_FRAME_BEGIN_INFO};
	if (!XR_CHECK(xrBeginFrame(m_session, &begin_info)))
		return;

	m_frame_active = true;
	m_display_time = frame_state.predictedDisplayTime;
	m_should_render = frame_state.shouldRender;

	locate(m_display_time);

	if (m_ui_mode == VrUiMode::Menu && !m_menu_panel_placed && m_head.valid)
		placeMenuPanel();

	if (m_should_render && acquire(m_ui_sc))
		m_driver->setScreenOverride(m_ui_sc.fbos[m_ui_sc.acquired], m_ui_sc.size);
	else
		m_driver->setScreenOverride(m_dummy_fbo, m_ui_sc.size);
}

bool OpenXrSystem::onEndScene()
{
	if (!m_frame_active)
		return false;
	m_frame_active = false;

	if (m_ui_sc.is_acquired) {
		drawKeyboard();
		drawPointer();
		release(m_ui_sc);
	}

	std::array<XrCompositionLayerProjectionView, 2> proj_views;
	XrCompositionLayerProjection proj{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
	XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
	XrCompositionLayerQuad kb_quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
	std::vector<XrCompositionLayerBaseHeader *> layers;

	if (m_should_render && m_eyes_done == 3) {
		for (u32 i = 0; i < 2; i++) {
			auto &v = proj_views[i];
			v = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
			v.pose = m_xr_views[i].pose;
			v.fov = m_xr_views[i].fov;
			v.subImage.swapchain = m_eye_sc[i].handle;
			v.subImage.imageRect.offset = {0, 0};
			v.subImage.imageRect.extent = {(s32)m_eye_sc[i].size.Width,
					(s32)m_eye_sc[i].size.Height};
		}
		proj.space = m_app_space;
		proj.viewCount = 2;
		proj.views = proj_views.data();
		layers.push_back((XrCompositionLayerBaseHeader *)&proj);
	}

	if (m_should_render) {
		const f32 aspect = (f32)m_ui_sc.size.Height / m_ui_sc.size.Width;
		quad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT |
				XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
		quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
		quad.subImage.swapchain = m_ui_sc.handle;
		quad.subImage.imageRect.offset = {0, 0};
		quad.subImage.imageRect.extent = {(s32)m_ui_sc.size.Width, (s32)m_ui_sc.size.Height};
		if (m_ui_mode == VrUiMode::Hud) {
			quad.space = m_view_space;
			quad.pose.orientation = {0, 0, 0, 1};
			quad.pose.position = {0, -0.05f * m_hud_distance, -m_hud_distance};
			quad.size = {m_hud_width, m_hud_width * aspect};
		} else {
			quad.space = m_app_space;
			quad.pose = m_menu_pose;
			quad.size = {m_menu_width, m_menu_width * aspect};
		}
		layers.push_back((XrCompositionLayerBaseHeader *)&quad);

		if (m_kb_visible) {
			kb_quad.layerFlags = quad.layerFlags;
			kb_quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
			kb_quad.subImage.swapchain = m_kb_sc.handle;
			kb_quad.subImage.imageRect.offset = {0, 0};
			kb_quad.subImage.imageRect.extent = {(s32)m_kb_sc.size.Width, (s32)m_kb_sc.size.Height};
			kb_quad.space = m_app_space;
			kb_quad.pose = m_kb_pose;
			kb_quad.size = {m_kb_size.X, m_kb_size.Y};
			layers.push_back((XrCompositionLayerBaseHeader *)&kb_quad);
		}
	}

	XrFrameEndInfo end_info{XR_TYPE_FRAME_END_INFO};
	end_info.displayTime = m_display_time;
	end_info.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
	end_info.layerCount = layers.size();
	end_info.layers = layers.data();
	XR_CHECK(xrEndFrame(m_session, &end_info));
	return true;
}

bool OpenXrSystem::beginEye(u32 eye)
{
	if (!isRenderingEyes() || eye > 1)
		return false;
	Swapchain &sc = m_eye_sc[eye];
	if (!acquire(sc))
		return false;
	m_driver->setScreenOverride(sc.fbos[sc.acquired], sc.size);
	return true;
}

void OpenXrSystem::endEye(u32 eye)
{
	Swapchain &sc = m_eye_sc[eye];
	if (!sc.is_acquired)
		return;
	// Depth is not needed after rendering, which saves bandwidth on tilers
	FramebufferGuard guard;
	glBindFramebuffer(GL_FRAMEBUFFER, sc.fbos[sc.acquired]);
	const GLenum attachments[] = {GL_DEPTH_ATTACHMENT};
	glInvalidateFramebuffer(GL_FRAMEBUFFER, 1, attachments);
	release(sc);
	m_eyes_done |= 1 << eye;
}

void OpenXrSystem::beginUi()
{
	if (m_ui_sc.is_acquired)
		m_driver->setScreenOverride(m_ui_sc.fbos[m_ui_sc.acquired], m_ui_sc.size);
	else
		m_driver->setScreenOverride(m_dummy_fbo, m_ui_sc.size);
}

v2f OpenXrSystem::getCombinedFov() const
{
	f32 left = std::min(m_eye_views[0].tan_left, m_eye_views[1].tan_left);
	f32 right = std::max(m_eye_views[0].tan_right, m_eye_views[1].tan_right);
	f32 down = std::min(m_eye_views[0].tan_down, m_eye_views[1].tan_down);
	f32 up = std::max(m_eye_views[0].tan_up, m_eye_views[1].tan_up);
	// Symmetric FOV covering both eyes
	f32 x = 2.0f * std::atan(std::max(-left, right));
	f32 y = 2.0f * std::atan(std::max(-down, up));
	return v2f(x, y);
}

/* UI panel */

void OpenXrSystem::setUiMode(VrUiMode mode)
{
	if (mode == m_ui_mode)
		return;
	releaseAllInput();
	m_ui_mode = mode;
	if (mode == VrUiMode::Menu)
		m_menu_panel_placed = false;
}

void OpenXrSystem::placeMenuPanel()
{
	// Place the panel in front of the user, upright, facing the user
	const XrPosef &head = m_head_xr;
	VrQuat q{head.orientation.x, head.orientation.y, head.orientation.z, head.orientation.w};
	v3f fwd = q.rotate(v3f(0, 0, -1));
	fwd.Y = 0;
	if (fwd.getLengthSQ() < 1e-4f)
		fwd = v3f(0, 0, -1);
	fwd.normalize();

	const f32 dist = g_settings->getFloat("vr_menu_distance");
	m_menu_pose.position = {
		head.position.x + fwd.X * dist,
		head.position.y - 0.1f,
		head.position.z + fwd.Z * dist,
	};
	VrQuat rot = VrQuat::fromAxisAngle(v3f(0, 1, 0), std::atan2(-fwd.X, -fwd.Z));
	m_menu_pose.orientation = {rot.x, rot.y, rot.z, rot.w};
	m_menu_panel_placed = true;
}

bool OpenXrSystem::hitQuad(const XrPosef &pose, v2f size, v2f &uv, f32 &dist) const
{
	// Work in OpenXR tracking space
	const XrPosef &aim = m_aim_xr[RIGHT];
	VrQuat aim_q{aim.orientation.x, aim.orientation.y, aim.orientation.z, aim.orientation.w};
	v3f origin(aim.position.x, aim.position.y, aim.position.z);
	v3f dir = aim_q.rotate(v3f(0, 0, -1));

	VrQuat quad_q{pose.orientation.x, pose.orientation.y, pose.orientation.z, pose.orientation.w};
	v3f center(pose.position.x, pose.position.y, pose.position.z);
	v3f normal = quad_q.rotate(v3f(0, 0, 1));

	f32 denom = dir.dotProduct(normal);
	if (denom > -1e-4f)
		return false;
	dist = (center - origin).dotProduct(normal) / denom;
	if (dist <= 0)
		return false;
	v3f local = conjugate(quad_q).rotate(origin + dir * dist - center);
	uv = v2f(local.X / size.X + 0.5f, 0.5f - local.Y / size.Y);
	return true;
}

void OpenXrSystem::updatePointer()
{
	m_pointer_valid = false;
	m_pointer_on_kb = false;
	m_kb_hover = -1;
	if (m_ui_mode != VrUiMode::Menu || !m_menu_panel_placed || !m_aim[RIGHT].valid)
		return;

	v2f uv;
	f32 dist, kb_dist = 1e9f;
	v2f kb_uv;
	bool kb_hit = m_kb_visible && hitQuad(m_kb_pose, m_kb_size, kb_uv, kb_dist) &&
			kb_uv.X >= 0 && kb_uv.X <= 1 && kb_uv.Y >= 0 && kb_uv.Y <= 1;

	const v2f panel_size(m_menu_width, m_menu_width * m_ui_sc.size.Height / m_ui_sc.size.Width);
	bool panel_hit = hitQuad(m_menu_pose, panel_size, uv, dist) &&
			uv.X >= -0.05f && uv.X <= 1.05f && uv.Y >= -0.05f && uv.Y <= 1.05f;

	if (kb_hit && (!panel_hit || kb_dist < dist)) {
		m_pointer_on_kb = true;
		m_kb_hover = m_kb.keyAt(kb_uv);
		return;
	}
	if (!panel_hit)
		return;

	m_pointer_valid = true;
	m_pointer_pos = v2s32(
			rangelim(uv.X, 0.0f, 1.0f) * (m_ui_sc.size.Width - 1),
			rangelim(uv.Y, 0.0f, 1.0f) * (m_ui_sc.size.Height - 1));
}

void OpenXrSystem::drawPointer()
{
	if (m_ui_mode != VrUiMode::Menu || !m_pointer_valid)
		return;
	m_driver->setRenderTargetEx(nullptr, video::ECBF_NONE);
	const s32 r = std::max<s32>(3, m_ui_sc.size.Width / 250);
	const v2s32 p = m_pointer_pos;
	m_driver->draw2DRectangle(video::SColor(255, 0, 0, 0),
			core::rect<s32>(p.X - r - 1, p.Y - r - 1, p.X + r + 1, p.Y + r + 1));
	m_driver->draw2DRectangle(m_lmb_down ? video::SColor(255, 80, 200, 255) :
			video::SColor(255, 255, 255, 255),
			core::rect<s32>(p.X - r, p.Y - r, p.X + r, p.Y + r));
}

/* Virtual keyboard */

void OpenXrSystem::updateKeyboard()
{
	bool wanted = false;
	const void *focus_id = nullptr;
	if (m_ui_mode == VrUiMode::Menu) {
		gui::IGUIElement *focus = m_device->getGUIEnvironment()->getFocus();
		if (focus && focus->getType() == gui::EGUIET_EDIT_BOX && focus->isEnabled())
			focus_id = focus;
		else if (m_text_input_requested)
			focus_id = &m_text_input_requested;
		wanted = focus_id != nullptr;
	}

	// "Hide" keeps the keyboard closed until another field gets focus
	if (focus_id != m_kb_dismissed_focus)
		m_kb_dismissed_focus = nullptr;
	wanted = wanted && !m_kb_dismissed_focus;

	if (wanted && !m_kb_visible)
		placeKeyboard();
	if (!wanted)
		m_kb_pressed = -1;
	m_kb_visible = wanted;
}

void OpenXrSystem::placeKeyboard()
{
	// Tilted like a desk keyboard, just below the menu panel
	const f32 tilt = 30.0f * core::DEGTORAD;
	const f32 gap = 0.04f;
	const f32 panel_h = m_menu_width * m_ui_sc.size.Height / m_ui_sc.size.Width;
	m_kb_size.X = m_menu_width * 0.85f;
	m_kb_size.Y = m_kb_size.X / m_kb.getAspect();

	const XrPosef &panel = m_menu_pose;
	VrQuat panel_q{panel.orientation.x, panel.orientation.y, panel.orientation.z, panel.orientation.w};
	v3f offset(0,
			-(panel_h / 2 + gap) - m_kb_size.Y / 2 * std::cos(tilt),
			m_kb_size.Y / 2 * std::sin(tilt));
	v3f pos = v3f(panel.position.x, panel.position.y, panel.position.z) + panel_q.rotate(offset);
	VrQuat rot = panel_q * VrQuat::fromAxisAngle(v3f(1, 0, 0), -tilt);

	m_kb_pose.position = {pos.X, pos.Y, pos.Z};
	m_kb_pose.orientation = {rot.x, rot.y, rot.z, rot.w};
}

void OpenXrSystem::drawKeyboard()
{
	if (!m_kb_visible || !acquire(m_kb_sc))
		return;
	m_driver->setScreenOverride(m_kb_sc.fbos[m_kb_sc.acquired], m_kb_sc.size);
	m_driver->setRenderTargetEx(nullptr, video::ECBF_COLOR, video::SColor(0, 0, 0, 0));
	gui::IGUIFont *font = g_fontengine ? g_fontengine->getFont(28, FM_Standard) : nullptr;
	m_kb.draw(m_driver, font, m_kb_sc.size, m_kb_hover, m_kb_pressed);
	release(m_kb_sc);
	beginUi();
}

void OpenXrSystem::pressKey(int index)
{
	VrKeyboard::Output out = m_kb.press(index);
	haptic(0.25f, 0.015f);

	switch (out.type) {
	case VrKeyboard::Output::TEXT: {
		core::stringw str(out.text.c_str());
		SEvent e{};
		e.EventType = EET_STRING_INPUT_EVENT;
		e.StringInput.Str = &str;
		m_device->postEventFromUser(e);
		break;
	}
	case VrKeyboard::Output::KEY: {
		SEvent e{};
		e.EventType = EET_KEY_INPUT_EVENT;
		e.KeyInput.Key = out.key;
		e.KeyInput.PressedDown = true;
		m_device->postEventFromUser(e);
		e.KeyInput.PressedDown = false;
		m_device->postEventFromUser(e);
		break;
	}
	case VrKeyboard::Output::CLOSE: {
		gui::IGUIElement *focus = m_device->getGUIEnvironment()->getFocus();
		m_kb_dismissed_focus = (focus && focus->getType() == gui::EGUIET_EDIT_BOX) ?
				(const void *)focus : (const void *)&m_text_input_requested;
		m_kb_visible = false;
		m_kb_pressed = -1;
		break;
	}
	case VrKeyboard::Output::NONE:
		break;
	}
}

void OpenXrSystem::haptic(f32 amplitude, f32 seconds)
{
	XrHapticActionInfo info{XR_TYPE_HAPTIC_ACTION_INFO};
	info.action = m_act_haptic;
	info.subactionPath = m_hand_paths[RIGHT];
	XrHapticVibration vib{XR_TYPE_HAPTIC_VIBRATION};
	vib.amplitude = amplitude;
	vib.duration = (XrDuration)(seconds * 1e9);
	vib.frequency = XR_FREQUENCY_UNSPECIFIED;
	xrApplyHapticFeedback(m_session, &info, (const XrHapticBaseHeader *)&vib);
}

/* Tracking to world */

v3f OpenXrSystem::toWorldOffset(v3f tracking_pos) const
{
	VrQuat turn = VrQuat::fromAxisAngle(v3f(0, 1, 0), -m_turn_deg * core::DEGTORAD);
	return turn.rotate(tracking_pos - m_center) * BS;
}

VrQuat OpenXrSystem::toWorldOrientation(const VrQuat &tracking_rot) const
{
	VrQuat turn = VrQuat::fromAxisAngle(v3f(0, 1, 0), -m_turn_deg * core::DEGTORAD);
	return turn * tracking_rot;
}

v2f OpenXrSystem::getLookYawPitch() const
{
	v3f fwd = toWorldOrientation(m_head.orientation).rotate(v3f(0, 0, 1));
	f32 yaw = std::atan2(-fwd.X, fwd.Z) * core::RADTODEG;
	f32 pitch = -std::asin(rangelim(fwd.Y, -1.0f, 1.0f)) * core::RADTODEG;
	return v2f(yaw, pitch);
}

/* Input */

void OpenXrSystem::update()
{
	pollEvents();
	if (!m_running)
		return;

	XrActiveActionSet active{m_action_set, XR_NULL_PATH};
	XrActionsSyncInfo sync{XR_TYPE_ACTIONS_SYNC_INFO};
	sync.countActiveActionSets = 1;
	sync.activeActionSets = &active;
	if (xrSyncActions(m_session, &sync) != XR_SUCCESS) {
		// Not focused (e.g. system menu open)
		releaseAllInput();
		return;
	}

	m_prev_in = m_in;
	m_prev_menu_btn = m_menu_btn;
	for (int hand : {LEFT, RIGHT}) {
		XrActionStateGetInfo info{XR_TYPE_ACTION_STATE_GET_INFO};
		info.subactionPath = m_hand_paths[hand];
		HandInput &in = m_in[hand];

		XrActionStateFloat f{XR_TYPE_ACTION_STATE_FLOAT};
		info.action = m_act_trigger;
		in.trigger = XR_SUCCEEDED(xrGetActionStateFloat(m_session, &info, &f)) && f.isActive ? f.currentState : 0;
		f = {XR_TYPE_ACTION_STATE_FLOAT};
		info.action = m_act_grip;
		in.grip = XR_SUCCEEDED(xrGetActionStateFloat(m_session, &info, &f)) && f.isActive ? f.currentState : 0;

		XrActionStateVector2f v{XR_TYPE_ACTION_STATE_VECTOR2F};
		info.action = m_act_stick;
		if (XR_SUCCEEDED(xrGetActionStateVector2f(m_session, &info, &v)) && v.isActive)
			in.stick = v2f(v.currentState.x, v.currentState.y);
		else
			in.stick = v2f(0, 0);

		auto get_bool = [&](XrAction action) {
			XrActionStateBoolean b{XR_TYPE_ACTION_STATE_BOOLEAN};
			info.action = action;
			return XR_SUCCEEDED(xrGetActionStateBoolean(m_session, &info, &b)) &&
					b.isActive && b.currentState;
		};
		in.stick_click = get_bool(m_act_stick_click);
		in.primary = get_bool(m_act_primary);
		in.secondary = get_bool(m_act_secondary);
		if (hand == LEFT)
			m_menu_btn = get_bool(m_act_menu);
	}

	// Controller poses for pointing; the views are relocated in onBeginScene
	if (m_display_time)
		locate(m_display_time);

	updateKeyboard();
	processInput();
}

void OpenXrSystem::processInput()
{
	u64 now = m_device->getTimer()->getRealTime();
	f32 dtime = m_last_update_ms ? (now - m_last_update_ms) / 1000.0f : 0.0f;
	m_last_update_ms = now;
	m_last_dtime = dtime;
	m_hotbar_repeat = std::max(0.0f, m_hotbar_repeat - dtime);
	m_scroll_timer = std::max(0.0f, m_scroll_timer - dtime);

	if (m_ui_mode == VrUiMode::Hud)
		processHudInput();
	else
		processMenuInput();
}

void OpenXrSystem::processHudInput()
{
	const HandInput &l = m_in[LEFT], &r = m_in[RIGHT];

	// Movement: the default keymap binds the left gamepad stick
	postGamepadAxis(GamepadAxis::LEFTX, l.stick.X);
	postGamepadAxis(GamepadAxis::LEFTY, -l.stick.Y);

	// Dig with the right trigger, place with the left trigger or right grip
	postGamepadAxis(GamepadAxis::RIGHT_TRIGGER, r.trigger);
	postGamepadAxis(GamepadAxis::LEFT_TRIGGER, std::max(l.trigger, r.grip));

	postGamepadButton(GamepadButton::SOUTH, r.primary); // A: jump
	postGamepadButton(GamepadButton::EAST, r.secondary); // B: sneak
	postGamepadButton(GamepadButton::WEST, l.primary); // X: aux1
	postGamepadButton(GamepadButton::NORTH, l.secondary); // Y: inventory
	postGamepadButton(GamepadButton::START, m_menu_btn); // Menu: pause
	postGamepadButton(GamepadButton::DPAD_LEFT, l.stick_click); // toggle fly
	postGamepadButton(GamepadButton::DPAD_DOWN, l.grip > 0.8f); // drop item
	postGamepadButton(GamepadButton::RIGHT_STICK, r.stick_click); // chat

	// Snap turning with the right stick
	const f32 turn_step = g_settings->getFloat("vr_snap_turn_angle");
	if (m_turn_armed && std::fabs(r.stick.X) > 0.7f) {
		m_turn_deg += r.stick.X < 0 ? turn_step : -turn_step;
		m_turn_deg = std::fmod(m_turn_deg, 360.0f);
		m_turn_armed = false;
		// Rotate around the head, not around the play area center
		recenter(false);
	} else if (std::fabs(r.stick.X) < 0.3f) {
		m_turn_armed = true;
	}

	// Hotbar selection with the right stick up/down
	bool up = r.stick.Y > 0.7f, down = r.stick.Y < -0.7f;
	if ((up || down) && m_hotbar_repeat <= 0.0f) {
		postGamepadButton(up ? GamepadButton::LEFT_SHOULDER : GamepadButton::RIGHT_SHOULDER, true);
		postGamepadButton(up ? GamepadButton::LEFT_SHOULDER : GamepadButton::RIGHT_SHOULDER, false);
		m_hotbar_repeat = 0.3f;
	} else if (!up && !down) {
		m_hotbar_repeat = 0.0f;
	}
}

void OpenXrSystem::processMenuInput()
{
	const HandInput &l = m_in[LEFT], &r = m_in[RIGHT], &pr = m_prev_in[RIGHT];

	if (m_head.valid && !m_menu_panel_placed)
		placeMenuPanel();

	v2s32 old_pos = m_pointer_pos;
	bool was_valid = m_pointer_valid;
	updatePointer();
	if (m_pointer_valid && (!was_valid || old_pos != m_pointer_pos))
		postMouse(EMIE_MOUSE_MOVED);

	// Trigger with hysteresis
	const bool trigger = (m_lmb_down || m_kb_pressed >= 0) ? r.trigger > 0.4f : r.trigger > 0.7f;

	// Keyboard keys, with auto-repeat for some keys
	if (m_kb_pressed >= 0) {
		if (!trigger || m_kb_hover != m_kb_pressed) {
			m_kb_pressed = -1;
		} else if (m_kb.isRepeatable(m_kb_pressed)) {
			m_kb_repeat -= m_last_dtime;
			if (m_kb_repeat <= 0.0f) {
				pressKey(m_kb_pressed);
				m_kb_repeat = 0.06f;
			}
		}
	} else if (trigger && !m_lmb_down && m_pointer_on_kb && r.trigger > 0.7f &&
			pr.trigger <= 0.7f && m_kb_hover >= 0) {
		m_kb_pressed = m_kb_hover;
		m_kb_repeat = 0.45f;
		pressKey(m_kb_pressed);
	}

	bool lmb = trigger && m_kb_pressed < 0 && (m_lmb_down || !m_pointer_on_kb);
	if (lmb != m_lmb_down && (m_pointer_valid || !lmb)) {
		m_lmb_down = lmb;
		postMouse(lmb ? EMIE_LMOUSE_PRESSED_DOWN : EMIE_LMOUSE_LEFT_UP);
	}
	bool rmb = m_rmb_down ? r.grip > 0.4f : r.grip > 0.7f;
	if (rmb != m_rmb_down && (m_pointer_valid || !rmb)) {
		m_rmb_down = rmb;
		postMouse(rmb ? EMIE_RMOUSE_PRESSED_DOWN : EMIE_RMOUSE_LEFT_UP);
	}

	// Scroll with either stick
	f32 scroll = std::fabs(r.stick.Y) > std::fabs(l.stick.Y) ? r.stick.Y : l.stick.Y;
	if (std::fabs(scroll) > 0.5f && m_scroll_timer <= 0.0f && m_pointer_valid) {
		postMouse(EMIE_MOUSE_WHEEL, scroll > 0 ? 1 : -1);
		m_scroll_timer = 0.12f / std::fabs(scroll);
	}

	// B, Y and the menu button close menus
	if ((r.secondary && !pr.secondary) ||
			(l.secondary && !m_prev_in[LEFT].secondary) ||
			(m_menu_btn && !m_prev_menu_btn))
		postEscape();
}

void OpenXrSystem::releaseAllInput()
{
	for (size_t i = 0; i < m_pad_buttons.size(); i++)
		if (m_pad_buttons[i])
			postGamepadButton((GamepadButton)i, false);
	for (size_t i = 0; i < m_pad_axes.size(); i++)
		if (m_pad_axes[i] != 0.0f)
			postGamepadAxis((GamepadAxis)i, 0.0f);
	if (m_lmb_down) {
		m_lmb_down = false;
		postMouse(EMIE_LMOUSE_LEFT_UP);
	}
	if (m_rmb_down) {
		m_rmb_down = false;
		postMouse(EMIE_RMOUSE_LEFT_UP);
	}
	// Don't fire presses for buttons still held after a mode change
	m_prev_in = m_in;
	m_prev_menu_btn = m_menu_btn;
}

void OpenXrSystem::postGamepadButton(GamepadButton button, bool down)
{
	bool &state = m_pad_buttons[(size_t)button];
	if (state == down)
		return;
	state = down;
	SEvent e{};
	e.EventType = EET_GAMEPAD_BUTTON_EVENT;
	e.GamepadButtonEvent.ID = 0;
	e.GamepadButtonEvent.Button = button;
	e.GamepadButtonEvent.PressedDown = down;
	m_device->postEventFromUser(e);
}

void OpenXrSystem::postGamepadAxis(GamepadAxis axis, f32 value)
{
	value = rangelim(value, -1.0f, 1.0f);
	f32 &state = m_pad_axes[(size_t)axis];
	if (std::fabs(state - value) < 0.01f && !(value == 0.0f && state != 0.0f))
		return;
	state = value;
	SEvent e{};
	e.EventType = EET_GAMEPAD_AXIS_EVENT;
	e.GamepadAxisEvent.ID = 0;
	e.GamepadAxisEvent.Axis = axis;
	e.GamepadAxisEvent.Value = (s16)(value * 32767.0f);
	m_device->postEventFromUser(e);
}

void OpenXrSystem::postMouse(EMOUSE_INPUT_EVENT type, s32 wheel)
{
	SEvent e{};
	e.EventType = EET_MOUSE_INPUT_EVENT;
	e.MouseInput.Event = type;
	e.MouseInput.X = m_pointer_pos.X;
	e.MouseInput.Y = m_pointer_pos.Y;
	e.MouseInput.Wheel = wheel;
	e.MouseInput.ButtonStates = (m_lmb_down ? SDL_BUTTON_LMASK : 0) |
			(m_rmb_down ? SDL_BUTTON_RMASK : 0);
	m_device->postEventFromUser(e);
}

void OpenXrSystem::postEscape()
{
	SEvent e{};
	e.EventType = EET_KEY_INPUT_EVENT;
	e.KeyInput.Key = KEY_ESCAPE;
	e.KeyInput.SystemKeyCode = EscapeKey.getScancode();
	e.KeyInput.PressedDown = true;
	m_device->postEventFromUser(e);
	e.KeyInput.PressedDown = false;
	m_device->postEventFromUser(e);
}

} // namespace

std::unique_ptr<VrSystem> VrSystem::create(IrrlichtDevice *device)
{
	if (!g_settings->getBool("enable_vr"))
		return nullptr;
	auto vr = std::make_unique<OpenXrSystem>(device);
	if (!vr->init()) {
		errorstream << "VR: initialization failed, continuing without VR" << std::endl;
		return nullptr;
	}
	// The Android on-screen keyboard can't be used in VR, we have our own
	SDL_SetHint(SDL_HINT_ENABLE_SCREEN_KEYBOARD, "0");
	infostream << "VR: initialized" << std::endl;
	return vr;
}

#endif
