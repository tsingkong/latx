#!/bin/sh
set -e
export CFLAGS="-Wno-error=unused-but-set-variable -Wno-error=unused-function  -Wformat -Werror=format-y2k"
make_configure=0
opt_level=1

help() {
    echo "Usage:"
    echo "  -c              configure"
    echo "  -O              [options]"
    echo "                  defaule: -O 1"
    echo "                  -O 0 : Disable all optimization, include basic"
    echo "                  -O 1 : Open stable optimization"
    echo "                  -O 2 : Open unstable optimization, include O1"
    echo "                  -O 3 : Open testing optimization, include O2"
    echo "  -h              help"
}

parseArgs() {
    while getopts "cO:h" opt; do
        case ${opt} in
        c)
            make_configure=1
            ;;
        O)
            opt_level="$OPTARG"
            ;;
        h)
            help
            exit
            ;;
        # 若选项需要参数但未收到，则走冒号分支
        :)
            help
            exit
            ;;
        # 若遇到未指定的选项，会走问号分支
        ?)
            help
            exit
            ;;
        esac
    done
}

make_cmd() {
    cd $(dirname $0)/../
    if [ $make_configure -eq 1 ] ; then
        rm -rf build32
    fi
    mkdir -p build32
    cd build32

    if [ $make_configure -eq 1 ] ; then
        if [ "$opt_level" = "0" ] ; then
            ../configure --target-list=i386-linux-user --enable-latx \
                --enable-guest-base-zero --disable-debug-info --optimize-O0 \
                --disable-docs
        elif [ "$opt_level" = "1" ] ; then
            ../configure --target-list=i386-linux-user --enable-latx \
                --enable-guest-base-zero --disable-debug-info --optimize-O1 \
                --extra-ldflags=-ldl --disable-docs
        elif [ "$opt_level" = "2" ] ; then
            ../configure --target-list=i386-linux-user --enable-latx \
                --enable-guest-base-zero --disable-debug-info --optimize-O2 \
                --extra-ldflags=-ldl --disable-docs
        elif [ "$opt_level" = "3" ] ; then
            ../configure --target-list=i386-linux-user --enable-latx \
                --enable-guest-base-zero --disable-debug-info --optimize-O3 \
                --disable-docs
        else
            echo "invalid options"
        fi
    fi

    if [ ! -f "/usr/bin/ninja" ]; then
        make -j $(nproc)
    else
        ninja
    fi
}

parseArgs "$@"
make_cmd
