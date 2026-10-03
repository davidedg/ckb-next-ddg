# ckb-next: RGB Driver for Linux

**ckb-next** is an open-source driver for Corsair keyboards and mice. It aims to bring the features of Corsair's proprietary CUE software to Linux operating systems. This project is currently a work in progress, but it already supports much of the same functionality, including full RGB animations. More features are coming soon. Testing and bug reports are appreciated!

> __DISCLAIMER__: ckb-next is not an official Corsair product. It is licensed under the GNU General Public License (version 2) in the hope that it will be useful, but with NO WARRANTY of any kind.

### Additional features of this fork [davidedg/ckb-next-ddg](https://github.com/davidedg/ckb-next-ddg):

<sup>  **_NOTE:_** This code is AI-assisted and the upstream developers have a strict no-AI policy, hence the separate fork.</sup>

- Command-line switching of profiles/modes by name or by relative/absolute
  position (`-p`/`-m`, `-P`/`-M`), optionally scoped to one device (`-D`)
- Environment variables (`CKBNEXT_*`) exposed to programs launched from
  "Launch Program" key bindings, identifying the triggering device/key and
  the active/next/previous profile and mode (opt-in `WITH_ENV_VARS` build flag)
- Daemon-side fix for profiles with more modes than hardware mode slots:
  configurable mode-slot count (`--modecount`), correct mode-name sync to the
  daemon, and a GUI warning/indicator for over-limit profiles
- Hardware Sensor animation: maps a live hardware sensor reading (e.g. CPU/GPU
  temperature or fan speed, via Linux `hwmon` or the [OpenLinkHub](https://github.com/jurkovic-nikola/OpenLinkHub)
  API) onto a color gradient, with source filtering and a live value display
- On-board (hardware) profiles of the K95 RGB Platinum, experimental and off by default: its three slots
  as the hardware profile of the GUI, read and written as iCUE writes them, with key remaps, macros,
  texts, key combinations, static lighting and the performance settings (Win Lock options, indicator
  colours). See below before enabling it.

#### K95 RGB Platinum on-board profiles (experimental)

> __WARNING__: this writes to the keyboard's flash memory. It has been tested only on a K95 RGB Platinum
> (`1b1c:1b2d`) with firmware 3.29 and bootloader 3.03, and the daemon does not touch the on-board profiles
> of any other firmware. A write that fails or is interrupted can leave a slot empty or incomplete until it
> is written again, and the keyboard may need to be reset. There is NO WARRANTY of any kind.

It is off unless the daemon is started with `--enable-experimental`, which also enables the devices whose
support is experimental. Without it the daemon sends nothing to the on-board profiles and the GUI handles the
keyboard as any other one. With systemd, run `sudo systemctl edit ckb-next-daemon.service` and add the
`ExecStart` line that `systemctl cat ckb-next-daemon.service` shows, with the option at the end (the path
depends on the package):

```
[Service]
ExecStart=
ExecStart=/usr/bin/ckb-next-daemon --enable-experimental
```

then `sudo systemctl restart ckb-next-daemon.service`.

- The firmware loads the remaps and macros of a slot when the slot becomes active: after writing the active
  slot, switch away from it and back with the profile button.
- The slots written here have no `PROFILE.ZIP`, iCUE's own copy of the profile, and iCUE shows them as
  empty. ckb-next and iCUE are not meant to manage the on-board profiles of the same keyboard.
- The protocol and the file formats: [devices/k95p.md](https://github.com/davidedg/corsair-protocol/blob/master/devices/k95p.md)
  in a fork of corsair-protocol.

![Screenshot](https://i.imgur.com/zMK9jOP.png)

\
Major features:

- Control multiple devices independently
- United States and European keyboard layouts
- Customizable key bindings
- Per-key lighting and animation
- Reactive lighting
- Multiple profiles/modes with hardware save function
- Adjustable mouse DPI with ability to change DPI on button press

### Important information regarding macOS
macOS is no longer officially supported. For more information, please refer to [issue #660](https://github.com/ckb-next/ckb-next/issues/660).

### General information

Most of the information can be found on [ckb-next wiki pages](https://github.com/ckb-next/ckb-next/wiki).

[Supported devices](https://github.com/ckb-next/ckb-next/wiki/Supported-Hardware).

[Linux Installation](https://github.com/ckb-next/ckb-next/wiki/Linux-Installation).

[Troubleshooting](https://github.com/ckb-next/ckb-next/wiki/Troubleshooting).

[Known Issues](https://github.com/ckb-next/ckb-next/wiki/Known-issues).

[Contributing](https://github.com/davidedg/ckb-next-ddg/wiki/Contributing).

[Community Pipe Animation Scripts](https://github.com/ckb-next/ckb-next/wiki/Advanced-Community-Pipe-Scripts).

### Contact

Maintainers reserve the rights to modify and remove issues, pull requests and comments therein, that are denunciating, off-topic, harmful, hateful and overall inappropriate.
Please be appreciative, humble and kind to each other.

* [GitHub Issues](https://github.com/davidedg/ckb-next-ddg/issues)
