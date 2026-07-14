init:
  git clone https://github.com/zmkfirmware/zmk.git
  docker run -u 1000:1000 --rm -it -v $(pwd):/workdir -w /workdir zmkfirmware/zmk-build-arm:stable west init -l config
  docker run -u 1000:1000 --rm -it -v $(pwd):/workdir -w /workdir zmkfirmware/zmk-build-arm:stable west update

# Build a single configuration
# Usage: just build-part <board> [shield] [cmake_args]
build-part board shield="" cmake_args="":
  #!/usr/bin/env bash
  set -e
  board="{{ board }}"
  shield="{{ shield }}"
  cmake_args="{{ cmake_args }}"

  # Board ids can contain '/' (Zephyr variants, e.g. xiao_ble//zmk). Keep the
  # real id for `-b`, but flatten '/' -> '_' for build dir / output filenames.
  board_slug="${board//\//_}"

  if [ -n "$shield" ]; then
    build_name="${shield}-${board_slug}"
    shield_arg="-DSHIELD=${shield}"
  else
    build_name="${board_slug}"
    shield_arg=""
  fi

  build_dir="/workdir/build/${build_name}"

  echo "=========================================="
  echo "Building: board=${board}, shield=${shield:-none}, cmake_args=${cmake_args:-none}"
  echo "=========================================="

  docker run -u 1000:1000 --rm -v "$(pwd)":/workdir -w /workdir zmkfirmware/zmk-build-arm:stable \
    sh -c "west zephyr-export && west build -s zmk/app -d '${build_dir}' -b '${board}' -- -DZMK_CONFIG=/workdir/config ${shield_arg} ${cmake_args}"

  cp "build/${build_name}/zephyr/zmk.uf2" "build/${build_name}-zmk.uf2"
  echo "Created: build/${build_name}-zmk.uf2"

# Build all configurations from build.yaml
build:
  #!/usr/bin/env bash
  set -e

  rm -f build/*.uf2

  count=$(yq '.include | length' build.yaml)
  for ((i=0; i<count; i++)); do
    board=$(yq -r ".include[$i].board" build.yaml)
    shield=$(yq -r ".include[$i].shield // \"\"" build.yaml)
    cmake_args=$(yq -r ".include[$i][\"cmake-args\"] // \"\"" build.yaml)
    just build-part "$board" "$shield" "$cmake_args"
  done

  echo "=========================================="
  echo "All builds completed!"
  ls -la build/*.uf2
  echo "=========================================="
