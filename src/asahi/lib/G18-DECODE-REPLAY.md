# G18 command-dump replay

Run 118 on physical J873 reached its first Gallium submission and returned EIO.
Its diagnostic dump then aborted, independently of the GPU failure. Preserve
that distinction: repairing this decoder does not repair rendering.

Failure model, recorded before the repair:

- G18 PPP pointers are compact addresses relative to 64 GiB. Following the
  unexpanded wire address hides valid captured state. Expand only the known
  G18 compact range; retain the wire address in the report.
- G18 Ioto programs use AGX3d instructions, not the older fixed Ioto grammar
  or AGX2 USC records. Select the G18 ISA explicitly, stop at `end`, and reject
  unknown or truncated instructions without guessing another ISA.
- A stateful decoder refill can approach the end of a tracked BO. Every refill
  must honor its remaining extent, including jumps and returns. A diagnostic
  error must not assert or read beyond the captured allocation.
- A decoded instruction cannot consume more bytes than remain in the buffer.
  Unknown records must not make progress beyond its boundary.

Verification must replay the saved run-118 VDM, PPP and Ioto bytes through the
production decoder and retain the resulting dump and source/input hashes.
Also replay a truncated tail and an unknown instruction. This checks actual
diagnostic behavior; it is not GPU execution or OpenGL conformance evidence.
