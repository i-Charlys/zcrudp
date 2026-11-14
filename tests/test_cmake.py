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
    assert any(prefix.rglob("zcrudpConfigVersion.cmake"))
    shutil.move(prefix, relocated)
    consumer = tmp / "consumer"
    run(CMAKE, "-S", ROOT / "tests/cmake_consumer", "-B", consumer,
        f"-DCMAKE_PREFIX_PATH={relocated}")
    build_and_test(consumer)
    shared = tmp / "shared"
    run(CMAKE, "-S", ROOT, "-B", shared, "-DBUILD_SHARED_LIBS=ON",
        "-DZCRUDP_WARNINGS_AS_ERRORS=ON")
    build_and_test(shared)
    nm = shutil.which("nm")
    session_sos = list(shared.glob("libzcrudp_session.so*"))
    if nm and session_sos:
        syms = subprocess.check_output([nm, "-D", "--defined-only", str(session_sos[0])],
                                       text=True)
        names = {line.split()[-1] for line in syms.splitlines() if line.strip()}
        assert "rudp_session_init" in names
        assert "rudp_rx_accept" not in names and "rudp_rx_drain" not in names
    embedded = tmp / "embedded"
    run(CMAKE, "-S", ROOT / "tests/cmake_consumer", "-B", embedded,
        f"-DZCRUDP_SOURCE_DIR={ROOT}", "-DZCRUDP_WINDOW_SIZE=32",
        "-DZCRUDP_MAX_CHANNELS=2")
    build_and_test(embedded)
    # Non-power-of-two channel count and core-only builds must compile & pass.
    odd_ch = tmp / "odd-channels"
    run(CMAKE, "-S", ROOT, "-B", odd_ch, "-DZCRUDP_MAX_CHANNELS=5",
        "-DZCRUDP_WARNINGS_AS_ERRORS=ON")
    build_and_test(odd_ch)
    # A core-only build with a different ABI must retain matching tests.
    core_only = tmp / "core-only"
    run(CMAKE, "-S", ROOT, "-B", core_only, "-DZCRUDP_BUILD_PROFILES=OFF",
        "-DZCRUDP_WINDOW_SIZE=2", "-DZCRUDP_MAX_CHANNELS=1")
    build_and_test(core_only)
    invalid = subprocess.run([CMAKE, "-S", str(ROOT), "-B", str(tmp / "invalid"),
                              "-DZCRUDP_WINDOW_SIZE=3"], capture_output=True, text=True)
    assert invalid.returncode != 0 and "power of two" in invalid.stderr
    too_large = subprocess.run([CMAKE, "-S", str(ROOT), "-B", str(tmp / "too-large"),
                                "-DZCRUDP_WINDOW_SIZE=32768"], capture_output=True, text=True)
    assert too_large.returncode != 0
print("PASS: CMake Release tests, relocatable install, shared visibility, embedded consumer, custom/odd ABI, core-only, invalid config")
