# XenosRecomp runs on the build host and turns PGR4's Xenos shaders into the
# embedded DXIL/SPIR-V cache. REBLUE_RECOMP selects the shader ABI shared with
# the reblue-derived renderer; PGR4_RECOMP selects PGR4's vertex input
# locations and interpolators, and (through GOTHAM_RECOMP) the vertex formats
# and fetch patterns PGR3 and PGR4 share, and keeps Blue Dragon's shader
# rewrites out. Revision 0x11 containers (XDK 6132) are upstream's own format.
add_subdirectory(XenosRecomp EXCLUDE_FROM_ALL)
target_compile_definitions(XenosRecomp PRIVATE REBLUE_RECOMP PGR4_RECOMP)
