docker run -u 1000:1000 --rm -it -v $(pwd):/workdir -w /workdir zmkfirmware/zmk-build-arm:stable

west init -l config
west update

west zephyr-export
west build -s zmk/app -d "${PWD}/dao_right" -b "dao_right"  -- -DZMK_CONFIG=${PWD}/config
west build -s zmk/app -d "${PWD}/dao_left" -b "dao_left"  -- -DZMK_CONFIG=${PWD}/config

docker run -u 1000:1000 --rm -it -v $(pwd):/workdir -w /workdir zmkfirmware/zmk-build-arm:stable sh -c 'west build -s zmk/app -d "/workdir/dao_right" -b "dao_right"  -- -DZMK_CONFIG=/workdir/config'

dao_right/zephyr/zmk.uf2
