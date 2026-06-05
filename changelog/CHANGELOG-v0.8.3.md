# 0.8.3 — Author attribution in portal footer

Single-line change with a wider intent. The portal footer (rendered on
every page via `http_send_page`) and the `/information` page's Firmware
row now read:

```
mqtt_broker 0.8.3 by Spencer Kittleson
```

## Implementation

- New `FW_AUTHOR` and `FW_FOOTER` macros in `main/version.h`.
  `FW_FOOTER` is a compile-time string-literal concat of `FW_NAME " "
FW_VERSION " by " FW_AUTHOR`, so future bumps update the footer
  automatically.
- Two render sites in `main/portal.c` (the page-wrapper footer and the
  Firmware row in `/information`) updated to use `FW_FOOTER` instead of
  composing `FW_NAME " " FW_VERSION` inline. This is a single source of
  truth — no risk of one site drifting from the other after a future
  refactor.

## Flash impact

`mqtt_broker.bin` grew from `0x120520` → `0x120550`, a delta of **48
bytes** (just the longer literal string emitted twice in the
page-wrapper RODATA).

## Verification

OTA-applied to `http://192.168.22.100/`. Re-captured
`docs/screenshots/timers/list_desktop.png` shows the new footer
rendering correctly. JSON API `firmware.name` / `firmware.version`
fields unchanged so machine consumers don't break.
