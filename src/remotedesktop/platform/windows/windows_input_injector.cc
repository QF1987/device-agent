#ifdef _WIN32

#include "remotedesktop/platform/windows/windows_input_injector.h"

#include <array>
#include <cstdio>

namespace device_agent::remotedesktop::windows {

namespace {

struct KeyMapping {
    WORD vk = 0;
    bool shift = false;
    bool ctrl = false;
    bool alt = false;
};

KeyMapping keysymToVk(uint32_t keysym) {
    if (keysym >= 'a' && keysym <= 'z') {
        return KeyMapping{static_cast<WORD>('A' + (keysym - 'a'))};
    }
    if ((keysym >= 'A' && keysym <= 'Z') || (keysym >= '0' && keysym <= '9')) {
        return KeyMapping{static_cast<WORD>(keysym)};
    }
    if (keysym >= 0x20 && keysym <= 0x7e) {
        SHORT mapped = VkKeyScanA(static_cast<char>(keysym));
        if (mapped != -1) {
            return KeyMapping{
                static_cast<WORD>(mapped & 0xff),
                (mapped & 0x0100) != 0,
                (mapped & 0x0200) != 0,
                (mapped & 0x0400) != 0,
            };
        }
    }
    if (keysym >= 0xffbe && keysym <= 0xffc9) {
        return KeyMapping{static_cast<WORD>(VK_F1 + (keysym - 0xffbe))};
    }
    switch (keysym) {
    case 0xff08: return KeyMapping{VK_BACK};
    case 0xff09: return KeyMapping{VK_TAB};
    case 0xff0d: return KeyMapping{VK_RETURN};
    case 0xff13: return KeyMapping{VK_PAUSE};
    case 0xff14: return KeyMapping{VK_SCROLL};
    case 0xff1b: return KeyMapping{VK_ESCAPE};
    case 0xff50: return KeyMapping{VK_HOME};
    case 0xff51: return KeyMapping{VK_LEFT};
    case 0xff52: return KeyMapping{VK_UP};
    case 0xff53: return KeyMapping{VK_RIGHT};
    case 0xff54: return KeyMapping{VK_DOWN};
    case 0xff55: return KeyMapping{VK_PRIOR};
    case 0xff56: return KeyMapping{VK_NEXT};
    case 0xff57: return KeyMapping{VK_END};
    case 0xff63: return KeyMapping{VK_INSERT};
    case 0xffff: return KeyMapping{VK_DELETE};
    case 0xff7f: return KeyMapping{VK_NUMLOCK};
    case 0xffe1:
    case 0xffe2: return KeyMapping{VK_SHIFT};
    case 0xffe3:
    case 0xffe4: return KeyMapping{VK_CONTROL};
    case 0xffe7:
    case 0xffe8: return KeyMapping{VK_LWIN};
    case 0xffe9:
    case 0xffea: return KeyMapping{VK_MENU};
    default:
        return {};
    }
}

bool keysymToUnicode(uint32_t keysym, WORD& code_unit) {
    uint32_t codepoint = 0;
    if (keysym >= 0x01000000 && keysym <= 0x0110ffff) {
        codepoint = keysym & 0x00ffffff;
    } else if (keysym >= 0x00a0 && keysym <= 0x00ff) {
        codepoint = keysym;
    } else {
        return false;
    }

    if (codepoint == 0 || codepoint > 0xffff || (codepoint >= 0xd800 && codepoint <= 0xdfff)) {
        return false;
    }
    code_unit = static_cast<WORD>(codepoint);
    return true;
}

bool sendOne(INPUT& input, std::string& err) {
    HDESK input_desktop = OpenInputDesktop(0, FALSE, GENERIC_ALL);
    if (input_desktop) {
        SetThreadDesktop(input_desktop);
        CloseDesktop(input_desktop);
    }
    if (SendInput(1, &input, sizeof(INPUT)) != 1) {
        if (input.type == INPUT_KEYBOARD) {
            keybd_event(static_cast<BYTE>(input.ki.wVk),
                        (input.ki.dwFlags & KEYEVENTF_UNICODE) != 0
                            ? static_cast<BYTE>(input.ki.wScan)
                            : static_cast<BYTE>(MapVirtualKey(input.ki.wVk, MAPVK_VK_TO_VSC)),
                        input.ki.dwFlags,
                        0);
            return true;
        }
        if (input.type == INPUT_MOUSE) {
            mouse_event(input.mi.dwFlags, input.mi.dx, input.mi.dy, input.mi.mouseData, 0);
            return true;
        }
        char buf[96]{};
        std::snprintf(buf, sizeof(buf), "SendInput failed: %lu", static_cast<unsigned long>(GetLastError()));
        err = buf;
        return false;
    }
    return true;
}

bool sendKey(WORD vk, bool down, std::string& err) {
    INPUT input{};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = vk;
    input.ki.dwFlags = down ? 0 : KEYEVENTF_KEYUP;
    return sendOne(input, err);
}

bool sendUnicode(WORD code_unit, bool down, std::string& err) {
    INPUT input{};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = 0;
    input.ki.wScan = code_unit;
    input.ki.dwFlags = KEYEVENTF_UNICODE | (down ? 0 : KEYEVENTF_KEYUP);
    return sendOne(input, err);
}

bool sendModifierChord(const KeyMapping& mapping, bool down, std::string& err) {
    if (down) {
        if (mapping.shift && !sendKey(VK_SHIFT, true, err)) return false;
        if (mapping.ctrl && !sendKey(VK_CONTROL, true, err)) return false;
        if (mapping.alt && !sendKey(VK_MENU, true, err)) return false;
        return sendKey(mapping.vk, true, err);
    }
    if (!sendKey(mapping.vk, false, err)) return false;
    if (mapping.alt && !sendKey(VK_MENU, false, err)) return false;
    if (mapping.ctrl && !sendKey(VK_CONTROL, false, err)) return false;
    if (mapping.shift && !sendKey(VK_SHIFT, false, err)) return false;
    return true;
}

}  // namespace

bool WindowsInputInjector::keyEvent(uint32_t rfb_keysym, bool down, std::string& err) {
    WORD unicode_unit = 0;
    if (keysymToUnicode(rfb_keysym, unicode_unit)) {
        return sendUnicode(unicode_unit, down, err);
    }

    KeyMapping mapping = keysymToVk(rfb_keysym);
    if (mapping.vk == 0) {
        // Keep the RFB session alive when guacd sends a keysym outside the
        // Phase 1 map; broader mapping belongs to the next input-hardening pass.
        return true;
    }
    return sendModifierChord(mapping, down, err);
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
    if ((button_mask & 0x08) != 0) {
        INPUT wheel{};
        wheel.type = INPUT_MOUSE;
        wheel.mi.dwFlags = MOUSEEVENTF_WHEEL;
        wheel.mi.mouseData = WHEEL_DELTA;
        if (!sendOne(wheel, err)) {
            return false;
        }
    }
    if ((button_mask & 0x10) != 0) {
        INPUT wheel{};
        wheel.type = INPUT_MOUSE;
        wheel.mi.dwFlags = MOUSEEVENTF_WHEEL;
        wheel.mi.mouseData = static_cast<DWORD>(-WHEEL_DELTA);
        if (!sendOne(wheel, err)) {
            return false;
        }
    }
    last_button_mask_ = button_mask & 0x07;
    return true;
}

}  // namespace device_agent::remotedesktop::windows

#endif  // _WIN32
