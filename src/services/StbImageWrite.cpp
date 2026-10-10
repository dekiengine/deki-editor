// stb_image_write's implementation, in a file of its own so it can be compiled
// optimised in every build (see CMakeLists.txt): the editor's screenshots
// (EditorCapture) and play mode frame captures (FrameCaptureService) encode PNGs
// on the main thread, and unoptimised the encoder is about three times slower.

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
