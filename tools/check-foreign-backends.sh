#!/usr/bin/env bash
#
# Type-check the backends this machine cannot build.
#
# Two of the four platform backends can never be compiled by the person
# writing them: whichever machine you are on, two of them are for another
# one. CI catches that eventually, but "eventually" is after a push, and
# the mistakes involved are typos and wrong struct field names -- things a
# compiler answers in a second if it can be handed the right headers.
#
# So hand it the right headers. This fetches the real Linux UAPI headers
# and the real mingw-w64 Windows headers, and runs clang over the evdev
# and XInput backends with -fsyntax-only, which does full semantic
# analysis without needing a linker, a libc or a target toolchain.
#
# What it proves: the code parses and type-checks against the genuine API
# -- struct fields, ioctl names, constants, signatures.
# What it does not prove: that it links, that it runs, or that MSVC agrees
# about warnings. That is what .github/workflows/ci.yml is for.
#
# Usage: tools/check-foreign-backends.sh [cache-dir]

set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cache="${1:-${TMPDIR:-/tmp}/gpplus-foreign-headers}"
uapi="$cache/uapi"
mingw="$cache/mingw"

fetch_linux_headers() {
  [ -f "$uapi/linux/uinput.h" ] && return 0
  echo "fetching Linux UAPI headers into $uapi"
  mkdir -p "$uapi/linux" "$uapi/asm-generic" "$uapi/asm"
  local base="https://raw.githubusercontent.com/torvalds/linux/master/include/uapi"
  local f
  for f in linux/input.h linux/input-event-codes.h linux/uinput.h \
           linux/ioctl.h linux/types.h \
           linux/posix_types.h linux/stddef.h linux/const.h \
           asm-generic/ioctl.h asm-generic/posix_types.h asm-generic/int-ll64.h; do
    curl -sfL -o "$uapi/$f" "$base/$f"
  done

  # What `make headers_install` would do for us: the arch indirections, the
  # word size, and dropping the kernel-internal annotations.
  echo '#include <asm-generic/int-ll64.h>' > "$uapi/asm/types.h"
  echo '#include <asm-generic/posix_types.h>' > "$uapi/asm/posix_types.h"
  printf '#define __BITS_PER_LONG 64\n' > "$uapi/asm/bitsperlong.h"
  printf '#define __BITS_PER_LONG 64\n#define __BITS_PER_LONG_LONG 64\n' \
    > "$uapi/asm-generic/bitsperlong.h"
  : > "$uapi/linux/compiler_types.h"
  printf '#define __user\n#include <asm-generic/ioctl.h>\n' > "$uapi/prefix.h"
  # The "don't use kernel headers from userspace" warning is aimed at
  # people compiling programs, which is not what this is.
  perl -pi -e 's{^#warning "Attempt to use kernel headers.*$}{}' "$uapi/linux/types.h"
}

fetch_windows_headers() {
  [ -f "$mingw/headers/include/xinput.h" ] && return 0
  echo "fetching mingw-w64 headers into $mingw"
  mkdir -p "$mingw"
  curl -sfL -o "$mingw/mingw.tar.gz" \
    "https://github.com/mingw-w64/mingw-w64/archive/refs/tags/v12.0.0.tar.gz"
  mkdir -p "$mingw/headers"
  tar xzf "$mingw/mingw.tar.gz" --strip-components=2 -C "$mingw/headers" \
    "mingw-w64-12.0.0/mingw-w64-headers/include"
  mv "$mingw/headers/include" "$mingw/headers/tmp" 2>/dev/null || true
  [ -d "$mingw/headers/tmp" ] && { rm -rf "$mingw/headers/include"; \
    mv "$mingw/headers/tmp" "$mingw/headers/include"; }

  # Only the base typedefs are stubbed. Every XInput struct, constant and
  # prototype comes from the real header, which is the point -- a stub of
  # those would just be this file agreeing with itself.
  mkdir -p "$mingw/shim"
  # GUID and its reference semantics. Ours rather than mingw's, because
  # mingw's guiddef.h pulls in the generated _mingw.h and from there the
  # whole CRT. The detail that matters is that REFGUID is a *reference* in
  # C++ and a pointer in C -- DIPROP_VIDPID and friends are integers cast
  # through it, so getting this wrong would make every property call fail
  # to compile here and compile fine on Windows, or worse, the reverse.
  cat > "$mingw/shim/guiddef.h" <<'HEADER'
#ifndef GPPLUS_GUIDDEF_SHIM
#define GPPLUS_GUIDDEF_SHIM
typedef struct _GUID {
  unsigned long Data1;
  unsigned short Data2;
  unsigned short Data3;
  unsigned char Data4[8];
} GUID;
#ifdef __cplusplus
#define REFGUID const GUID &
#define REFIID const GUID &
#define REFCLSID const GUID &
inline int IsEqualGUID(REFGUID a, REFGUID b) {
  return __builtin_memcmp(&a, &b, sizeof(GUID)) == 0;
}
#else
#define REFGUID const GUID *
#define REFIID const GUID *
#endif
#define DEFINE_GUID(n, a, b, c, d, e, f, g, h, i, j, k) extern const GUID n;
typedef GUID *LPGUID;
typedef GUID IID;
typedef GUID CLSID;
#endif
HEADER

  cat > "$mingw/shim/windef.h" <<'HEADER'
#ifndef GPPLUS_WINDEF_SHIM
#define GPPLUS_WINDEF_SHIM
#include <stdint.h>
#include <guiddef.h>
typedef uint8_t BYTE;
typedef uint16_t WORD;
typedef uint32_t DWORD;
typedef int16_t SHORT;
typedef int32_t LONG;
typedef int BOOL;
typedef int WINBOOL;
typedef unsigned long long ULONGLONG;
typedef unsigned long long UINT_PTR;
typedef long long INT_PTR;
typedef long LONG_PTR;
typedef char CHAR;
typedef wchar_t WCHAR;
typedef void *PVOID;
typedef void *LPVOID;
typedef void *HANDLE;
typedef void *HWND;
typedef void *HINSTANCE;
typedef void *HMODULE;
typedef void *HICON;
typedef void *HCURSOR;
typedef void *HBRUSH;
typedef void *HMENU;
typedef long HRESULT;
typedef const char *LPCSTR;
typedef const wchar_t *LPCWSTR;
typedef char *LPSTR;
typedef wchar_t *LPWSTR;
typedef LONG *LPLONG;
typedef DWORD *LPDWORD;
typedef BYTE *LPBYTE;
typedef const void *LPCVOID;
typedef unsigned int UINT;
typedef WORD ATOM;
typedef LONG_PTR LPARAM;
typedef UINT_PTR WPARAM;
typedef LONG_PTR LRESULT;
typedef struct _RECT { LONG left, top, right, bottom; } RECT;
typedef struct _POINT { LONG x, y; } POINT;
typedef struct _FILETIME { DWORD dwLowDateTime, dwHighDateTime; } FILETIME;
typedef LRESULT (*WNDPROC)(HWND, UINT, WPARAM, LPARAM);
typedef struct tagWNDCLASSEXW {
  UINT cbSize, style;
  WNDPROC lpfnWndProc;
  int cbClsExtra, cbWndExtra;
  HINSTANCE hInstance;
  HICON hIcon;
  HCURSOR hCursor;
  HBRUSH hbrBackground;
  LPCWSTR lpszMenuName, lpszClassName;
  HICON hIconSm;
} WNDCLASSEXW;
#define WINAPI
#define CALLBACK
#define WINBASEAPI
#define __GNU_EXTENSION
#define ERROR_SUCCESS 0L
#define ERROR_DEVICE_NOT_CONNECTED 1167L
#define ERROR_CLASS_ALREADY_EXISTS 1410L
#define MAX_PATH 260
#define CP_UTF8 65001
#define HWND_MESSAGE ((HWND)(LONG_PTR)-3)
#define LOWORD(v) ((WORD)((UINT_PTR)(v) & 0xFFFF))
#define HIWORD(v) ((WORD)(((UINT_PTR)(v) >> 16) & 0xFFFF))
#define FAILED(hr) ((HRESULT)(hr) < 0)
#define SUCCEEDED(hr) ((HRESULT)(hr) >= 0)
#ifdef __cplusplus
extern "C" {
#endif
ULONGLONG GetTickCount64(void);
DWORD GetLastError(void);
HMODULE GetModuleHandleW(LPCWSTR);
ATOM RegisterClassExW(const WNDCLASSEXW *);
HWND CreateWindowExW(DWORD, LPCWSTR, LPCWSTR, DWORD, int, int, int, int, HWND,
                     HMENU, HINSTANCE, LPVOID);
BOOL DestroyWindow(HWND);
LRESULT DefWindowProcW(HWND, UINT, WPARAM, LPARAM);
int WideCharToMultiByte(UINT, DWORD, LPCWSTR, int, LPSTR, int, LPCSTR, BOOL *);
HMODULE LoadLibraryW(LPCWSTR);
void *GetProcAddress(HMODULE, LPCSTR);
#ifdef __cplusplus
}
#endif
#endif
HEADER

  # The COM plumbing dinput.h reaches for through objbase.h -> rpc.h -> the
  # whole CRT. Only the macros are ours; GUID and its comparisons come from
  # mingw's real guiddef.h, and every DirectInput struct, constant and
  # method signature from its real dinput.h -- which is the point, since a
  # stub of those would be the check agreeing with itself.
  cat > "$mingw/shim/objbase.h" <<'HEADER'
#ifndef GPPLUS_OBJBASE_SHIM
#define GPPLUS_OBJBASE_SHIM
#include <windef.h>

typedef unsigned long ULONG;
typedef void *LPUNKNOWN;

#define interface struct
#define STDMETHODCALLTYPE
#define STDMETHOD(m) virtual HRESULT STDMETHODCALLTYPE m
#define STDMETHOD_(t, m) virtual t STDMETHODCALLTYPE m
#define PURE = 0
#define THIS_
#define THIS void
#define DECLARE_INTERFACE(i) struct i
#define DECLARE_INTERFACE_(i, b) struct i : public b
#define CONST_VTBL
#define FAR
#define S_OK ((HRESULT)0L)
/* winnt.h's A/W indirection, which dinput.h uses to name each generic
   struct after its ANSI twin. */
#define WINELIB_NAME_AW(x) x##A
#define DECL_WINELIB_TYPE_AW(type) typedef WINELIB_NAME_AW(type) type;

struct IUnknown {
  STDMETHOD(QueryInterface)(THIS_ REFIID, void **) PURE;
  STDMETHOD_(ULONG, AddRef)(THIS) PURE;
  STDMETHOD_(ULONG, Release)(THIS) PURE;
};
#endif
HEADER

  printf '/* empty: mingw dx helper, nothing needed for a syntax check */\n' \
    > "$mingw/shim/_mingw_dxhelper.h"
  cp "$mingw/shim/windef.h" "$mingw/shim/windows.h"
}

# The ViGEm client headers the Windows end-to-end test is written against.
# Pinned to the same tag tests/CMakeLists.txt fetches, so this is a check of
# the API that will actually be built. Only the base typedefs are stubbed,
# as above; every function, struct and constant is the real header's.
vigem_tag="v1.21.222.0"
fetch_vigem_headers() {
  local dir="$cache/vigem"
  [ -f "$dir/include/ViGEm/Client.h" ] && [ -f "$dir/tag-$vigem_tag" ] && return 0
  echo "fetching ViGEmClient $vigem_tag headers into $dir"
  rm -rf "$dir"
  mkdir -p "$dir/include/ViGEm/km" "$dir/shim"
  local base="https://raw.githubusercontent.com/nefarius/ViGEmClient/$vigem_tag"
  local f
  for f in Client.h Common.h Util.h km/BusShared.h; do
    curl -sfL -o "$dir/include/ViGEm/$f" "$base/include/ViGEm/$f"
  done

  # A Windows.h of our own, named the way the test spells it. It sits ahead
  # of the mingw shim directory on the include path, and pulls in that
  # shim's windef.h for the types the backends already needed.
  cat > "$dir/shim/Windows.h" <<'HEADER'
#ifndef GPPLUS_VIGEM_WINDOWS_SHIM
#define GPPLUS_VIGEM_WINDOWS_SHIM
#include <windef.h>
typedef unsigned char UCHAR;
typedef unsigned short USHORT;
typedef unsigned long ULONG;
typedef ULONG *PULONG;
typedef unsigned char BOOLEAN;
typedef void VOID;
#define FORCEINLINE inline
#define RtlZeroMemory(d, n) __builtin_memset((d), 0, (n))
/* SAL annotations: static-analysis hints with no effect on the types. */
#define _Function_class_(x)
#define _In_
#define _Out_
#define _Inout_
#define _In_opt_
#define _Out_opt_
#endif
HEADER
  : > "$dir/shim/pshpack1.h"
  : > "$dir/shim/poppack.h"
  : > "$dir/tag-$vigem_tag"
}

# The generated headers the backends include come from a real configure of
# this project; any build directory with them will do.
generated=""
for candidate in "$root/build/generated" "$root"/build*/generated; do
  if [ -f "$candidate/gpplus/config.hpp" ]; then
    generated="$candidate"
    break
  fi
done
if [ -z "$generated" ]; then
  echo "no configured build directory found; run cmake -S . -B build first" >&2
  exit 1
fi

CXX="${CXX:-clang++}"
common=(-fsyntax-only -std=c++17 -Wall -Wextra
        -I"$root/include" -I"$root/src" -I"$generated")
status=0

fetch_linux_headers
for linux_src in src/backend_evdev.cpp tests/test_uinput.cpp; do
  echo "checking $linux_src against Linux UAPI headers"
  if "$CXX" "${common[@]}" -include "$uapi/prefix.h" -I"$uapi" \
       "$root/$linux_src"; then
    echo "  ok"
  else
    echo "  FAILED"
    status=1
  fi
done

fetch_windows_headers
for win in xinput dinput; do
  echo "checking src/backend_$win.cpp against mingw-w64 headers"
  if "$CXX" "${common[@]}" -I"$mingw/shim" -I"$mingw/headers/include" \
       "$root/src/backend_$win.cpp"; then
    echo "  ok"
  else
    echo "  FAILED"
    status=1
  fi
done

fetch_vigem_headers
echo "checking tests/test_vigem.cpp against mingw-w64 and ViGEmClient headers"
if "$CXX" "${common[@]}" -I"$cache/vigem/shim" -I"$cache/vigem/include" \
     -I"$mingw/shim" -I"$mingw/headers/include" "$root/tests/test_vigem.cpp"; then
  echo "  ok"
else
  echo "  FAILED"
  status=1
fi

exit $status
