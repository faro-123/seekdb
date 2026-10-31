"""Build-command regression only; this does not compile HIP or execute a GPU."""

import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


class BuildTests(unittest.TestCase):
    def test_contest_target_does_not_require_gpu_discovery(self):
        bash = shutil.which("bash")
        if not bash:
            self.skipTest("bash is required to exercise the production build script")
        script = Path(__file__).resolve().parents[3] / "tools/hipvs_bridge/build.sh"
        with tempfile.TemporaryDirectory(prefix="hipvs build ") as directory:
            root = Path(directory)
            hipvs = root / "hipvs"
            header = hipvs / "include/cuvs/core/c_api.h"
            library = hipvs / "lib/libcuvs_c.so"
            for path in (header, library):
                path.parent.mkdir(parents=True, exist_ok=True)
                path.touch()
            compiler = root / "hipcc"
            compiler.write_bytes('''#!/usr/bin/env bash
set -eu
target=0
output=
for argument in "$@"; do
  [[ "$argument" != --offload-arch=gfx1100 ]] || target=1
done
if [[ "$target" != 1 ]]; then
  echo 'GPU discovery unavailable: explicit offload target required' >&2
  exit 86
fi
printf '%s\\n' "$@" > "$CAPTURE_FILE"
while (( $# )); do
  if [[ "$1" == -o ]]; then output=$2; break; fi
  shift
done
touch "$output"
'''.encode("utf-8"))
            compiler.chmod(0o755)
            capture = root / "arguments.txt"
            output = root / "output directory/libseekdb_cuvs_bridge.so"
            env = dict(os.environ, HIPCC="./hipcc", HIPVS_ROOT="./hipvs",
                       ROCM_ROOT="./rocm", CAPTURE_FILE="arguments.txt")
            result = subprocess.run([bash, script.as_posix(), "output directory/libseekdb_cuvs_bridge.so"],
                                    cwd=root, env=env, text=True, encoding="utf-8", capture_output=True)
            self.assertEqual(0, result.returncode, result.stderr)
            self.assertTrue(output.is_file())
            arguments = capture.read_text().splitlines()
            self.assertEqual(1, arguments.count("--offload-arch=gfx1100"))
            self.assertTrue(any(arg.endswith("/seekdb_cuvs_bridge.c") for arg in arguments))
            self.assertTrue(any(arg.endswith("/seekdb_cuvs_graph.hip") for arg in arguments))


if __name__ == "__main__":
    unittest.main()
