#include "SimulatorBootScene.h"

namespace DekiEditor
{

// The scene the desktop simulator boots: a PlatformSetupComponent whose setup
// components open the SDL3 window (2x, the panel's size) and enable keyboard
// and mouse; once they are up it loads the project's startup scene. The
// typeIds are HashString() of the two component class names (Deki::TypeId<T>()).
//
// They are QUALIFIED names. These are literals in a string, so nothing tells
// the compiler when the class they name moves: 0.16.0 put the SDL3 components
// into DekiSdl3 and left these two hashes naming classes that no longer exist.
// An ObjectRef whose componentTypeId matches nothing resolves to null, so the
// display and input setup never ran and a new project came up black. The load
// path now maps a pre-rename id through DEKI_FORMER_NAME, which rescues
// projects already created, but a template should write the current ids.
// SimulatorBootSceneTests checks every typeId against the name written beside
// it, so the next rename fails a test instead of shipping.
static const char* const kSimulatorBootScene = R"json({
  "objects": [
    {
      "active": true,
      "children": [
        {
          "active": true,
          "children": [],
          "components": [
            {
              "class": "SDL3DisplaySetup",
              "properties": { "windowWidth": 320, "windowHeight": 240, "windowScale": 2 },
              "type": "DekiSdl3::SDL3DisplaySetup"
            }
          ],
          "id": 600,
          "name": "SDL3 Display",
          "transform": { "rotation": 0.0, "scaleX": 1.0, "scaleY": 1.0, "x": 0.0, "y": 0.0 }
        },
        {
          "active": true,
          "children": [],
          "components": [
            {
              "class": "SDL3InputSetup",
              "properties": { "enableKeyboard": true, "enableMouse": true },
              "type": "DekiSdl3::SDL3InputSetup"
            }
          ],
          "id": 601,
          "name": "SDL3 Input",
          "transform": { "rotation": 0.0, "scaleX": 1.0, "scaleY": 1.0, "x": 0.0, "y": 0.0 }
        }
      ],
      "components": [
        {
          "class": "PlatformSetupComponent",
          "properties": {
            "setupComponents": [ { "id": 600, "typeId": 4013418057 }, { "id": 601, "typeId": 577352591 } ]
          },
          "type": "Deki::PlatformSetupComponent"
        }
      ],
      "id": 1,
      "name": "Setup",
      "transform": { "rotation": 0.0, "scaleX": 1.0, "scaleY": 1.0, "x": 0.0, "y": 0.0 }
    }
  ]
}
)json";

const char* SimulatorBootSceneJson()
{
    return kSimulatorBootScene;
}

}  // namespace DekiEditor
