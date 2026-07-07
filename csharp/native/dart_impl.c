/* Native plugin TU: emits the DART implementation from the amalgamation.
 * Build this into dart.dll / libdart.so / libdart.dylib for the Unity plugin
 * (and any consumer that wants a prebuilt native library instead of the
 * plain-.NET compile-on-first-use path). See build.ps1 / build.sh. */
#define DART_IMPLEMENTATION
#include "dart.h"
