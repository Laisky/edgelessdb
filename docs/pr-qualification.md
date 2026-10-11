# Native dependency qualification and remaining ERT acceptance

This draft preserves all three original dependency PR histories. It updates the Go graph to EGo 1.10.0, MarbleRun 1.9.2, gRPC 1.84.0, maintained crypto/net/protobuf and klog 2.140.0. Go 1.26.9 is the preferred toolchain (minimum 1.26.1). The legacy MarbleRun version had three called advisories fixed by the maintained version; the mismatched old klog/new logr API is also repaired.

Module verification, all-package vet/build, the full native race suite, integration-tagged test compilation and all-package source vulnerability scanning passed. The scan found 0 called / 0 imported vulnerabilities, with 1 advisory only in unused required-module packages. Native tests retain the repository's existing runtime/database mocks; they do not establish a running C++ enclave/MariaDB system.

The existing nightly dispatch is now restricted to the upstream edgelesssys/edgelessdb repository, retaining its push-to-main condition. A merge in this fork cannot dispatch a deployment to another repository.

Remaining acceptance requires the full CMake/MariaDB/RocksDB/ERT build and simulation/runtime suite. The existing build script invokes ertgo and the two C++ submodules are not initialized in the task clone. The official Edgeless RT 0.5.4 Ubuntu 22.04 package was extracted only into the task directory; ertgo version measured Go 1.27.1, missing the requested 1.27.2 security patch. Its SHA-256 was 1edfa13138ed75868b051056ed2e6faa1b17cb35cedca3315c474d44c8f52b35. A compatible patched runtime/compiler must qualify before merging this draft. No host SDK install, enclave enrollment, service credential, signing key, image publication or deployment was used.

Official runtime release: https://github.com/edgelesssys/edgelessrt/releases/tag/v0.5.4
