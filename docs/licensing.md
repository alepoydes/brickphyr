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

## Planned dependency audit

Dependencies remain separate west checkouts. The bring-up build uses Zephyr,
CMSIS 6, STM32 HAL, and SDK libraries; it contains no Pybricks, MicroPython, WAMR,
or radio service-pack binary. The following is a preliminary platform inventory,
not a completed distribution audit of a firmware image:

| Component | Known main license | Work still required before distribution |
| --- | --- | --- |
| Zephyr | Apache-2.0 for most code | Inspect selected modules/HALs and notices |
| Pybricks own marked code | MIT | Exclude or separately resolve third-party restrictions |
| MicroPython | MIT for core code | Inspect selected port and libraries |
| WAMR | Apache-2.0 WITH LLVM-exception | Preserve applicable license and notices |
| BTstack used by Pybricks | Noncommercial restriction without separate permission | Excluded from the default Brickphyr distribution |
| TI controller initialization/service pack | Not yet established for the actual included artifact | Identify source/version and redistribution terms |

Using Zephyr Bluetooth avoids depending on BTstack; it does not answer the TI
binary licensing question. Likewise, reusing a flashing protocol does not grant
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
