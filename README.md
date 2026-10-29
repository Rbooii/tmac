# tmac

A tiny, animated terminal temperature monitor for Apple silicon Macs. No root,
no daemon, no dependencies — just a native C binary.

```
 tmac                                              MacBook Air 13" (M3) · 8-core

   43.4°C   SOC DIE                    thermal: nominal · fanless
   ▁▂▂▃▃▄▄▄▃▃▂▂▃▃▄▅▅▅▄▄▃▃▂▂▃▄▄▅▅▆▅▄▄▃▃▂▂▃▄

   HOT    44.5   ████████████████████░░░░░░░░
   AVG    43.6   ██████████████████░░░░░░░░░░
   COOL   42.3   █████████████████░░░░░░░░░░░

   BOARD  36.4      NAND  35.0      BATTERY  27.9 · 89%

   q quit  space pause  +/- interval  t theme  h help
```

## Highlights

- **SoC die temperatures** (every `PMU tdie` channel), with hot / average / cool.
- **Board, NAND and battery** temperatures in a slim secondary row.
- **Battery charge** and **thermal pressure** state.
- **Live sparkline** with a rolling history window, eased values and a breathing
  status dot.
- **Responsive layout**: adapts down to very small terminals.
- **Themes**: `mint`, `ice`, `mono`; truecolor with automatic 256-color fallback.

## Requirements

- Apple silicon Mac (M1 or newer) running macOS 12+.
- Xcode Command Line Tools (`xcode-select --install`).

## Build

```sh
make
```

The binary is written to `./tmac`. Install it with:

```sh
sudo make install          # /usr/local/bin/tmac
# or
make install PREFIX="$HOME/.local"
```

## Usage

```sh
tmac                       # animated live view
tmac --once                # one snapshot, script friendly
tmac --interval 500        # sample every 500 ms (default 250)
tmac --theme ice           # mint | ice | mono
tmac --plain               # live view without colors
tmac --help
```

When stdout is not a terminal, `tmac` automatically prints one snapshot and
exits, so `tmac | grep SOC` works.

### Keys

| Key        | Action                    |
|------------|---------------------------|
| `q` / `esc`| quit                      |
| `space`    | pause / resume sampling   |
| `+` / `-`  | change sample interval    |
| `t`        | next theme                |
| `r`        | reset the sparkline       |
| `h`        | toggle help               |

## How it works

`tmac` talks to the private `IOHIDEventSystemClient` API and reads temperature
services exposed with usage page `0xff00`, usage `5` (`PMU tdie*`, `PMU tdev*`,
`NAND CH0 temp`, `gas gauge battery`). Each service is read as an
`kIOHIDEventTypeTemperature` event.

Thermal pressure comes from the Darwin notification state
`com.apple.system.thermalpressurelevel`; battery percentage comes from the
`AppleSmartBattery` IORegistry node.

Rendering is a single buffered `write()` per frame at 20 FPS, with sensor
sampling decoupled at 4 Hz. The terminal is restored on `q`, `Ctrl-C`, `SIGINT`,
`SIGTERM` and `SIGHUP`.

## Notes

- Apple does not expose numeric die temperatures through public frameworks.
  This tool uses a private IOHID interface. It works on current macOS releases
  without special entitlements, but a future macOS update could change that.
- MacBook Air models are fanless, so there is no fan RPM to show.
- Some PMU channels report `~-1 °C` when unused; those readings are ignored.
- `--plain` disables colors but still uses the alternate screen while live.

## Troubleshooting

| Symptom | Fix |
|---|---|
| `no temperature sensors found` | Run on Apple silicon; check that no security tool blocks IOHID access. |
| Sparkline stays empty | The first samples take a second; press `r` to restart the graph. |
| Colors look flat | Your terminal may only support 256 colors; try `--theme mono` or a truecolor terminal. |
| Leftover escape codes after a crash | Run `reset`, or `tput rmcup`. |

## Uninstall

```sh
sudo make uninstall
# or
rm /usr/local/bin/tmac
```
