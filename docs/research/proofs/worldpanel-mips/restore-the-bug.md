# worldpanel-mips — restore-the-bug record for VaCuus.World.MipContentGPU

The durable copy of both outcomes (logs rotate; this file does not). Venue: Linux,
Vulkan, editor automation (`UnrealEditor-Cmd … -ExecCmds="Automation RunTests
VaCuus.World.MipContentGPU; Quit" -unattended -nosplash`, no `-nullrhi`), UE 5.8.1,
2026-08-02. The suppression was commenting out the one `GenerateDestinationMips(RHICmdList)`
call at the sink's copy tail (`VaCuusWorldSink.cpp`, `CopyToDestination`); the copy
path itself was untouched both runs, which is why every mip 0 assertion held.

## Suppressed (the bug restored) — Result={Fail}

```
[2026.08.02-06.19.23:970][197]LogAutomationController: Error: Test Completed. Result={Fail} Name={MipContentGPU} Path={VaCuus.World.MipContentGPU}
[2026.08.02-06.19.23:970][197]LogAutomationController: Error: Expected 'Every copy generated the chain' to be 1, but it was 0. [.../Tests/VaCuusWorldComponentTest.cpp(781)]
[2026.08.02-06.19.23:970][197]LogAutomationController: Error: Expected 'mip 1 over the div equals the div color (the generation): got (0 0 0 0), expected (208 64 48 255) +/-6' to be true.
[2026.08.02-06.19.23:970][197]LogAutomationController: GPU evidence: mip0(24,12)=(208 64 48 255), mip1(12,6)=(0 0 0 0), mip1(56,28)=(0 0 0 0), 1 cop(ies), 0 generation(s)
```

## Restored — Result={Success}

```
[2026.08.02-06.20.17:469][205]LogAutomationController: Display: Test Completed. Result={Success} Name={MipContentGPU} Path={VaCuus.World.MipContentGPU}
[2026.08.02-06.20.17:469][205]LogAutomationController: GPU evidence: mip0(24,12)=(208 64 48 255), mip1(12,6)=(208 64 48 255), mip1(56,28)=(0 0 0 0), 1 cop(ies), 1 generation(s)
```

Reading: mip 0 keeps its exact bytes either way (the copy is not under test); mip 1
is the generator's output alone — (0 0 0 0) with the call suppressed, the div color
to the byte with it restored, and the generation counter is the 1:1-per-copy
observable that fails first.
