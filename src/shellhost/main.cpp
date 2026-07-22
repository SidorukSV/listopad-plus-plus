// Placeholder host for the sparse identity package.
//
// A package manifest must name an Executable, and Windows refuses to launch any
// binary in the package's external content directory that the manifest does not
// declare. Naming the editor there would bind it to the package identity, which
// makes the shell draw its taskbar button on an accent-coloured backplate. This
// binary exists purely to satisfy the manifest: the context menu runs through
// the COM surrogate host, so nothing ever executes it.

#include <windows.h>

int WINAPI wWinMain(HINSTANCE, HINSTANCE, wchar_t*, int) { return 0; }
