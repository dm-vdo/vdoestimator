
vdoestimator can be used to estimate how much space VDO can save
before creating a VDO volume. It can also be used to estimate the
effect of configuring the index with the memorySize and sparse
options.

Usage:

./vdoestimator --index Index_File DataLocation

There are several options one can use in vdoestimator:

  vdoestimator  --help
  vdoestimator [OPTION]... PATH

  Options:
    --compressionOnly Calculate compression only saving
    --dedupeOnly      Calculate deduplication
    --help            Print this help message and exit
    --index           Specify the location and name of the UDS index file (required)
    --memorySize      Specifies the amount of UDS server memory in gigabytes (the default size is 0.25 GB). The special decimal values 0.25, 0.5 and 0.75 can be used as can any positive integer up to 1024.
    --reuse           Reuse an existing index file
    --sparse          Use a sparse index
    --verbose         Verbose run

There are two required arguments:
    --index   Specify the location and name of the UDS index file
    PATH      A directory tree or block device to be scanned

Changing the memorySize or sparse index may indicate a better storage
savings rate.  Please refer to VDO documentation for the effect of
changing these parameters.

Example:

The vdoestimator output is fairly self explanatory. The important
numbers to look at are the dedupe percentage, percent saved
compression and total percent saved.  The below output example
represents a scan of approximately 500G of storage that contains test
data. If this data was stored on VDO, it would realize a savings of
approximately 72%.

Duration: 1h:31m:1s
Sparse Index: 0
Files Scanned: 2419506
Files Skipped: 0
Bytes Scanned: 510717808354
Entries Indexed: 35572176
Dedupe Request Posts Found: 83450727
Dedupe Request Posts Not Found: 42687679
Dedupe Percentage: 66.158%
Compressed Bytes: 31618975088
Percent Saved Compression: 6.191%
Total Bytes Used: 141808225265
Total Percent Saved: 72.234%
Peak Concurrent Requests: 2000

