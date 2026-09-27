# gonzod

Topology-aware system policy compiler. 

## What it owns

- CPU / SMT / NUMA / storage discovery
- cgroup v2 lanes: interactive / batch / maintenance
- RT scheduler ceilings and timeslices
- `schedutil` + min-freq floor, udev hook on AC change
- IRQ affinity for noisy devices (NIC, GPU, NVMe, wifi)
- Dirty writeback tuned by storage type (eMMC/HDD batch vs SSD trickle)
- Block-queue scheduler and readahead (`bfq` on slow, `none` on fast)

Policy is applied at lifecycle edges. There is no polling loop.

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

`install.sh` also installs `linux-image-rt-amd64` and `linux-headers-rt-amd64`, then runs `update-grub`. Reboot onto that image before treating PREEMPT_RT as present. The RT kernel is not removed by `--uninstall`.

Status lives at `/run/gonzo/status`. Topology at `/run/gonzo/topology`.
Log at `/var/log/gonzod.log`.
