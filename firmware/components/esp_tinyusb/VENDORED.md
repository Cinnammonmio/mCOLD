# esp_tinyusb, vendored

Copied from the ESP Component Registry (espressif/esp_tinyusb, the version in
idf_component.yml beside this file) on 2026-10-05, because its MSC storage
glue (tusb_msc_storage.c) defines the TinyUSB SCSI callbacks for a real FAT
partition, and mCOLD answers them itself with a virtual, read-only drive built
from the trip log (firmware/src/usbdrive.cpp).

The one change: CMakeLists.txt no longer builds tusb_msc_storage.c. Everything
else is as fetched (tests removed). Apache-2.0, see LICENSE.

To update: fetch the new version, copy it over this folder, and make the same
one change.
