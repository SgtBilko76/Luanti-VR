// Luanti
// SPDX-License-Identifier: LGPL-2.1-or-later

#include "vr_keyboard.h"

#include <IVideoDriver.h>
#include <IGUIFont.h>
#include <cwctype>

namespace {
// All rows are this many key units wide
constexpr f32 ROW_UNITS = 12.0f;
constexpr int ROW_COUNT = 5;
// Gap between keys, in key units
constexpr f32 KEY_GAP = 0.08f;
}

VrKeyboard::VrKeyboard()
{
	layout();
}

f32 VrKeyboard::getAspect() const
{
	return ROW_UNITS / ROW_COUNT;
}

void VrKeyboard::layout()
{
	using S = Special;
	std::vector<std::vector<KeyDef>> rows(ROW_COUNT);
	auto chars = [](std::vector<KeyDef> &row, const wchar_t *str) {
		for (; *str; str++)
			row.push_back({*str, S::NONE, 1.0f});
	};
	auto special = [](std::vector<KeyDef> &row, S s, f32 width) {
		row.push_back({0, s, width});
	};
	auto spacer = [](std::vector<KeyDef> &row, f32 width) {
		row.push_back({0, S::NONE, width});
	};

	if (!m_symbols) {
		chars(rows[0], L"1234567890");
		special(rows[0], S::BACKSPACE, 2.0f);
		chars(rows[1], L"qwertyuiop-/");
		spacer(rows[2], 0.5f);
		chars(rows[2], L"asdfghjkl");
		special(rows[2], S::ENTER, 2.5f);
		special(rows[3], S::SHIFT, 1.5f);
		chars(rows[3], L"zxcvbnm,.");
		special(rows[3], S::SHIFT, 1.5f);
		special(rows[4], S::SYMBOLS, 2.0f);
		special(rows[4], S::LEFT, 1.0f);
		special(rows[4], S::RIGHT, 1.0f);
		special(rows[4], S::SPACE, 5.0f);
		chars(rows[4], L"_");
		special(rows[4], S::CLOSE, 2.0f);
	} else {
		chars(rows[0], L"!@#$%^&*()");
		special(rows[0], S::BACKSPACE, 2.0f);
		chars(rows[1], L"~`+={}[]\\|_?");
		spacer(rows[2], 0.5f);
		chars(rows[2], L"<>'\";:!?&");
		special(rows[2], S::ENTER, 2.5f);
		special(rows[3], S::SYMBOLS, 1.5f);
		chars(rows[3], L"@#$%^*()-");
		special(rows[3], S::SYMBOLS, 1.5f);
		special(rows[4], S::SYMBOLS, 2.0f);
		special(rows[4], S::LEFT, 1.0f);
		special(rows[4], S::RIGHT, 1.0f);
		special(rows[4], S::SPACE, 5.0f);
		chars(rows[4], L".");
		special(rows[4], S::CLOSE, 2.0f);
	}

	m_keys.clear();
	for (int y = 0; y < ROW_COUNT; y++) {
		f32 x = 0.0f;
		for (const KeyDef &def : rows[y]) {
			if (def.ch || def.special != S::NONE) {
				core::rect<f32> rect(
						(x + KEY_GAP / 2) / ROW_UNITS,
						(y + KEY_GAP / 2) / ROW_COUNT,
						(x + def.width - KEY_GAP / 2) / ROW_UNITS,
						(y + 1 - KEY_GAP / 2) / ROW_COUNT);
				m_keys.push_back({def, rect});
			}
			x += def.width;
		}
	}
}

int VrKeyboard::keyAt(v2f pos) const
{
	for (size_t i = 0; i < m_keys.size(); i++)
		if (m_keys[i].rect.isPointInside(pos))
			return i;
	return -1;
}

bool VrKeyboard::isRepeatable(int index) const
{
	if (index < 0 || index >= (int)m_keys.size())
		return false;
	Special s = m_keys[index].def.special;
	return s == Special::BACKSPACE || s == Special::LEFT || s == Special::RIGHT ||
			s == Special::SPACE;
}

VrKeyboard::Output VrKeyboard::press(int index)
{
	Output out;
	if (index < 0 || index >= (int)m_keys.size())
		return out;
	const KeyDef def = m_keys[index].def;

	switch (def.special) {
	case Special::NONE: {
		wchar_t ch = def.ch;
		if (m_shift || m_caps)
			ch = std::towupper(ch);
		out.type = Output::TEXT;
		out.text = std::wstring(1, ch);
		if (m_shift && !m_caps)
			m_shift = false;
		break;
	}
	case Special::SPACE:
		out.type = Output::TEXT;
		out.text = L" ";
		break;
	case Special::BACKSPACE:
		out.type = Output::KEY;
		out.key = KEY_BACK;
		break;
	case Special::ENTER:
		out.type = Output::KEY;
		out.key = KEY_RETURN;
		break;
	case Special::LEFT:
		out.type = Output::KEY;
		out.key = KEY_LEFT;
		break;
	case Special::RIGHT:
		out.type = Output::KEY;
		out.key = KEY_RIGHT;
		break;
	case Special::SHIFT:
		// Tap once for one capital letter, twice for caps lock
		if (m_caps) {
			m_caps = false;
			m_shift = false;
		} else if (m_shift) {
			m_caps = true;
		} else {
			m_shift = true;
		}
		break;
	case Special::SYMBOLS:
		m_symbols = !m_symbols;
		layout();
		break;
	case Special::CLOSE:
		out.type = Output::CLOSE;
		break;
	}
	return out;
}

std::wstring VrKeyboard::getLabel(const KeyDef &def) const
{
	switch (def.special) {
	case Special::NONE:
		return std::wstring(1, (m_shift || m_caps) ? std::towupper(def.ch) : def.ch);
	case Special::BACKSPACE: return L"Back";
	case Special::ENTER: return L"Enter";
	case Special::SHIFT: return m_caps ? L"SHIFT" : L"Shift";
	case Special::SYMBOLS: return m_symbols ? L"ABC" : L"?123";
	case Special::SPACE: return L"";
	case Special::LEFT: return L"<";
	case Special::RIGHT: return L">";
	case Special::CLOSE: return L"Hide";
	}
	return L"";
}

void VrKeyboard::draw(video::IVideoDriver *driver, gui::IGUIFont *font,
		core::dimension2du size, int hovered, int pressed) const
{
	driver->draw2DRectangle(video::SColor(235, 24, 24, 30),
			core::recti(0, 0, size.Width, size.Height));

	for (size_t i = 0; i < m_keys.size(); i++) {
		const PlacedKey &key = m_keys[i];
		core::recti rect(
				key.rect.UpperLeftCorner.X * size.Width,
				key.rect.UpperLeftCorner.Y * size.Height,
				key.rect.LowerRightCorner.X * size.Width,
				key.rect.LowerRightCorner.Y * size.Height);

		bool is_special = key.def.special != Special::NONE &&
				key.def.special != Special::SPACE;
		bool active = key.def.special == Special::SHIFT && (m_shift || m_caps);

		video::SColor color = is_special ? video::SColor(255, 48, 48, 58) :
				video::SColor(255, 72, 72, 84);
		if (active)
			color = video::SColor(255, 60, 110, 170);
		if ((int)i == hovered)
			color = video::SColor(255, 105, 105, 125);
		if ((int)i == pressed)
			color = video::SColor(255, 80, 160, 230);
		driver->draw2DRectangle(color, rect);

		if (font) {
			std::wstring label = getLabel(key.def);
			font->draw(label.c_str(), rect, video::SColor(255, 255, 255, 255), true, true);
		}
	}
}
