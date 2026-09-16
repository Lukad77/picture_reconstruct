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

## Physical V4L2 camera run

Check the camera first:

```bash
v4l2-ctl --list-devices
```

Start the receiver/reconstructor:

```bash
./capture_and_network/build-linux/linux_reconstruct_receiver \
  19090 receiver_spool capture_and_network/linux_e2e/spots_example.csv output 256 256
```

Start camera index 0 in another terminal:

```bash
./capture_and_network/build-linux/linux_pipeline_sender \
  RECEIVER_IP 19090 sender_spool capture_and_network/linux_e2e/scan_example.csv 0 640 480 10
```

Replace the example CSV files with the real scan positions and calibrated spot
locations. Each acquired frame is paired with the corresponding row of the
scan CSV. If a motorized stage is used, move it to that row's coordinates and
settle it before the corresponding camera acquisition; stage control is
hardware-specific and is not implemented by this repository.
