// macOS: raw HID through IOKit, for everything GameController will not
// touch.
//
// GameController.framework supports the pads Apple decided to support:
// MFi, Xbox, DualShock/DualSense, Switch Pro. A twenty-year-old USB pad,
// an arcade stick, a flight yoke, a dance mat -- those are invisible to it
// and perfectly visible here, as a numbered pile of buttons and axes. The
// mapping database is what turns that pile back into a controller, which
// is the reason this library carries the database at all.
//
// Elements are read with IOHIDDeviceGetValue rather than a queue and a run
// loop, so the whole backend stays inside poll() and needs no thread and
// no CFRunLoop of its own -- a game's main loop is not obliged to have one.

#include "backend.hpp"
#include "mapping.hpp"

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/hid/IOHIDLib.h>

#import <Foundation/Foundation.h>
#include <objc/message.h>
#include <objc/runtime.h>

namespace gpp::detail {
namespace {

struct Element {
  IOHIDElementRef ref = nullptr;
  CFIndex logical_min = 0;
  CFIndex logical_max = 0;
};

struct Entry {
  IOHIDDeviceRef device = nullptr;
  int slot = -1;
  std::vector<Element> buttons;
  std::vector<Element> axes;
  std::vector<Element> hats;
};

std::string cf_string(CFTypeRef ref) {
  if (ref == nullptr || CFGetTypeID(ref) != CFStringGetTypeID()) {
    return {};
  }
  auto s = static_cast<CFStringRef>(ref);
  const CFIndex len = CFStringGetMaximumSizeForEncoding(CFStringGetLength(s),
                                                        kCFStringEncodingUTF8) +
                      1;
  std::string out(static_cast<std::size_t>(len), '\0');
  if (!CFStringGetCString(s, out.data(), len, kCFStringEncodingUTF8)) {
    return {};
  }
  out.resize(std::char_traits<char>::length(out.c_str()));
  return out;
}

int cf_int(IOHIDDeviceRef device, CFStringRef key) {
  CFTypeRef ref = IOHIDDeviceGetProperty(device, key);
  if (ref == nullptr || CFGetTypeID(ref) != CFNumberGetTypeID()) {
    return 0;
  }
  int value = 0;
  CFNumberGetValue(static_cast<CFNumberRef>(ref), kCFNumberIntType, &value);
  return value;
}

/**
 * @brief Whether GameController.framework has this device.
 *
 * Asked at runtime through the Objective-C runtime rather than by linking
 * GameController, for two reasons. A build with only this backend
 * compiled should not have to link a framework it does not use -- and when
 * GameController is not in the process, NSClassFromString returns nil, the
 * answer is "no", and this backend correctly claims everything. When the
 * other backend *is* compiled, the framework is loaded and answers
 * honestly, so the same pad is not reported twice.
 *
 * `supportsHIDDevice:` is not in the public headers. It is what Apple's own
 * frameworks and SDL both use for this exact question, and it is behind a
 * respondsToSelector check, so the worst case on a system where it is gone
 * is a duplicate device rather than a crash.
 */
bool gamecontroller_owns(IOHIDDeviceRef device) {
  static Class gc_class = NSClassFromString(@"GCController");
  if (gc_class == nil) {
    return false;
  }
  static SEL selector = NSSelectorFromString(@"supportsHIDDevice:");
  if (![gc_class respondsToSelector:selector]) {
    return false;
  }
  using Fn = BOOL (*)(Class, SEL, IOHIDDeviceRef);
  return reinterpret_cast<Fn>(objc_msgSend)(gc_class, selector, device) == YES;
}

bool is_gamepad_usage(IOHIDDeviceRef device) {
  const int page = cf_int(device, CFSTR(kIOHIDPrimaryUsagePageKey));
  const int usage = cf_int(device, CFSTR(kIOHIDPrimaryUsageKey));
  if (page != kHIDPage_GenericDesktop) {
    return false;
  }
  return usage == kHIDUsage_GD_Joystick || usage == kHIDUsage_GD_GamePad ||
         usage == kHIDUsage_GD_MultiAxisController;
}

/// Hat switches report a position within a declared range rather than an
/// angle. The conversion is in mapping.cpp, next to the DirectInput one
/// and tested alongside it.
std::uint8_t hat_mask(CFIndex value, CFIndex min, CFIndex max) {
  return hat_mask_from_position(static_cast<int>(value), static_cast<int>(min),
                                static_cast<int>(max));
}

class IokitBackend final : public Backend {
public:
  const char *name() const noexcept override { return "iokit"; }

  bool init(Host *host) override {
    host_ = host;
    manager_ = IOHIDManagerCreate(kCFAllocatorDefault, kIOHIDOptionsTypeNone);
    if (manager_ == nullptr) {
      return false;
    }
    // Matching on the three desktop usages rather than on nothing: without
    // it the manager hands over every mouse, keyboard and laptop lid
    // sensor on the machine.
    CFMutableArrayRef matches =
        CFArrayCreateMutable(kCFAllocatorDefault, 3, &kCFTypeArrayCallBacks);
    for (int usage : {kHIDUsage_GD_Joystick, kHIDUsage_GD_GamePad,
                      kHIDUsage_GD_MultiAxisController}) {
      CFMutableDictionaryRef dict = CFDictionaryCreateMutable(
          kCFAllocatorDefault, 2, &kCFTypeDictionaryKeyCallBacks,
          &kCFTypeDictionaryValueCallBacks);
      const int page = kHIDPage_GenericDesktop;
      CFNumberRef page_ref =
          CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &page);
      CFNumberRef usage_ref =
          CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &usage);
      CFDictionarySetValue(dict, CFSTR(kIOHIDDeviceUsagePageKey), page_ref);
      CFDictionarySetValue(dict, CFSTR(kIOHIDDeviceUsageKey), usage_ref);
      CFArrayAppendValue(matches, dict);
      CFRelease(page_ref);
      CFRelease(usage_ref);
      CFRelease(dict);
    }
    IOHIDManagerSetDeviceMatchingMultiple(manager_, matches);
    CFRelease(matches);

    if (IOHIDManagerOpen(manager_, kIOHIDOptionsTypeNone) != kIOReturnSuccess) {
      // Refused, usually by a privacy prompt that has not been answered.
      // Not fatal: the game runs, this backend does not.
      CFRelease(manager_);
      manager_ = nullptr;
      return false;
    }
    return true;
  }

  void shutdown() override {
    for (auto &e : entries_) {
      close_entry(*e);
    }
    entries_.clear();
    if (manager_ != nullptr) {
      IOHIDManagerClose(manager_, kIOHIDOptionsTypeNone);
      CFRelease(manager_);
      manager_ = nullptr;
    }
    host_ = nullptr;
  }

  void poll(bool rescan) override {
    if (rescan) {
      scan();
    }
    for (auto &owned : entries_) {
      read(*owned);
    }
  }

private:
  void scan() {
    CFSetRef set = IOHIDManagerCopyDevices(manager_);
    if (set == nullptr) {
      return;
    }
    const CFIndex count = CFSetGetCount(set);
    std::vector<const void *> devices(static_cast<std::size_t>(count));
    CFSetGetValues(set, devices.data());

    // Gone first, so a slot freed here can be taken below.
    for (std::size_t i = entries_.size(); i-- > 0;) {
      Entry &e = *entries_[i];
      const bool still_there =
          std::find(devices.begin(), devices.end(),
                    static_cast<const void *>(e.device)) != devices.end();
      if (!still_there) {
        close_entry(e);
        host_->remove_device(e.slot);
        entries_.erase(entries_.begin() + static_cast<long>(i));
      }
    }

    for (const void *raw : devices) {
      auto device = static_cast<IOHIDDeviceRef>(const_cast<void *>(raw));
      if (find(device) != nullptr || !is_gamepad_usage(device)) {
        continue;
      }
      if (gamecontroller_owns(device)) {
        continue;
      }
      add(device);
    }
    CFRelease(set);
  }

  Entry *find(IOHIDDeviceRef device) {
    for (auto &e : entries_) {
      if (e->device == device) {
        return e.get();
      }
    }
    return nullptr;
  }

  void add(IOHIDDeviceRef device) {
    if (IOHIDDeviceOpen(device, kIOHIDOptionsTypeNone) != kIOReturnSuccess) {
      return;
    }
    auto entry = std::make_unique<Entry>();
    entry->device = device;

    CFArrayRef elements =
        IOHIDDeviceCopyMatchingElements(device, nullptr, kIOHIDOptionsTypeNone);
    if (elements != nullptr) {
      const CFIndex n = CFArrayGetCount(elements);
      for (CFIndex i = 0; i < n; ++i) {
        auto el = static_cast<IOHIDElementRef>(
            const_cast<void *>(CFArrayGetValueAtIndex(elements, i)));
        classify(*entry, el);
      }
      CFRelease(elements);
    }

    if (entry->buttons.empty() && entry->axes.empty()) {
      IOHIDDeviceClose(device, kIOHIDOptionsTypeNone);
      return;
    }

    DeviceDesc desc;
    desc.name =
        cf_string(IOHIDDeviceGetProperty(device, CFSTR(kIOHIDProductKey)));
    if (desc.name.empty()) {
      desc.name = "HID Gamepad";
    }
    desc.vendor =
        static_cast<std::uint16_t>(cf_int(device, CFSTR(kIOHIDVendorIDKey)));
    desc.product =
        static_cast<std::uint16_t>(cf_int(device, CFSTR(kIOHIDProductIDKey)));
    desc.version = static_cast<std::uint16_t>(
        cf_int(device, CFSTR(kIOHIDVersionNumberKey)));
    // Bus 3 -- USB -- unconditionally, which is what SDL writes on this
    // platform whatever the device is really plugged into. It is wrong
    // about Bluetooth and it has to stay wrong, because every macOS line
    // in the database was generated that way.
    desc.guid =
        make_guid(0x03, desc.vendor, desc.product, desc.version, desc.name);
    desc.raw_button_count = static_cast<int>(entry->buttons.size());
    desc.raw_axis_count = static_cast<int>(entry->axes.size());
    desc.raw_hat_count = static_cast<int>(entry->hats.size());
    // Force feedback here means ForceFeedback.framework and a second
    // protocol; the pads that have it are the pads GameController already
    // took. Left off rather than half-done.
    desc.caps.rumble = false;
    desc.handle = entry.get();
    desc.unique_key =
        "iokit:" +
        cf_string(IOHIDDeviceGetProperty(device, CFSTR(kIOHIDSerialNumberKey)));
    if (desc.unique_key == "iokit:") {
      desc.unique_key.clear();
    }

    if (host_->hardware_claimed(desc.vendor, desc.product, desc.unique_key)) {
      IOHIDDeviceClose(device, kIOHIDOptionsTypeNone);
      return;
    }

    const int slot = host_->add_device(desc);
    if (slot < 0) {
      IOHIDDeviceClose(device, kIOHIDOptionsTypeNone);
      return;
    }
    entry->slot = slot;
    entries_.push_back(std::move(entry));
  }

  static void classify(Entry &e, IOHIDElementRef el) {
    if (IOHIDElementGetType(el) == kIOHIDElementTypeCollection) {
      return;
    }
    Element item;
    item.ref = el;
    item.logical_min = IOHIDElementGetLogicalMin(el);
    item.logical_max = IOHIDElementGetLogicalMax(el);

    const uint32_t page = IOHIDElementGetUsagePage(el);
    const uint32_t usage = IOHIDElementGetUsage(el);
    if (page == kHIDPage_Button) {
      e.buttons.push_back(item);
      return;
    }
    if (page != kHIDPage_GenericDesktop) {
      return;
    }
    switch (usage) {
    case kHIDUsage_GD_X:
    case kHIDUsage_GD_Y:
    case kHIDUsage_GD_Z:
    case kHIDUsage_GD_Rx:
    case kHIDUsage_GD_Ry:
    case kHIDUsage_GD_Rz:
    case kHIDUsage_GD_Slider:
    case kHIDUsage_GD_Dial:
    case kHIDUsage_GD_Wheel:
      // Appended in the order the device reports them, not sorted by
      // usage. The database's `a0`, `a1` are positions in exactly this
      // list, and a tidier order would silently rebind every mapping.
      e.axes.push_back(item);
      break;
    case kHIDUsage_GD_Hatswitch:
      e.hats.push_back(item);
      break;
    default:
      break;
    }
  }

  void read(Entry &e) {
    for (std::size_t i = 0; i < e.buttons.size(); ++i) {
      CFIndex v = 0;
      if (value_of(e.device, e.buttons[i], v)) {
        host_->set_raw_button(e.slot, static_cast<int>(i), v != 0);
      }
    }
    for (std::size_t i = 0; i < e.axes.size(); ++i) {
      CFIndex v = 0;
      if (!value_of(e.device, e.axes[i], v)) {
        continue;
      }
      const CFIndex lo = e.axes[i].logical_min;
      const CFIndex hi = e.axes[i].logical_max;
      float normalized = 0.0f;
      if (hi > lo) {
        const double t =
            static_cast<double>(v - lo) / static_cast<double>(hi - lo);
        normalized = static_cast<float>(t * 2.0 - 1.0);
      }
      host_->set_raw_axis(e.slot, static_cast<int>(i), normalized);
    }
    for (std::size_t i = 0; i < e.hats.size(); ++i) {
      CFIndex v = 0;
      if (value_of(e.device, e.hats[i], v)) {
        host_->set_raw_hat(
            e.slot, static_cast<int>(i),
            hat_mask(v, e.hats[i].logical_min, e.hats[i].logical_max));
      }
    }
  }

  static bool value_of(IOHIDDeviceRef device, const Element &el, CFIndex &out) {
    IOHIDValueRef value = nullptr;
    if (IOHIDDeviceGetValue(device, el.ref, &value) != kIOReturnSuccess ||
        value == nullptr) {
      return false;
    }
    // A report longer than a machine word is a device sending something
    // this backend does not understand -- a touchpad blob, a payload -- and
    // IOHIDValueGetIntegerValue on it is undefined.
    if (IOHIDValueGetLength(value) > static_cast<CFIndex>(sizeof(CFIndex))) {
      return false;
    }
    out = IOHIDValueGetIntegerValue(value);
    return true;
  }

  static void close_entry(Entry &e) {
    if (e.device != nullptr) {
      IOHIDDeviceClose(e.device, kIOHIDOptionsTypeNone);
      e.device = nullptr;
    }
  }

  Host *host_ = nullptr;
  IOHIDManagerRef manager_ = nullptr;
  std::vector<std::unique_ptr<Entry>> entries_;
};

} // namespace

Backend *make_backend_iokit() { return new IokitBackend(); }

} // namespace gpp::detail
