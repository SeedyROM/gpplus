# The mapping tests compile the parser directly: it is where every
# interesting decision lives, and going through the public API would mean
# needing hardware to test a string parser.
add_executable(gpplus_mapping_tests test_mapping.cpp ${CMAKE_SOURCE_DIR}/src/mapping.cpp)
target_link_libraries(gpplus_mapping_tests PRIVATE gpplus::gpplus)
target_include_directories(gpplus_mapping_tests PRIVATE
  ${CMAKE_SOURCE_DIR}/src ${CMAKE_BINARY_DIR}/generated)
add_test(NAME mapping COMMAND gpplus_mapping_tests)

if(GPPLUS_BACKEND_VIRTUAL)
  add_executable(gpplus_context_tests test_context.cpp)
  target_link_libraries(gpplus_context_tests PRIVATE gpplus::gpplus)
  add_test(NAME context COMMAND gpplus_context_tests)

  add_executable(gpplus_binding_tests test_binding.cpp)
  target_link_libraries(gpplus_binding_tests PRIVATE gpplus::gpplus)
  add_test(NAME binding COMMAND gpplus_binding_tests)
else()
  message(STATUS
    "gpplus: GPPLUS_BACKEND_VIRTUAL is off, so the context tests are skipped")
endif()

# The uinput test asks the kernel for a real gamepad and drives the evdev
# backend through real device nodes. Gated on the header, like a backend
# is: a machine without it simply does not build the test.
include(CheckSourceCompiles)
check_source_compiles(CXX "
  #include <linux/uinput.h>
  int main() {
    struct uinput_setup setup;
    struct uinput_abs_setup abs;
    (void)setup; (void)abs;
    (void)UI_DEV_SETUP;
    (void)UI_ABS_SETUP;
    return 0;
  }" GPPLUS_PROBE_UINPUT)

if(GPPLUS_PROBE_UINPUT)
  add_executable(gpplus_uinput_tests test_uinput.cpp)
  target_link_libraries(gpplus_uinput_tests PRIVATE gpplus::gpplus)
  add_test(NAME uinput COMMAND gpplus_uinput_tests)
  # The test exits 77 when /dev/uinput is missing or not writable, which is
  # a skip rather than a failure -- the module is a root-level decision, not
  # something a test can arrange for itself.
  set_tests_properties(uinput PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 60)
endif()

# The ViGEmBus test is the Windows counterpart of the uinput one: it asks a
# kernel driver for emulated controllers and drives the XInput and
# DirectInput backends through them. Off by default, because it fetches a
# dependency and can only run where the driver is installed. Turning it on
# where it cannot work is an error rather than a quiet omission, for the same
# reason a required backend is: a test that vanishes is worse than one that
# fails.
option(GPPLUS_TEST_VIGEM
  "Build the ViGEmBus end-to-end test (Windows; needs the ViGEmBus driver to run)" OFF)

if(GPPLUS_TEST_VIGEM)
  if(NOT WIN32)
    message(FATAL_ERROR
      "GPPLUS_TEST_VIGEM is for Windows: ViGEmBus is a Windows driver.")
  endif()
  foreach(backend xinput dinput)
    if(NOT backend IN_LIST GPPLUS_BACKEND_IDS)
      message(FATAL_ERROR
        "GPPLUS_TEST_VIGEM needs the ${backend} backend, and this build has "
        "only: ${GPPLUS_BACKEND_IDS}. Configure with "
        "-DGPPLUS_BACKEND_XINPUT=ON -DGPPLUS_BACKEND_DINPUT=ON.")
    endif()
  endforeach()

  include(FetchContent)
  # Fetched and left alone: SOURCE_SUBDIR points at a directory that does
  # not exist, so FetchContent populates the tree without add_subdirectory().
  # Upstream's CMakeLists declares no link libraries and drags in a resource
  # script this test has no use for, so the one source file is built here.
  # Keep the tag in step with tools/check-foreign-backends.sh, which
  # type-checks the test against the same headers.
  FetchContent_Declare(vigemclient
    GIT_REPOSITORY https://github.com/nefarius/ViGEmClient.git
    GIT_TAG        v1.21.222.0
    GIT_SHALLOW    TRUE
    SOURCE_SUBDIR  gpplus-do-not-add)
  FetchContent_MakeAvailable(vigemclient)

  add_library(gpplus_vigemclient STATIC
    "${vigemclient_SOURCE_DIR}/src/ViGEmClient.cpp")
  target_include_directories(gpplus_vigemclient PUBLIC
    "${vigemclient_SOURCE_DIR}/include")
  target_compile_features(gpplus_vigemclient PRIVATE cxx_std_17)
  target_link_libraries(gpplus_vigemclient PUBLIC setupapi)

  add_executable(gpplus_vigem_tests test_vigem.cpp)
  target_link_libraries(gpplus_vigem_tests PRIVATE
    gpplus::gpplus gpplus_vigemclient)
  add_test(NAME vigem COMMAND gpplus_vigem_tests)
  # Exits 77 when the driver is missing, which is a skip and not a failure.
  # The timeout is generous: a virtual pad can take seconds to enumerate.
  set_tests_properties(vigem PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 180)
endif()
