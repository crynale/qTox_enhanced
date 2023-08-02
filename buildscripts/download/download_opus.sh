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

OPUS_VERSION=1.4
OPUS_HASH=c9b32b4253be5ae63d1ff16eea06b94b5f0f2951b7a02aceef58e3a3ce49c51f

source "$(dirname "$(realpath "$0")")/common.sh"

download_verify_extract_tarball \
    "https://github.com/xiph/opus/releases/download/v1.4/opus-${OPUS_VERSION}.tar.gz" \
    "${OPUS_HASH}"
