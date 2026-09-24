// Luanti
// SPDX-License-Identifier: LGPL-2.1-or-later

#include "vr_pipeline.h"

#include "config.h"

#if USE_OPENXR

#include "vr.h"
#include "client/render/pipeline.h"
#include "client/render/plain.h"
#include "client/camera.h"
#include "client/client.h"
#include "client/clientenvironment.h"
#include "client/clientmap.h"
#include "constants.h"
#include <ICameraSceneNode.h>

// Off-center perspective projection for Irrlicht's left-handed system
// with OpenGL depth range
static core::matrix4 buildProjection(const VrEyeView &v, f32 znear, f32 zfar)
{
	core::matrix4 m(core::matrix4::EM4CONST_NOTHING);
	const f32 w = v.tan_right - v.tan_left;
	const f32 h = v.tan_up - v.tan_down;
	m[0] = 2.0f / w;
	m[1] = 0;
	m[2] = 0;
	m[3] = 0;
	m[4] = 0;
	m[5] = 2.0f / h;
	m[6] = 0;
	m[7] = 0;
	m[8] = -(v.tan_right + v.tan_left) / w;
	m[9] = -(v.tan_up + v.tan_down) / h;
	m[10] = (zfar + znear) / (zfar - znear);
	m[11] = 1;
	m[12] = 0;
	m[13] = 0;
	m[14] = 2.0f * znear * zfar / (znear - zfar);
	m[15] = 0;
	return m;
}

// Screen target that always clears, since it is activated once per eye
class VrEyeTarget : public RenderTarget
{
public:
	void activate(PipelineContext &context) override
	{
		context.device->getVideoDriver()->setRenderTargetEx(nullptr,
				video::ECBF_ALL, context.clear_color);
		RenderTarget::activate(context);
	}
};

class VrStereoStep : public TrivialRenderStep
{
public:
	VrStereoStep(RenderStep *step3d, RenderStep *postfx) :
		m_step3d(step3d), m_postfx(postfx)
	{
		m_ray_material.MaterialType = video::EMT_SOLID;
		m_ray_material.Thickness = 2;
	}

	void run(PipelineContext &context) override
	{
		if (!g_vr || !g_vr->isRenderingEyes())
			return;

		Camera *camera = context.client->getCamera();
		scene::ICameraSceneNode *node = camera->getCameraNode();
		auto *driver = context.device->getVideoDriver();

		const v3f old_pos = node->getPosition();
		const v3f old_target = node->getTarget();
		const v3f old_up = node->getUpVector();
		const core::matrix4 old_proj = node->getProjectionMatrix();

		const v3f base = camera->getPosition() - intToFloat(camera->getOffset(), BS);

		for (u32 eye = 0; eye < 2; eye++) {
			if (!g_vr->beginEye(eye))
				continue;

			const VrEyeView &view = g_vr->getEyeView(eye);
			const VrQuat rot = g_vr->toWorldOrientation(view.pose.orientation);
			const v3f pos = base + g_vr->toWorldOffset(view.pose.position);

			node->setPosition(pos);
			node->updateAbsolutePosition();
			node->setUpVector(rot.rotate(v3f(0, 1, 0)));
			node->setTarget(pos + rot.rotate(v3f(0, 0, 1)) * 100.0f);
			node->setProjectionMatrix(buildProjection(view,
					node->getNearValue(), node->getFarValue()));
			node->updateMatrices();

			m_step3d->run(context);
			drawRay(driver);
			m_postfx->run(context);

			g_vr->endEye(eye);
		}

		node->setPosition(old_pos);
		node->updateAbsolutePosition();
		node->setUpVector(old_up);
		node->setTarget(old_target);
		node->setProjectionMatrix(old_proj);
		node->updateMatrices();

		g_vr->beginUi();
	}

private:
	void drawRay(video::IVideoDriver *driver)
	{
		core::line3df ray;
		if (!g_vr->getPointerRay(ray))
			return;
		driver->setTransform(video::ETS_WORLD, core::IdentityMatrix);
		driver->setMaterial(m_ray_material);
		driver->draw3DLine(ray.start, ray.end, video::SColor(255, 90, 200, 255));
	}

	RenderStep *m_step3d;
	RenderStep *m_postfx;
	video::SMaterial m_ray_material;
};

// Clears the UI panel to transparent so the world stays visible behind the HUD
class VrUiClearStep : public TrivialRenderStep
{
public:
	void run(PipelineContext &context) override
	{
		if (!g_vr)
			return;
		g_vr->beginUi();
		context.device->getVideoDriver()->setRenderTargetEx(nullptr,
				video::ECBF_ALL, video::SColor(0, 0, 0, 0));
	}
};

void populateVRPipeline(RenderPipeline *pipeline, Client *client)
{
	// Post-processing is too expensive for mobile VR, render straight into
	// the swapchain images.
	auto *step3d = pipeline->own(std::make_unique<Draw3D>());
	step3d->setRenderTarget(pipeline->createOwned<VrEyeTarget>());
	auto *postfx = pipeline->own(std::make_unique<MapPostFxStep>());
	postfx->setRenderTarget(nullptr);

	pipeline->addStep<VrStereoStep>(step3d, postfx);
	pipeline->addStep<VrUiClearStep>();
	pipeline->addStep<DrawHUD>();
}

#endif
