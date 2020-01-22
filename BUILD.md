In the following instructions, $BUILD_DIR will refer to the directory
in which you build the software.

~~~
export BUILD_DIR=/path/to/build_directory_root
~~~

Clone all of the repositories that are needed
~~~
git clone https://github.com/lz4/lz4 $BUILD_DIR/lz4
git clone https://github.com/dm-vdo/vdoestimator $BUILD_DIR/vdoestimator

# vdoestimator currently requires these specific versions of the kvdo and vdo
# repositories.
git clone --branch 6.2.2.24 https://github.com/dm-vdo/kvdo $BUILD_DIR/kvdo
git clone --branch 6.2.2.33 https://github.com/dm-vdo/vdo $BUILD_DIR/vdo
~~~

Build the software
~~~
make -C $BUILD_DIR/vdo/utils/uds/
make -C $BUILD_DIR/lz4/lib/
make -C $BUILD_DIR/vdoestimator/
~~~
