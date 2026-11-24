// ==WindhawkMod==
// @id              bluetooth-battery-tray
// @name            Device Battery in Taskbar
// @description     Shows the battery level of connected Bluetooth and wireless devices next to the system tray (Windows 11)
// @version         1.0
// @author          kadmielp
// @github          https://github.com/kadmielp
// @include         explorer.exe
// @architecture    x86-64
// @compilerOptions -lole32 -loleaut32 -lruntimeobject -lshell32 -lsetupapi -lhid -lcfgmgr32
// ==/WindhawkMod==

// Source code is published under The GNU General Public License v3.0.
//
// Parts of the taskbar access code (GetTaskbarXamlRoot, RunFromWindowThread,
// FindSystemTrayFrame and the XAML tree helpers) are adapted from the
// "Taskbar tray icon spacing and grid" mod by m417z:
// https://github.com/ramensoftware/windhawk-mods
//
// The Razer mouse and XInput controller reading, the charging indicator and
// the display modes are based on the "Wireless Battery Indicator" mod by
// simonzk27:
// https://github.com/simonzk27/windhawk-mods/blob/main/mods/taskbar-wireless-battery.wh.cpp

// ==WindhawkModReadme==
/*
# Device Battery in Taskbar

Automatically detects connected Bluetooth devices (headphones, mice, keyboards,
controllers, pens...) of any brand and shows an icon plus the battery level of
each one in the taskbar, next to the Wi-Fi / volume / battery button.

* A device appears when it connects and disappears when it disconnects.
* Only devices that report their battery level to Windows are shown (the same
  value that appears in Settings > Bluetooth & devices).
* Read directly from the device, without extra software:
  * PS4 / PS5 controllers (Bluetooth or USB), with charging state.
  * Xbox controllers through USB or the Xbox Wireless Adapter (4 approximate
    levels, shown as "~65%").
  * Razer mice on the HyperSpeed dongle or cable, with charging state.
  * JBL Quantum 350 Wireless headsets on their USB receiver.
* Other devices that use a USB receiver (2.4 GHz dongle) don't report battery
  to Windows and are not supported.

Hover a device to see its name, click it to open the Bluetooth settings.

Only Windows 11 is supported.
*/
// ==/WindhawkModReadme==

// ==WindhawkModSettings==
/*
- position: leftOfTray
  $name: Position
  $description: >-
    Only "Left of the tray" leaves the space of the taskbar apps alone. The
    other options make the tray wider, which can move apps into the "..."
    overflow menu.
  $options:
  - leftOfTray: Left of the tray
  - beforeControlCenter: Left of the Wi-Fi / volume / battery button
  - afterControlCenter: Between the Wi-Fi / volume / battery button and the clock
  - beforeTrayIcons: Left of the tray icons
- displayMode: iconAndPercent
  $name: Display mode
  $options:
  - iconAndPercent: Icon and percentage
  - iconOnly: Icon only
  - percentOnly: Percentage only
  - batteryBar: Icon and battery bar
- showChargingIndicator: true
  $name: Show charging indicator
  $description: A lightning bolt while the device charges, for devices that report it (PlayStation controllers, Razer mice).
- showOnlyBelow: 100
  $name: Only show devices at or below this battery level (%)
  $description: 100 shows every device. For example, 30 shows a device only when its battery is at 30% or less.
- lowBatteryThreshold: 20
  $name: Low battery level (%)
- lowBatteryColor: "#FF6B6B"
  $name: Low battery color
  $description: Color used at or below the low battery level, in the #RRGGBB format. Leave empty to disable.
- colorIconWhenLow: true
  $name: Also color the icon when the battery is low
- textColor: ""
  $name: Text and icon color
  $description: Leave empty to follow the taskbar theme. Format #RRGGBB.
- fontFamily: ""
  $name: Font
  $description: Leave empty for the default taskbar font.
- fontSize: 14
  $name: Text size
- bold: false
  $name: Bold text
- iconSize: 15
  $name: Icon size
- iconTextSpacing: 4
  $name: Spacing between icon and percentage
- textVerticalOffset: 0
  $name: Percentage vertical offset
  $description: Moves the percentage up (negative) or down (positive), in pixels, to fine-tune its alignment with the icon.
- deviceSpacing: 12
  $name: Spacing between devices
- marginLeft: 6
  $name: Left margin
- marginRight: 6
  $name: Right margin
- showTooltip: true
  $name: Show device name on hover
- openSettingsOnClick: true
  $name: Open Bluetooth settings on click
- refreshInterval: 15
  $name: Refresh interval (seconds)
  $description: How often devices and battery levels are checked.
- excludedDevices: [""]
  $name: Hidden devices
  $description: Devices whose name contains any of these texts are not shown (case insensitive).
- iconOverrides:
  - - name: ""
      $name: Device name contains
    - type: headphones
      $name: Icon
      $options:
      - headphones: Headphones
      - headset: Headset
      - mouse: Mouse
      - keyboard: Keyboard
      - gamepad: Game controller
      - speaker: Speaker
      - phone: Phone
      - pen: Pen / tablet
      - generic: Bluetooth
  $name: Icon overrides
  $description: Choose the icon for a device when the automatic detection gets it wrong.
*/
// ==/WindhawkModSettings==

#include <windhawk_utils.h>

#include <shellapi.h>
#include <setupapi.h>
#include <cfgmgr32.h>
#include <devpropdef.h>

#include <xinput.h>

extern "C" {
#include <hidsdi.h>
}

#include <algorithm>
#include <atomic>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#undef GetCurrentTime

#include <winrt/Windows.Devices.Enumeration.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.UI.Xaml.Controls.h>
#include <winrt/Windows.UI.Xaml.Input.h>
#include <winrt/Windows.UI.Xaml.Media.h>
#include <winrt/Windows.UI.Text.h>
#include <winrt/Windows.UI.Xaml.h>
#include <winrt/base.h>

using namespace winrt::Windows::UI::Xaml;
using winrt::Windows::Devices::Enumeration::DeviceInformation;
using winrt::Windows::Devices::Enumeration::DeviceInformationKind;
using winrt::Windows::Foundation::IInspectable;
using winrt::Windows::Foundation::IPropertyValue;
using winrt::Windows::Foundation::PropertyType;

// ---------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------

enum class Position {
    leftOfTray,
    beforeControlCenter,
    afterControlCenter,
    beforeTrayIcons
};

enum class DeviceType {
    generic,
    headphones,
    headset,
    mouse,
    keyboard,
    gamepad,
    speaker,
    phone,
    pen,
};

struct IconOverride {
    std::wstring name;
    DeviceType type;
};

enum class DisplayMode { iconAndPercent, iconOnly, percentOnly, batteryBar };

struct Settings {
    Position position = Position::leftOfTray;
    DisplayMode displayMode = DisplayMode::iconAndPercent;
    bool showChargingIndicator = true;
    std::wstring fontFamily;
    bool bold = false;
    int showOnlyBelow = 100;
    int lowBatteryThreshold = 20;
    std::optional<winrt::Windows::UI::Color> lowBatteryColor;
    bool colorIconWhenLow = true;
    std::optional<winrt::Windows::UI::Color> textColor;
    int fontSize = 14;
    int iconSize = 15;
    int iconTextSpacing = 4;
    int textVerticalOffset = 0;
    int deviceSpacing = 12;
    int marginLeft = 6;
    int marginRight = 6;
    bool showTooltip = true;
    bool openSettingsOnClick = true;
    int refreshInterval = 15;
    std::vector<std::wstring> excludedDevices;
    std::vector<IconOverride> iconOverrides;
    unsigned generation = 0;
};

std::mutex g_settingsMutex;
Settings g_settings;

std::atomic<bool> g_unloading;
HANDLE g_stopEvent;
HANDLE g_wakeEvent;
std::thread g_workerThread;

std::wstring ToLower(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), ::towlower);
    return s;
}

bool ContainsNoCase(const std::wstring& haystack, const std::wstring& needle) {
    if (needle.empty()) {
        return false;
    }
    return ToLower(haystack).find(ToLower(needle)) != std::wstring::npos;
}

std::optional<winrt::Windows::UI::Color> ParseColor(std::wstring s) {
    s.erase(std::remove_if(s.begin(), s.end(), ::iswspace), s.end());
    if (!s.empty() && s[0] == L'#') {
        s.erase(0, 1);
    }
    if (s.size() != 6 && s.size() != 8) {
        return std::nullopt;
    }
    for (wchar_t c : s) {
        if (!iswxdigit(c)) {
            return std::nullopt;
        }
    }
    unsigned long v = wcstoul(s.c_str(), nullptr, 16);
    winrt::Windows::UI::Color color{};
    if (s.size() == 6) {
        color.A = 0xFF;
        color.R = (v >> 16) & 0xFF;
        color.G = (v >> 8) & 0xFF;
        color.B = v & 0xFF;
    } else {
        color.A = (v >> 24) & 0xFF;
        color.R = (v >> 16) & 0xFF;
        color.G = (v >> 8) & 0xFF;
        color.B = v & 0xFF;
    }
    return color;
}

DeviceType DeviceTypeFromString(const std::wstring& s) {
    static const std::pair<const wchar_t*, DeviceType> kMap[] = {
        {L"headphones", DeviceType::headphones},
        {L"headset", DeviceType::headset},
        {L"mouse", DeviceType::mouse},
        {L"keyboard", DeviceType::keyboard},
        {L"gamepad", DeviceType::gamepad},
        {L"speaker", DeviceType::speaker},
        {L"phone", DeviceType::phone},
        {L"pen", DeviceType::pen},
    };
    for (auto& [name, type] : kMap) {
        if (s == name) {
            return type;
        }
    }
    return DeviceType::generic;
}

std::wstring GetString(PCWSTR name, int index = -1) {
    PCWSTR value = index >= 0 ? Wh_GetStringSetting(name, index)
                              : Wh_GetStringSetting(name);
    std::wstring result = value ? value : L"";
    Wh_FreeStringSetting(value);
    return result;
}

void LoadSettings() {
    Settings s;

    std::wstring position = GetString(L"position");
    if (position == L"beforeControlCenter") {
        s.position = Position::beforeControlCenter;
    } else if (position == L"afterControlCenter") {
        s.position = Position::afterControlCenter;
    } else if (position == L"beforeTrayIcons") {
        s.position = Position::beforeTrayIcons;
    }

    std::wstring displayMode = GetString(L"displayMode");
    if (displayMode == L"iconOnly") {
        s.displayMode = DisplayMode::iconOnly;
    } else if (displayMode == L"percentOnly") {
        s.displayMode = DisplayMode::percentOnly;
    } else if (displayMode == L"batteryBar") {
        s.displayMode = DisplayMode::batteryBar;
    }
    s.showChargingIndicator = Wh_GetIntSetting(L"showChargingIndicator");
    s.fontFamily = GetString(L"fontFamily");
    s.bold = Wh_GetIntSetting(L"bold");
    s.showOnlyBelow = std::clamp(Wh_GetIntSetting(L"showOnlyBelow"), 0, 100);
    s.lowBatteryThreshold =
        std::clamp(Wh_GetIntSetting(L"lowBatteryThreshold"), 0, 100);
    s.lowBatteryColor = ParseColor(GetString(L"lowBatteryColor"));
    s.colorIconWhenLow = Wh_GetIntSetting(L"colorIconWhenLow");
    s.textColor = ParseColor(GetString(L"textColor"));
    s.fontSize = std::clamp(Wh_GetIntSetting(L"fontSize"), 6, 40);
    s.iconSize = std::clamp(Wh_GetIntSetting(L"iconSize"), 6, 40);
    s.iconTextSpacing = std::clamp(Wh_GetIntSetting(L"iconTextSpacing"), 0, 50);
    s.textVerticalOffset =
        std::clamp(Wh_GetIntSetting(L"textVerticalOffset"), -20, 20);
    s.deviceSpacing = std::clamp(Wh_GetIntSetting(L"deviceSpacing"), 0, 100);
    s.marginLeft = std::clamp(Wh_GetIntSetting(L"marginLeft"), 0, 100);
    s.marginRight = std::clamp(Wh_GetIntSetting(L"marginRight"), 0, 100);
    s.showTooltip = Wh_GetIntSetting(L"showTooltip");
    s.openSettingsOnClick = Wh_GetIntSetting(L"openSettingsOnClick");
    s.refreshInterval =
        std::clamp(Wh_GetIntSetting(L"refreshInterval"), 2, 3600);

    for (int i = 0; i < 100; i++) {
        std::wstring name = GetString(L"excludedDevices[%d]", i);
        if (name.empty()) {
            break;
        }
        s.excludedDevices.push_back(name);
    }

    for (int i = 0; i < 100; i++) {
        std::wstring name = GetString(L"iconOverrides[%d].name", i);
        if (name.empty()) {
            break;
        }
        s.iconOverrides.push_back(
            {name, DeviceTypeFromString(GetString(L"iconOverrides[%d].type", i))});
    }

    std::lock_guard lock(g_settingsMutex);
    s.generation = g_settings.generation + 1;
    g_settings = std::move(s);
}

Settings GetSettingsSnapshot() {
    std::lock_guard lock(g_settingsMutex);
    return g_settings;
}

// ---------------------------------------------------------------------------
// Device detection (runs on a background thread)
// ---------------------------------------------------------------------------

// DEVPKEY_Bluetooth_Battery: the battery level shown in Settings.
constexpr wchar_t kBatteryKey[] = L"{104EA319-6EE2-4701-BD47-8DDBF425BBE5} 2";
constexpr wchar_t kContainerIdKey[] = L"System.Devices.ContainerId";
constexpr wchar_t kConnectedKey[] = L"System.Devices.Connected";
constexpr wchar_t kCategoryKey[] = L"System.Devices.Category";

struct DeviceEntry {
    std::wstring name;
    int battery;
    DeviceType type;
    bool charging = false;
    // Battery reported in coarse levels (XInput), shown as "~65%".
    bool approximate = false;

    bool operator==(const DeviceEntry&) const = default;
};

DeviceType DetectDeviceType(const std::wstring& name,
                            const std::vector<std::wstring>& categories,
                            const Settings& settings);

bool IsDeviceExcluded(const std::wstring& name, const Settings& settings) {
    for (auto& e : settings.excludedDevices) {
        if (ContainsNoCase(name, e)) {
            return true;
        }
    }
    return false;
}

// Adds a device read directly from hardware, applying the same filters as
// the devices Windows reports.
void AddDirectDevice(std::vector<DeviceEntry>& result,
                     const Settings& settings,
                     std::wstring name,
                     int battery,
                     bool charging,
                     bool approximate) {
    if (IsDeviceExcluded(name, settings) || battery > settings.showOnlyBelow) {
        return;
    }

    Wh_Log(L"%s: %d%%%s%s", name.c_str(), battery,
           approximate ? L" (approximate)" : L"",
           charging ? L" (charging)" : L"");

    DeviceType type = DetectDeviceType(name, {}, settings);
    result.push_back({std::move(name), battery, type, charging, approximate});
}

std::optional<int> PropertyToInt(const IInspectable& value) {
    auto pv = value.try_as<IPropertyValue>();
    if (!pv) {
        return std::nullopt;
    }
    switch (pv.Type()) {
        case PropertyType::UInt8:
            return pv.GetUInt8();
        case PropertyType::Int16:
            return pv.GetInt16();
        case PropertyType::UInt16:
            return pv.GetUInt16();
        case PropertyType::Int32:
            return pv.GetInt32();
        case PropertyType::UInt32:
            return (int)pv.GetUInt32();
        default:
            return std::nullopt;
    }
}

std::vector<std::wstring> PropertyToStrings(const IInspectable& value) {
    std::vector<std::wstring> result;
    auto pv = value.try_as<IPropertyValue>();
    if (!pv) {
        return result;
    }
    if (pv.Type() == PropertyType::StringArray) {
        winrt::com_array<winrt::hstring> arr;
        pv.GetStringArray(arr);
        for (auto& s : arr) {
            result.emplace_back(s);
        }
    } else if (pv.Type() == PropertyType::String) {
        result.emplace_back(pv.GetString());
    }
    return result;
}

DeviceType DetectDeviceType(const std::wstring& name,
                            const std::vector<std::wstring>& categories,
                            const Settings& settings) {
    for (auto& o : settings.iconOverrides) {
        if (ContainsNoCase(name, o.name)) {
            return o.type;
        }
    }

    // Categories reported by Windows, e.g. "Audio.Headphone", "Input.Mouse".
    static const std::pair<const wchar_t*, DeviceType> kCategoryMap[] = {
        {L"headset", DeviceType::headset},
        {L"headphone", DeviceType::headphones},
        {L"mouse", DeviceType::mouse},
        {L"keyboard", DeviceType::keyboard},
        {L"gaming", DeviceType::gamepad},
        {L"gamepad", DeviceType::gamepad},
        {L"joystick", DeviceType::gamepad},
        {L"speaker", DeviceType::speaker},
        {L"phone", DeviceType::phone},
        {L"pen", DeviceType::pen},
        {L"digitizer", DeviceType::pen},
    };
    for (auto& category : categories) {
        for (auto& [key, type] : kCategoryMap) {
            if (ContainsNoCase(category, key)) {
                return type;
            }
        }
    }

    // Fall back to the device name.
    static const std::pair<const wchar_t*, DeviceType> kNameMap[] = {
        {L"headset", DeviceType::headset},
        {L"headphone", DeviceType::headphones},
        {L"buds", DeviceType::headphones},
        {L"airpods", DeviceType::headphones},
        {L"fone", DeviceType::headphones},
        {L"earphone", DeviceType::headphones},
        {L"wh-", DeviceType::headphones},
        {L"wf-", DeviceType::headphones},
        {L"mouse", DeviceType::mouse},
        {L"keyboard", DeviceType::keyboard},
        {L"teclado", DeviceType::keyboard},
        {L"controller", DeviceType::gamepad},
        {L"gamepad", DeviceType::gamepad},
        {L"dualsense", DeviceType::gamepad},
        {L"dualshock", DeviceType::gamepad},
        {L"joy-con", DeviceType::gamepad},
        {L"controle", DeviceType::gamepad},
        {L"speaker", DeviceType::speaker},
        {L"soundbar", DeviceType::speaker},
        {L"caixa", DeviceType::speaker},
        {L"quantum", DeviceType::headset},
        {L"jbl", DeviceType::speaker},
        {L"phone", DeviceType::phone},
        {L"galaxy", DeviceType::phone},
        {L"pen", DeviceType::pen},
        {L"stylus", DeviceType::pen},
        {L"tablet", DeviceType::pen},
        {L"inspiroy", DeviceType::pen},
        {L"wacom", DeviceType::pen},
        {L"huion", DeviceType::pen},
    };
    for (auto& [key, type] : kNameMap) {
        if (ContainsNoCase(name, key)) {
            return type;
        }
    }

    return DeviceType::generic;
}

std::wstring GuidToString(const winrt::guid& g) {
    WCHAR buffer[64];
    StringFromGUID2(reinterpret_cast<const GUID&>(g), buffer,
                    ARRAYSIZE(buffer));
    return ToLower(buffer);
}

// ---------------------------------------------------------------------------
// Sony controllers (DualShock 4, DualSense)
//
// These don't report their battery to Windows, but include it in the input
// reports they stream. Over Bluetooth they start in a basic mode without it,
// and switch to the full reports once a calibration feature report is read,
// as Steam and DS4Windows do. Battery and charging bits follow the Linux
// hid-playstation driver.
// ---------------------------------------------------------------------------

enum class SonyModel { DualShock4, DualSense };

struct SonyBatteryReading {
    int battery;
    bool charging;
};

std::optional<SonyModel> SonyModelFromProductId(USHORT productId) {
    switch (productId) {
        case 0x05C4:  // DualShock 4 (first generation)
        case 0x09CC:  // DualShock 4 (second generation)
        case 0x0BA0:  // DualShock 4 USB wireless adapter
            return SonyModel::DualShock4;
        case 0x0CE6:  // DualSense
        case 0x0DF2:  // DualSense Edge
            return SonyModel::DualSense;
    }
    return std::nullopt;
}

std::optional<SonyBatteryReading> ParseSonyReport(SonyModel model,
                                                  const BYTE* report,
                                                  DWORD size,
                                                  bool usbLayout) {
    if (size < 1) {
        return std::nullopt;
    }

    BYTE id = report[0];

    if (model == SonyModel::DualShock4) {
        size_t offset;
        if (id == 0x11) {
            offset = 32;  // Bluetooth full report.
        } else if (id == 0x01 && usbLayout) {
            offset = 30;  // USB report.
        } else {
            return std::nullopt;
        }
        if (offset >= size) {
            return std::nullopt;
        }

        BYTE status = report[offset];
        int level = status & 0x0F;
        bool cable = (status & 0x10) != 0;
        if (cable) {
            if (level < 10) {
                return SonyBatteryReading{level * 10 + 5, true};
            }
            if (level == 10) {
                return SonyBatteryReading{100, false};
            }
            return std::nullopt;
        }
        return SonyBatteryReading{std::min((level + 1) * 10 + 5, 100), false};
    }

    size_t offset;
    if (id == 0x31) {
        offset = 54;  // Bluetooth full report.
    } else if (id == 0x01 && usbLayout) {
        offset = 53;  // USB report.
    } else {
        return std::nullopt;
    }
    if (offset >= size) {
        return std::nullopt;
    }

    BYTE status = report[offset];
    int level = status & 0x0F;
    switch ((status & 0xF0) >> 4) {
        case 0x0:
            return SonyBatteryReading{std::min(level * 10 + 5, 100), false};
        case 0x1:
            return SonyBatteryReading{std::min(level * 10 + 5, 100), true};
        case 0x2:
            return SonyBatteryReading{100, false};
    }
    return std::nullopt;
}

std::optional<SonyBatteryReading> ReadSonyBattery(PCWSTR devicePath,
                                                  SonyModel model) {
    HANDLE device = CreateFile(devicePath, GENERIC_READ | GENERIC_WRITE,
                               FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                               OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    if (device == INVALID_HANDLE_VALUE) {
        Wh_Log(L"Can't open controller: %u", GetLastError());
        return std::nullopt;
    }

    std::optional<SonyBatteryReading> result;
    PHIDP_PREPARSED_DATA preparsed = nullptr;
    HANDLE event = CreateEvent(nullptr, TRUE, FALSE, nullptr);

    do {
        HIDP_CAPS caps{};
        if (!event || !HidD_GetPreparsedData(device, &preparsed) ||
            HidP_GetCaps(preparsed, &caps) != HIDP_STATUS_SUCCESS ||
            caps.InputReportByteLength == 0) {
            break;
        }

        // Over USB the reports are 64 bytes. Bluetooth descriptors declare
        // larger ones, and need the switch to the full reports.
        bool usbLayout = caps.InputReportByteLength == 64;
        if (!usbLayout && caps.FeatureReportByteLength > 0) {
            std::vector<BYTE> feature(caps.FeatureReportByteLength);
            feature[0] = model == SonyModel::DualShock4 ? 0x02 : 0x05;
            if (!HidD_GetFeature(device, feature.data(),
                                 (ULONG)feature.size())) {
                Wh_Log(L"Feature report failed: %u", GetLastError());
            }
        }

        std::vector<BYTE> report(caps.InputReportByteLength);
        for (int attempt = 0; attempt < 16 && !result; attempt++) {
            OVERLAPPED overlapped{};
            overlapped.hEvent = event;
            ResetEvent(event);

            DWORD read = 0;
            if (!ReadFile(device, report.data(), (DWORD)report.size(), &read,
                          &overlapped)) {
                if (GetLastError() != ERROR_IO_PENDING) {
                    break;
                }
                if (WaitForSingleObject(event, 500) != WAIT_OBJECT_0) {
                    CancelIoEx(device, &overlapped);
                    GetOverlappedResult(device, &overlapped, &read, TRUE);
                    break;
                }
                if (!GetOverlappedResult(device, &overlapped, &read, FALSE)) {
                    break;
                }
            }

            result = ParseSonyReport(model, report.data(), read, usbLayout);
        }
    } while (false);

    if (preparsed) {
        HidD_FreePreparsedData(preparsed);
    }
    if (event) {
        CloseHandle(event);
    }
    CloseHandle(device);
    return result;
}

void QuerySonyControllers(const Settings& settings,
                          std::vector<DeviceEntry>& result) {
    GUID hidGuid;
    HidD_GetHidGuid(&hidGuid);

    HDEVINFO devInfo = SetupDiGetClassDevs(
        &hidGuid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devInfo == INVALID_HANDLE_VALUE) {
        return;
    }

    int counts[2] = {};

    SP_DEVICE_INTERFACE_DATA interfaceData{sizeof(interfaceData)};
    for (DWORD i = 0; SetupDiEnumDeviceInterfaces(devInfo, nullptr, &hidGuid,
                                                  i, &interfaceData);
         i++) {
        DWORD size = 0;
        SetupDiGetDeviceInterfaceDetail(devInfo, &interfaceData, nullptr, 0,
                                        &size, nullptr);
        if (!size) {
            continue;
        }

        std::vector<BYTE> buffer(size);
        auto* detail =
            reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(buffer.data());
        detail->cbSize = sizeof(*detail);
        if (!SetupDiGetDeviceInterfaceDetail(devInfo, &interfaceData, detail,
                                             size, nullptr, nullptr)) {
            continue;
        }

        // Cheap check by path before opening anything: Sony's vendor id.
        std::wstring path = ToLower(detail->DevicePath);
        if (path.find(L"054c") == std::wstring::npos) {
            continue;
        }

        HANDLE probe =
            CreateFile(detail->DevicePath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
                       nullptr, OPEN_EXISTING, 0, nullptr);
        if (probe == INVALID_HANDLE_VALUE) {
            continue;
        }
        HIDD_ATTRIBUTES attributes{sizeof(attributes)};
        bool gotAttributes = HidD_GetAttributes(probe, &attributes);
        CloseHandle(probe);
        if (!gotAttributes || attributes.VendorID != 0x054C) {
            continue;
        }

        auto model = SonyModelFromProductId(attributes.ProductID);
        if (!model) {
            continue;
        }

        auto reading = ReadSonyBattery(detail->DevicePath, *model);
        if (!reading) {
            Wh_Log(L"No battery reading from %s", detail->DevicePath);
            continue;
        }

        int& count = counts[*model == SonyModel::DualShock4 ? 0 : 1];
        count++;
        std::wstring name = *model == SonyModel::DualShock4 ? L"PS4 Controller"
                                                            : L"PS5 Controller";
        if (count > 1) {
            name += L" " + std::to_wstring(count);
        }

        AddDirectDevice(result, settings, std::move(name), reading->battery,
                        reading->charging, /*approximate=*/false);
    }

    SetupDiDestroyDeviceInfoList(devInfo);
}

// ---------------------------------------------------------------------------
// Xbox controllers through USB or the Xbox Wireless Adapter (XInput)
//
// XInput reports the battery in four coarse levels. Controllers paired over
// Bluetooth show up here as well, but Windows already reports their exact
// level, so as many controllers as Windows listed are skipped.
// ---------------------------------------------------------------------------

using XInputGetState_t = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
using XInputGetBatteryInformation_t =
    DWORD(WINAPI*)(DWORD, BYTE, XINPUT_BATTERY_INFORMATION*);

void QueryXInputControllers(const Settings& settings,
                            std::vector<DeviceEntry>& result,
                            int bluetoothXboxCount) {
    // Loaded for every query: a long running process doesn't otherwise
    // notice controllers which connect after XInput was loaded.
    HMODULE module = LoadLibraryEx(L"xinput1_4.dll", nullptr,
                                   LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!module) {
        return;
    }

    auto getState =
        (XInputGetState_t)GetProcAddress(module, "XInputGetState");
    auto getBattery = (XInputGetBatteryInformation_t)GetProcAddress(
        module, "XInputGetBatteryInformation");

    struct Reading {
        int level;  // -1 when unknown.
    };
    std::vector<Reading> readings;

    if (getState && getBattery) {
        for (DWORD i = 0; i < XUSER_MAX_COUNT; i++) {
            XINPUT_STATE state{};
            if (getState(i, &state) != ERROR_SUCCESS) {
                continue;
            }

            XINPUT_BATTERY_INFORMATION battery{};
            if (getBattery(i, BATTERY_DEVTYPE_GAMEPAD, &battery) !=
                ERROR_SUCCESS) {
                battery.BatteryType = BATTERY_TYPE_UNKNOWN;
            }

            Wh_Log(L"XInput %u: battery type %u, level %u", i,
                   battery.BatteryType, battery.BatteryLevel);

            switch (battery.BatteryType) {
                case BATTERY_TYPE_WIRED:
                    // No battery to show.
                    break;
                case BATTERY_TYPE_ALKALINE:
                case BATTERY_TYPE_NIMH: {
                    static const int kLevels[] = {5, 30, 65, 100};
                    readings.push_back(
                        {kLevels[std::min<BYTE>(battery.BatteryLevel, 3)]});
                    break;
                }
                default:
                    readings.push_back({-1});
                    break;
            }
        }
    }

    FreeLibrary(module);

    // Skip the controllers Windows already reported, those without a known
    // level first.
    std::stable_sort(readings.begin(), readings.end(),
                     [](const Reading& a, const Reading& b) {
                         return a.level < 0 && b.level >= 0;
                     });
    int skip = std::min<int>(bluetoothXboxCount, (int)readings.size());
    readings.erase(readings.begin(), readings.begin() + skip);

    int count = 0;
    for (const auto& reading : readings) {
        if (reading.level < 0) {
            continue;
        }
        count++;
        std::wstring name = L"Xbox Controller";
        if (count > 1) {
            name += L" " + std::to_wstring(count);
        }
        AddDirectDevice(result, settings, std::move(name), reading.level,
                        /*charging=*/false, /*approximate=*/true);
    }
}

// ---------------------------------------------------------------------------
// Razer mice (HyperSpeed dongle or cable)
//
// Razer mice answer 90-byte feature report requests on their mouse HID
// interface, as documented by the OpenRazer project: command class 0x07,
// id 0x80 returns the battery level (0-255) and id 0x84 the charging state.
// ---------------------------------------------------------------------------

#pragma pack(push, 1)
struct RazerReport {
    BYTE reportId;
    BYTE status;
    BYTE transactionId;
    USHORT remainingPackets;
    BYTE protocolType;
    BYTE dataSize;
    BYTE commandClass;
    BYTE commandId;
    BYTE arguments[80];
    BYTE crc;
    BYTE reserved;
};
#pragma pack(pop)

static_assert(sizeof(RazerReport) == 91);

struct RazerModel {
    USHORT productId;
    PCWSTR name;
    bool wired;
};

const RazerModel kRazerModels[] = {
    {0x007A, L"Razer Viper Ultimate", true},
    {0x007B, L"Razer Viper Ultimate", false},
    {0x007C, L"Razer DeathAdder V2 Pro", true},
    {0x007D, L"Razer DeathAdder V2 Pro", false},
    {0x0083, L"Razer Basilisk X HyperSpeed", false},
    {0x0088, L"Razer Basilisk Ultimate", false},
    {0x008F, L"Razer Naga Pro", true},
    {0x0090, L"Razer Naga Pro", false},
    {0x00A5, L"Razer Viper V2 Pro", true},
    {0x00A6, L"Razer Viper V2 Pro", false},
    {0x00AA, L"Razer Basilisk V3 Pro", true},
    {0x00AB, L"Razer Basilisk V3 Pro", false},
    {0x00B2, L"Razer DeathAdder V3 HyperSpeed", false},
    {0x00B6, L"Razer DeathAdder V3 Pro", true},
    {0x00B7, L"Razer DeathAdder V3 Pro", false},
    {0x00C0, L"Razer Viper V3 Pro", true},
    {0x00C1, L"Razer Viper V3 Pro", false},
};

BYTE RazerCrc(const RazerReport& report) {
    // XOR of the bytes from the remaining packets field up to the arguments,
    // counted without the report id.
    const BYTE* bytes = reinterpret_cast<const BYTE*>(&report) + 1;
    BYTE crc = 0;
    for (int i = 2; i < 88; i++) {
        crc ^= bytes[i];
    }
    return crc;
}

bool RazerQuery(HANDLE device,
                BYTE transactionId,
                BYTE commandClass,
                BYTE commandId,
                RazerReport& response) {
    RazerReport request{};
    request.transactionId = transactionId;
    request.dataSize = 0x02;
    request.commandClass = commandClass;
    request.commandId = commandId;
    request.crc = RazerCrc(request);

    for (int attempt = 0; attempt < 3; attempt++) {
        if (g_unloading) {
            return false;
        }
        if (!HidD_SetFeature(device, &request, sizeof(request))) {
            return false;
        }
        Sleep(60);
        response = {};
        if (!HidD_GetFeature(device, &response, sizeof(response))) {
            return false;
        }
        if (response.status == 0x02 &&
            response.commandClass == commandClass &&
            response.commandId == commandId) {
            return true;
        }
        if (response.status != 0x01) {  // 0x01: busy, try again.
            return false;
        }
        Sleep(100);
    }
    return false;
}

void QueryRazerMice(const Settings& settings,
                    std::vector<DeviceEntry>& result) {
    struct Reading {
        std::wstring name;
        bool wired;
        int battery;
        bool charging;
    };
    std::vector<Reading> readings;

    GUID hidGuid;
    HidD_GetHidGuid(&hidGuid);

    HDEVINFO devInfo = SetupDiGetClassDevs(
        &hidGuid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devInfo == INVALID_HANDLE_VALUE) {
        return;
    }

    SP_DEVICE_INTERFACE_DATA interfaceData{sizeof(interfaceData)};
    for (DWORD i = 0; SetupDiEnumDeviceInterfaces(devInfo, nullptr, &hidGuid,
                                                  i, &interfaceData);
         i++) {
        DWORD size = 0;
        SetupDiGetDeviceInterfaceDetail(devInfo, &interfaceData, nullptr, 0,
                                        &size, nullptr);
        if (!size) {
            continue;
        }

        std::vector<BYTE> buffer(size);
        auto* detail =
            reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(buffer.data());
        detail->cbSize = sizeof(*detail);
        if (!SetupDiGetDeviceInterfaceDetail(devInfo, &interfaceData, detail,
                                             size, nullptr, nullptr)) {
            continue;
        }

        if (ToLower(detail->DevicePath).find(L"vid_1532") ==
            std::wstring::npos) {
            continue;
        }

        HANDLE device = CreateFile(detail->DevicePath, 0,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                   OPEN_EXISTING, 0, nullptr);
        if (device == INVALID_HANDLE_VALUE) {
            continue;
        }

        HIDD_ATTRIBUTES attributes{sizeof(attributes)};
        HIDP_CAPS caps{};
        bool capsOk = false;
        PHIDP_PREPARSED_DATA preparsed = nullptr;
        if (HidD_GetAttributes(device, &attributes) &&
            HidD_GetPreparsedData(device, &preparsed)) {
            capsOk = HidP_GetCaps(preparsed, &caps) == HIDP_STATUS_SUCCESS;
            HidD_FreePreparsedData(preparsed);
        }

        // The mouse collection carries the 90-byte feature reports.
        if (capsOk && caps.UsagePage == 0x01 && caps.Usage == 0x02 &&
            caps.FeatureReportByteLength == sizeof(RazerReport)) {
            const RazerModel* model = nullptr;
            for (const auto& m : kRazerModels) {
                if (m.productId == attributes.ProductID) {
                    model = &m;
                    break;
                }
            }

            // Different models expect different transaction ids.
            for (BYTE transactionId : {0x1F, 0x3F, 0xFF}) {
                RazerReport response{};
                if (!RazerQuery(device, transactionId, 0x07, 0x80, response)) {
                    continue;
                }

                Reading reading{model ? model->name : L"Razer Mouse",
                                model && model->wired,
                                (response.arguments[1] * 100 + 127) / 255,
                                false};
                if (RazerQuery(device, transactionId, 0x07, 0x84, response)) {
                    reading.charging = response.arguments[1] != 0;
                }
                readings.push_back(std::move(reading));
                break;
            }
        }

        CloseHandle(device);
    }

    SetupDiDestroyDeviceInfoList(devInfo);

    // A mouse plugged in by cable shows up twice, through the cable and
    // through the dongle. Keep one, preferring the cable reading.
    std::vector<std::wstring> seen;
    for (const auto& reading : readings) {
        if (std::find(seen.begin(), seen.end(), reading.name) != seen.end()) {
            continue;
        }
        seen.push_back(reading.name);

        const Reading* best = &reading;
        bool charging = false;
        for (const auto& other : readings) {
            if (other.name != reading.name) {
                continue;
            }
            if (other.wired && !best->wired) {
                best = &other;
            }
            if (other.charging || (other.wired && other.battery < 100)) {
                charging = true;
            }
        }

        AddDirectDevice(result, settings, best->name, best->battery, charging,
                        /*approximate=*/false);
    }
}

// JBL Quantum 350 Wireless USB receivers (vendor 0ECB). JBL's QuantumENGINE app
// reads the headset battery as HID feature report 0x49: the report id followed
// by one byte with the percentage. Reading it doesn't change anything on the
// headset. The receiver answers 0 while the headset is off.
const USHORT kJblQuantumProductIds[] = {
    0x206B,  // Quantum 350 Wireless (350T, first receiver revision)
    0x20B2,  // Quantum 350 Wireless (350T, second receiver revision)
    0x2047,  // Quantum 350 Wireless (350B)
};

void QueryJblQuantumHeadsets(const Settings& settings,
                             std::vector<DeviceEntry>& result) {
    GUID hidGuid;
    HidD_GetHidGuid(&hidGuid);

    HDEVINFO devInfo = SetupDiGetClassDevs(
        &hidGuid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devInfo == INVALID_HANDLE_VALUE) {
        return;
    }

    SP_DEVICE_INTERFACE_DATA interfaceData{sizeof(interfaceData)};
    for (DWORD i = 0; SetupDiEnumDeviceInterfaces(devInfo, nullptr, &hidGuid,
                                                  i, &interfaceData);
         i++) {
        DWORD size = 0;
        SetupDiGetDeviceInterfaceDetail(devInfo, &interfaceData, nullptr, 0,
                                        &size, nullptr);
        if (!size) {
            continue;
        }

        std::vector<BYTE> buffer(size);
        auto* detail =
            reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(buffer.data());
        detail->cbSize = sizeof(*detail);
        if (!SetupDiGetDeviceInterfaceDetail(devInfo, &interfaceData, detail,
                                             size, nullptr, nullptr)) {
            continue;
        }

        if (ToLower(detail->DevicePath).find(L"vid_0ecb") ==
            std::wstring::npos) {
            continue;
        }

        HANDLE device = CreateFile(detail->DevicePath, 0,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                   OPEN_EXISTING, 0, nullptr);
        if (device == INVALID_HANDLE_VALUE) {
            continue;
        }

        HIDD_ATTRIBUTES attributes{sizeof(attributes)};
        HIDP_CAPS caps{};
        bool capsOk = false;
        PHIDP_PREPARSED_DATA preparsed = nullptr;
        if (HidD_GetAttributes(device, &attributes) &&
            HidD_GetPreparsedData(device, &preparsed)) {
            capsOk = HidP_GetCaps(preparsed, &caps) == HIDP_STATUS_SUCCESS;
            HidD_FreePreparsedData(preparsed);
        }

        bool known = false;
        for (USHORT pid : kJblQuantumProductIds) {
            if (attributes.ProductID == pid) {
                known = true;
                break;
            }
        }

        // Only the collection with JBL's feature reports, not the media keys.
        if (capsOk && known && caps.FeatureReportByteLength > 0) {
            std::vector<BYTE> report(caps.FeatureReportByteLength);
            report[0] = 0x49;
            if (HidD_GetFeature(device, report.data(), (ULONG)report.size()) &&
                report[0] == 0x49) {
                int battery = report[1];
                if (battery > 0 && battery <= 100) {
                    AddDirectDevice(result, settings, L"JBL Quantum 350",
                                    battery, /*charging=*/false,
                                    /*approximate=*/false);
                } else {
                    Wh_Log(L"JBL Quantum: headset off or not paired (%d)",
                           battery);
                }
            }
        }

        CloseHandle(device);
    }

    SetupDiDestroyDeviceInfoList(devInfo);
}

// DEVPKEY_Bluetooth_Battery.
const DEVPROPKEY kBluetoothBatteryKey = {
    {0x104EA319, 0x6EE2, 0x4701, {0xBD, 0x47, 0x8D, 0xDB, 0xF4, 0x25, 0xBB, 0xE5}},
    2};

// DEVPKEY_Device_ContainerId.
const DEVPROPKEY kDeviceContainerIdKey = {
    {0x8C7ED206, 0x3F8A, 0x4827, {0xB3, 0xAB, 0xAE, 0x9E, 0x1F, 0xAE, 0xFC, 0x6C}},
    2};

// Battery level of every present Bluetooth device node, one per physical device
// (container). Only the Bluetooth enumerators are listed: listing every device
// of the PC to find the few with a battery cost a lot more.
std::map<std::wstring, int> QueryBluetoothBatteries() {
    std::map<std::wstring, int> batteryByContainer;

    for (PCWSTR enumerator :
         {L"BTHENUM", L"BTHLE", L"BTHLEDEVICE", L"BTHHFENUM"}) {
        ULONG flags = CM_GETIDLIST_FILTER_ENUMERATOR |
                      CM_GETIDLIST_FILTER_PRESENT;
        std::vector<WCHAR> list;
        CONFIGRET cr;
        do {
            ULONG size = 0;
            cr = CM_Get_Device_ID_List_Size(&size, enumerator, flags);
            if (cr != CR_SUCCESS || size <= 1) {
                break;
            }
            list.resize(size);
            cr = CM_Get_Device_ID_List(enumerator, list.data(), size, flags);
        } while (cr == CR_BUFFER_SMALL);
        if (cr != CR_SUCCESS || list.empty()) {
            continue;
        }

        for (PCWSTR id = list.data(); *id; id += wcslen(id) + 1) {
            DEVINST devInst;
            if (CM_Locate_DevNode(&devInst, const_cast<PWSTR>(id),
                                  CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS) {
                continue;
            }

            DEVPROPTYPE type;
            BYTE battery = 0;
            ULONG size = sizeof(battery);
            if (CM_Get_DevNode_Property(devInst, &kBluetoothBatteryKey, &type,
                                        &battery, &size, 0) != CR_SUCCESS ||
                type != DEVPROP_TYPE_BYTE || battery > 100) {
                continue;
            }

            GUID container;
            size = sizeof(container);
            if (CM_Get_DevNode_Property(devInst, &kDeviceContainerIdKey, &type,
                                        reinterpret_cast<BYTE*>(&container),
                                        &size, 0) != CR_SUCCESS ||
                type != DEVPROP_TYPE_GUID) {
                continue;
            }

            std::wstring key =
                GuidToString(reinterpret_cast<const winrt::guid&>(container));
            auto it = batteryByContainer.find(key);
            if (it == batteryByContainer.end() || it->second < battery) {
                batteryByContainer[key] = battery;
            }
        }
    }

    return batteryByContainer;
}

std::vector<DeviceEntry> QueryDevices(const Settings& settings) {
    auto batteryByContainer = QueryBluetoothBatteries();

    auto containerProps = winrt::single_threaded_vector<winrt::hstring>(
        {kConnectedKey, kCategoryKey});
    std::vector<DeviceEntry> result;

    for (auto& [id, battery] : batteryByContainer) {
        try {
            auto container =
                DeviceInformation::CreateFromIdAsync(
                    winrt::hstring(id), containerProps,
                    DeviceInformationKind::DeviceContainer)
                    .get();
            auto props = container.Properties();

            bool connected = false;
            if (auto v = props.TryLookup(kConnectedKey)) {
                connected = winrt::unbox_value_or<bool>(v, false);
            }

            std::wstring name(container.Name());
            if (!connected) {
                Wh_Log(L"Skipping disconnected device: %s (%d%%)", name.c_str(),
                       battery);
                continue;
            }

            bool excluded = false;
            for (auto& e : settings.excludedDevices) {
                if (ContainsNoCase(name, e)) {
                    excluded = true;
                    break;
                }
            }
            if (excluded || battery > settings.showOnlyBelow) {
                continue;
            }

            std::vector<std::wstring> categories;
            if (auto v = props.TryLookup(kCategoryKey)) {
                categories = PropertyToStrings(v);
            }

            result.push_back(
                {name, battery, DetectDeviceType(name, categories, settings)});
        } catch (...) {
            Wh_Log(L"Container %s query failed: %08X", id.c_str(),
                   winrt::to_hresult().value);
        }
    }

    // Counted before anything is added below, so that only the controllers
    // Windows reported count.
    int bluetoothXboxCount = (int)std::count_if(
        result.begin(), result.end(), [](const DeviceEntry& d) {
            return ContainsNoCase(d.name, L"xbox");
        });

    try {
        QuerySonyControllers(settings, result);
    } catch (...) {
        Wh_Log(L"Sony controller query failed");
    }

    try {
        QueryXInputControllers(settings, result, bluetoothXboxCount);
    } catch (...) {
        Wh_Log(L"XInput query failed");
    }

    try {
        QueryRazerMice(settings, result);
    } catch (...) {
        Wh_Log(L"Razer query failed");
    }

    try {
        QueryJblQuantumHeadsets(settings, result);
    } catch (...) {
        Wh_Log(L"JBL Quantum query failed");
    }

    std::sort(result.begin(), result.end(),
              [](const DeviceEntry& a, const DeviceEntry& b) {
                  return ToLower(a.name) < ToLower(b.name);
              });

    return result;
}

// ---------------------------------------------------------------------------
// Taskbar access
// ---------------------------------------------------------------------------

void* CTaskBand_ITaskListWndSite_vftable;

using CTaskBand_GetTaskbarHost_t = void*(WINAPI*)(void* pThis, void** result);
CTaskBand_GetTaskbarHost_t CTaskBand_GetTaskbarHost_Original;

void* TaskbarHost_FrameHeight_Original;

using std__Ref_count_base__Decref_t = void(WINAPI*)(void* pThis);
std__Ref_count_base__Decref_t std__Ref_count_base__Decref_Original;

XamlRoot GetTaskbarXamlRoot(HWND hTaskbarWnd) {
    HWND hTaskSwWnd = (HWND)GetProp(hTaskbarWnd, L"TaskbandHWND");
    if (!hTaskSwWnd) {
        return nullptr;
    }

    void* taskBand = (void*)GetWindowLongPtr(hTaskSwWnd, 0);
    if (!taskBand) {
        return nullptr;
    }

    void* taskBandForTaskListWndSite = taskBand;
    for (int i = 0; *(void**)taskBandForTaskListWndSite !=
                    CTaskBand_ITaskListWndSite_vftable;
         i++) {
        if (i == 20) {
            return nullptr;
        }
        taskBandForTaskListWndSite = (void**)taskBandForTaskListWndSite + 1;
    }

    void* taskbarHostSharedPtr[2]{};
    CTaskBand_GetTaskbarHost_Original(taskBandForTaskListWndSite,
                                      taskbarHostSharedPtr);
    if (!taskbarHostSharedPtr[0] && !taskbarHostSharedPtr[1]) {
        return nullptr;
    }

    size_t taskbarElementIUnknownOffset = 0x10;
    {
        // 48:83EC 28 | sub rsp,28
        // 48:83C1 48 | add rcx,48
        const BYTE* b = (const BYTE*)TaskbarHost_FrameHeight_Original;
        if (b[0] == 0x48 && b[1] == 0x83 && b[2] == 0xEC && b[4] == 0x48 &&
            b[5] == 0x83 && b[6] == 0xC1 && b[7] <= 0x7F) {
            taskbarElementIUnknownOffset = b[7];
        } else {
            Wh_Log(L"Unsupported TaskbarHost::FrameHeight");
        }
    }

    auto* taskbarElementIUnknown = *(IUnknown**)((BYTE*)taskbarHostSharedPtr[0] +
                                                 taskbarElementIUnknownOffset);

    FrameworkElement taskbarElement = nullptr;
    taskbarElementIUnknown->QueryInterface(winrt::guid_of<FrameworkElement>(),
                                           winrt::put_abi(taskbarElement));

    auto result = taskbarElement ? taskbarElement.XamlRoot() : nullptr;

    std__Ref_count_base__Decref_Original(taskbarHostSharedPtr[1]);

    return result;
}

HWND FindCurrentProcessTaskbarWnd() {
    HWND hTaskbarWnd = nullptr;
    EnumWindows(
        [](HWND hWnd, LPARAM lParam) -> BOOL {
            DWORD dwProcessId;
            WCHAR className[32];
            if (GetWindowThreadProcessId(hWnd, &dwProcessId) &&
                dwProcessId == GetCurrentProcessId() &&
                GetClassName(hWnd, className, ARRAYSIZE(className)) &&
                _wcsicmp(className, L"Shell_TrayWnd") == 0) {
                *reinterpret_cast<HWND*>(lParam) = hWnd;
                return FALSE;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&hTaskbarWnd));
    return hTaskbarWnd;
}

using RunFromWindowThreadProc_t = void(WINAPI*)(void* parameter);

bool RunFromWindowThread(HWND hWnd,
                         RunFromWindowThreadProc_t proc,
                         void* procParam) {
    static const UINT runFromWindowThreadRegisteredMsg =
        RegisterWindowMessage(L"Windhawk_RunFromWindowThread_" WH_MOD_ID);

    struct RUN_FROM_WINDOW_THREAD_PARAM {
        RunFromWindowThreadProc_t proc;
        void* procParam;
    };

    DWORD dwThreadId = GetWindowThreadProcessId(hWnd, nullptr);
    if (dwThreadId == 0) {
        return false;
    }

    if (dwThreadId == GetCurrentThreadId()) {
        proc(procParam);
        return true;
    }

    HHOOK hook = SetWindowsHookEx(
        WH_CALLWNDPROC,
        [](int nCode, WPARAM wParam, LPARAM lParam) -> LRESULT {
            if (nCode == HC_ACTION) {
                const CWPSTRUCT* cwp = (const CWPSTRUCT*)lParam;
                if (cwp->message == runFromWindowThreadRegisteredMsg) {
                    RUN_FROM_WINDOW_THREAD_PARAM* param =
                        (RUN_FROM_WINDOW_THREAD_PARAM*)cwp->lParam;
                    param->proc(param->procParam);
                }
            }
            return CallNextHookEx(nullptr, nCode, wParam, lParam);
        },
        nullptr, dwThreadId);
    if (!hook) {
        return false;
    }

    RUN_FROM_WINDOW_THREAD_PARAM param;
    param.proc = proc;
    param.procParam = procParam;
    SendMessage(hWnd, runFromWindowThreadRegisteredMsg, 0, (LPARAM)&param);

    UnhookWindowsHookEx(hook);
    return true;
}

// ---------------------------------------------------------------------------
// XAML tree helpers
// ---------------------------------------------------------------------------

FrameworkElement EnumChildElements(
    FrameworkElement element,
    std::function<bool(FrameworkElement)> enumCallback) {
    int childrenCount = Media::VisualTreeHelper::GetChildrenCount(element);
    for (int i = 0; i < childrenCount; i++) {
        auto child = Media::VisualTreeHelper::GetChild(element, i)
                         .try_as<FrameworkElement>();
        if (child && enumCallback(child)) {
            return child;
        }
    }
    return nullptr;
}

FrameworkElement FindChildByName(FrameworkElement element, PCWSTR name) {
    return EnumChildElements(element, [name](FrameworkElement child) {
        return child.Name() == name;
    });
}

FrameworkElement FindChildByClassName(FrameworkElement element,
                                      PCWSTR className) {
    return EnumChildElements(element, [className](FrameworkElement child) {
        return winrt::get_class_name(child) == className;
    });
}

// On newer builds, the XAML root content is the taskbar frame, and the tray
// frame is an item of its repeater.
FrameworkElement FindSystemTrayFrame(FrameworkElement xamlRootContent) {
    if (winrt::get_class_name(xamlRootContent) != L"Taskbar.TaskbarFrame") {
        return FindChildByClassName(xamlRootContent,
                                    L"SystemTray.SystemTrayFrame");
    }

    FrameworkElement child = xamlRootContent;
    if (!(child = FindChildByName(child, L"RootGrid")) ||
        !(child = FindChildByName(child, L"TaskbarFrameRepeater"))) {
        return nullptr;
    }

    FrameworkElement systemTrayFrame = nullptr;
    EnumChildElements(child, [&systemTrayFrame](FrameworkElement item) {
        if (winrt::get_class_name(item) != L"Taskbar.TaskbarExtensionElement") {
            return false;
        }
        auto extensionElement = item.try_as<Controls::ContentControl>();
        auto content =
            extensionElement
                ? extensionElement.Content().try_as<FrameworkElement>()
                : nullptr;
        if (!content ||
            winrt::get_class_name(content) != L"SystemTray.SystemTrayFrame") {
            return false;
        }
        systemTrayFrame = content;
        return true;
    });

    return systemTrayFrame;
}

// A Grid with columns on older builds, a horizontal StackPanel on newer ones.
Controls::Panel FindSystemTrayFrameGrid(XamlRoot xamlRoot) {
    FrameworkElement child = xamlRoot.Content().try_as<FrameworkElement>();
    if (child && (child = FindSystemTrayFrame(child)) &&
        (child = FindChildByName(child, L"SystemTrayFrameGrid"))) {
        return child.try_as<Controls::Panel>();
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// UI (runs on the taskbar thread)
// ---------------------------------------------------------------------------

winrt::weak_ref<Controls::StackPanel> g_panel;
winrt::weak_ref<Controls::ColumnDefinition> g_column;
Position g_appliedPosition;
std::wstring g_lastSignature;

// leftOfTray: the panel sits in the tray's parent grid, spanning it, with
// margins that keep its desired size at zero. The taskbar sizes the app area
// from the tray's size, which this leaves untouched.
winrt::weak_ref<FrameworkElement> g_outsideTrayFrame;
int g_outsideGap = 6;
FrameworkElement::LayoutUpdated_revoker g_outsideLayoutRevoker;

void UpdateOutsidePanelMargin() {
    auto panel = g_panel.get();
    auto tray = g_outsideTrayFrame.get();
    if (!panel || !tray) {
        return;
    }

    auto grid = Media::VisualTreeHelper::GetParent(panel).try_as<UIElement>();
    if (!grid) {
        return;
    }

    double trayX;
    try {
        trayX = tray.TransformToVisual(grid).TransformPoint({0, 0}).X;
    } catch (...) {
        return;
    }

    double width =
        panel.Visibility() == Visibility::Visible ? panel.ActualWidth() : 0;

    double left = trayX - g_outsideGap - width;
    double right = -(trayX - g_outsideGap);
    auto m = panel.Margin();
    if (std::abs(m.Left - left) < 0.5 && std::abs(m.Right - right) < 0.5) {
        return;
    }

    panel.Margin(ThicknessHelper::FromLengths(left, 0, right, 0));
}

bool InsertOutsideTray(FrameworkElement trayFrame) {
    auto grid =
        Media::VisualTreeHelper::GetParent(trayFrame).try_as<Controls::Grid>();
    if (!grid) {
        Wh_Log(L"Tray parent isn't a grid");
        return false;
    }

    Controls::StackPanel panel;
    panel.Orientation(Controls::Orientation::Horizontal);
    panel.HorizontalAlignment(HorizontalAlignment::Left);
    panel.VerticalAlignment(VerticalAlignment::Stretch);
    int cols = (int)grid.ColumnDefinitions().Size();
    Controls::Grid::SetColumn(panel, 0);
    Controls::Grid::SetColumnSpan(panel, cols > 0 ? cols : 1);
    Controls::Grid::SetRow(panel, Controls::Grid::GetRow(trayFrame));
    Controls::Grid::SetRowSpan(panel, Controls::Grid::GetRowSpan(trayFrame));
    // Zero desired width until the first update.
    panel.Margin(ThicknessHelper::FromLengths(0, 0, -10000, 0));
    grid.Children().Append(panel);

    g_panel = panel;
    g_column = nullptr;
    g_appliedPosition = Position::leftOfTray;
    g_lastSignature.clear();
    g_outsideTrayFrame = trayFrame;
    g_outsideLayoutRevoker = panel.LayoutUpdated(
        winrt::auto_revoke,
        [](auto&&, auto&&) { UpdateOutsidePanelMargin(); });

    Wh_Log(L"Inserted left of the tray");
    return true;
}

wchar_t GlyphForType(DeviceType type) {
    switch (type) {
        case DeviceType::headphones:
            return (wchar_t)0xE7F6;
        case DeviceType::headset:
            return (wchar_t)0xE95B;
        case DeviceType::mouse:
            return (wchar_t)0xE962;
        case DeviceType::keyboard:
            return (wchar_t)0xE765;
        case DeviceType::gamepad:
            return (wchar_t)0xE7FC;
        case DeviceType::speaker:
            return (wchar_t)0xE7F5;
        case DeviceType::phone:
            return (wchar_t)0xE8EA;
        case DeviceType::pen:
            return (wchar_t)0xE70F;
        case DeviceType::generic:
            break;
    }
    return (wchar_t)0xE702;
}

void RemoveFromTaskbar() {
    auto panel = g_panel.get();
    auto column = g_column.get();
    g_panel = nullptr;
    g_column = nullptr;
    g_lastSignature.clear();
    g_outsideLayoutRevoker.revoke();
    g_outsideTrayFrame = nullptr;

    if (!panel) {
        return;
    }

    auto parent = Media::VisualTreeHelper::GetParent(panel)
                      .try_as<Controls::Panel>();
    if (!parent) {
        return;
    }

    uint32_t index;
    if (parent.Children().IndexOf(panel, index)) {
        parent.Children().RemoveAt(index);
    }

    auto grid = parent.try_as<Controls::Grid>();
    if (grid && column && grid.ColumnDefinitions().IndexOf(column, index)) {
        grid.ColumnDefinitions().RemoveAt(index);
        for (auto child : grid.Children()) {
            auto fe = child.try_as<FrameworkElement>();
            if (!fe) {
                continue;
            }
            int c = Controls::Grid::GetColumn(fe);
            if (c > (int)index) {
                Controls::Grid::SetColumn(fe, c - 1);
            }
        }
    }

    Wh_Log(L"Removed from taskbar");
}

int ColumnOfChild(Controls::Grid grid, PCWSTR name) {
    auto child = FindChildByName(grid, name);
    return child ? Controls::Grid::GetColumn(child) : -1;
}

Controls::StackPanel CreateDevicesPanel(Position position) {
    Controls::StackPanel panel;
    panel.Orientation(Controls::Orientation::Horizontal);
    panel.VerticalAlignment(VerticalAlignment::Stretch);
    g_panel = panel;
    g_column = nullptr;
    g_appliedPosition = position;
    g_lastSignature.clear();
    return panel;
}

int IndexOfChild(Controls::Panel parent, PCWSTR name) {
    auto child = FindChildByName(parent, name);
    uint32_t index;
    if (child && parent.Children().IndexOf(child, index)) {
        return (int)index;
    }
    return -1;
}

// Newer builds: the tray is a horizontal StackPanel, insert next to the
// anchor element.
bool InsertIntoStackPanel(Controls::Panel parent, Position position) {
    int target = -1;
    int controlCenter = IndexOfChild(parent, L"ControlCenterButton");
    switch (position) {
        case Position::leftOfTray:
        case Position::beforeControlCenter:
            target = controlCenter;
            break;
        case Position::afterControlCenter:
            target = controlCenter >= 0 ? controlCenter + 1 : -1;
            break;
        case Position::beforeTrayIcons:
            for (PCWSTR name : {L"NotifyIconStack", L"NotificationAreaIcons"}) {
                int i = IndexOfChild(parent, name);
                if (i >= 0 && (target < 0 || i < target)) {
                    target = i;
                }
            }
            break;
    }

    if (target < 0) {
        Wh_Log(L"Anchor element not found");
        return false;
    }

    parent.Children().InsertAt(target, CreateDevicesPanel(position));
    Wh_Log(L"Inserted into tray panel at index %d", target);
    return true;
}

bool InsertIntoTaskbar(Controls::Panel parent, Position position) {
    auto grid = parent.try_as<Controls::Grid>();
    if (!grid) {
        return InsertIntoStackPanel(parent, position);
    }

    auto columns = grid.ColumnDefinitions();
    if (columns.Size() < 2) {
        Wh_Log(L"Unexpected tray layout (%u columns)", columns.Size());
        return false;
    }

    int target = -1;
    int controlCenter = ColumnOfChild(grid, L"ControlCenterButton");
    switch (position) {
        case Position::leftOfTray:
        case Position::beforeControlCenter:
            target = controlCenter;
            break;
        case Position::afterControlCenter:
            target = controlCenter >= 0 ? controlCenter + 1 : -1;
            break;
        case Position::beforeTrayIcons: {
            for (PCWSTR name : {L"NotifyIconStack", L"NotificationAreaIcons"}) {
                int c = ColumnOfChild(grid, name);
                if (c >= 0 && (target < 0 || c < target)) {
                    target = c;
                }
            }
            break;
        }
    }

    if (target < 0) {
        Wh_Log(L"Anchor element not found");
        return false;
    }
    target = std::min(target, (int)columns.Size());

    for (auto child : grid.Children()) {
        auto fe = child.try_as<FrameworkElement>();
        if (!fe) {
            continue;
        }
        int c = Controls::Grid::GetColumn(fe);
        if (c >= target) {
            Controls::Grid::SetColumn(fe, c + 1);
        }
    }

    Controls::ColumnDefinition column;
    column.Width(GridLengthHelper::Auto());
    columns.InsertAt(target, column);

    auto panel = CreateDevicesPanel(position);
    Controls::Grid::SetColumn(panel, target);
    grid.Children().Append(panel);
    g_column = column;

    Wh_Log(L"Inserted into taskbar at column %d", target);
    return true;
}

std::wstring MakeSignature(const std::vector<DeviceEntry>& devices,
                           const Settings& settings,
                           ElementTheme theme) {
    std::wstring s = std::to_wstring(settings.generation) + L"|" +
                     std::to_wstring((int)theme);
    for (auto& d : devices) {
        s += L"|" + d.name + L":" + std::to_wstring(d.battery) + L":" +
             std::to_wstring((int)d.type) + L":" +
             std::to_wstring(d.charging) + std::to_wstring(d.approximate);
    }
    return s;
}

UIElement CreateDeviceItem(const DeviceEntry& device, ElementTheme theme,
                           const Settings& settings) {
    bool low = device.battery <= settings.lowBatteryThreshold &&
               settings.lowBatteryColor.has_value();

    Media::Brush normalBrush = nullptr;
    if (settings.textColor) {
        normalBrush = Media::SolidColorBrush(*settings.textColor);
    }
    Media::Brush lowBrush = nullptr;
    if (low) {
        lowBrush = Media::SolidColorBrush(*settings.lowBatteryColor);
    }

    Controls::StackPanel item;
    item.Orientation(Controls::Orientation::Horizontal);
    item.VerticalAlignment(VerticalAlignment::Center);
    item.Spacing(settings.iconTextSpacing);
    // A background makes the whole item respond to hover and clicks.
    item.Background(Media::SolidColorBrush(winrt::Windows::UI::Color{0, 0, 0, 0}));

    auto makeGlyph = [&](wchar_t glyph, double size) {
        Controls::FontIcon icon;
        icon.FontFamily(
            Media::FontFamily(L"Segoe Fluent Icons, Segoe MDL2 Assets"));
        icon.Glyph(std::wstring(1, glyph));
        icon.FontSize(size);
        icon.VerticalAlignment(VerticalAlignment::Center);
        if (low && settings.colorIconWhenLow) {
            icon.Foreground(lowBrush);
        } else if (normalBrush) {
            icon.Foreground(normalBrush);
        }
        return icon;
    };

    bool showIcon = settings.displayMode != DisplayMode::percentOnly;
    bool showText = settings.displayMode == DisplayMode::iconAndPercent ||
                    settings.displayMode == DisplayMode::percentOnly;
    bool showBar = settings.displayMode == DisplayMode::batteryBar;

    if (showIcon) {
        item.Children().Append(
            makeGlyph(GlyphForType(device.type), settings.iconSize));
    }

    if (showBar) {
        // The bar follows the text color, which by default is the taskbar's.
        Media::Brush barBrush = low ? lowBrush : normalBrush;
        if (!barBrush) {
            barBrush = Media::SolidColorBrush(
                theme == ElementTheme::Light
                    ? winrt::Windows::UI::Color{0xFF, 0x00, 0x00, 0x00}
                    : winrt::Windows::UI::Color{0xFF, 0xFF, 0xFF, 0xFF});
        }
        double barWidth = std::max(16.0, settings.iconSize * 1.5);
        double barHeight = std::max(8.0, settings.iconSize * 0.6);

        Controls::Border outer;
        outer.Width(barWidth);
        outer.Height(barHeight);
        outer.BorderThickness(ThicknessHelper::FromUniformLength(1));
        outer.CornerRadius(CornerRadiusHelper::FromUniformRadius(2));
        outer.Padding(ThicknessHelper::FromUniformLength(1));
        outer.VerticalAlignment(VerticalAlignment::Center);
        outer.BorderBrush(barBrush);

        Controls::Border fill;
        fill.HorizontalAlignment(HorizontalAlignment::Left);
        fill.Width(std::max(1.0, (barWidth - 4) * device.battery / 100.0));
        fill.CornerRadius(CornerRadiusHelper::FromUniformRadius(1));
        fill.Background(barBrush);
        outer.Child(fill);
        item.Children().Append(outer);
    }

    if (showText) {
        Controls::TextBlock text;
        std::wstring value = std::to_wstring(device.battery) + L"%";
        if (device.approximate) {
            value = L"~" + value;
        }
        text.Text(value);
        text.FontSize(settings.fontSize);
        if (!settings.fontFamily.empty()) {
            text.FontFamily(Media::FontFamily(settings.fontFamily));
        }
        if (settings.bold) {
            text.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
        }
        text.VerticalAlignment(VerticalAlignment::Center);
        // Center the visible glyphs instead of the full line box.
        text.TextLineBounds(TextLineBounds::Tight);
        if (settings.textVerticalOffset) {
            Media::TranslateTransform offset;
            offset.Y(settings.textVerticalOffset);
            text.RenderTransform(offset);
        }
        if (low) {
            text.Foreground(lowBrush);
        } else if (normalBrush) {
            text.Foreground(normalBrush);
        }
        item.Children().Append(text);
    }

    if (device.charging && settings.showChargingIndicator) {
        // LightningBolt.
        item.Children().Append(makeGlyph(0xE945, settings.iconSize * 0.75));
    }

    if (settings.showTooltip) {
        std::wstring tooltip = device.name + L": ";
        if (device.approximate) {
            tooltip += L"about ";
        }
        tooltip += std::to_wstring(device.battery) + L"%";
        if (device.charging) {
            tooltip += L" (charging)";
        }
        Controls::ToolTipService::SetToolTip(item, winrt::box_value(tooltip));
    }

    if (settings.openSettingsOnClick) {
        item.Tapped([](IInspectable const&,
                       Input::TappedRoutedEventArgs const& args) {
            args.Handled(true);
            ShellExecute(nullptr, L"open", L"ms-settings:bluetooth", nullptr,
                         nullptr, SW_SHOWNORMAL);
        });
    }

    return item;
}

void UpdateTaskbar(const std::vector<DeviceEntry>& devices,
                   const Settings& settings,
                   HWND hTaskbarWnd) {
    auto xamlRoot = GetTaskbarXamlRoot(hTaskbarWnd);
    if (!xamlRoot) {
        Wh_Log(L"Getting XamlRoot failed");
        return;
    }

    auto grid = FindSystemTrayFrameGrid(xamlRoot);
    if (!grid) {
        Wh_Log(L"SystemTrayFrameGrid not found");
        static bool dumped = false;
        if (!dumped) {
            dumped = true;
            std::function<void(DependencyObject, int)> dump =
                [&](DependencyObject o, int depth) {
                    if (depth > 14) {
                        return;
                    }
                    auto fe = o.try_as<FrameworkElement>();
                    std::wstring line(depth * 2, L' ');
                    line += winrt::get_class_name(o);
                    if (fe && !fe.Name().empty()) {
                        line += L" #" + std::wstring(fe.Name());
                    }
                    if (fe) {
                        line += L" col=" + std::to_wstring(
                                               Controls::Grid::GetColumn(fe));
                    }
                    if (auto g = o.try_as<Controls::Grid>()) {
                        line += L" cols=" +
                                std::to_wstring(g.ColumnDefinitions().Size());
                    }
                    Wh_Log(L"TREE %s", line.c_str());
                    int n = Media::VisualTreeHelper::GetChildrenCount(o);
                    for (int i = 0; i < n; i++) {
                        dump(Media::VisualTreeHelper::GetChild(o, i),
                             depth + 1);
                    }
                };
            dump(xamlRoot.Content(), 0);
        }
        return;
    }

    bool outside = settings.position == Position::leftOfTray;
    auto trayFrame =
        Media::VisualTreeHelper::GetParent(grid).try_as<FrameworkElement>();
    auto expectedParent =
        outside && trayFrame
            ? Media::VisualTreeHelper::GetParent(trayFrame)
            : DependencyObject(grid);

    auto panel = g_panel.get();
    auto panelParent =
        panel ? Media::VisualTreeHelper::GetParent(panel) : nullptr;
    bool inPlace = panel && g_appliedPosition == settings.position &&
                   (panelParent == expectedParent ||
                    (outside && panelParent == grid));
    if (!inPlace) {
        RemoveFromTaskbar();
        bool inserted = false;
        if (outside && trayFrame) {
            inserted = InsertOutsideTray(trayFrame);
        }
        if (!inserted) {
            // The tray isn't in a grid on this build, fall back to inside it.
            inserted = InsertIntoTaskbar(
                grid, outside ? Position::beforeControlCenter
                              : settings.position);
            g_appliedPosition = settings.position;
        }
        if (!inserted) {
            return;
        }
        panel = g_panel.get();
    }
    bool panelOutside = g_outsideTrayFrame.get() != nullptr;

    ElementTheme theme = grid.ActualTheme();
    std::wstring signature = MakeSignature(devices, settings, theme);
    if (signature == g_lastSignature) {
        return;
    }
    g_lastSignature = signature;

    panel.RequestedTheme(theme);
    panel.Spacing(settings.deviceSpacing);
    if (panelOutside) {
        g_outsideGap = settings.marginRight;
    } else {
        panel.Margin(ThicknessHelper::FromLengths(settings.marginLeft, 0,
                                                  settings.marginRight, 0));
    }

    auto children = panel.Children();
    children.Clear();
    for (auto& device : devices) {
        children.Append(CreateDeviceItem(device, theme, settings));
    }

    panel.Visibility(devices.empty() ? Visibility::Collapsed
                                     : Visibility::Visible);

    Wh_Log(L"Showing %d device(s)", (int)devices.size());
}

// Returns true if the UI was found and updated.
bool ApplyToTaskbar(const std::vector<DeviceEntry>* devices,
                    const Settings* settings) {
    HWND hTaskbarWnd = FindCurrentProcessTaskbarWnd();
    if (!hTaskbarWnd) {
        return false;
    }

    struct Param {
        HWND hTaskbarWnd;
        const std::vector<DeviceEntry>* devices;
        const Settings* settings;
        bool ok;
    } param{hTaskbarWnd, devices, settings, false};

    RunFromWindowThread(
        hTaskbarWnd,
        [](void* p) {
            auto& param = *(Param*)p;
            try {
                if (!param.devices) {
                    RemoveFromTaskbar();
                } else {
                    UpdateTaskbar(*param.devices, *param.settings,
                                  param.hTaskbarWnd);
                    param.ok = g_panel.get() != nullptr;
                }
            } catch (...) {
                Wh_Log(L"UI error: %08X", winrt::to_hresult().value);
            }
        },
        &param);

    return param.ok;
}

// ---------------------------------------------------------------------------
// Worker thread
// ---------------------------------------------------------------------------

void WorkerThread() {
    winrt::init_apartment(winrt::apartment_type::multi_threaded);

    while (!g_unloading) {
        Settings settings = GetSettingsSnapshot();

        std::vector<DeviceEntry> devices;
        try {
            devices = QueryDevices(settings);
        } catch (...) {
            Wh_Log(L"Device query failed: %08X", winrt::to_hresult().value);
        }

        bool ok = false;
        if (!g_unloading) {
            ok = ApplyToTaskbar(&devices, &settings);
        }

        // Retry quickly while the taskbar isn't ready yet.
        DWORD wait = ok ? settings.refreshInterval * 1000 : 2000;
        HANDLE handles[] = {g_stopEvent, g_wakeEvent};
        if (WaitForMultipleObjects(2, handles, FALSE, wait) == WAIT_OBJECT_0) {
            break;
        }
    }

    winrt::uninit_apartment();
}

// ---------------------------------------------------------------------------
// Windhawk entry points
// ---------------------------------------------------------------------------

bool ResolveTaskbarDllSymbols() {
    HMODULE module =
        LoadLibraryEx(L"taskbar.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!module) {
        Wh_Log(L"Failed to load taskbar.dll");
        return false;
    }

    WindhawkUtils::SYMBOL_HOOK taskbarDllHooks[] = {
        {
            {LR"(const CTaskBand::`vftable'{for `ITaskListWndSite'})"},
            &CTaskBand_ITaskListWndSite_vftable,
        },
        {
            {LR"(public: virtual class std::shared_ptr<class TaskbarHost> __cdecl CTaskBand::GetTaskbarHost(void)const )"},
            &CTaskBand_GetTaskbarHost_Original,
        },
        {
            {LR"(public: int __cdecl TaskbarHost::FrameHeight(void)const )"},
            &TaskbarHost_FrameHeight_Original,
        },
        {
            {LR"(public: void __cdecl std::_Ref_count_base::_Decref(void))"},
            &std__Ref_count_base__Decref_Original,
        },
    };

    return WindhawkUtils::HookSymbols(module, taskbarDllHooks,
                                      ARRAYSIZE(taskbarDllHooks));
}

BOOL Wh_ModInit() {
    Wh_Log(L">");

    LoadSettings();

    if (!ResolveTaskbarDllSymbols()) {
        Wh_Log(L"Failed to resolve taskbar.dll symbols");
        return FALSE;
    }

    g_stopEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
    g_wakeEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (!g_stopEvent || !g_wakeEvent) {
        return FALSE;
    }

    return TRUE;
}

void Wh_ModAfterInit() {
    Wh_Log(L">");

    g_workerThread = std::thread(WorkerThread);
}

void Wh_ModBeforeUninit() {
    Wh_Log(L">");

    g_unloading = true;
    SetEvent(g_stopEvent);
    if (g_workerThread.joinable()) {
        g_workerThread.join();
    }

    ApplyToTaskbar(nullptr, nullptr);
}

void Wh_ModUninit() {
    Wh_Log(L">");

    if (g_stopEvent) {
        CloseHandle(g_stopEvent);
    }
    if (g_wakeEvent) {
        CloseHandle(g_wakeEvent);
    }
}

void Wh_ModSettingsChanged() {
    Wh_Log(L">");

    LoadSettings();
    SetEvent(g_wakeEvent);
}
