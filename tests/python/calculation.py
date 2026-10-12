# SPDX-License-Identifier: Apache-2.0
import gc


def fibonacci(count):
    a, b = 0, 1
    for _ in range(count):
        a, b = b, a + b
    return a


gc.collect()
print(fibonacci(20))
