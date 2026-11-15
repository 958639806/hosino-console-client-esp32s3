# Retained upstream license texts

The application integrates or modifies material from these separately licensed
projects. The repository's license for original integration work does not
replace their copyrights, SPDX notices, or licenses. These four files are exact
Git object bytes from the revisions below; `copy-index.json` records SHA-256.
Windows checkout newline conversion was ignored only while comparing the
working copies with the original Git objects.

| Project | Revision | Original license path | Retained file |
| --- | --- | --- | --- |
| [Waveshare ESP32-S3-Touch-AMOLED-2.16](https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-2.16) | `225a62bff11b5d0a0b607873860d39485a9a9685` | `LICENSE` | `waveshare-2.16-LICENSE.txt` |
| [Waveshare ESP32-S3-Touch-AMOLED-1.75](https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.75) | `e4344e70c2fa78a13e8a06566507f1ba8af6672a` | `LICENSE` | `waveshare-1.75-LICENSE.txt` |
| [Waveshare ESP32-S3-Touch-AMOLED-1.8](https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.8) | `78e13f852929c2ab4f9d5e0ad1c50ea378dbf2b4` | `LICENSE` | `waveshare-1.8-LICENSE.txt` |
| [Espressif ESP-Brookesia](https://github.com/espressif/esp-brookesia) | `b22c488f50bafe53342c8e171081bd736396ef58` | `license.txt` | `esp-brookesia-LICENSE.txt` |

All four retained project license files are Apache-2.0. This statement does not
relicense their dependencies or third-party assets. Leaf licenses and font
notices retained elsewhere in this repository still apply. Donor-app path and
local-change notes remain in the components' `PORT_SOURCE.md`, `SOURCE.md`,
`HOSINO_SOURCE.md`, and provenance manifests.

Only the 2.16 fixed upstream checkout is a build input to
`scripts/prepare_project.py`; the 1.75/1.8/Brookesia donor code used by the
additional apps is already vendored in the overlays. The first checkout must
contain `examples/esp-idf/03_esp-brookesia` including its Brookesia core and
SquareLine app. Merely downloading a board BSP directory is insufficient.
