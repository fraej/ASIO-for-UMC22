# Optimization notes

Version 1.0.0 is the first public release and contains every change below. The version numbers in these notes (1.4.0, 1.4.1, 1.5.0) refer to development builds before that release, kept so the measurements stay comparable; 1.5.0 is the code released as 1.0.0.

## Input alignment and extra output buffers

Development build 1.5.0 added two engine features.

| Change | Effect | Limit |
| --- | --- | --- |
| Input alignment | Keeps enough capture requests in flight to see 8 ms of recorded input (two to nine packets), counts at every buffer how many recorded packets wait behind the current one, and every 40 ms skips what waited beyond a reserve of at least 2 ms (and one buffer) during the whole window. | Only skips input that is already recorded, so it never waits for input. Windows' default 10 ms capture blocks leave nothing to skip. A dropout within 80 ms after a skip adds a buffer to the reserve. |
| `ExtraOutputBuffers=0..2` | Queues one or two more output buffers. Each lets the host be one buffer late without a dropout. | Each adds one buffer of output latency, which is reported to the host. After a dropout, the delay is measured once the deeper queue has refilled. |

### Measured on a UMC22

Same setup as the 1.4.1 results below, 5-second runs. "Input queued" is the driver's record position minus
every input buffer the engine consumed: all recorded input not yet delivered to the host, sampled 12 times
per run.

| Input queued (average of two runs) | 1.4.1 | 1.5.0 |
| --- | --- | --- |
| 48 samples, default capture | 10.8 ms | 9.9 ms (nothing skipped) |
| 48 samples, `CaptureBufferMs=3` | 11.1 ms | 5.1 ms (5 buffers skipped) |
| 96 samples, default capture | 11.0 ms | 10.9 ms (nothing skipped) |
| 96 samples, `CaptureBufferMs=3` | 10.7 ms | 6.8 ms (2 buffers skipped) |

With the host made late on purpose about 100 times per run (a busy loop in every 50th callback):

| Output dropouts | No extra buffer | 1 extra buffer | 2 extra buffers |
| --- | --- | --- | --- |
| 48 samples, host 1.3 ms late | 126 | 0 | 0 |
| 48 samples, host 2.3 ms late | 170 | 110 | 0 |
| 96 samples, host 2.6 ms late | 57 | 0 | – |

- Runs without a late host had no dropouts, skipped input after dropouts, or I/O errors.
- With `CaptureBufferMs=3`, one extra output buffer and the late host together, there were no dropouts, and
  about 5.7 ms (48 samples) and 6.7 ms (96 samples) of input was queued.
- Even with 126–170 dropouts per run, queued input stayed at its usual 10–11 ms: dropout recovery and input
  alignment did not drift.
- The reported input latency keeps its 10 ms baseline estimate, so with 3 ms requests it overstates the real
  input latency until the loopback tool calibrates it.
- The physical round trip, long sessions and DAW load are not yet measured.

## 1.4.1 optimization build

Based on development build 1.4.0.
The changes combine lower per-buffer overhead with an optional request for
shorter Windows USB capture buffering. They were measured on a UMC22 in short
benchmark runs (see [Hardware results](#hardware-results)); the physical round
trip and long DAW sessions are not yet measured.

### Combined changes

| Change | Effect | Limit |
| --- | --- | --- |
| `CaptureBufferMs=3..10` | Requests a shorter capture URB duration through `KSPROPERTY_AUDIO_BUFFER_DURATION`, before streaming. | Defaults to `0` (unchanged); rejection is diagnostic only. Acceptance is not a measured delay. |
| Capture requeue before host DSP | Copies input into the ASIO buffer, then returns the completed packet to Windows before invoking the host. | Still uses two packets per direction and paired completion scheduling. |
| Atomic stop polling | Removes repeated zero-timeout event waits from the audio loop. | The stop event remains necessary to wake blocking waits; lifecycle calls remain serialized. |
| Bounded input recovery | Can discard up to four stale completed input packets per callback after a stall. | Stops starting more skips after 100 microseconds or one eighth of a period, and yields to output. Kernel-call duration itself is not bounded. |
| Grouped SSE2 output conversion | Packs eight stereo frames using two saturating packs instead of four. | This reduces conversion work, not USB latency. AVX2 is not required. |

The host reads its own converted input after the capture packet is returned to
Windows. Requeue or completion failure stops streaming before another host
callback; outstanding packets are still cancelled and drained before release.
The ASIO ABI and two-input/two-output format are unchanged.

The reported input latency retains the baseline estimate. Replacing 10 ms with
the requested duration would mistake a successful property SET for a hardware
measurement. Use the loopback tool to calibrate the setting that actually works
on the target system.

The loopback tool also handles negative corrections larger than the reported
output delay. It reduces the input offset first, then output if needed, instead
of losing part of the correction when output latency is clamped to zero. Positive
corrections still increase the output offset. This calibrates the measured total;
it does not establish the delay of each direction independently.

### Software validation

Validated on a Linux x86-64 build host using MinGW-w64 GCC 12 and Wine 8:

- `make test`: conversion, latency calibration, engine, ASIO lifecycle and
  compiled-DLL load tests pass.
- Engine simulation covers every multiple of 48 from 48 through 1008 samples
  at 44,100 and 48,000 Hz, with steady streaming and an injected host stall.
  The five-period stall case discards three stale input buffers in one callback,
  preserving the expected input sequence and counting one dropout.
- Longer stalls exercise the four-skip cap and a simulated 200-microsecond
  submission cost exercises the time limit. Recovery submission/completion
  failures, early-requeue failure, stop while waiting, delayed cancellation,
  join failure and restart are covered.
- Capture-duration tests cover synchronous and pending requests, acceptance,
  rejection, invalid values and changes attempted while streaming.
- Host input remains correct after the raw capture packet is overwritten.
- Calibration covers a 7 ms capture reduction, positive and negative corrections,
  offsets already clamped to zero, both rates and multiple buffer sizes. The
  corrected reported total agrees with the supplied measurement within one sample.
- Conversion tests cover every 16-bit PCM value, random float bit patterns,
  clipping, NaNs/infinities, channel masks, unaligned output, tails, all supported
  lengths, rounding modes and denormal handling. These also pass GCC 14
  AddressSanitizer and UndefinedBehaviorSanitizer checks (`detect_leaks=0`).
- The built DLL loads without additional MinGW runtime DLLs. Its import table
  contains only Windows system libraries; the 1.4.1 DLL reported version 10401
  and two input/two output channels.

These tests establish behavior under the modeled conditions. They do not
measure Windows USB scheduling, DPC/ISR delays, DAW processing load or physical
round-trip latency; the hardware results below measure some of these on a UMC22.

### Hardware results

Measured on a UMC22 with Windows' USB Audio 1.0 driver (`usbaudio`) under Windows 11 on an Intel Core
i7-1165G7, at 48 kHz, streaming silence through the engine with a minimal host callback. Each
configuration ran three times for 6 seconds, interleaved with 1.4.0.

| | 1.4.0 | 1.4.1 | 1.4.1, `CaptureBufferMs=3` |
| --- | --- | --- | --- |
| Audio-thread CPU per buffer, 48 samples | 87.7 µs | 85.4 µs | 88.7 µs |
| Audio-thread CPU per buffer, 96 samples | about 128 µs | about 120 µs | about 106 µs (noisy) |
| Input waiting in Windows' driver, 48 samples | 8.2–10.2 ms | 8.1–8.2 ms | 7.8 ms in every run |
| Input waiting in Windows' driver, 96 samples | 7.2–8.2 ms | 6.9–8.3 ms | 6.3–6.5 ms |
| System DPC time | 0.2–1.5% | 0.3–1.3% | 0.4–1.5% |
| Dropouts in these runs | 0 | 0 | 0 |

- CPU is the audio thread's cycle count, including its time inside Windows calls. Almost all of it
  is spent in the USB audio driver's request handling; the 1.4.1 changes save about 2.6% at 48 samples.
- `usbaudio` accepts `CaptureBufferMs` values of 3 and 5. It rejects a wrong property ID (error 1168),
  a wrong property set (error 1170), and 1 ms (error 50), so acceptance is a real answer from the driver.
- With 3 ms requests, input waits 0.3–1 ms less on average and varies less between starts, which keeps
  a loopback calibration valid across restarts. Most of the remaining wait is input recorded while
  playback starts, which the engine does not trim. At 96 samples the system handled about 170 more
  DPCs per second; DPC time stayed below 1.5%.
- In other short runs at 48 samples, single dropouts occurred occasionally with both versions.
- The 1.4.0 DLL needs MinGW's `libwinpthread-1.dll` and fails to load (Windows error 126) when MinGW is
  not on the `PATH`. The 1.4.1 DLL loads with only Windows system directories on the `PATH`.
- Defects planted in each new feature (input copy order, recovery time budget and skip cap, stop flag,
  capture-request error, both SSE2 packing paths, calibration) all make the automated tests fail.

These are short runs on an idle system. Long sessions, DAW load, and the physical round trip still
need the procedure below.

### Test on the interface

Follow [hardware validation](HARDWARE_VALIDATION.md). Compare the previous DLL
with this build at `CaptureBufferMs=0`, then compare `5` and `3` on this build.
Close the host before replacing the DLL or changing the setting, and keep the
previous DLL and calibration settings available for comparison.

Start at 96 samples/48 kHz, then try 48 samples under the intended DAW workload.
Record the capture request result, raw measured round trip, dropout counters
and audible discontinuities. Calibrate only after selecting a stable setting.
If a shorter request is unstable, increase it or restore `0`; choose a larger
ASIO buffer if the host cannot meet the callback deadline.

Microsoft documents the property and its DPC cost in
[KSPROPERTY_AUDIO_BUFFER_DURATION](https://learn.microsoft.com/en-us/windows-hardware/drivers/audio/ksproperty-audio-buffer-duration).
The 3..10 ms request range and registry-override precedence follow its Windows 7
USB audio guidance (pages 33–34 of this
[archived paper](https://doczz.net/doc/7735697/changes-to-the-windows-7-usb-audio-class-driver));
the results above confirm that Windows 11's `usbaudio` accepts these requests on a UMC22.
