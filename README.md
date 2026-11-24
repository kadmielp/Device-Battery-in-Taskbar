# Bluetooth Battery in Taskbar

A [Windhawk](https://windhawk.net) mod for Windows 11 that shows your connected Bluetooth devices and their battery levels in the taskbar, next to the system tray.

![A pen tablet at 10% in the low battery color and a game controller at 74%, shown left of the system tray](images/taskbar-example.png)

## Features

- **Automatic detection**: works with any brand, with no setup.
- **Devices read directly**, without extra software:

  | Device | Battery | Charging |
  |---|---|---|
  | PS4 / PS5 controllers (Bluetooth or USB) | ✅ 10% steps | ✅ |
  | Xbox controllers via USB or Xbox Wireless Adapter | ✅ approximate (4 levels, shown as `~65%`) | — |
  | Razer mice (HyperSpeed dongle or cable) | ✅ exact % | ✅ |
  | JBL Quantum 350 Wireless (USB receiver) | ✅ exact % | — |

  Xbox controllers paired over Bluetooth are reported by Windows with their exact level.
- **Charging indicator**: a lightning bolt next to the percentage while charging.
- **Display modes**: icon + percentage, icon only, percentage only, or icon + battery bar.
- **One icon and percentage per device**: headphones, headset, mouse, keyboard, game controller, speaker, phone, pen/tablet, or a generic Bluetooth icon.
- **Live updates**: devices appear when they connect and disappear when they disconnect.
- **Hides devices that don't report a battery level.**
- **Low battery color** (red by default), optionally applied to the icon too.
- **Tooltip** with the device name, and a **click opens Bluetooth settings**.
- **Follows the taskbar's light or dark theme.**

## Requirements

- Windows 11, with the XAML taskbar. Tested on build 26300 (Insider).
- Windhawk 1.7 or later.
- Devices that report their battery level to Windows. A device does this if its battery appears in **Settings → Bluetooth & devices**. PlayStation controllers are the exception: the mod reads them directly.

### Not supported

Apart from the devices listed above, devices that connect through a **USB receiver/dongle (2.4 GHz)**, such as Logitech Unifying/Lightspeed or other JBL Quantum headsets, don't report their battery to Windows. The receiver also stays connected while the device is off. Supporting them would need code for each brand's protocol.

## Installation

1. Open Windhawk and click **Create a New Mod**.
2. Replace the editor's contents with [`bluetooth-battery-tray.wh.cpp`](bluetooth-battery-tray.wh.cpp).
3. Click **Compile Mod**, then **Exit editing mode**.

To update, open the mod, click **Edit**, paste the new version and compile again.

## Settings

| Setting | Default | Description |
|---|---|---|
| Position | Left of the tray | Where the devices appear. See [Position](#position). |
| Display mode | Icon and percentage | Icon and percentage, icon only, percentage only, or icon and battery bar. |
| Show charging indicator | On | Lightning bolt while charging, for devices that report it. |
| Only show devices at or below | 100 | For example, 30 shows a device only once its battery is at 30% or less. |
| Low battery level | 20 | The battery level that triggers the low battery color. |
| Low battery color | `#FF6B6B` | `#RRGGBB`. Leave empty to turn the color change off. |
| Also color the icon when low | On | Applies the low battery color to the icon as well. |
| Text and icon color | *(empty)* | Empty follows the taskbar theme. Otherwise `#RRGGBB`. |
| Font | *(empty)* | Font of the percentage. Empty uses the taskbar font. |
| Text size / Icon size | 14 / 15 | In pixels. |
| Bold text | Off | |
| Spacing between icon and percentage | 4 | |
| Spacing between devices | 12 | |
| Left / right margin | 6 / 6 | With "Left of the tray", the right margin is the gap to the tray. |
| Show device name on hover | On | |
| Open Bluetooth settings on click | On | |
| Percentage vertical offset | 0 | Moves the percentage up (negative) or down (positive) to align it with the icon. |
| Refresh interval | 15 s | How often devices and battery levels are checked. |
| Hidden devices | — | Device names, or parts of names, to hide. Not case sensitive. |
| Icon overrides | — | Chooses the icon for a device name when automatic detection gets it wrong. |

### Position

- **Left of the tray** (recommended): the devices sit next to the tray without making it wider, so **the taskbar apps keep their space**.
- **Left of the Wi-Fi / volume / battery button**, **Between the battery and the clock**, **Left of the tray icons**: inside the tray.

  These make the tray wider. With a centered taskbar, Windows reserves that width on both sides, so apps can move into the **"…"** overflow menu, especially on smaller screens or with Taskbar Styler themes.

## Compatibility with Windows 11 Taskbar Styler

The mod works with the [Windows 11 Taskbar Styler](https://windhawk.net/mods/windows-11-taskbar-styler) and its themes, such as LiquidGlass. It doesn't use XAML diagnostics, so it doesn't conflict with Taskbar Styler.

- With **Left of the tray**, the devices sit outside the tray's "pill", just to its left.
- To keep the apps centered and push the clock and tray to the right edge, add these **Control styles** in Taskbar Styler:

  | Target | Style |
  |---|---|
  | `SystemTray.SystemTrayFrame` | `HorizontalAlignment=Right`<br>`Margin=0,0,0,0` |

## Troubleshooting

**Nothing appears**
- Check that the device is connected and shows its battery in **Settings → Bluetooth & devices**.
- Wait up to the refresh interval (15 s by default).
- To see what the mod is doing, turn on logs: open the mod in Windhawk, go to the **Advanced** tab and set **Debug logging → Mod logs**.

**Apps moved into the "…" menu**
- Set **Position** to **Left of the tray**.

**The percentage isn't aligned with the icon**
- Adjust **Percentage vertical offset** by ±1 or ±2.

**Wrong icon**
- Add an entry under **Icon overrides** with part of the device name and the correct icon.

## How it works

- **Detection**: a background thread lists only the present Bluetooth devices (CfgMgr32, enumerators `BTHENUM`, `BTHLE`, `BTHLEDEVICE` and `BTHHFENUM`) and reads their `DEVPKEY_Bluetooth_Battery` property, the same value Windows Settings shows. It groups devices by `ContainerId` and keeps the ones whose container reports `System.Devices.Connected`.
- **PlayStation controllers**: the mod finds Sony HID devices (vendor `054C`) and reads one input report, where the battery level is stored. Over Bluetooth, it first reads a calibration feature report, which switches the controller to the full reports that carry the battery, as Steam and DS4Windows do. The values come in 10% steps, decoded as in the Linux `hid-playstation` driver.
- **Xbox controllers (XInput)**: `xinput1_4.dll` is loaded for each refresh, so controllers connected later are seen. Controllers already reported over Bluetooth are skipped to avoid duplicates.
- **Razer mice**: 90-byte HID feature reports on the mouse interface (vendor `1532`), as documented by [OpenRazer](https://github.com/openrazer/openrazer): class `0x07`, command `0x80` for the battery and `0x84` for charging.
- **JBL Quantum 350 Wireless**: the mod reads HID feature report `0x49` from the USB receiver (vendor `0ECB`), the same read JBL's QuantumENGINE app makes. The report holds the battery percentage in one byte, and reading it doesn't send any command to the headset. While the headset is off the receiver answers 0, and the mod hides it.
- **Device type**: comes from the container's category (`Input.Mouse`, `Audio.Headphone`, …). If the category doesn't say, the mod falls back to keywords in the device name.
- **Interface**: the mod gets the taskbar's `XamlRoot` through `taskbar.dll` symbols, without XAML diagnostics. It then inserts a `StackPanel` with a `FontIcon` (Segoe Fluent Icons) and a `TextBlock` for each device. All XAML changes run on the taskbar thread.
- **"Left of the tray" layout**: the panel goes into the tray's parent grid, with margins that keep its desired size at zero. That keeps the tray's size unchanged, and the tray's size is what the taskbar uses to work out the app area.
- **Unloading**: disabling the mod removes the panel and restores the original layout.

## License

[GNU General Public License v3.0](LICENSE).

Parts of the taskbar access code are adapted from the [Taskbar tray icon spacing and grid](https://windhawk.net/mods/taskbar-notification-icon-spacing) mod by [m417z](https://github.com/m417z), which is also published under the GPL-3.0.

The Razer mouse and XInput controller reading, the charging indicator and the display modes are based on the [Wireless Battery Indicator](https://github.com/simonzk27/windhawk-mods/blob/main/mods/taskbar-wireless-battery.wh.cpp) mod by [simonzk27](https://github.com/simonzk27).

## Files

- [`bluetooth-battery-tray.wh.cpp`](bluetooth-battery-tray.wh.cpp): the mod source.
- [`estilos-para-colar.txt`](estilos-para-colar.txt): the Taskbar Styler styles, ready to paste.
- [`LICENSE`](LICENSE): the full GPL-3.0 text.
