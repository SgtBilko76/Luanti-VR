// Luanti
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

class RenderPipeline;
class Client;

// Renders both eyes into the OpenXR swapchains and the HUD into the UI panel
void populateVRPipeline(RenderPipeline *pipeline, Client *client);
