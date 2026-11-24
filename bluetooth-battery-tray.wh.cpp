// ==WindhawkMod==
// @id              bluetooth-battery-tray
// @name            Bluetooth Battery in Taskbar
// @description     Shows connected Bluetooth devices and their battery level next to the system tray (Windows 11)
// @version         1.0
// @author          kadmielp
// @github          https://github.com/kadmielp
// @include         explorer.exe
// @architecture    x86-64
// @compilerOptions -lole32 -loleaut32 -lruntimeobject -lshell32
// ==/WindhawkMod==

// Source code is published under The GNU General Public License v3.0.
//
// Parts of the taskbar access code (GetTaskbarXamlRoot, RunFromWindowThread,
// FindSystemTrayFrame and the XAML tree helpers) are adapted from the
// "Taskbar tray icon spacing and grid" mod by m417z:
// https://github.com/ramensoftware/windhawk-mods

// ==WindhawkModReadme==
/*
# Bluetooth Battery in Taskbar

Automatically detects connected Bluetooth devices (headphones, mice, keyboards,
controllers, pens...) of any brand and shows an icon plus the battery level of
each one in the taskbar, next to the Wi-Fi / volume / battery button.

* A device appears when it connects and disappears when it disconnects.
* Only devices that report their battery level to Windows are shown (the same
  value that appears in Settings > Bluetooth & devices).
* Devices that use a USB receiver (2.4 GHz dongle) don't report battery to
  Windows and are not supported.

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
- showIcon: true
  $name: Show device icon
- showPercentage: true
  $name: Show battery percentage
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
- fontSize: 14
  $name: Text size
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

struct Settings {
    Position position = Position::leftOfTray;
    bool showIcon = true;
    bool showPercentage = true;
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

    s.showIcon = Wh_GetIntSetting(L"showIcon");
    s.showPercentage = Wh_GetIntSetting(L"showPercentage");
    if (!s.showIcon && !s.showPercentage) {
        s.showPercentage = true;
    }
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

    bool operator==(const DeviceEntry&) const = default;
};

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

std::vector<DeviceEntry> QueryDevices(const Settings& settings) {
    auto deviceProps = winrt::single_threaded_vector<winrt::hstring>(
        {kBatteryKey, kContainerIdKey});

    winrt::Windows::Devices::Enumeration::DeviceInformationCollection
        devices{nullptr};
    try {
        // Only devices currently present.
        devices = DeviceInformation::FindAllAsync(
                      L"System.Devices.Present:=System.StructuredQueryType.Boolean#True",
                      deviceProps,
                      DeviceInformationKind::Device)
                      .get();
    } catch (...) {
        Wh_Log(L"Filtered query failed (%08X), querying all devices",
               winrt::to_hresult().value);
        devices = DeviceInformation::FindAllAsync(
                      L"", deviceProps, DeviceInformationKind::Device)
                      .get();
    }

    // One entry per physical device (container).
    std::map<std::wstring, int> batteryByContainer;
    for (const auto& device : devices) {
        auto props = device.Properties();
        auto batteryValue = props.TryLookup(kBatteryKey);
        auto containerValue = props.TryLookup(kContainerIdKey);
        if (!batteryValue || !containerValue) {
            continue;
        }

        auto battery = PropertyToInt(batteryValue);
        auto containerId = containerValue.try_as<winrt::Windows::Foundation::
                                                      IReference<winrt::guid>>();
        if (!battery || *battery < 0 || *battery > 100 || !containerId) {
            continue;
        }

        std::wstring id = GuidToString(containerId.Value());
        auto it = batteryByContainer.find(id);
        if (it == batteryByContainer.end() || it->second < *battery) {
            batteryByContainer[id] = *battery;
        }
    }

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
             std::to_wstring((int)d.type);
    }
    return s;
}

UIElement CreateDeviceItem(const DeviceEntry& device,
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

    if (settings.showIcon) {
        Controls::FontIcon icon;
        icon.FontFamily(Media::FontFamily(L"Segoe Fluent Icons, Segoe MDL2 Assets"));
        icon.Glyph(std::wstring(1, GlyphForType(device.type)));
        icon.FontSize(settings.iconSize);
        icon.VerticalAlignment(VerticalAlignment::Center);
        if (low && settings.colorIconWhenLow) {
            icon.Foreground(lowBrush);
        } else if (normalBrush) {
            icon.Foreground(normalBrush);
        }
        item.Children().Append(icon);
    }

    if (settings.showPercentage) {
        Controls::TextBlock text;
        text.Text(std::to_wstring(device.battery) + L"%");
        text.FontSize(settings.fontSize);
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

    if (settings.showTooltip) {
        Controls::ToolTipService::SetToolTip(
            item, winrt::box_value(device.name + L": " +
                                   std::to_wstring(device.battery) + L"%"));
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
        children.Append(CreateDeviceItem(device, settings));
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
