# decoupling-logind changes

This fork adds the minimum required to run SDDM without logind. Everything
else — PAM policy, service supervision, runtime configuration, session desktop
entries and initial VT policy — stays with the distribution package.

## Mandatory seat backend

`General.SeatBackend` is required and must be either `logind` or `multiseatd`.
There is no default and no silent fallback; an unset or unknown value is fatal.

With `SeatBackend=multiseatd` the daemon talks to:

| Property  | Value                                 |
| --------- | ------------------------------------- |
| service   | `org.decoupling.multiseatd1`          |
| path      | `/org/decoupling/multiseatd1`         |
| interface | `org.decoupling.multiseatd1.Manager`  |

The proxy is generated from `data/interfaces/org.decoupling.multiseatd1.Manager.xml`.
Seats come from `ListSeats` plus the `SeatNew` and `SeatRemoved` signals.
Losing the bus name removes every seat, and a new owner is relisted, so
multiseatd can be restarted independently of SDDM.

## Late seat connection handoff

For Wayland sessions and the Wayland greeter, the root `sddm-helper` resolves
the authenticated target user's UID and calls `CreateClient(XDG_SEAT, uid)`
immediately before starting the session process. `General.SeatdSocketDir` has
been removed; there is no filesystem socket connection:

```text
root sddm-helper
    ├─ user Wayland compositor lineage receives fd N
    └─ sddm-helper-start-wayland temporarily receives fd N
         ├─ configured Wayland compositor receives fd N
         └─ QML greeter receives neither fd N nor SEATD_FD
```

* the returned descriptor is duplicated close-on-exec and only the intended child clears
  that flag between `fork()` and `exec()`;
* `SEATD_FD` carries the real descriptor number, which is not forced to 3;
* the parent closes its copy once the child is running, so seatd sees EOF when
  the compositor lineage exits;
* `sddm-helper-start-wayland` validates the inherited descriptor, passes it to
  the compositor only, and drops both the descriptor and `SEATD_FD` before
  starting the greeter;
* the root helper retains the opaque UUIDv7 client ID and explicitly revokes
  it after normal exit, failed startup, forced stop and teardown; EOF and
  repeated revocation are safe;
* `LIBSEAT_BACKEND=seatd` selects the existing patched C libseat backend;
* seat removal and D-Bus owner loss stop displays and their compositor/helper
  process groups rather than assuming that a compositor exits on backend EOF;
* `X11` sessions get nothing: Xorg does not consume an inherited libseat
  connection.

## Source tests

Configure a separate build directory with `-DBUILD_WITH_QT6=ON -DNO_SYSTEMD=ON`
(or use Qt 5), then build and run `ctest --output-on-failure`. The private-bus
`SeatConnection` test checks target UIDs, FD passing, move-only cleanup,
normal/startup-failure/forced-stop/teardown revocation and unrelated-child
isolation. `WaylandFd` verifies that the compositor receives the actual FD and
the QML greeter receives none; `MultiseatSeatBackend` covers removal and owner
loss. No running SDDM or physical device is required.
