# Vendored third-party code

Copied unmodified. Only the files this project needs are included.

| Folder | Source | Version | Archive SHA-256 |
|---|---|---|---|
| `cmsis_core/` | https://github.com/ARM-software/CMSIS_5 (`CMSIS/Core/Include`) | tag `5.9.0` | `9ce693dd15c41f02506adda79323c077912b3211303ca0d9b28b8ebe735a7031` |
| `cmsis_device_h7/` | https://github.com/STMicroelectronics/cmsis-device-h7 (`Include`) | tag `v1.10.7` | `0dc612b4fe4d953cec2cbca1a97e0ca23895c27869d4999ef26c223952ac1ae4` |
| `nanopb/` | https://github.com/nanopb/nanopb, release asset `nanopb-0.4.9.1.tar.gz` (runtime: `pb.h`, `pb_common`, `pb_encode`, `pb_decode`) | `0.4.9.1` | `882cd8473ad932b24787e676a808e4fb29c12e086d20bcbfbacc66c183094b5c` |

Licenses are next to the headers in each folder. The ST HAL is not vendored. nanopb (zlib licence) is linked into node1 only; `pb_decode` is used by the PC tests only. Its code generator is not vendored: it comes from PyPI (`nanopb==0.4.9.1`, sdist SHA-256 `96244de4e41b986ac5778196e66708195bad503e95dece5dc3a2e739527d7e26`) through the uv project in `dashboard/`.
