#!/bin/bash

script_dir="$(dirname $(readlink -e $0))"

BATCH_ARGS=("--simulate" "-O0" "-fwhole-program" "--clock-period=15" "-D'printf(fmt, ...)='" "--channels-type=MEM_ACC_NN" "--experimental-setup=BAMBU")
OUT_SUFFIX="all_clang_CHStone-frontend"

python3 $script_dir/../../etc/scripts/test_panda.py --tool=bambu \
   --args="--configuration-name=CLANG4-O0-wp-NN  --compiler=I386_CLANG4  ${BATCH_ARGS[*]}" \
   --args="--configuration-name=CLANG5-O0-wp-NN  --compiler=I386_CLANG5  ${BATCH_ARGS[*]}" \
   --args="--configuration-name=CLANG6-O0-wp-NN  --compiler=I386_CLANG6  ${BATCH_ARGS[*]}" \
   --args="--configuration-name=CLANG7-O0-wp-NN  --compiler=I386_CLANG7  ${BATCH_ARGS[*]}" \
   --args="--configuration-name=CLANG8-O0-wp-NN  --compiler=I386_CLANG8  ${BATCH_ARGS[*]}" \
   --args="--configuration-name=CLANG9-O0-wp-NN  --compiler=I386_CLANG9  ${BATCH_ARGS[*]}" \
   --args="--configuration-name=CLANG10-O0-wp-NN --compiler=I386_CLANG10 ${BATCH_ARGS[*]}" \
   --args="--configuration-name=CLANG11-O0-wp-NN --compiler=I386_CLANG11 ${BATCH_ARGS[*]}" \
   --args="--configuration-name=CLANG12-O0-wp-NN --compiler=I386_CLANG12 ${BATCH_ARGS[*]}" \
   -lCHStone_list \
   -o "out_${OUT_SUFFIX}" -b$script_dir \
   --name="${OUT_SUFFIX}" "$@"
