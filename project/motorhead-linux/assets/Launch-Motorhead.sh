#!/usr/bin/env bash
set -euo pipefail
game_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
cd -- "$game_dir"
# A Windows-to-Linux file copy may lose the executable bit. Launching this
# script with /bin/bash repairs just the game binary, without sudo.
if [[ ! -x "$game_dir/Motorhead" ]]; then
    chmod u+x -- "$game_dir/Motorhead"
fi
export SDL_VIDEODRIVER="${SDL_VIDEODRIVER:-x11}"
exec "$game_dir/Motorhead" "$@"
