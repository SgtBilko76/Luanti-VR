// Luanti
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include "config.h"

#if USE_OPENXR

#include "irrlichttypes_bloated.h"
#include <IVideoDriver.h>
#include <line3d.h>
#include <array>
#include <memory>

class IrrlichtDevice;

/*
	All VR math uses Irrlicht's coordinate system (left-handed, +Y up,
	+Z forward). Tracking data is in meters, world data in nodes * BS.
*/

struct VrQuat
{
	f32 x = 0.0f, y = 0.0f, z = 0.0f, w = 1.0f;

	static VrQuat fromAxisAngle(v3f axis, f32 radians);

	// Hamilton product: (a * b) rotates by b first, then by a
	VrQuat operator*(const VrQuat &o) const;
	v3f rotate(v3f v) const;
};

struct VrPose
{
	v3f position;
	VrQuat orientation;
	bool valid = false;
};

struct VrEyeView
{
	VrPose pose;
	// Tangents of the half-angles of the (asymmetric) field of view
	f32 tan_left = -1.0f, tan_right = 1.0f, tan_down = -1.0f, tan_up = 1.0f;
};

enum class VrUiMode
{
	// World-locked panel with laser pointer (menus, formspecs, loading screens)
	Menu,
	// Head-locked transparent overlay, controllers act as gamepad
	Hud,
};

class VrSystem : public video::IVideoDriver::IFrameHook
{
public:
	// Returns nullptr if no OpenXR runtime is available.
	static std::unique_ptr<VrSystem> create(IrrlichtDevice *device);

	virtual ~VrSystem() = default;

	// Polls runtime events and controller state and turns them into
	// Irrlicht input events. Call once per main loop iteration.
	virtual void update() = 0;

	// True while the runtime wants the application to run
	virtual bool isSessionRunning() const = 0;
	// True if the runtime asked us to exit
	virtual bool shouldQuit() const = 0;

	// Eye rendering, only valid between beginScene() and endScene()
	virtual bool isRenderingEyes() const = 0;
	virtual const VrEyeView &getEyeView(u32 eye) const = 0;
	// Binds the eye swapchain image as screen framebuffer
	virtual bool beginEye(u32 eye) = 0;
	virtual void endEye(u32 eye) = 0;
	// Binds the UI panel as screen framebuffer again
	virtual void beginUi() = 0;

	virtual void setUiMode(VrUiMode mode) = 0;
	// Shows the keyboard for text input that isn't an edit box (chat console)
	virtual void setTextInputRequested(bool requested) = 0;
	virtual VrUiMode getUiMode() const = 0;

	// Combined vertical and horizontal FOV of both eyes (radians)
	virtual v2f getCombinedFov() const = 0;

	// Head / controller poses in tracking space
	virtual VrPose getHeadPose() const = 0;
	virtual VrPose getAimPose() const = 0;

	// Converts tracking space to player relative world space (BS units),
	// taking snap turning and recentering into account.
	virtual v3f toWorldOffset(v3f tracking_pos) const = 0;
	virtual VrQuat toWorldOrientation(const VrQuat &tracking_rot) const = 0;

	// Head direction as Luanti yaw and pitch (degrees)
	virtual v2f getLookYawPitch() const = 0;

	// Controller ray to draw in the world (relative to the camera offset)
	void setPointerRay(const core::line3df &ray, bool visible)
	{
		m_pointer_ray = ray;
		m_pointer_ray_visible = visible;
	}
	bool getPointerRay(core::line3df &ray) const
	{
		ray = m_pointer_ray;
		return m_pointer_ray_visible;
	}

private:
	core::line3df m_pointer_ray;
	bool m_pointer_ray_visible = false;
};

extern VrSystem *g_vr;

#endif
