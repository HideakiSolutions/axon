#!/usr/bin/env bash
set -euo pipefail

# Ubuntu 22.04 has Vulkan development headers but no glslc package in its
# default repositories. Stage LunarG's Jammy build without changing the ABI
# baseline of the Linux release runner.
version='2025.2~rc1-1lunarg22.04-1'
sha256='53a024e2939e13e86caa49930450f38fdfbc528e129d8ecffc142d7e5ff27d5c'
stage="${RUNNER_TEMP:?RUNNER_TEMP must be set}/axon-glslc"
package="${stage}/shaderc.deb"
mkdir -p "${stage}"
curl --fail --location --retry 3 --silent --show-error \
  "https://packages.lunarg.com/vulkan/pool/main/s/shaderc/shaderc_${version}_amd64.deb" \
  --output "${package}"
echo "${sha256}  ${package}" | sha256sum --check --status
dpkg-deb --extract "${package}" "${stage}"
"${stage}/usr/bin/glslc" --version
echo "${stage}/usr/bin" >> "${GITHUB_PATH:?GITHUB_PATH must be set}"
