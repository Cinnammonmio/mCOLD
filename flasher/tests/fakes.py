"""The test's names for the simulator in mflash/sim.py, one box per bus."""
from mflash.sim import (BRINGUP, GNSS, HEALTH, MAC, SimBox, SimBus, SimHardware, app_image,
                        build_dir, partition_table)

__all__ = ["BRINGUP", "GNSS", "HEALTH", "MAC", "FakeBox", "FakeHardware", "app_image",
           "build_dir", "partition_table"]

FakeBox = SimBox


def FakeHardware(box: SimBox) -> SimHardware:
    return SimHardware(SimBus([box]))
