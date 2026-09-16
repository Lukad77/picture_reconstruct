# Linux x86 end-to-end pipeline

## Dependencies

Ubuntu/Debian packages:

```bash
sudo apt update
sudo apt install -y build-essential cmake pkg-config libopencv-dev v4l-utils
```

CUDA is not required. Reconstruction uses the CPU OpenCV path so it also runs
on x86 machines without an NVIDIA GPU.

## Reproducible synthetic run

```bash
chmod +x capture_and_network/linux_e2e/run_synthetic_e2e.sh
./capture_and_network/linux_e2e/run_synthetic_e2e.sh
```

The script builds all Linux targets, runs the transport regression test, sends
16 synthetic raw camera frames through the same Protocol V2 path used by the
camera program, and writes:

```text
capture_and_network/linux_e2e/runtime/output/reconstruction.tif
capture_and_network/linux_e2e/runtime/output/weight_map.tif
capture_and_network/linux_e2e/runtime/output/spot_signals.csv
```

Both programs load JSON configuration. From `capture_and_network`, start them
without repeated positional arguments:

```bash
./build-linux/linux_reconstruct_receiver
./build-linux/linux_pipeline_sender
```

The defaults are `config/receiver.json` and `config/sender.json`. Override them
with `--config /path/to/file.json` or the
`PICTURE_RECONSTRUCT_RECEIVER_CONFIG` and
`PICTURE_RECONSTRUCT_SENDER_CONFIG` environment variables.

To compare synchronous and windowed transfer throughput on the current machine:

```bash
./build-linux/benchmark_v2_transfer
```

## Physical V4L2 camera run

Check the camera first:

```bash
v4l2-ctl --list-devices
```

Start the receiver/reconstructor:

```bash
cd capture_and_network
./build-linux/linux_reconstruct_receiver --config config/receiver.json
```

Start camera index 0 in another terminal:

```bash
./build-linux/linux_pipeline_sender --config config/sender.v4l2.example.json
```

Replace the example CSV files with the real scan positions and calibrated spot
locations. Each acquired frame is paired with the corresponding row of the
scan CSV. If a motorized stage is used, move it to that row's coordinates and
settle it before the corresponding camera acquisition; stage control is
hardware-specific and is not implemented by this repository.
