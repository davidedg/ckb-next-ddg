#ifndef INPUT_H
#define INPUT_H

#include "includes.h"
#include "usb.h"

// Open input device. Return 0 on success
int os_inputopen(usbdevice* kb);
// Close input device
void os_inputclose(usbdevice* kb);

// Updates keypresses on input device. Lock imutex first (see device.h).
void inputupdate(usbdevice* kb);
// Read indicator LED state and send it back to the keyboard if needed. Lock dmutex first.
void updateindicators_kb(usbdevice* kb, int force);

// Initializes key bindings for a device. Lock imutex first.
void initbind(binding* bind, usbdevice* kb);
// Frees key binding data for a device. Lock imutex first.
void freebind(binding* bind);

// Cancel and drain all playback before closing input. Called with imutex held;
// releases it while waiting and returns with it held. Workers never take dmutex.
void stop_macros(usbdevice* kb);

// Note: unlike most commands, bind commands will lock/unlock the device's imutex
// Binds a key
void cmd_bind(usbdevice* kb, usbmode* mode, int dummy, int keyindex, const char* to);
// Unbinds a key
void cmd_unbind(usbdevice* kb, usbmode* mode, int dummy, int keyindex, const char* ignored);
// Resets a key binding
void cmd_rebind(usbdevice* kb, usbmode* mode, int dummy, int keyindex, const char* ignored);
// Creates or updates a macro. Pass null strings to clear all macros
void cmd_macro(usbdevice* kb, usbmode* mode, const int notifynumber, const char* keys, const char* assignment);

#ifdef OS_LINUX
// Is a key a modifier?
#define IS_MOD(s) ((s) == KEY_CAPSLOCK || (s) == KEY_NUMLOCK || (s) == KEY_SCROLLLOCK || (s) == KEY_LEFTSHIFT || (s) == KEY_RIGHTSHIFT || (s) == KEY_LEFTCTRL || (s) == KEY_RIGHTCTRL || (s) == KEY_LEFTMETA || (s) == KEY_RIGHTMETA || (s) == KEY_LEFTALT || (s) == KEY_RIGHTALT || (s) == KEY_FN)
#else
// Scroll Lock and Num Lock aren't modifiers on OSX
#define IS_MOD(s) ((s) == KEY_CAPSLOCK || (s) == KEY_LEFTSHIFT || (s) == KEY_RIGHTSHIFT || (s) == KEY_LEFTCTRL || (s) == KEY_RIGHTCTRL || (s) == KEY_LEFTMETA || (s) == KEY_RIGHTMETA || (s) == KEY_LEFTALT || (s) == KEY_RIGHTALT || (s) == KEY_FN)
#endif

// OS-specific event handlers. Should only be called within the above functions.

// Generate a keypress or mouse button event
void os_keypress(usbdevice* kb, int scancode, int down);
// Generate mouse movement
void os_mousemove(usbdevice* kb, int x, int y);
void os_mousescroll(usbdevice* kb, int x, int y);

// Perform OS-specific setup for indicator lights. Called when the device is created. Return 0 on success.
int os_setupindicators(usbdevice* kb);

// OS Specific function to either send a report to the input subsystem, or sync the previously sent events
void os_inputsync(usbdevice* kb, int s_kb, int m);

// The binding owns the macro until removal. An active worker then owns it until
// completion. abort is written with imutex and mmutex2 held; event playback reads
// it under imutex, FIFO waiters under mmutex2. All other macro state uses imutex.
typedef struct _macro_param {
    usbdevice* kb;
    keymacro* macro;
    struct _macro_param* next;
    char abort;
    char removed; // protected by imutex: binding has transferred ownership
} macro_param;

#endif  // INPUT_H
