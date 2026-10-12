# Licensing and provenance

New Brickphyr code and documentation use [Apache-2.0](../LICENSE). Dependencies
and reused files keep their original licenses and notices. This is not a claim
that every byte in a future firmware image is Apache-2.0.

## Contribution policy

Use `SPDX-License-Identifier: Apache-2.0` in new source files where appropriate.
Keep truthful copyright attribution and preserve existing notices when modifying
or moving code. Changes intended for Pybricks' MIT-licensed components remain
under MIT so that they can be accepted there without an extra license requirement.
Follow the destination project's rules when contributing upstream.

Apache-2.0 was selected for the main project to align with Zephyr and provide an
explicit contributor patent grant. MIT components can be used while preserving
their notices; selecting Apache for Brickphyr does not erase those obligations.

Selected source ports retain their original license and copyright notices.
Record the original repository, exact revision, file paths and substantive changes
beside each port. The [IMU processing port](../src/imu/README.md), for example,
retains Pybricks' MIT license while its Zephyr integration is Brickphyr code.

## Planned dependency audit

Dependencies remain separate west checkouts. The bring-up build uses Zephyr,
CMSIS 6, STM32 HAL, SDK libraries, and MIT-licensed Pybricks resistor-ladder and
TLC5955 helpers, plus selected IMU, sound, battery, charger and external-port adaptations. Their
[license notice](../LICENSES/Pybricks-MIT.txt) accompanies the source.
IMU acquisition also compiles the ST/Pybricks register helpers and
public continuation macros from the pinned checkout, retaining their
[ST notice](../LICENSES/ST-LSM6DS3TR-C.txt) and
[BSD notice](../LICENSES/Pybricks-BSD.txt) in the firmware package.
The motor computation additionally carries the
[LEGO BSD-3-Clause notice](../LICENSES/LEGO-BSD-3-Clause.txt); selected tables and
initialization retain their notices in the [adapter](../src/motor/README.md).
The optional Bluetooth build adds Mbed TLS, TF-PSA-Crypto and the
CC2564C service pack described below. The experimental Python configuration adds
the MIT-licensed MicroPython core and shared GC/output helpers, with its
[license notice](../LICENSES/MicroPython-MIT.txt) in the firmware package. WAMR
is not integrated. The optional Python library also compiles selected Pybricks
bindings and their portable helpers directly from the dependency, covered by
the existing Pybricks MIT and LEGO BSD notices. The adapted hub bindings retain
their source attribution. The directly compiled 5x5 font also carries the 2016
British Broadcasting Corporation copyright in the Pybricks MIT notice.
The following is a preliminary platform inventory,
not a completed distribution audit of a firmware image:

| Component | Known main license | Work still required before distribution |
| --- | --- | --- |
| Zephyr | Apache-2.0 for most code | Inspect selected modules/HALs and notices |
| Pybricks own marked code | MIT | Exclude or separately resolve third-party restrictions |
| MicroPython | MIT for core code | Inspect selected port and libraries |
| WAMR | Apache-2.0 WITH LLVM-exception | Preserve applicable license and notices |
| BTstack used by Pybricks | Noncommercial restriction without separate permission | Excluded from the default Brickphyr distribution |
| TI CC2564C service pack | TI text file license; binary use on TI devices only | Preserve the unmodified artifact and supplied notices |
| Mbed TLS / TF-PSA-Crypto | Apache-2.0 OR GPL-2.0-or-later; Apache-2.0 selected | Preserve license texts in the firmware package |

The Bluetooth build embeds `initscripts/TIInit_6.12.26.bts` from the pinned
[official TI service-packs repository](https://git.ti.com/cgit/ti-bt/service-packs/tree/?id=3aa1d75f3c2ae77f6e4d36194e3d281b899ab149).
The artifact remains unchanged; runtime platform configuration disables eHCILL
deep sleep because the selected H:4 transport does not implement it. The
[TI license](https://git.ti.com/cgit/ti-bt/service-packs/tree/LICENSE?id=3aa1d75f3c2ae77f6e4d36194e3d281b899ab149)
and cryptographic-library licenses accompany the firmware in `ReadMe_OSS.txt`.
No BTstack code is linked.

Reusing a flashing protocol does not grant
permission to redistribute LEGO firmware. Use legitimate existing restoration
tools and acquisition paths rather than putting proprietary images in Git.

At the first firmware release, derive the inventory from the resolved and actually
included dependencies. Ship required license texts under `LICENSES/` and any
required attribution notices with the artifacts. Add `NOTICE` when there are
applicable notices to carry, not a fabricated list of copyright owners.

## Primary references

- [Apache-2.0 terms](https://www.apache.org/licenses/LICENSE-2.0)
- [Zephyr license](https://github.com/zephyrproject-rtos/zephyr/blob/main/LICENSE)
- [Pybricks license and third-party warning](https://github.com/pybricks/pybricks-micropython/blob/master/LICENSE)
- [MicroPython license](https://github.com/micropython/micropython/blob/master/LICENSE)
- [WAMR license](https://github.com/wasm-micro-runtime/wasm-micro-runtime/blob/main/LICENSE)
- [BTstack license](https://github.com/bluekitchen/btstack/blob/master/LICENSE)

Check the files at the revisions actually selected for a build; upstream default
branch links above are starting points, not license locks.
