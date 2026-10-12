from pybricks.hubs import PrimeHub
from pybricks.parameters import Axis
from pybricks.tools import wait

imu = PrimeHub(top_side=Axis.Z, front_side=Axis.X).imu
print("Calibration:", imu.settings())
for _ in range(10):
    try:
        print("Up:", imu.up(), "Tilt:", imu.tilt())
        print("Acceleration:", imu.acceleration(), "Yaw rate:", imu.angular_velocity(Axis.Z))
        print("Heading:", imu.heading(), "Rotation:", imu.rotation(Axis.Z), "Epoch:", imu.epoch())
    except OSError as error:
        print("IMU unavailable:", error)
    wait(100)
