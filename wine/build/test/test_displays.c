/* What Wine tells a program about the displays: which one is the main ("primary") one, their
 * sizes, refresh rates and places. A console program: it opens no window.
 *
 * Built and run by run_displays.sh. */
#include <windows.h>
#include <stdio.h>
#include <string.h>

static BOOL CALLBACK monitor(HMONITOR handle, HDC dc, RECT *rect, LPARAM param)
{
    MONITORINFOEXA info;
    (void)dc; (void)rect; (void)param;
    memset(&info, 0, sizeof(info));
    info.cbSize = sizeof(info);
    if (GetMonitorInfoA(handle, (MONITORINFO *)&info))
        printf("monitor  %-14s at %ld,%ld  %ldx%ld%s\n", info.szDevice, info.rcMonitor.left, info.rcMonitor.top,
               info.rcMonitor.right - info.rcMonitor.left, info.rcMonitor.bottom - info.rcMonitor.top,
               (info.dwFlags & MONITORINFOF_PRIMARY) ? "  PRIMARY" : "");
    return TRUE;
}

int main(void)
{
    DISPLAY_DEVICEA device, screen;
    DEVMODEA mode;
    DWORD i;
    HDC dc;

    SetProcessDPIAware();
    for (i = 0;; i++)
    {
        memset(&device, 0, sizeof(device));
        device.cb = sizeof(device);
        if (!EnumDisplayDevicesA(NULL, i, &device, 0)) break;
        if (!(device.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP)) continue;
        memset(&mode, 0, sizeof(mode));
        mode.dmSize = sizeof(mode);
        memset(&screen, 0, sizeof(screen));
        screen.cb = sizeof(screen);
        EnumDisplayDevicesA(device.DeviceName, 0, &screen, 0);
        printf("display  %-14s", device.DeviceName);
        if (EnumDisplaySettingsA(device.DeviceName, ENUM_CURRENT_SETTINGS, &mode))
            printf(" %lux%lu at %lu Hz, place %ld,%ld", mode.dmPelsWidth, mode.dmPelsHeight, mode.dmDisplayFrequency,
                   mode.dmPosition.x, mode.dmPosition.y);
        printf("%s  [%s]\n", (device.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) ? "  PRIMARY" : "", screen.DeviceString);
    }
    EnumDisplayMonitors(NULL, NULL, monitor, 0);
    memset(&mode, 0, sizeof(mode));
    mode.dmSize = sizeof(mode);
    if (EnumDisplaySettingsA(NULL, ENUM_CURRENT_SETTINGS, &mode))
        printf("a program asking for \"the display\" gets: %lux%lu at %lu Hz\n", mode.dmPelsWidth, mode.dmPelsHeight,
               mode.dmDisplayFrequency);
    dc = GetDC(NULL);
    printf("desktop: %dx%d; all displays together: %dx%d from %d,%d\n", GetDeviceCaps(dc, HORZRES), GetDeviceCaps(dc, VERTRES),
           GetSystemMetrics(SM_CXVIRTUALSCREEN), GetSystemMetrics(SM_CYVIRTUALSCREEN), GetSystemMetrics(SM_XVIRTUALSCREEN),
           GetSystemMetrics(SM_YVIRTUALSCREEN));
    ReleaseDC(NULL, dc);
    return 0;
}
