# Backend detection.
#
# The rule this file exists to enforce: a backend is compiled only when the
# thing it needs has been found *and* a probe using it actually built. Not
# "we are on Linux, so assume evdev" -- a musl container without kernel
# headers is Linux too, and a #ifdef __linux__ would hand it a file it
# cannot compile. So detection is a try_compile of the same API the backend
# uses, and its result decides whether the source file is ever passed to
# the compiler.
#
# Each backend ends with two things set in the parent scope:
#   GPPLUS_HAVE_<NAME>   TRUE/FALSE
#   GPPLUS_<NAME>_LIBS   what it has to link
# and, when enabled, appends to GPPLUS_BACKEND_IDS / GPPLUS_BACKEND_SOURCES.

# Captured while this file is being read: inside a function,
# CMAKE_CURRENT_LIST_DIR is whichever file is calling, not this one.
set(_GPPLUS_MODULE_DIR "${CMAKE_CURRENT_LIST_DIR}")

include(CheckSourceCompiles)
include(CMakePushCheckState)

# A tri-state option: AUTO detects, ON requires (and fails the configure if
# detection says no, so that a build meant for a platform cannot silently
# ship without its backend), OFF never builds it.
function(gpplus_backend_option name description)
  set(GPPLUS_BACKEND_${name} "AUTO" CACHE STRING "${description} (AUTO/ON/OFF)")
  set_property(CACHE GPPLUS_BACKEND_${name} PROPERTY STRINGS AUTO ON OFF)
endfunction()

function(_gpplus_resolve name detected out_var)
  set(request "${GPPLUS_BACKEND_${name}}")
  if(request STREQUAL "OFF")
    set(${out_var} FALSE PARENT_SCOPE)
  elseif(request STREQUAL "ON")
    if(NOT detected)
      message(FATAL_ERROR
        "GPPLUS_BACKEND_${name}=ON but its probe did not build. "
        "Set it to AUTO to skip this backend, or install what it needs.")
    endif()
    set(${out_var} TRUE PARENT_SCOPE)
  else()
    set(${out_var} "${detected}" PARENT_SCOPE)
  endif()
endfunction()

function(gpplus_detect_backends)
  set(ids "")
  set(sources "")
  set(libs "")
  set(src_dir "${_GPPLUS_MODULE_DIR}/../src")

  # --- Portable: the synthetic backend ---------------------------------
  # No detection, because there is nothing to detect: it is plain C++ and
  # builds anywhere this library does.
  if(GPPLUS_BACKEND_VIRTUAL)
    list(APPEND ids virtual)
    list(APPEND sources "${src_dir}/backend_virtual.cpp")
  endif()

  # --- Apple: GameController.framework --------------------------------
  if(APPLE)
    find_library(GPPLUS_GAMECONTROLLER_FRAMEWORK GameController)
    find_library(GPPLUS_COREHAPTICS_FRAMEWORK CoreHaptics)
    find_library(GPPLUS_FOUNDATION_FRAMEWORK Foundation)
    set(detected FALSE)
    if(GPPLUS_GAMECONTROLLER_FRAMEWORK AND GPPLUS_COREHAPTICS_FRAMEWORK)
      cmake_push_check_state(RESET)
      set(CMAKE_REQUIRED_LIBRARIES
          "${GPPLUS_GAMECONTROLLER_FRAMEWORK}" "${GPPLUS_COREHAPTICS_FRAMEWORK}"
          "${GPPLUS_FOUNDATION_FRAMEWORK}")
      set(CMAKE_REQUIRED_FLAGS "-fobjc-arc")
      check_source_compiles(OBJCXX "
        #import <GameController/GameController.h>
        #import <CoreHaptics/CoreHaptics.h>
        int main() {
          NSArray<GCController *> *cs = [GCController controllers];
          GCExtendedGamepad *pad = cs.firstObject.extendedGamepad;
          (void)pad.buttonA.isPressed;
          (void)GCHapticsLocalityLeftHandle;
          (void)CHHapticEventTypeHapticContinuous;
          return 0;
        }" GPPLUS_PROBE_GAMECONTROLLER)
      cmake_pop_check_state()
      set(detected ${GPPLUS_PROBE_GAMECONTROLLER})
    endif()
    _gpplus_resolve(GAMECONTROLLER "${detected}" use_it)
    if(use_it)
      list(APPEND ids gamecontroller)
      list(APPEND sources "${src_dir}/backend_gamecontroller.mm")
      list(APPEND libs "${GPPLUS_GAMECONTROLLER_FRAMEWORK}"
                       "${GPPLUS_COREHAPTICS_FRAMEWORK}"
                       "${GPPLUS_FOUNDATION_FRAMEWORK}")
    endif()

    # --- Apple: raw IOKit HID ------------------------------------------
    # For the pads GameController refuses: arcade sticks, flight gear, the
    # twenty-year-old USB thing in the drawer. These arrive as numbered
    # buttons and axes, and the mapping database is what makes them a
    # controller.
    find_library(GPPLUS_IOKIT_FRAMEWORK IOKit)
    find_library(GPPLUS_COREFOUNDATION_FRAMEWORK CoreFoundation)
    set(detected FALSE)
    if(GPPLUS_IOKIT_FRAMEWORK AND GPPLUS_COREFOUNDATION_FRAMEWORK)
      cmake_push_check_state(RESET)
      set(CMAKE_REQUIRED_LIBRARIES "${GPPLUS_IOKIT_FRAMEWORK}"
                                   "${GPPLUS_COREFOUNDATION_FRAMEWORK}")
      check_source_compiles(CXX "
        #include <IOKit/hid/IOHIDLib.h>
        #include <CoreFoundation/CoreFoundation.h>
        int main() {
          IOHIDManagerRef m = IOHIDManagerCreate(kCFAllocatorDefault,
                                                 kIOHIDOptionsTypeNone);
          (void)m;
          return 0;
        }" GPPLUS_PROBE_IOKIT)
      cmake_pop_check_state()
      set(detected ${GPPLUS_PROBE_IOKIT})
    endif()
    _gpplus_resolve(IOKIT "${detected}" use_it)
    if(use_it)
      list(APPEND ids iokit)
      list(APPEND sources "${src_dir}/backend_iokit.mm")
      list(APPEND libs "${GPPLUS_IOKIT_FRAMEWORK}"
                       "${GPPLUS_COREFOUNDATION_FRAMEWORK}")
    endif()
  endif()

  # --- Linux and friends: evdev ----------------------------------------
  # Not gated on "is this Linux" but on "does this compile", which is the
  # same question for FreeBSD's evdev layer and a different question for a
  # Linux container without kernel headers.
  if(NOT APPLE AND NOT WIN32)
    cmake_push_check_state(RESET)
    check_source_compiles(CXX "
      #include <linux/input.h>
      #include <sys/ioctl.h>
      #include <fcntl.h>
      int main() {
        struct input_event ev;
        struct ff_effect fx;
        (void)ev; (void)fx;
        (void)EVIOCGBIT(EV_ABS, 0);
        (void)EVIOCSFF;
        (void)ABS_HAT0X;
        (void)BTN_GAMEPAD;
        return 0;
      }" GPPLUS_PROBE_EVDEV)
    cmake_pop_check_state()
    _gpplus_resolve(EVDEV "${GPPLUS_PROBE_EVDEV}" use_it)
    if(use_it)
      list(APPEND ids evdev)
      list(APPEND sources "${src_dir}/backend_evdev.cpp")
    endif()
  endif()

  # --- Windows: DirectInput 8 -------------------------------------------
  # For the hardware XInput cannot see. Deprecated for fifteen years and
  # still the only Windows API whose enumeration order the database's
  # Windows mappings were written against.
  if(WIN32)
    # The import libraries are linked by name rather than located with
    # find_library first. The linker knows where the SDK keeps them, and
    # whether the probe links is the only question this block is asking; a
    # separate search in front of it is one more way to answer "no" for a
    # reason that has nothing to do with whether the API is there.
    cmake_push_check_state(RESET)
    set(CMAKE_REQUIRED_LIBRARIES dinput8 dxguid)
    check_source_compiles(CXX "
      #define DIRECTINPUT_VERSION 0x0800
      #include <windows.h>
      #include <dinput.h>
      int main() {
        IDirectInput8W *di = 0;
        (void)DirectInput8Create(GetModuleHandleW(0), DIRECTINPUT_VERSION,
                                 IID_IDirectInput8W, (void **)&di, 0);
        (void)DIPROP_VIDPID;
        (void)DIPROP_GUIDANDPATH;
        (void)c_dfDIJoystick2;
        DIJOYSTATE2 state;
        (void)state.rgdwPOV[0];
        return 0;
      }" GPPLUS_PROBE_DINPUT)
    cmake_pop_check_state()
    _gpplus_resolve(DINPUT "${GPPLUS_PROBE_DINPUT}" use_it)
    if(use_it)
      list(APPEND ids dinput)
      list(APPEND sources "${src_dir}/backend_dinput.cpp")
      list(APPEND libs dinput8 dxguid)
    endif()
  endif()

  # --- Windows: XInput --------------------------------------------------
  if(WIN32)
    set(detected FALSE)
    set(xinput_lib "")
    # 1_4 ships with Windows 8 and later; 9_1_0 is the redistributable
    # stub that is present everywhere back to Vista. Neither is searched
    # for by name at runtime here -- the link is static and the probe is
    # what proves the SDK has it. Linked by name, for the reason above.
    foreach(candidate xinput1_4 xinput9_1_0 xinput)
      if(NOT detected)
        cmake_push_check_state(RESET)
        set(CMAKE_REQUIRED_LIBRARIES "${candidate}")
        check_source_compiles(CXX "
          #include <windows.h>
          #include <xinput.h>
          int main() {
            XINPUT_STATE s;
            XINPUT_VIBRATION v = {0, 0};
            (void)XInputGetState(0, &s);
            (void)XInputSetState(0, &v);
            return 0;
          }" GPPLUS_PROBE_XINPUT_${candidate})
        cmake_pop_check_state()
        if(GPPLUS_PROBE_XINPUT_${candidate})
          set(detected TRUE)
          set(xinput_lib "${candidate}")
        endif()
      endif()
    endforeach()
    _gpplus_resolve(XINPUT "${detected}" use_it)
    if(use_it)
      list(APPEND ids xinput)
      list(APPEND sources "${src_dir}/backend_xinput.cpp")
      list(APPEND libs "${xinput_lib}")
    endif()
  endif()

  set(GPPLUS_BACKEND_IDS "${ids}" PARENT_SCOPE)
  set(GPPLUS_BACKEND_SOURCES "${sources}" PARENT_SCOPE)
  set(GPPLUS_BACKEND_LIBS "${libs}" PARENT_SCOPE)
endfunction()
