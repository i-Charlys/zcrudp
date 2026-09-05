"""Host-tool integration tests; Python standard library only, no core changes."""
import csv
import os
from pathlib import Path
import selectors
import socket
import subprocess
import tempfile
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]
DEMO = str(ROOT / os.environ.get("DEMO_BIN", "build/demo_loss"))
BENCH = str(ROOT / os.environ.get("BENCH_BIN", "build/bench_rudp"))


def ready(process):
    with selectors.DefaultSelector() as selector:
        selector.register(process.stdout, selectors.EVENT_READ)
        assert selector.select(5), "peer startup timed out"
        line = process.stdout.readline()
        assert line.startswith("READY"), line


def pair(loss, latency, jitter, count, duration, interactive=False):
    # Reserve two distinct ephemeral UDP ports, then release just before launch.
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as a, socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as b:
        a.bind(("127.0.0.1", 0))
        b.bind(("127.0.0.1", 0))
        port_a, port_b = a.getsockname()[1], b.getsockname()[1]
    common = ["--loss", str(loss), "--latency", str(latency), "--jitter", str(jitter),
              "--timeout", "150", "--interval", "30"]
    processes = []
    try:
        server = subprocess.Popen([DEMO, "server", "--port", str(port_a), "--peer-port", str(port_b),
                                   "--duration", str(duration + 300), *common],
                                  stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        processes.append(server)
        ready(server)
        client = subprocess.Popen([DEMO, "client", "--port", str(port_b), "--peer-port", str(port_a),
                                   "--duration", str(duration), "--count", str(count), *common],
                                  stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        processes.append(client)
        ready(client)
        commands = "r 1234\nu 2345\ninvalid\nstats\n" if interactive else "stats\n"
        client_out, client_err = client.communicate(commands, timeout=10)
        server_out, server_err = server.communicate(timeout=10)
        assert client.returncode == 0, (client_out, client_err)
        assert server.returncode == 0, (server_out, server_err)
        assert "queue_overflow=0" in client_out and "queue_overflow=0" in server_out
        reliable = [int(line.split("value=")[1]) for line in server_out.splitlines() if line.startswith("RX reliable ")]
        unreliable = [int(line.split("value=")[1]) for line in server_out.splitlines() if line.startswith("RX unreliable ")]
        if loss == 100:
            assert not reliable and not unreliable
            assert "DROP simulated" in client_out
        else:
            assert reliable == ([1234] if interactive else list(range(count))), (server_out, client_out)
            assert unreliable == sorted(set(unreliable)), server_out
            assert "pending=0" in client_out.split("STATS")[-1], client_out
            if loss == 0 and jitter == 0:
                assert unreliable == ([2345] if interactive else list(range(count))), server_out
            if loss:
                assert "DROP simulated" in server_out + client_out
                assert "retries=0 " not in client_out.split("STATS")[-1], client_out
    finally:
        for process in processes:
            if process.poll() is None:
                process.kill()
            process.communicate()


def main():
    for args in (["--help"],):
        subprocess.run([DEMO, *args], check=True, capture_output=True)
    for args in (["client", "--loss", "101"], ["client", "--seed", "0"], ["client", "--port"], ["client", "--peer", "bad"]):
        assert subprocess.run([DEMO, *args], capture_output=True).returncode == 2
    pair(0, 0, 0, 6, 800)
    pair(0, 60, 0, 0, 800, interactive=True)
    pair(30, 15, 20, 10, 3000)
    pair(100, 0, 0, 2, 400)
    with tempfile.TemporaryDirectory(prefix="zcrudp-bench-") as directory:
        report = Path(directory)
        subprocess.run([BENCH, "--iterations", "10000", "--csv", str(report / "out.csv"),
                        "--svg", str(report / "out.svg")], check=True, capture_output=True, timeout=10)
        with (report / "out.csv").open() as f:
            rows = list(csv.DictReader(f))
        assert len(rows) == 5
        for row in rows:
            low, median, high = [float(row[key]) for key in ("min_ns_per_op", "median_ns_per_op", "max_ns_per_op")]
            assert 0 < low <= median <= high
            assert abs(float(row["Mops_per_s"]) * median - 1000) < 2
        root = ET.parse(report / "out.svg").getroot()
        assert len(root.findall(".//{http://www.w3.org/2000/svg}rect")) == 11
        assert subprocess.run([BENCH, "--iterations", "0"], capture_output=True).returncode == 2
    print("PASS: CLI validation, real UDP peers, reliable recovery, unreliable ordering, total loss, CSV/SVG reports")


if __name__ == "__main__":
    main()
