"""PlatformIO post-script: pad and sign the app image for OTA (src/ota.h).

The sdkconfig asks for signed apps without hardware secure boot
(SECURE_SIGNED_APPS_NO_SECURE_BOOT, RSA-3072, the Secure Boot V2 scheme).
idf.py would pad and sign the image itself; PlatformIO runs elf2image on
its own and does neither, so this script does both:

  1. elf2image gets --secure-pad-v2, as ESP-IDF's own build adds it, so
     the signature block lands where the bootloader and OTA code look;
  2. after firmware.bin is built, espsecure signs it in place and checks
     the result, and a copy named after the version goes beside it
     (mCOLD_<version>.bin) -- the file to put on the server.

The key: keys/ota_signing.pem in the project, or the path in
MCOLD_SIGNING_KEY. It is gitignored and must never be committed; keep a
copy somewhere safe -- a box only accepts updates signed with the key its
running image was signed with. Make one with:

  python ~/.platformio/packages/tool-esptoolpy/espsecure.py \\
      generate_signing_key --version 2 --scheme rsa3072 keys/ota_signing.pem

espsecure needs `cryptography` and `ecdsa` in PlatformIO's Python
(~/.platformio/penv): `python -m pip install cryptography ecdsa`.
"""
import os
import re
import shutil
import subprocess

Import("env")  # noqa: F821  (SCons)

PROJECT = env.subst("$PROJECT_DIR")  # noqa: F821
KEY = os.environ.get("MCOLD_SIGNING_KEY") or os.path.join(PROJECT, "keys", "ota_signing.pem")

# 1. Pad. The ElfToBin action is one command string; espidf.py already
#    added its flags in front of "-o $TARGET".
builder = env["BUILDERS"]["ElfToBin"]  # noqa: F821
cmd = builder.action.cmd_list
if "--secure-pad-v2" not in cmd:
    assert " -o $TARGET" in cmd, "elf2image command changed: " + cmd
    builder.action.cmd_list = cmd.replace(" -o $TARGET", " --secure-pad-v2 -o $TARGET", 1)


def version():
    with open(os.path.join(PROJECT, "CMakeLists.txt"), encoding="utf-8") as f:
        m = re.search(r'set\(PROJECT_VER\s+"([^"]+)"\)', f.read())
    return m.group(1) if m else "unknown"


def sign(target, source, env):
    image = str(target[0])
    if not os.path.isfile(KEY):
        print("\n*** No OTA signing key at %s." % KEY)
        print("*** An unsigned image cannot accept OTA updates; see tools/sign_app.py.\n")
        return 1
    espsecure = os.path.join(env.PioPlatform().get_package_dir("tool-esptoolpy"), "espsecure.py")
    py = env.subst("$PYTHONEXE")
    signed = image + ".signed"
    for args in (["sign_data", "--version", "2", "--keyfile", KEY, "--output", signed, image],
                 ["verify_signature", "--version", "2", "--keyfile", KEY, signed]):
        r = subprocess.run([py, espsecure] + args, capture_output=True, text=True)
        if r.returncode:
            print(r.stdout + r.stderr)
            return 1
    os.replace(signed, image)
    out = os.path.join(os.path.dirname(image), "mCOLD_%s.bin" % version())
    shutil.copyfile(image, out)
    print("Signed for OTA: %s (%d bytes)" % (out, os.path.getsize(out)))
    return 0


env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", sign)  # noqa: F821
