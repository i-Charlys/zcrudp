"""CMake package/embedding smoke tests; no network or system installation."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
CMAKE = os.environ.get("CMAKE", "cmake")
CTEST = os.environ.get("CTEST", "ctest")


def run(*args):
    subprocess.run([str(a) for a in args], check=True)


def build_and_test(build):
    run(CMAKE, "--build", build, "--parallel", "2")
    run(CTEST, "--test-dir", build, "--output-on-failure")


with tempfile.TemporaryDirectory(prefix="zcrudp-cmake-") as temporary:
    tmp = Path(temporary)
    build = tmp / "library"
    prefix = tmp / "original prefix"
    relocated = tmp / "relocated prefix"
    run(CMAKE, "-S", ROOT, "-B", build, "-DCMAKE_BUILD_TYPE=Release",
        "-DZCRUDP_WARNINGS_AS_ERRORS=ON")
    build_and_test(build)
    run(CMAKE, "--install", build, "--prefix", prefix)
    shutil.move(prefix, relocated)
    consumer = tmp / "consumer"
    run(CMAKE, "-S", ROOT / "tests/cmake_consumer", "-B", consumer,
        f"-DCMAKE_PREFIX_PATH={relocated}")
    build_and_test(consumer)
    embedded = tmp / "embedded"
    run(CMAKE, "-S", ROOT / "tests/cmake_consumer", "-B", embedded,
        f"-DZCRUDP_SOURCE_DIR={ROOT}", "-DZCRUDP_WINDOW_SIZE=32",
        "-DZCRUDP_MAX_CHANNELS=2")
    build_and_test(embedded)
    # A core-only build with a different ABI must retain matching tests.
    core_only = tmp / "core-only"
    run(CMAKE, "-S", ROOT, "-B", core_only, "-DZCRUDP_BUILD_PROFILES=OFF",
        "-DZCRUDP_WINDOW_SIZE=2", "-DZCRUDP_MAX_CHANNELS=1")
    build_and_test(core_only)
    invalid = subprocess.run([CMAKE, "-S", str(ROOT), "-B", str(tmp / "invalid"),
                              "-DZCRUDP_WINDOW_SIZE=3"], capture_output=True, text=True)
    assert invalid.returncode != 0 and "power of two" in invalid.stderr
print("PASS: CMake Release tests, relocatable install, embedded consumer, custom ABI, core-only, invalid config")
