"""Two concurrent timers using the Pybricks library, without moving hardware."""

from pybricks.parameters import Axis, Color
from pybricks.tools import StopWatch, cross, multitask, run_task, wait


async def timer(name, delay):
    clock = StopWatch()
    await wait(delay)
    print(name, clock.time(), "ms")
    return name


async def main():
    print("Cross product:", tuple(cross(Axis.X, Axis.Y)))
    print("Half brightness:", tuple(Color.YELLOW * 0.5))
    print("Completed:", await multitask(timer("first", 20), timer("second", 40)))


run_task(main())
