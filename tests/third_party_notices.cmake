# Every dependency in cmake/Dependencies.cmake has its license text in
# third_party/licenses/ and an entry in THIRD_PARTY_NOTICES.md, and every
# license text the notices link to exists (LICENSE.md 11.1).
#   cmake -DROOT=<repo> -P third_party_notices.cmake

# FetchContent name -> license file (third_party/licenses/<file>.txt).
set(known
    SDL3=SDL3 doctest=doctest vulkan_headers=Vulkan-Headers volk=volk vma=VulkanMemoryAllocator
    spirv_cross=SPIRV-Cross libtess2=libtess2 freetype=FreeType msdfgen=msdfgen
    msdf_atlas_gen=msdf-atlas-gen miniaudio=miniaudio tracy=Tracy imgui=DearImGui
    outfit=Outfit figtree=Figtree jetbrains_mono=JetBrainsMono box2d=box2d wgpu_native=wgpu-native)

file(READ "${ROOT}/cmake/Dependencies.cmake" deps)
file(READ "${ROOT}/THIRD_PARTY_NOTICES.md" notices)
string(REGEX MATCHALL "FetchContent_Declare\\([A-Za-z0-9_]+" declared "${deps}")
set(errors "")
foreach(d IN LISTS declared)
    string(REPLACE "FetchContent_Declare(" "" name "${d}")
    set(file "")
    foreach(pair IN LISTS known)
        if(pair MATCHES "^${name}=(.+)$")
            set(file "${CMAKE_MATCH_1}")
        endif()
    endforeach()
    if(file STREQUAL "")
        string(APPEND errors "  ${name}: new dependency; add its license text to third_party/licenses/, a row to THIRD_PARTY_NOTICES.md and a pair to tests/third_party_notices.cmake\n")
    elseif(NOT EXISTS "${ROOT}/third_party/licenses/${file}.txt")
        string(APPEND errors "  ${name}: third_party/licenses/${file}.txt is missing\n")
    elseif(NOT notices MATCHES "third_party/licenses/${file}\\.txt")
        string(APPEND errors "  ${name}: THIRD_PARTY_NOTICES.md doesn't link third_party/licenses/${file}.txt\n")
    endif()
endforeach()

# Links in the notices point at files that exist.
string(REGEX MATCHALL "third_party/licenses/[A-Za-z0-9_.-]+\\.txt" linked "${notices}")
foreach(l IN LISTS linked)
    if(NOT EXISTS "${ROOT}/${l}")
        string(APPEND errors "  THIRD_PARTY_NOTICES.md links ${l}, which doesn't exist\n")
    endif()
endforeach()

if(NOT errors STREQUAL "")
    message(FATAL_ERROR "third-party notices are out of date:\n${errors}")
endif()
list(LENGTH declared n)
message(STATUS "third-party notices: ${n} dependencies covered")
