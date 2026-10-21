# Measuring GPU time on Linux

For Juan Llorca Martínez, or anyone testing on Linux. Card 35 (the Xenos shader compiler) is
measured on macOS and Linux before and after each phase, on the same route. With the game capped
at 30 FPS the frame time hides how much work the GPU does, so the runtime writes the GPU time of
every frame (`host_gpu_time_us`, from Vulkan timestamp queries) next to the frame time.

## 1. Build

On the branch to measure (for the baseline: `feat/35-xenos-shader-compiler` at the commit named
on the card):

```bash
git fetch origin
git switch feat/35-xenos-shader-compiler
git pull
./iniciar-linux.sh
```

Then start the game once from the launcher so that its settings file exists, and quit.

## 2. Record

```bash
scripts/gpu_baseline.sh start linux-baseline
```

This enables the per-frame CSV in the launcher's `azura.toml` (a copy of the file is kept). Start
the game from the launcher and play the route:

1. Menus, the Bink intro and the campaign from the start.
2. Play past the room with light shafts, as far as you like (10 to 30 minutes).
3. Go back to the room with light shafts, or stay in it at the end, and look at the busiest part
   of it for one minute.
4. Quit the game.

```bash
scripts/gpu_baseline.sh finish
```

This restores `azura.toml` and prints the summary.

## 3. Send

On the Trello card, or to Carolina Molina Elvira:

- The summary printed by `finish`.
- The CSV it names (`~/.local/share/com.gears3azura.launcher/gpu_baseline/linux-baseline_*.csv`).
- The GPU and driver: the first lines of the newest `azura_*.log` in
  `~/.local/share/com.gears3azura.launcher/runtime/logs/` print `deviceName`, `driverName` and
  `driverInfo`.

If the summary says `host_gpu_time_us is 0`, the driver has no timestamps on the graphics queue;
send it anyway, the frame time is still useful.

## macOS reference (base M4, Metal backend)

Same route, 21 October 2026, 9.1 minutes of gameplay: GPU time per frame median 18.9 ms, p90
23.1 ms, p99 30.2 ms; frame time median 33.3 ms (the cap), p99 104.6 ms (hitches that are not GPU
time).
