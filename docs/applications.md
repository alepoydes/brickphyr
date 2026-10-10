# Applications and runtime contract

This is the selected application model, not a released ABI. Function signatures,
binary metadata, and wire encodings will be defined with the first implementation.
Do not build a second API description by copying fields into multiple runtimes.

## Application formats

One application executes at a time, under one of the engines enabled in the
installed firmware. Native applications do not initialize MicroPython or WAMR.
Python and WASM use the same device services through their respective bindings.

For interpreted engines, document the privilege of the VM, its bindings, and any
native extensions. Language-level restrictions alone are not evidence of MPU
isolation. Shared service ownership and lifetime rules apply to every engine.

Use Zephyr LLEXT as the initial native loader. Evaluate the actual ELF relocation,
floating-point ABI, memory alignment, and toolchain requirements on the target
before publishing SDK flags. C, C++, Rust, and Zig can be investigated as producers
of a compatible native artifact; no working Brickphyr integration for them is yet
claimed. Language runtimes, constructors, unwinding, and standard libraries can
add requirements beyond a shared CPU target.

Use WAMR for WASM. Start with a supported execution mode that fits the hub;
interpreter versus AOT remains a measured implementation choice. A new JIT is not
part of bring-up. Portable WASM still needs compatible hardware imports and memory
limits. Native MicroPython `.mpy` modules are a separate compatibility question,
not the Brickphyr native application format.

## Admission and lifetime

The supervisor must establish the following order, whatever internal state names
the implementation uses:

1. Identify the application format and required engine, ABI, capabilities, and
   resources. Reject incompatible or malformed input before application execution.
2. Stop the preceding application and confirm device operations/callbacks are
   quiescent before replacing its resources. Uploading bytes is not a STOP.
3. Load into owned memory, resolve allowed imports, and establish the application
   memory domain and permissions. Do not expose arbitrary kernel symbols.
4. Run application initialization and entry in the application domain, including
   constructors supplied by the image.
5. On explicit stop, normal return, or fault, cancel outstanding work, put owned
   actuators in their specified stopped condition, and release resources before
   unloading code. Do not free buffers while a driver or callback can still use them.

The privileged supervisor must be able to terminate an application that never
yields. User cleanup is optional assistance, not a prerequisite for stopping.
Recovery from a fault is only reported after the corresponding platform action
succeeds; an unconfirmed actuator stop cannot be presented as a clean idle state.
The board implementation must define its actual stop/inhibit mechanism.

Zephyr's loader does not automatically add an extension to a user memory domain.
Initialization and teardown can execute image-provided code, so their calling
context matters. See [LLEXT loading](https://docs.zephyrproject.org/latest/services/llext/load.html).

## Device interface

Expose the operations applications need, not unrestricted register access. Keep
the native path short, with caller-owned buffers and bounded calls wherever
possible. A syscall boundary does not require allocating a Python object, copying
an entire history, or dispatching through a generic service framework.

For every public operation specify units, numeric range, ownership, lifetime,
blocking/cancellation behavior, errors, and whether it allocates. For sensing,
include acquisition time and validity. For commands, distinguish queue acceptance
from application at the device. Multi-motor timing semantics must be stated, not
inferred from two consecutive function calls.

Motor direct drive and servo commands share resource ownership. Flash writes,
controller pairing, or a diagnostic shell cannot bypass the application manager
to fight an active owner. Read-only monitoring should use existing observations
when it can, with a documented bounded cost.

A future SDK is generated/released from the same definitions as the firmware
exports. Initially require a compatible SDK/firmware ABI identity rather than
adding layers to preserve every experimental build. Reject mismatches clearly.

## Host and input devices

An autonomous application can run with no computer. Normal host disconnection,
recorder failure, or a full log queue must not implicitly STOP it. The application's
input policy determines what controller loss means; the firmware reports the event
and stops treating stale commands as fresh input.

Use one controlling desktop connection owner per hub, including uploads and
diagnostics. Other local tools must use that owner or receive a useful busy result.
Supporting multiple observers later requires explicit semantics, not competing
uncoordinated subscribers. This host ownership does not prohibit a simultaneous
Xbox connection through the firmware's Bluetooth stack.

Provide upload, start, explicit stop, capabilities, and logs first. Add equipment
monitoring when needed. Reuse public desktop transport/flashing APIs where they
match; keep protocol extensions distinct and advertise them. Do not assume the
unchanged Pybricks editor can operate every new Brickphyr application format.

## Diagnostics

Normal output consists of state transitions, actionable faults, and bounded
statistics. Detailed capture is an explicit debugging option. Active control may
drop debug messages rather than block indefinitely behind a slow host; report the
drop count. Keep mandatory device events separate from optional diagnostics.

Retain operation context and the original fault through secondary logging/cleanup
failures. Do not add redundant checksums to disposable debug data without an
identified corruption problem. Firmware images and persistent data have separate
integrity and compatibility requirements.
