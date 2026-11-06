# PlatformIO post-build script: copies the firmware image to <project>/firmware/franke-can_<env>.bin
# after every successful build, so it is easy to find for the OTA upload in the web UI.
import os
import shutil

Import("env")  # noqa: F821  (provided by PlatformIO/SCons)


def copy_firmware(source, target, env):
    src = str(target[0])
    out_dir = os.path.join(env.subst("$PROJECT_DIR"), "firmware")
    os.makedirs(out_dir, exist_ok=True)
    dst = os.path.join(out_dir, "franke-can_%s.bin" % env.subst("$PIOENV"))
    shutil.copyfile(src, dst)
    print("copy_firmware: %s (%d KB)" % (os.path.relpath(dst, env.subst("$PROJECT_DIR")), os.path.getsize(dst) // 1024))


env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", copy_firmware)  # noqa: F821
