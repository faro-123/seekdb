# Bridge buffer fault-injection test

From the repository root, with a C compiler:

```bash
gcc -std=c11 -Wall -Wextra -Werror \
  -Iunittest/oblib/vector/hipvs_bridge_mock \
  unittest/oblib/vector/test_hipvs_bridge_buffers.c \
  -o /tmp/test_hipvs_bridge_buffers
/tmp/test_hipvs_bridge_buffers
```

The shim tests production bridge allocation ownership and error propagation on
CPU. It does not test the real cuVS ABI, GPU execution, recall, or performance.
Do not add this include directory to a product build.

Also compile and run the AMD include path:

```bash
gcc -std=c11 -Wall -Wextra -Werror -D__HIP_PLATFORM_AMD__ \
  -Iunittest/oblib/vector/hipvs_bridge_mock \
  unittest/oblib/vector/test_hipvs_bridge_buffers.c \
  -o /tmp/test_hipvs_bridge_buffers_amd
/tmp/test_hipvs_bridge_buffers_amd
```

The mock AMD runtime deliberately does not supply CUDA_R_32F. As in the frozen
hipVS headers, cuvs/library_types.h supplies that mapping. This catches a
missing mapping include while preserving the same FP32 value.
