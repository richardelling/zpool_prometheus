# Prometheus metrics for ZFS pools
The _zpool_prometheus_ program produces 
[prometheus](https://github.com/prometheus)-compatible
metrics from zpools. In the UNIX tradition, _zpool_prometheus_
does one thing: read statistics from a pool and print them to
stdout. In many ways, this is a metrics-friendly output of 
statistics normally observed via the _zpool_ command.

## ZFS Versions
There are many implementations of ZFS on many OSes. The current
version is tested to work on:
* [ZFSonLinux](https://github.com/zfsonlinux/zfs) version 0.7 and later

This should compile and run on other ZFS versions, though many 
do not have the latency histograms. Pull requests are welcome.

## Metrics Categories
The following metric types are collected:

| type | description | zpool equivalent |
|---|---|---|
| zpool_stats | general size and data | zpool list |
| zpool_scan_stats | scrub, rebuild, and resilver statistics | zpool status |
| zpool_latency | latency histograms for the top-level pool vdev | zpool iostat -w |
| zpool_vdev | per-vdev stats, currently queues | zpool iostat -q | 

To be consistent with other prometheus collectors, each
metric has HELP and TYPE comments.

### Metric Names
Metric names are a mashup of:<br>
**\<type as above>\_\<ZFS internal name>\_\<units>**

For example, the pool's size metric is:<br>
**zpool_stats_size_bytes**

### Labels
The following labels are added to the metrics:

| label | description |
|---|---|
| name | pool name |
| state | pool state, as shown by _zpool status_ |

### Values
Currently, [prometheus](https://github.com/prometheus) values must be
type float. This is unfortunate because many ZFS metrics are 64-bit 
unsigned ints. When the actual metric values exceed the mantissa 
size of the floats, hilarity ensues.

## Building
Building is simplified by using cmake.
It is as simple as possible, but no simpler.
By default, [ZFSonLinux](https://github.com/zfsonlinux/zfs) 
installs the necessary header and library files in _/usr/local_.
If you place those files elsewhere, then edit _CMakeLists.txt_ and
change the _INSTALL_DIR_
```bash
cmake .
make
```
If successful, the _zpool_prometheus_ executable is created.

## Installing
Installation is left as an exercise for the reader because
there are many different methods that can be used.
Ultimately the method depends on how the local metrics collection is 
implemented and the local access policies.

There are two basic methods known to work:
1. Run a HTTP server that runs _zpool_prometheus_.
   A simple python+flask example server is included as _serve_zpool_prometheus.py_
2. Run a scheduled (eg cron) job that redirects the output
   to a file that is subsequently read by 
   [node_exporter](https://github.com/prometheus/node_exporter)

Helpful comments in the source code are available.

To install the _zpool_prometheus_ executable in _INSTALL_DIR_, use
```bash
make install
```

## Caveat Emptor
* Like the _zpool_ command, _zpool_prometheus_ takes a reader 
  lock on spa_config for each imported pool. If this lock blocks,
  then the command will also block indefinitely and might be
  unkillable. This is not a normal condition, but can occur if 
  there are bugs in the kernel modules. 
  For this reason, care should be taken:
  * avoid spawning many of these commands hoping that one might 
    finish
  * avoid frequent updates or short prometheus scrape time
    intervals, because the locks can interfere with the performance
    of other instances of _zpool_ or _zpool_prometheus_

* Metric values can overflow because the internal ZFS unsigned 64-bit
  int values do not transform to floats without loss of precision.

* Histogram sum values are always zero. This is because ZFS does
  not record that data currently. For most histogram uses this isn't
  a problem, but be aware of prometheus histogram queries that
  expect a non-zero histogram sum.

## Feedback Encouraged
Pull requests and issues are greatly appreciated. Visit
https://github.com/richardelling/zpool_prometheus
