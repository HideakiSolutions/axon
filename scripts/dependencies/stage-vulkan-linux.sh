#!/usr/bin/env bash
set -euo pipefail

# Ubuntu 22.04 lacks glslc and its system Vulkan headers are too old for
# ggml-vulkan. Pin LunarG's Jammy builds while retaining the 22.04 ABI baseline.
stage="${RUNNER_TEMP:?RUNNER_TEMP must be set}/axon-vulkan"
mkdir -p "${stage}"

fetch_verified() {
  local url="$1" sha256="$2" package="$3"
  curl --fail --location --retry 3 --silent --show-error \
    "${url}" --output "${package}"
  echo "${sha256}  ${package}" | sha256sum --check --status
  dpkg-deb --extract "${package}" "${stage}"
}

fetch_verified \
  'https://packages.lunarg.com/vulkan/pool/main/s/shaderc/shaderc_2025.2~rc1-1lunarg22.04-1_amd64.deb' \
  '53a024e2939e13e86caa49930450f38fdfbc528e129d8ecffc142d7e5ff27d5c' \
  "${stage}/shaderc.deb"
fetch_verified \
  'https://packages.lunarg.com/vulkan/pool/main/v/vulkan-headers/vulkan-headers_1.4.313.0~rc1-1lunarg22.04-1_all.deb' \
  '587b2d8e79416b394170ab61557c98765570cd153730f819a917866d78f45e1a' \
  "${stage}/vulkan-headers.deb"

# /usr/local/include precedes /usr/include for CMake and GCC on the Jammy
# runner. Keep libvulkan.so from Ubuntu's libvulkan-dev package.
sudo cp -a "${stage}/usr/include/vulkan" "${stage}/usr/include/vk_video" /usr/local/include/
"${stage}/usr/bin/glslc" --version
echo "${stage}/usr/bin" >> "${GITHUB_PATH:?GITHUB_PATH must be set}"
