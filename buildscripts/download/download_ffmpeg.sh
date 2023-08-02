#!/bin/bash

#    Copyright © 2021 by The qTox Project Contributors
#
#    This program is libre software: you can redistribute it and/or modify
#    it under the terms of the GNU General Public License as published by
#    the Free Software Foundation, either version 3 of the License, or
#    (at your option) any later version.
#
#    This program is distributed in the hope that it will be useful,
#    but WITHOUT ANY WARRANTY; without even the implied warranty of
#    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
#    GNU General Public License for more details.
#
#    You should have received a copy of the GNU General Public License
#    along with this program.  If not, see <http://www.gnu.org/licenses/>.

set -euo pipefail

FFMPEG_VERSION=5.1.3
FFMPEG_HASH=1b113593ff907293be7aed95acdda5e785dd73616d7d4ec90a0f6adbc5a0312e

source "$(dirname "$(realpath "$0")")/common.sh"

download_verify_extract_tarball \
    "https://www.ffmpeg.org/releases/ffmpeg-${FFMPEG_VERSION}.tar.xz" \
    "${FFMPEG_HASH}"
