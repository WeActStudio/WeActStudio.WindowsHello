**English** · [简体中文](CONTRIBUTING.zh-CN.md)

# Contributing

Issues and pull requests are welcome. This driver was reverse-engineered, so  
**a verification report from any real machine is valuable**.

## Before filing an issue

Run the self-check and paste the output:

```bash
sudo ./scripts/doctor.sh
```

Then add whichever of these apply (different problems need different pieces):

```bash
lsusb | grep -i 28e9                    # is the device there
lsusb -d 28e9:5159 -v | head -25        # descriptors / endpoints
fprintd-list $USER                      # what fprintd sees
journalctl -u fprintd -b --no-pager | tail -50
dmesg | tail -30
uname -a; cat /etc/os-release | head -3
```

If the problem is reproducible, describe the **exact steps** and **expected vs actual**.  
For matching problems, state which enrollment attempt this was and how many templates are in  
the database (`sudo ./tools/fp-list.sh` prints the on-chip record count).

## Code contributions

- Driver code lives in `src/`; follow libfprint's style (GNU indentation, `g_autoptr`, etc.);
- build it at least once before submitting:
  ```bash
  # vendor/libfprint-1.94.100.tar.gz is committed — nothing to fetch
  sudo ./scripts/install.sh --drivers moc5159
  sudo ./scripts/doctor.sh
  ```
- if you changed protocol-related logic, attach a capture or a real-machine log to the PR  
  (USBPcap or usbmon, either is fine) and say which machine and firmware version you used;
- **do not commit unpacked libfprint source under `vendor/`** — the pinned archive  
  `vendor/libfprint-1.94.100.tar.gz` is the only file that belongs there, and it must stay  
  at exactly 1.94.100 (see `.gitignore`);
- do not commit build output under `build/`.

## About reverse-engineering data

`docs/en/04-protocol-notes.md` records **only what has been verified**. If you want to add a  
conclusion about a new command, bring the evidence with it:

1. Trigger conditions (what command was sent, what state the device was in);
2. The complete reply (128 bytes in hex — do not truncate);
3. A control group (how the same command replies under different conditions).

Conclusions backed only by an observation, with no control, easily become the next person's trap.

## Known "this is not a bug"

All of the following are firmware behaviour; a driver PR cannot fix them (details in  
`docs/en/04-protocol-notes.md` §6):

- enrolling a second finger has no effect — the firmware compares only the first record;
- `0x30` is always `0x00` — it cannot be used to detect a finger press;
- creating a second template database makes the device unresponsive until replugged.

## License

Submitted code is licensed under **LGPL-2.1-or-later**, consistent with the project.
