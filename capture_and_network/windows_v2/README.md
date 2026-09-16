# Windows Protocol V2 test environment

This environment uses the installed Visual Studio Build Tools and stores the
source, build tree, executables, sender spool, and receiver spool on drive D.
It does not require OpenCV or a physical camera; the sender generates raw
grayscale frames so the network and recovery path can be tested independently.

Build and test from PowerShell:

```powershell
powershell -ExecutionPolicy Bypass -File .\capture_and_network\windows_v2\build.ps1
```

Run the two processes in separate terminals:

```powershell
.\capture_and_network\windows_v2\build\Release\v2_receiver.exe 19228 D:\cpp_prjs\picture_reconstruct\capture_and_network\windows_v2\runtime\receiver
.\capture_and_network\windows_v2\build\Release\v2_sender.exe 127.0.0.1 19228 D:\cpp_prjs\picture_reconstruct\capture_and_network\windows_v2\runtime\sender 3
```

Received durable frames are stored as
`runtime\receiver\<task-id>\<frame-seq>.frame`. Each file contains an 80-byte
Protocol V2 metadata block followed by raw pixels. Sender files remain in its
spool until the receiver has durably written the matching frame and returned
an ACK. Restarting the sender replays pending files after Resume negotiation.

The automated test sends a four-chunk frame, drops its first ACK, reconnects,
verifies idempotent delivery, then disconnects the next frame before its chunks
and verifies whole-frame retransmission.
