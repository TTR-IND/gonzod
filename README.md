# gonzod 2.0

Topology-aware system policy compiler. Salvage of rookd.

rookd got good, then it kept growing until the pieces fought each
other. gonzod is the same laptop-tested policy with a hard fence
around what it is allowed to touch.

## What it owns

- CPU / SMT / NUMA / storage discovery
- cgroup v2 lanes: interactive / batch / maintenance
- Scheduler ceilings and timeslices (stock kernel)
- `schedutil` + min-freq floor, udev hook on AC change
- IRQ affinity for noisy devices (NIC, GPU, NVMe, wifi)
- Dirty writeback tuned by storage type (eMMC/HDD batch vs SSD trickle)
- Block-queue scheduler and readahead (`bfq` on slow, `none` on fast)

Policy is applied at lifecycle edges. There is no polling loop.
Requires the stock kernel booted with `preempt=lazy`.

## What it does not own

| lever              | owner        |
|--------------------|--------------|
| ZRAM, zswap, swappiness, discard | detritusd    |
| page-cache hold    | gonzocache   |
| Xorg / pipewire runit scripts    | the packages that ship them |

rookd enabled zswap *and* expected rookpager to own ZRAM. It also
rewrote the Xorg runit service. That is the wall. Those calls are
gone.

`memory.min` is gone too. A hard memory guarantee on the interactive
lane is pages detritusd cannot reclaim. Interactive gets `memory.low`
(preference). Batch and maintenance get `memory.high` caps.

detritusd is bound into the interactive lane so reclaim stays
scheduled under pressure. gonzocache is bound into batch so the
speculative hold yields first.

## Commands

```
gonzod enable      discover + apply
gonzod inspect     report idle, EPP, writeback, device PM
gonzod login       bind the current session into lanes
gonzod reset       tear down + apply
gonzod disable     tear down
gonzod cpufreq     udev hook
gonzod status
gonzod install     elogind resume, login autostart, runit boot service
gonzod uninstall
```

## Install

```bash
sudo ./install.sh
gonzod status
```

`install.sh` does not install a real-time kernel. It appends `preempt=lazy` to the default Linux GRUB entry (via `GRUB_CMDLINE_LINUX_DEFAULT`, then confirms the first menuentry’s `linux` line) and runs `update-grub`. Reboot so the stock kernel picks up the flag. Confirm with `cat /proc/cmdline`.

Status lives at `/run/gonzo/status`. Topology at `/run/gonzo/topology`.
Log at `/var/log/gonzod.log`.


## 2.0 changes

- SSD/NVMe writeback is now batched (expire 3000, writeback 500, 32/128 MiB) so the device can idle.
- cpuset.cpus is actually written for the calculated interactive/batch sets when the strategy is PARTITIONED.
- Intel HWP Energy Performance Preference is discovered and set (balance_performance on AC, power on battery).
- `gonzod inspect` reports cpuidle driver/governor, EPP, writeback timers and a sample of PCI runtime status.
- Reset restores writeback to batched defaults.

Policy remains event-driven. No permanent daemon.
