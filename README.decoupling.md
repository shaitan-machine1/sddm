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

For Wayland sessions and the Wayland greeter, the root `sddm-helper` connects
to `<General.SeatdSocketDir>/<XDG_SEAT>.sock` immediately before starting the
session process:

```text
root sddm-helper
    ├─ user Wayland compositor lineage receives fd N
    └─ sddm-helper-start-wayland temporarily receives fd N
         ├─ configured Wayland compositor receives fd N
         └─ QML greeter receives neither fd N nor SEATD_FD
```

* the descriptor is opened close-on-exec and only the intended child clears
  that flag between `fork()` and `exec()`;
* `SEATD_FD` carries the real descriptor number, which is not forced to 3;
* the parent closes its copy once the child is running, so seatd sees EOF when
  the compositor lineage exits;
* `sddm-helper-start-wayland` validates the inherited descriptor, passes it to
  the compositor only, and drops both the descriptor and `SEATD_FD` before
  starting the greeter;
* `X11` sessions get nothing: Xorg does not consume an inherited libseat
  connection.
