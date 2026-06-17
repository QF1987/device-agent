#ifdef _WIN32

#include "remotedesktop/platform/windows/windows_input_injector.h"

#include <array>

namespace device_agent::remotedesktop::windows {

namespace {

WORD keysymToVk(uint32_t keysym) {
    if (keysym >= 'a' && keysym <= 'z') {
        return static_cast<WORD>('A' + (keysym - 'a'));
    }
    if ((keysym >= 'A' && keysym <= 'Z') || (keysym >= '0' && keysym <= '9')) {
        return static_cast<WORD>(keysym);
    }
    switch (keysym) {
    case 0xff08: return VK_BACK;
    case 0xff09: return VK_TAB;
    case 0xff0d: return VK_RETURN;
    case 0xff1b: return VK_ESCAPE;
    case 0xff50: return VK_HOME;
    case 0xff51: return VK_LEFT;
    case 0xff52: return VK_UP;
    case 0xff53: return VK_RIGHT;
    case 0xff54: return VK_DOWN;
    case 0xff55: return VK_PRIOR;
    case 0xff56: return VK_NEXT;
    case 0xff57: return VK_END;
    case 0xffff: return VK_DELETE;
    case 0xffe1: return VK_SHIFT;
    case 0xffe2: return VK_SHIFT;
    case 0xffe3: return VK_CONTROL;
    case 0xffe4: return VK_CONTROL;
    case 0xffe9: return VK_MENU;
    case 0xffea: return VK_MENU;
    default:
        return 0;
    }
}

bool sendOne(INPUT& input, std::string& err) {
    if (SendInput(1, &input, sizeof(INPUT)) != 1) {
        err = "SendInput failed";
        return false;
    }
    return true;
}

}  // namespace

bool WindowsInputInjector::keyEvent(uint32_t rfb_keysym, bool down, std::string& err) {
    WORD vk = keysymToVk(rfb_keysym);
    if (vk == 0) {
        // Keep the RFB session alive when guacd sends a keysym outside the
        // Phase 1 map; broader mapping belongs to the next input-hardening pass.
        return true;
    }
    INPUT input{};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = vk;
    input.ki.dwFlags = down ? 0 : KEYEVENTF_KEYUP;
    return sendOne(input, err);
}

bool WindowsInputInjector::pointerEvent(uint8_t button_mask, uint16_t x, uint16_t y, std::string& err) {
    const int width = GetSystemMetrics(SM_CXSCREEN);
    const int height = GetSystemMetrics(SM_CYSCREEN);
    INPUT move{};
    move.type = INPUT_MOUSE;
    move.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE;
    move.mi.dx = width > 1 ? static_cast<LONG>((static_cast<uint64_t>(x) * 65535) / (width - 1)) : 0;
    move.mi.dy = height > 1 ? static_cast<LONG>((static_cast<uint64_t>(y) * 65535) / (height - 1)) : 0;
    if (!sendOne(move, err)) {
        return false;
    }

    struct Button {
        uint8_t mask;
        DWORD down_flag;
        DWORD up_flag;
    };
    constexpr std::array<Button, 3> buttons{{
        {1, MOUSEEVENTF_LEFTDOWN, MOUSEEVENTF_LEFTUP},
        {2, MOUSEEVENTF_MIDDLEDOWN, MOUSEEVENTF_MIDDLEUP},
        {4, MOUSEEVENTF_RIGHTDOWN, MOUSEEVENTF_RIGHTUP},
    }};

    for (const auto& button : buttons) {
        const bool was_down = (last_button_mask_ & button.mask) != 0;
        const bool is_down = (button_mask & button.mask) != 0;
        if (was_down == is_down) {
            continue;
        }
        INPUT click{};
        click.type = INPUT_MOUSE;
        click.mi.dwFlags = is_down ? button.down_flag : button.up_flag;
        if (!sendOne(click, err)) {
            return false;
        }
    }
    last_button_mask_ = button_mask & 0x07;
    return true;
}

}  // namespace device_agent::remotedesktop::windows

#endif  // _WIN32
