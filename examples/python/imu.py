from pybricks.hubs import PrimeHub
from pybricks.parameters import Axis
from pybricks.tools import wait

imu = PrimeHub().imu
for _ in range(10):
    try:
        print("Up:", imu.up(), "Tilt:", imu.tilt())
        print("Acceleration:", imu.acceleration(), "Yaw rate:", imu.angular_velocity(Axis.Z))
    except OSError as error:
        print("IMU unavailable:", error)
    wait(100)
