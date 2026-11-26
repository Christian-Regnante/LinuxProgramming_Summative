"""Differential fuzz test: random files -> compare ./sensor_stats with reference.py.

Usage: python3 tests/fuzz.py [iterations]
Files are built from realistic tokens (valid numbers, boundary values, junk,
empty lines, CR/LF mixes) and some are larger than the 4096-byte read buffer so
records straddle read() chunk boundaries.
"""
import os, random, subprocess, sys, tempfile
sys.path.insert(0, os.path.dirname(__file__))
from reference import analyse

TOKENS = ["0", "1", "7", "42", "99", "100", "101", "-0", "-5", "007", "250",
          "", "abc", "12a", " 5", "5 ", "+3", "-", "--1", "3.5", "\r",
          "99999999999999999999999", "50\r5", "\t"]
ENDINGS = ["\n", "\r\n", "\n", "\n"]

def random_file(rng):
    n = rng.choice([0, 1, 5, 20, 200, 1500])
    parts = []
    for _ in range(n):
        tok = rng.choice(TOKENS) if rng.random() < 0.7 else str(rng.randint(-20, 130))
        parts.append(tok + rng.choice(ENDINGS))
    data = "".join(parts)
    if data and rng.random() < 0.3:          # sometimes drop the final line ending
        data = data.rstrip("\n").rstrip("\r")
    return data.encode()

def main():
    iterations = int(sys.argv[1]) if len(sys.argv) > 1 else 300
    rng = random.Random(2026)                 # fixed seed -> reproducible
    root = os.path.join(os.path.dirname(__file__), "..")
    big = 0
    with tempfile.TemporaryDirectory() as tmp:
        path = os.path.join(tmp, "input.txt")
        for i in range(iterations):
            data = random_file(rng)
            big += len(data) > 4096
            with open(path, "wb") as f:
                f.write(data)
            got = subprocess.run([os.path.join(root, "sensor_stats"), path],
                                 capture_output=True, text=True)
            want = analyse(data)
            if got.returncode != 0 or got.stdout != want:
                print(f"MISMATCH on iteration {i}; input saved to fuzz_failure.txt")
                with open("fuzz_failure.txt", "wb") as f:
                    f.write(data)
                print("expected:\n" + want + "got:\n" + got.stdout + got.stderr)
                sys.exit(1)
    print(f"Fuzz OK: {iterations} random files agree with the reference "
          f"({big} of them larger than the 4096-byte read buffer).")

if __name__ == "__main__":
    main()
