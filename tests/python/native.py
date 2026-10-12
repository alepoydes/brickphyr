# SPDX-License-Identifier: Apache-2.0
import gc

import micropython


@micropython.native
def transform(value):
    return (value + 7) * 3


@micropython.viper
def triangular(n: int) -> int:
    total = 0
    for i in range(n):
        total += i
    return total


assert transform(7) == 42
gc.collect()
assert triangular(100) == 4950
assert transform(-3) == 12
print("native and viper 42 4950")
