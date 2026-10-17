# Windows / UMC22 validation

The automated tests simulate the KS device and verify packet ownership, errors, cancellation, sample conversion, and the ASIO lifecycle. They do not establish hardware compatibility or a latency improvement. Use this procedure before treating a new DLL as a validated hardware release.

## Record the environment

Record Windows version, CPU, DAW/version, USB port/controller, device hardware IDs, sample rate, buffer size, and DLL version. The default device filter expects `VID_08BB&PID_2902`, or the device named by `DeviceName`. Record the error from the driver if initialization fails; the driver requires both PCM pins on the same KS filter. `build/tools/latency_test.exe --info` prints what the registered driver reports to hosts, and the control panel shows which Windows driver is bound to the device (`usbaudio` for Windows' USB Audio 1.0 driver).

Status: version 1.0.0 is the first public release; 1.0.1 adds the measured latency model (see [optimization notes](OPTIMIZATION_NOTES.md#built-in-latency-model)).

The development builds before it (numbered 1.4 to 1.5 below) were measured as follows. Development build 1.4 streams on a UMC22 under Windows 11 in a DAW, and in 3-4 second benchmark runs at 48, 96, and 192 samples (48 kHz) and 96 samples (44.1 kHz) with no dropouts on an idle system. Injected host stalls of 10 and 30 ms were each counted as one dropout, and the input queued in Windows' driver returned to 7-10 ms afterwards (without the dropout handling: about 100 ms). 48-sample buffers can crackle under a full DAW session load. The physical round trip, long sessions, and other interfaces are not yet measured.

Development build 1.4.1 added earlier capture requeueing, bounded recovery of multiple stale input packets, atomic stop polling, grouped SSE2 output conversion, and the optional `CaptureBufferMs` request. In 6-second benchmark runs at 48 and 96 samples it streamed without dropouts, with and without 3 ms capture requests, which Windows' USB Audio 1.0 driver accepts; see [optimization notes](OPTIMIZATION_NOTES.md#hardware-results). Long sessions under DAW load and the physical round trip with 3 ms requests are not yet measured.

Development build 1.5.0, released as 1.0.0, added input alignment and optional extra output buffers. In 5-second benchmark runs on a UMC22, input alignment lowered the queued input with `CaptureBufferMs=3` from about 11 ms to 5 ms (48 samples) and 7 ms (96 samples), and left default capture alone. One extra output buffer removed every dropout caused by a host 1.3 ms late at 48 samples. See [optimization notes](OPTIMIZATION_NOTES.md#input-alignment-and-extra-output-buffers). Long sessions under DAW load are not yet measured.

Keep the previous DLL available for a comparison in the same setup. Close the host before replacing a loaded DLL.

## Functional checks

- Verify left/right playback and both capture channels independently. Check that selecting only one host output leaves the other silent.
- Check silence, quiet audio, full-scale signals, and clipping. Listen for clicks at startup and shutdown.
- Repeat start/stop at least 100 times, then dispose/recreate buffers. Memory and handle counts should settle rather than grow each cycle.
- Change between 44.1 and 48 kHz through stop/dispose/set-rate/create/start. Confirm the reported and recorded rates agree. Attempts to change the rate with buffers still allocated should return an error and preserve the current rate.
- Unplug the interface while streaming. The stream should stop with an error and request a host reset if supported. Reconnect and reopen the driver. Confirm the DAW remains usable.
- Open a second host: if the device rejects another pin instance, it should return an error without affecting the existing instance.

## Measure latency and stability

Use a physical output-to-input loopback with appropriate levels and run `build/tools/latency_test.exe`. It plays a pulse sequence through the registered driver, matches emitted and recorded pulses, and compares the round trip with the latency the driver reports; `--write` stores the recommended input and output latency offsets. The correction reduces input first when the measured total is shorter, and increases output when it is longer. Repeat after calibrating: the reported and measured round trips should then agree within a few samples.

| Sample rate | Buffer sizes | Duration per case |
| --- | --- | --- |
| 48,000 Hz | 48, 96, 144, 192 | At least 30 minutes |
| 44,100 Hz | 96, 144, 192 | At least 30 minutes |

For each case, test an idle session and a representative DAW session with plugins, recording, and normal background activity. Record physical round-trip delay, audible discontinuities, CPU load, output dropouts, skipped input buffers, callback budget overruns, longest callback/interval, short reads, timeouts, and I/O errors from the ASIO control panel. Inspect DPC/ISR behavior separately if glitches persist.

First test with `CaptureBufferMs=0`. Then compare `CaptureBufferMs=5` and `3`, closing and reopening the host after each edit. Record whether the control panel reports acceptance or rejection, and keep the same latency offsets while comparing raw measured round trips. Only calibrate with `--write` after choosing a stable setting. A successful property request is not a measured duration: Windows version, USB stack, and a `CapturePacketsPerUrb` registry override can affect the result. No registry change is required by this driver. Also compare `ExtraOutputBuffers=0` and `1` at the smallest buffer size you want to use, and record the control panel's input alignment line.

Choose the smallest buffer that remains stable under the intended workload. A successful 48-sample initialization alone does not demonstrate stable 1 ms operation. The dropout counter detects output running out of queued audio at the driver; it cannot see underruns inside the USB stack or the device.

After any dropouts, record a loopback pulse again: the round trip should match the value measured before them, because the driver skips the input recorded while output was stalled.

## Current behavior to account for

The packet queues are serviced as capture/playback pairs, with input requeued before host processing. After a dropout, already-recorded capture packets are skipped until input lines up with output again. Each callback has a skip count and time budget, checked between I/O operations; a slow kernel call can exceed the time budget. Timing is based on observed completions and a software sample counter, not a hardware position register; sample positions advance by the output time lost in dropouts. The first two render packets contain silence. The original 10 ms input estimate describes the observed default USB capture buffering. A shorter capture request leaves that estimate intact until loopback offsets are applied. Include these details when interpreting startup behavior and measured latency.

Control/lifecycle calls should be serialized by the host and performed outside audio callbacks. A stop requested from the worker signals shutdown but returns an error until a control thread can join it. Buffer disposal does not free resources after a failed join. A device driver that never completes cancelled I/O can delay shutdown indefinitely; the driver preserves outstanding buffers rather than freeing memory still owned by the kernel.
