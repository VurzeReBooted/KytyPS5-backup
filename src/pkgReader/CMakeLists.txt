include(FetchContent)
find_program(KYTY_RUSTC rustc HINTS "$ENV{HOME}/.cargo/bin" REQUIRED)
FetchContent_Declare(kyty_pkg_decoder
	GIT_REPOSITORY https://github.com/awake-devel/pkg-to-anyps5.git
	GIT_TAG 2e9b12267840f9777f8d825335c4b4938160d9c4)
FetchContent_MakeAvailable(kyty_pkg_decoder)

# Compile only the read-only parser and codec. The extractor CLI is not linked.
foreach(module bytes error fih inner kraken naps pfs source)
	configure_file("${kyty_pkg_decoder_SOURCE_DIR}/src/${module}.rs" "${module}.rs" COPYONLY)
	list(APPEND modules "${CMAKE_CURRENT_BINARY_DIR}/${module}.rs")
endforeach()
configure_file(reader.rs reader.rs COPYONLY)
set(reader_dir "${CMAKE_BINARY_DIR}")
if(KYTY_GENERATOR_IS_MULTI_CONFIG)
	set(reader_dir "${reader_dir}/$<CONFIG>")
endif()
if(APPLE AND CMAKE_OSX_ARCHITECTURES STREQUAL "x86_64")
	set(rust_target --target x86_64-apple-darwin)
endif()
set(reader "${reader_dir}/kyty_pkg_reader${CMAKE_EXECUTABLE_SUFFIX}")
add_custom_command(OUTPUT "${reader}"
	COMMAND ${CMAKE_COMMAND} -E make_directory "${reader_dir}"
	COMMAND "${KYTY_RUSTC}" ${rust_target} --edition=2024 -A dead_code -C opt-level=3
		"${CMAKE_CURRENT_BINARY_DIR}/reader.rs" -o "${reader}"
	DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/reader.rs" ${modules}
	VERBATIM)
add_custom_target(kyty_pkg_reader_build DEPENDS "${reader}")
install(PROGRAMS "${reader}" DESTINATION .)
install(FILES "${kyty_pkg_decoder_SOURCE_DIR}/LICENSE" DESTINATION . RENAME LICENSE.pkg-reader)
if(APPLE AND KYTY_BUILD_LAUNCHER)
	install(PROGRAMS "${reader}" DESTINATION "KytyPS5.app/Contents/Resources")
	install(FILES "${kyty_pkg_decoder_SOURCE_DIR}/LICENSE" DESTINATION "KytyPS5.app/Contents/Resources" RENAME LICENSE.pkg-reader)
endif()
