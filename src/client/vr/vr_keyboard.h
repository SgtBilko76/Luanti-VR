// Luanti
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include "irrlichttypes_bloated.h"
#include <Keycodes.h>
#include <string>
#include <vector>

#include <IVideoDriver.h>
#include <IGUIFont.h>

// On-screen keyboard for VR, operated with the laser pointer.
// Layout and hit testing work in normalized coordinates (0..1).
class VrKeyboard
{
public:
	struct Output
	{
		enum Type { NONE, TEXT, KEY, CLOSE } type = NONE;
		std::wstring text;
		EKEY_CODE key = KEY_UNKNOWN;
	};

	VrKeyboard();

	// Index of the key at the given position, or -1
	int keyAt(v2f pos) const;
	Output press(int index);
	// Keys like backspace repeat while held
	bool isRepeatable(int index) const;

	void draw(video::IVideoDriver *driver, gui::IGUIFont *font,
			core::dimension2du size, int hovered, int pressed) const;

	// Width / height of the keyboard in key units
	f32 getAspect() const;

private:
	enum class Special { NONE, BACKSPACE, ENTER, SHIFT, SYMBOLS, SPACE, LEFT, RIGHT, CLOSE };

	struct KeyDef
	{
		wchar_t ch = 0;
		Special special = Special::NONE;
		f32 width = 1.0f;
	};

	struct PlacedKey
	{
		KeyDef def;
		core::rect<f32> rect;
	};

	void layout();
	std::wstring getLabel(const KeyDef &def) const;

	bool m_symbols = false;
	bool m_shift = false;
	bool m_caps = false;
	std::vector<PlacedKey> m_keys;
};
