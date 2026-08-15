/**
 * @file
 */

#include "Toolbar.h"
#include "IMGUIEx.h"
#include "IMGUIStyle.h"
#include "ScopedStyle.h"

namespace ui {

Toolbar::Toolbar(const core::String &name, const ImVec2 &size, command::CommandExecutionListener *listener)
	: _nextId(0), _pos(ImGui::GetCursorScreenPos()), _startingPosX(_pos.x), _size(size), _listener(listener), _id(name),
	  _windowWidth(ImGui::GetContentRegionAvail().x) {
}

Toolbar::~Toolbar() {
	end();
}

void Toolbar::next() {
	++_nextId;
	ImGui::SameLine();
	_pos = ImGui::GetCursorScreenPos();
}

void Toolbar::newline() {
	// If there's not enough horizontal space left for another button, wrap to the next line.
	const float avail = ImGui::GetContentRegionAvail().x;
	if (_pos.x > _startingPosX && avail < _size.x) {
		ImGui::NewLine();
		_pos = ImGui::GetCursorScreenPos();
	}
}

void Toolbar::end() {
	// If we placed any items on the same line as the toolbar start, make sure we move
	// to the next line so following widgets don't end up inline with the toolbar.
	newline();
	if (_pos.x > _startingPosX) {
		ImGui::NewLine();
		_pos = ImGui::GetCursorScreenPos();
	}
}

void Toolbar::applyIconStyle(ui::ScopedStyle &style) {
	const bool studio = ImGui::IsStudioStyle();
	// Studio frames are rounded; 1px padding pinches the glyph against the curve.
	if (studio) {
		style.setFramePadding(ImVec2(5.0f, 5.0f));
		style.setItemSpacing(ImVec2(4.0f, 4.0f));
		style.setFrameRounding(4.0f);
	} else {
		style.setFramePadding(ImVec2(1.0f, 1.0f));
		style.setItemSpacing(ImVec2(1.0f, 1.0f));
		ImGui::AlignTextToFramePadding();
	}
	style.setButtonTextAlign(ImVec2(0.5f, 0.5f));
}

void Toolbar::applyDisabledStyle(ui::ScopedStyle &style) {
	style.highlight(ImGuiCol_Text);
}

bool Toolbar::button(const char *icon, const char *command, bool disable) {
	newline();
	ui::ScopedStyle style;
	applyIconStyle(style);
	if (disable) {
		applyDisabledStyle(style);
	}
	ImGui::PushID(_id.c_str());
	char label[64];
	core::String::formatBuf(label, sizeof(label), "%s###button%d", icon, _nextId);
	bool pressed = ImGui::CommandButton(label, command, nullptr, _size, _listener);
	ImGui::PopID();
	next();

	return pressed;
}

bool Toolbar::commandButton(const char *icon, const char *command, bool disable) {
	newline();
	ui::ScopedStyle style;
	applyIconStyle(style);
	if (disable) {
		applyDisabledStyle(style);
	}
	ImGui::PushID(_id.c_str());
	char label[64];
	core::String::formatBuf(label, sizeof(label), "%s###%s", icon, command);
	bool pressed = ImGui::CommandButton(label, command, nullptr, _size, _listener);
	ImGui::PopID();
	next();

	return pressed;
}

} // namespace ui
