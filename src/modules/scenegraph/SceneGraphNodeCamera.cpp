/**
 * @file
 */

#include "SceneGraphNodeCamera.h"
#include "core/StringUtil.h"
#include "scenegraph/SceneGraph.h"
#include "scenegraph/SceneGraphTransform.h"

namespace scenegraph {

SceneGraphNodeCamera::SceneGraphNodeCamera(const core::UUID &uuid) : Super(SceneGraphNodeType::Camera, uuid) {
}

float SceneGraphNodeCamera::farPlane() const {
	return propertyf(PropCamFarPlane);
}

void SceneGraphNodeCamera::setFarPlane(float val) {
	setProperty(PropCamFarPlane, core::string::toString(val));
}

float SceneGraphNodeCamera::nearPlane() const {
	return propertyf(PropCamNearPlane);
}

void SceneGraphNodeCamera::setNearPlane(float val) {
	setProperty(PropCamNearPlane, core::string::toString(val));
}

bool SceneGraphNodeCamera::isOrthographic() const {
	return property(PropCamMode) == Modes[0];
}

void SceneGraphNodeCamera::setOrthographic() {
	setProperty(PropCamMode, Modes[0]);
}

bool SceneGraphNodeCamera::isPerspective() const {
	return property(PropCamMode) == Modes[1];
}

void SceneGraphNodeCamera::setPerspective() {
	setProperty(PropCamMode, Modes[1]);
}

int SceneGraphNodeCamera::width() const {
	return property(PropCamWidth).toInt();
}

void SceneGraphNodeCamera::setWidth(int val) {
	setProperty(PropCamWidth, core::string::toString(val));
}

int SceneGraphNodeCamera::height() const {
	return property(PropCamHeight).toInt();
}

void SceneGraphNodeCamera::setHeight(int val) {
	setProperty(PropCamHeight, core::string::toString(val));
}

int SceneGraphNodeCamera::fieldOfView() const {
	return property(PropCamFov).toInt();
}

void SceneGraphNodeCamera::setFieldOfView(int val) {
	setProperty(PropCamFov, core::string::toString(val));
}

float SceneGraphNodeCamera::aspectRatio() const {
	return property(PropCamAspect).toFloat();
}

void SceneGraphNodeCamera::setAspectRatio(float val) {
	setProperty(PropCamAspect, core::string::toString(val));
}

bool SceneGraphNodeCamera::isTargetRotation() const {
	const core::String &val = property(PropCamRotation);
	if (val.empty()) {
		return true;
	}
	return val != "eye";
}

void SceneGraphNodeCamera::setTargetRotation(bool target) {
	setProperty(PropCamRotation, target ? "target" : "eye");
}

void SceneGraphNodeCamera::setTarget(const glm::vec3 &target) {
	setProperty(PropCamTarget, core::String::format("%f %f %f", target.x, target.y, target.z));
}

glm::vec3 SceneGraphNodeCamera::target() const {
	const core::String &val = property(PropCamTarget);
	if (val.empty()) {
		return glm::vec3(0.0f);
	}
	float out[3] = {0.0f, 0.0f, 0.0f};
	core::string::parseVec3(val, out);
	return glm::vec3(out[0], out[1], out[2]);
}

void SceneGraphNodeCamera::setTargetDistance(float distance) {
	setProperty(PropCamTargetDistance, core::string::toString(distance));
}

float SceneGraphNodeCamera::targetDistance() const {
	const float distance = propertyf(PropCamTargetDistance);
	if (distance <= 0.0f) {
		return 100.0f;
	}
	return distance;
}

} // namespace scenegraph
