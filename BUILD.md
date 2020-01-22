In the following instructions, $BUILD_DIR will refer to the directory
in which you build the software.

Get the vdo package:
~~~
cd $BUILD_DIR
git clone https://github.com/dm-vdo/vdo
~~~

Build the uds library:
~~~
make -C $BUILD_DIR/vdo/utils/uds/
~~~

Get and build the LZ4 library:
~~~
git clone https://github.com/lz4/lz4
make -C $BUILD_DIR/lz4/lib/
~~~

Get and build vdoestimator:
~~~
git clone https://github.com/dm-vdo/vdoestimator
make -C $BUILD_DIR/vdoestimator/
~~~
