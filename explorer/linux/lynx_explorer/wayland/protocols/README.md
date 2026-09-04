# Generated Wayland protocol sources

These files are produced by `wayland-scanner` from the protocol definitions in
`wayland-protocols`, and are committed rather than generated during the build.
Committing them keeps the build hermetic: it needs neither `wayland-scanner`
nor the `wayland-protocols` XML installed on the host.

| file | protocol |
| --- | --- |
| `xdg-shell-client-protocol.h`, `xdg-shell-protocol.c` | `stable/xdg-shell` |
| `xdg-decoration-client-protocol.h`, `xdg-decoration-protocol.c` | `unstable/xdg-decoration` |

## Regenerating

Run `regenerate.py`, which refuses to run with an unsuitable scanner:

```sh
python3 regenerate.py --scanner /path/to/wayland-scanner
```

**The scanner must be older than 1.20.** From 1.20 on, the generated client
headers call `wl_proxy_marshal_flags()`, which libwayland only exports from
1.20 onwards, and the Debian sysroot this repository builds against
(`build/config/sysroot.gni`) ships libwayland 1.17. Older scanners emit
`wl_proxy_marshal()` and `wl_proxy_marshal_constructor()`, which 1.17 does
export; the result still runs against every newer libwayland, so targeting the
older API costs nothing and buys a wider compatibility range.

`regenerate.py`'s docstring has a recipe for building a suitable scanner from
the wayland checkout this repository already syncs for Linux.
