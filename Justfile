init:
  git clone https://github.com/zmkfirmware/zmk.git
  docker run -u 1000:1000 --rm -it -v $(pwd):/workdir -w /workdir zmkfirmware/zmk-build-arm:stable west init -l config
  docker run -u 1000:1000 --rm -it -v $(pwd):/workdir -w /workdir zmkfirmware/zmk-build-arm:stable west update

build-part part:
  docker run -u 1000:1000 --rm -v $(pwd):/workdir -w /workdir zmkfirmware/zmk-build-arm:stable \
    sh -c 'west zephyr-export && west build -s zmk/app -d "/workdir/build/{{ part }}" -b "{{ part }}"  -- -DZMK_CONFIG=/workdir/config'

  cp build/{{ part }}/zephyr/zmk.uf2 build/{{ part }}-zmk.uf2

build:
  yq -o=json eval build.yaml | jq '.include[] | .board' -r | xargs -I{} just build-part {}
