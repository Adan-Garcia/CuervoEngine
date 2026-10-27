# Example plugins (milestone 10.5c, docs/PLUGINS.md). Each is a package whose
# package.toml names its libraries under lib/, so the builds write there, as
# a plugin shipped with binaries would have them.

# The C++ plugin: header-only wrapper, nothing of the engine linked in.
function(cuervo_cpp_plugin name)
    set(dir ${CMAKE_CURRENT_SOURCE_DIR}/${name})
    add_library(${name}_plugin SHARED ${dir}/src/${name}.cpp)
    add_library(${name}_plugin_static STATIC ${dir}/src/${name}.cpp)
    foreach(t ${name}_plugin ${name}_plugin_static)
        target_include_directories(${t} PRIVATE ${PROJECT_SOURCE_DIR})
        # The engine's warnings and determinism flags (no fast-math, no FMA contraction).
        target_link_libraries(${t} PRIVATE project::flags)
        set_target_properties(${t} PROPERTIES
            OUTPUT_NAME ${name}
            POSITION_INDEPENDENT_CODE ON
            CXX_VISIBILITY_PRESET hidden
            LIBRARY_OUTPUT_DIRECTORY ${dir}/lib
            RUNTIME_OUTPUT_DIRECTORY ${dir}/lib
            ARCHIVE_OUTPUT_DIRECTORY ${dir}/lib)
    endforeach()
    # Windows: the DLL is <name>.dll for every toolchain (plain C ABI, package.toml
    # `windows`); the static libraries are per ABI (`windows_static`, `windows_gnu_static`).
    # The MSVC ABI (cl, clang-cl, clang++) writes the DLL's import library as <name>.lib, so
    # its static one needs another name.
    if(MSVC OR CMAKE_CXX_SIMULATE_ID STREQUAL "MSVC")
        set_target_properties(${name}_plugin_static PROPERTIES OUTPUT_NAME ${name}_static)
    elseif(MINGW)
        set_target_properties(${name}_plugin PROPERTIES PREFIX "")
        set_target_properties(${name}_plugin_static PROPERTIES OUTPUT_NAME ${name}_gnu)
    endif()
endfunction()

cuervo_cpp_plugin(orbit)

# The Rust plugin: cargo, offline (the cuervo-plugin crate has no
# dependencies), into this build tree; the libraries are copied to lib/.
find_program(CUERVO_CARGO cargo HINTS $ENV{HOME}/.cargo/bin)
set(CUERVO_EXAMPLE_PLUGINS orbit_plugin orbit_plugin_static)
if(CUERVO_CARGO)
    function(cuervo_rust_plugin name)
        set(dir ${CMAKE_CURRENT_SOURCE_DIR}/${name})
        set(target_dir ${CMAKE_BINARY_DIR}/cargo)
        set(release_dir ${target_dir}/release)
        set(cargo_target "")
        if(MINGW)
            # A MinGW-w64 build links the GNU target's static library into AOT
            # modules (package.toml `windows_gnu_static`); without that Rust target
            # only the DLL is built, from the host's (plain C ABI, any toolchain).
            execute_process(COMMAND rustc --print sysroot OUTPUT_VARIABLE _sysroot
                            OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
            if(_sysroot AND EXISTS "${_sysroot}/lib/rustlib/x86_64-pc-windows-gnu")
                set(cargo_target --target x86_64-pc-windows-gnu)
                set(release_dir ${target_dir}/x86_64-pc-windows-gnu/release)
                set(libs ${name}.dll lib${name}.a)
                set(names ${name}.dll lib${name}_gnu.a)
            else()
                message(STATUS "Rust target x86_64-pc-windows-gnu not installed: the ${name} plugin has no "
                               "MinGW static library (rustup target add x86_64-pc-windows-gnu)")
                set(libs ${name}.dll)
                set(names ${name}.dll)
            endif()
        elseif(WIN32)
            set(libs ${name}.dll ${name}.lib)
            set(names ${libs})
        else()
            set(libs lib${name}.so lib${name}.a)
            set(names ${libs})
        endif()
        set(outputs "")
        set(copy_commands "")
        foreach(lib out IN ZIP_LISTS libs names)
            list(APPEND outputs ${dir}/lib/${out})
            list(APPEND copy_commands COMMAND ${CMAKE_COMMAND} -E copy ${release_dir}/${lib} ${dir}/lib/${out})
        endforeach()
        file(GLOB_RECURSE crate_sources ${PROJECT_SOURCE_DIR}/bindings/rust/cuervo-plugin/src/*.rs)
        add_custom_command(OUTPUT ${outputs}
            COMMAND ${CMAKE_COMMAND} -E env CARGO_TARGET_DIR=${target_dir}
                ${CUERVO_CARGO} build --offline --release --quiet ${cargo_target} --manifest-path ${dir}/Cargo.toml
            COMMAND ${CMAKE_COMMAND} -E make_directory ${dir}/lib
            ${copy_commands}
            DEPENDS ${dir}/Cargo.toml ${dir}/src/lib.rs ${crate_sources}
            COMMENT "cargo: the ${name} plugin (Rust)"
            VERBATIM)
        add_custom_target(${name}_plugin ALL DEPENDS ${outputs})
    endfunction()
    cuervo_rust_plugin(noise)
    list(APPEND CUERVO_EXAMPLE_PLUGINS noise_plugin)
else()
    message(STATUS "cargo not found: the Rust example plugin (examples/plugins/noise) is not built")
endif()

# The C# plugin: `dotnet build` (desktop only; the engine hosts .NET through
# hostfxr, cuervo/script/dotnet_host.hpp).
find_program(CUERVO_DOTNET dotnet)
if(CUERVO_DOTNET)
    set(dir ${CMAKE_CURRENT_SOURCE_DIR}/hues)
    file(GLOB binding_sources ${PROJECT_SOURCE_DIR}/bindings/dotnet/Cuervo.Plugin/*.cs)
    add_custom_command(OUTPUT ${dir}/bin/Hues.dll
        COMMAND ${CMAKE_COMMAND} -E env DOTNET_CLI_TELEMETRY_OPTOUT=1 DOTNET_NOLOGO=1
            ${CUERVO_DOTNET} build ${dir}/Hues.csproj -c Release -o ${dir}/bin --nologo -v quiet
            -p:BaseIntermediateOutputPath=${CMAKE_BINARY_DIR}/dotnet/hues/obj/
        DEPENDS ${dir}/Hues.csproj ${dir}/src/Plugin.cs ${binding_sources}
            ${PROJECT_SOURCE_DIR}/bindings/dotnet/Cuervo.Plugin/Cuervo.Plugin.csproj
        COMMENT "dotnet: the hues plugin (C#)"
        VERBATIM)
    add_custom_target(hues_plugin ALL DEPENDS ${dir}/bin/Hues.dll)
    list(APPEND CUERVO_EXAMPLE_PLUGINS hues_plugin)
else()
    message(STATUS "dotnet not found: the C# example plugin (examples/plugins/hues) is not built")
endif()

add_custom_target(example_plugins DEPENDS ${CUERVO_EXAMPLE_PLUGINS})

