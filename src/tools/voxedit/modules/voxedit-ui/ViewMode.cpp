/**
 * @file
 */

#include "ViewMode.h"
#include "SceneSettingsPanel.h"
#include "app/I18N.h"
#include "core/ArrayLength.h"
#include "core/ConfigVar.h"
#include "core/Var.h"
#include "engine-config.h"
#include "ui/IMGUIStyle.h"
#include "voxel/SurfaceExtractor.h"
#include "voxedit-util/Config.h"

namespace voxedit {

static const uint64_t ALL_FLAGS = ((uint64_t)-1) & ~(VIEWMODE_FLAG_PALFORMAT6BIT | VIEWMODE_FLAG_NOSPLIT);
static const uint64_t DEFAULT_FLAGS =
	ALL_FLAGS & ~(VIEWMODE_FLAG_NORMALPALETTE | VIEWMODE_FLAG_MEMENTOPANEL | VIEWMODE_FLAG_NODEPROPERTIESPANEL);
static const uint64_t SIMLPE_FLAGS =
	DEFAULT_FLAGS &
	~(VIEWMODE_FLAG_ALL_VIEWPORTS | VIEWMODE_FLAG_MEMENTOPANEL | VIEWMODE_FLAG_CAMERAPANEL |
	  VIEWMODE_FLAG_LSYSTEMPANEL | VIEWMODE_FLAG_SCRIPTPANEL | VIEWMODE_FLAG_NETWORKPANEL);
static const uint64_t COMMANDANDCONQUER_FLAGS =
	SIMLPE_FLAGS | VIEWMODE_FLAG_NORMALPALETTE | VIEWMODE_FLAG_PALFORMAT6BIT;
static const uint64_t MINECRAFTSKIN_FLAGS =
	SIMLPE_FLAGS & ~(VIEWMODE_FLAG_GAMEMODEPANEL | VIEWMODE_FLAG_ASSETPANEL | VIEWMODE_FLAG_ANIMATIONS | VIEWMODE_FLAG_RENDERPANEL);
static const uint64_t ACEOFSPADES_FLAGS = (DEFAULT_FLAGS & ~(VIEWMODE_FLAG_ANIMATIONS)) | VIEWMODE_FLAG_NOSPLIT;
static const uint64_t STUDIO_FLAGS =
	SIMLPE_FLAGS &
	~(VIEWMODE_FLAG_ALL_VIEWPORTS | VIEWMODE_FLAG_ASSETPANEL | VIEWMODE_FLAG_ANIMATIONS |
	  VIEWMODE_FLAG_CAMERAPANEL | VIEWMODE_FLAG_GAMEMODEPANEL | VIEWMODE_FLAG_NETWORKPANEL | VIEWMODE_FLAG_LSYSTEMPANEL |
	  VIEWMODE_FLAG_SCRIPTPANEL);

static const uint64_t s_viewModeFlags[] = {
	DEFAULT_FLAGS,			 // Default
	SIMLPE_FLAGS,			 // Simple
	ALL_FLAGS,				 // All
	COMMANDANDCONQUER_FLAGS, // TiberianSun
	COMMANDANDCONQUER_FLAGS, // RedAlert2
	MINECRAFTSKIN_FLAGS,	 // MinecraftSkin
	ACEOFSPADES_FLAGS,		 // AceOfSpades
	STUDIO_FLAGS			 // Studio
};
static_assert(lengthof(s_viewModeFlags) == (int)ViewMode::Max, "Viewmode flags don't match existing viewmodes");

uint64_t viewModeFlags(ViewMode viewMode) {
	if (viewMode == ViewMode::Max) {
		return 0u;
	}
	return s_viewModeFlags[(int)viewMode];
}

static void applyStudioDisplay(bool enable) {
	if (enable) {
		applyShadingMode(ShadingMode::Studio);
		if (core::VarPtr meshMode = core::getVar(cfg::VoxelMeshMode)) {
			meshMode->setVal((int)voxel::SurfaceExtractionType::Cubic);
		}
		if (core::VarPtr mergeQuads = core::getVar(cfg::VoxelMergeQuads)) {
			mergeQuads->setVal(false);
		}
		if (core::VarPtr viewportColor = core::getVar(cfg::VoxEditViewportColor)) {
			viewportColor->setVal("0.91 0.91 0.92 1.0");
		}
		if (core::VarPtr style = core::getVar(cfg::UIStyle)) {
			style->setVal(ImGui::StyleStudio);
		}
		if (core::VarPtr bloom = core::getVar(cfg::ClientBloom)) {
			bloom->setVal(false);
		}
		if (core::VarPtr aabb = core::getVar(cfg::VoxEditShowaabb)) {
			aabb->setVal(false);
		}
		return;
	}

	if (core::VarPtr studioBevel = core::getVar(cfg::RenderStudioBevel)) {
		if (!studioBevel->boolVal()) {
			return;
		}
	}
	applyShadingMode(ShadingMode::Lit);
	if (core::VarPtr meshMode = core::getVar(cfg::VoxelMeshMode)) {
		meshMode->setVal((int)voxel::SurfaceExtractionType::Binary);
	}
	if (core::VarPtr mergeQuads = core::getVar(cfg::VoxelMergeQuads)) {
		mergeQuads->setVal(true);
	}
	if (core::VarPtr viewportColor = core::getVar(cfg::VoxEditViewportColor)) {
		viewportColor->setVal("0 0 0 0");
	}
}

void applyViewModePanelCvars(ViewMode viewMode) {
	const uint64_t flags = viewModeFlags(viewMode);
	core::getVar(cfg::VoxEditShowPalette)->setVal(true);
	core::getVar(cfg::VoxEditShowNormalPalette)->setVal((flags & VIEWMODE_FLAG_NORMALPALETTE) != 0u);
	core::getVar(cfg::VoxEditShowMemento)->setVal((flags & VIEWMODE_FLAG_MEMENTOPANEL) != 0u);
	core::getVar(cfg::VoxEditShowCamera)->setVal((flags & VIEWMODE_FLAG_CAMERAPANEL) != 0u);
	core::getVar(cfg::VoxEditShowLSystem)->setVal((flags & VIEWMODE_FLAG_LSYSTEMPANEL) != 0u);
	core::getVar(cfg::VoxEditShowScript)->setVal((flags & VIEWMODE_FLAG_SCRIPTPANEL) != 0u);
	core::getVar(cfg::VoxEditShowNetwork)->setVal((flags & VIEWMODE_FLAG_NETWORKPANEL) != 0u);
	core::getVar(cfg::VoxEditShowGameMode)->setVal((flags & VIEWMODE_FLAG_GAMEMODEPANEL) != 0u);
	core::getVar(cfg::VoxEditShowAssets)->setVal((flags & VIEWMODE_FLAG_ASSETPANEL) != 0u);
#if USE_YOCTO
	core::getVar(cfg::VoxEditShowRender)->setVal((flags & VIEWMODE_FLAG_RENDERPANEL) != 0u);
#endif
	const bool animations = (flags & VIEWMODE_FLAG_ANIMATIONS) != 0u;
	core::getVar(cfg::VoxEditShowAnimationSettings)->setVal(animations);
	core::getVar(cfg::VoxEditShowAnimationTimeline)->setVal(animations);
	core::getVar(cfg::VoxEditShowScene)->setVal(true);
	core::getVar(cfg::VoxEditShowTools)->setVal(true);
	core::getVar(cfg::VoxEditShowSceneSettings)->setVal(true);
	core::getVar(cfg::UIShowConsole)->setVal(true);
	core::getVar(cfg::VoxEditShowHelp)->setVal(true);
	core::getVar(cfg::VoxEditShowNodeProperties)->setVal((flags & VIEWMODE_FLAG_NODEPROPERTIESPANEL) != 0u);
	core::getVar(cfg::VoxEditShowNodeInspector)->setVal(true);
	core::getVar(cfg::VoxEditShowBrushes)->setVal(true);
	applyStudioDisplay(viewMode == ViewMode::Studio);
}

const char *getViewModeString(ViewMode viewMode) {
	switch (viewMode) {
	case ViewMode::Simple:
		return _("Simple");
	case ViewMode::All:
		return _("All");
	case ViewMode::TiberianSun:
		return _("Tiberian Sun");
	case ViewMode::RedAlert2:
		return _("Red Alert 2");
	case ViewMode::MinecraftSkin:
		return _("Minecraft Skin");
	case ViewMode::AceOfSpades:
		return _("Ace Of Spades");
	case ViewMode::Studio:
		return _("Studio");
	case ViewMode::Max:
	case ViewMode::Default:
		break;
	}
	return _("Default");
}

} // namespace voxedit
