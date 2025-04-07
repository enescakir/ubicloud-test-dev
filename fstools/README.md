# FSTools

A collection of filesystem tools for low-level operations on block devices.

## Building

```
$ sudo apt install build-essential libext2fs-dev
$ make
```

## blk2files

A utility to find files on ext2/ext3/ext4 filesystems based on their physical
location (LBA range) on a block device.

### Usage

```
blk2files --block <device> --path <starting_path> --start <start_lba> --end <end_lba> [options]
```

#### Required arguments:
- `-b, --block <device>`: Block device containing the filesystem to scan
- `-p, --path <path>`: Path within the filesystem to start scanning from
- `-s, --start <lba>`: Starting LBA to search for files
- `-e, --end <lba>`: Ending LBA to search for files

#### Optional arguments:
- `-l, --log`: Log all files encountered during the scan
- `-v, --verbose`: Enable verbose output
- `-h, --help`: Display help information

### Example (Locating truncated files)

Assume we have a disk image that has been truncated. To identify the truncated
range, use `gdisk -l`:

```
$ gdisk -l /path/to/image.raw
...

Disk /path/to/image.raw: 157284000 sectors, 75.0 GiB

...

Number  Start (sector)    End (sector)  Size       Code  Name
   1          227328       157286366   74.9 GiB    8300
  14            2048           10239   4.0 MiB     EF02
  15           10240          227327   106.0 MiB   EF00
```

From the above, note that the disk has 157284000 sectors, but partition 1 ends
at 157286366. So, to find corrupt files, we need to find files that start in
the range 157284000 ... 157286366.

To do this, we run the following inside the VM:

```bash
$ sudo ./blk2files -b /dev/vda1 -p / -s 157284000 -e 157286366
```

It will output a list of files whose first LBA falls within the given range:

```
ubi@vmchsnqq:~$ sudo ./blk2files -b /dev/vda1 -p / -s 157284000 -e 157286366
...
/usr/include/stdio.h (inode: 68975, first lba: 157284112)
/usr/include/signal.h (inode: 68970, first lba: 157284016)
/usr/include/stdint.h (inode: 68974, first lba: 157284088)
```

Notes:
* A file will have corrupt data if it appears in the above list AND was part of the
  original image. Some of these files may have been created after the VM booted,
  in which case they contain valid data.

* A file might have truncated data and not appear in the above list—for example, if
  its starting LBA is before the specified range but it ends within the range.
  Checking only the first LBA was simpler, faster, and sufficient for our purposes,
  so that's what was used.

To verify whether any of the above files are corrupt, use `head` to inspect files
that would normally contain valid textual data:

```bash
$ head -n2 /usr/include/stdint.h
X
 @bXP-uY
...
```
