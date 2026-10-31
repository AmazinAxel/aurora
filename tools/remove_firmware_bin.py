# firmware.bin holds the WiFi password in plaintext, so it never stays on disk:
# gone after upload, or straight after the build if nothing is uploading.
Import("env")

import os

from SCons.Script import COMMAND_LINE_TARGETS

BIN = env.subst("$BUILD_DIR/${PROGNAME}.bin")


def remove_bin(source, target, env):
    if os.path.exists(BIN):
        os.remove(BIN)


env.AddPostAction("upload" if "upload" in COMMAND_LINE_TARGETS else BIN, remove_bin)
