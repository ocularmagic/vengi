set(LIB voxelpathtracer)
set(SRCS
	PathTracer.cpp PathTracer.h IPathTracer.h
	PathTracerState.h
	PathTracerSampling.h
	PathTracerCamera.cpp PathTracerCamera.h
	PathTracerPrimary.cpp PathTracerPrimary.h
	PathTracerMaterial.h
	PathTracerScene.h
	PathTracerTraversal.cpp PathTracerTraversal.h
	PathTracerTraversalWGSL.h
	PathTracerWebGPU.cpp PathTracerWebGPU.h
	Appearance.cpp Appearance.h
	PathTracerHdri.cpp PathTracerHdri.h
	VoxelDDAPathTracer.cpp VoxelDDAPathTracer.h
)

engine_add_module(TARGET ${LIB} SRCS ${SRCS} DEPENDENCIES yocto voxelrender image io stb_image tinyexr)

set(TEST_SRCS
	tests/PathTracerTest.cpp
	tests/webgpu/PathTracerTraversalWebGPU.html
	tests/webgpu/README.md
)
set_source_files_properties(
	tests/webgpu/PathTracerTraversalWebGPU.html
	tests/webgpu/README.md
	PROPERTIES HEADER_FILE_ONLY TRUE
)
set(TEST_FILES
	tests/hmec.vxl
)
gtest_suite_begin(tests-${LIB} TEMPLATE ${ROOT_DIR}/src/modules/core/tests/main.cpp.in)
gtest_suite_sources(tests-${LIB} ${TEST_SRCS})
gtest_suite_files(tests-${LIB} ${TEST_FILES})
gtest_suite_deps(tests-${LIB} ${LIB} test-app voxelformat)
gtest_suite_end(tests-${LIB})
