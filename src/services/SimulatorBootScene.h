#pragma once

namespace DekiEditor
{

/**
 * @brief The boot scene every new project's desktop simulator starts from.
 *
 * A PlatformSetupComponent whose setup components open the SDL3 window and
 * enable keyboard and mouse; once they are up it loads the project's startup
 * scene.
 *
 * It lives in its own translation unit so the tests can read it without
 * linking ProjectTemplate.cpp, which drags in the platform registry and every
 * firmware builder. The reason it is worth testing at all: the scene names its
 * setup components twice, once as a "type" string and once as a numeric typeId,
 * and the number is a literal inside a string. Nothing tells the compiler when
 * the class it names is renamed, which is how 0.16.0 shipped a template still
 * naming the pre-namespace SDL3 classes — the ids matched nothing, the setup
 * never ran, and a new project came up black.
 */
const char* SimulatorBootSceneJson();

}  // namespace DekiEditor
