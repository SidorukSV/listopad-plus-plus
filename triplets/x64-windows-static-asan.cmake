set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE static)
set(VCPKG_LIBRARY_LINKAGE static)

# Keep the complete static dependency graph under the same AddressSanitizer
# and MSVC STL annotation ABI as the application.
set(VCPKG_C_FLAGS "/fsanitize=address /Zi")
set(VCPKG_CXX_FLAGS "/fsanitize=address /Zi")
set(VCPKG_LINKER_FLAGS "/INCREMENTAL:NO")
