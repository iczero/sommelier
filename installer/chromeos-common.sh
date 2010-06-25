# Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
#
# This contains common constants and functions for installer scripts. This must
# evaluate properly for both /bin/bash and /bin/sh, since it's used both to
# create the initial image at compile time and to install or upgrade a running
# image.

# The GPT tables describe things in terms of 512-byte sectors, but some
# filesystems prefer 4096-byte blocks. These functions help with alignment
# issues.

# This returns the size of a file or device in 512-byte sectors, rounded up if
# needed.
# Invoke as: subshell
# Args: FILENAME
# Return: whole number of sectors needed to fully contain FILENAME
numsectors() {
  if [ -b "${1}" ]; then
    dev=${1##*/}
    if [ -e /sys/block/$dev/size ]; then
      cat /sys/block/$dev/size
    else
      part=${1##*/}
      block=$(get_block_dev_from_partition_dev "${1}")
      block=${block##*/}
      cat /sys/block/$block/$part/size
    fi
  else
    local bytes=$(stat -c%s "$1")
    local sectors=$(( $bytes / 512 ))
    local rem=$(( $bytes % 512 ))
    if [ $rem -ne 0 ]; then
      sectors=$(( $sectors + 1 ))
    fi
    echo $sectors
  fi
}

# Round a number of 512-byte sectors up to an integral number of 2Mb
# blocks. Divisor is 2 * 1024 * 1024 / 512 == 4096.
# Invoke as: subshell
# Args: SECTORS
# Return: Next largest multiple-of-8 sectors (ex: 4->8, 33->40, 32->32)
roundup() {
  local num=$1
  local div=${2:-4096}
  local rem=$(( $num % $div ))

  if [ $rem -ne 0 ]; then
    num=$(($num + $div - $rem))
  fi
  echo $num
}

# Truncate a number of 512-byte sectors down to an integral number of 2Mb
# blocks. Divisor is 2 * 1024 * 1024 / 512 == 4096.
# Invoke as: subshell
# Args: SECTORS
# Return: Next smallest multiple-of-8 sectors (ex: 4->0, 33->32, 32->32)
rounddown() {
  local num=$1
  local div=${2:-4096}
  local rem=$(( $num % $div ))

  if [ $rem -ne 0 ]; then
    num=$(($num - $rem))
  fi
  echo $num
}

# Locate the cgpt tool. It should already be installed in the build chroot,
# but some of these functions may be invoked outside the chroot (by
# image_to_usb or similar), so we need to find it.
GPT=""

locate_gpt() {
  if [ -z "$GPT" ]; then
    GPT=$(which cgpt 2>/dev/null) || /bin/true
    if [ -z "$GPT" ]; then
      if [ -x "${DEFAULT_CHROOT_DIR:-}/usr/bin/cgpt" ]; then
        GPT="${DEFAULT_CHROOT_DIR:-}/usr/bin/cgpt"
      else
        echo "can't find cgpt tool" 1>&2
        exit 1
      fi
    fi
  fi
}

# This installs a GPT into the specified device or file, using the given
# components. If the target is a block device or the FORCE_FULL arg is "true"
# we'll do a full install. Otherwise, it'll be just enough to boot.
# Invoke as: command (not subshell)
# Args: TARGET ROOTFS_IMG KERNEL_IMG STATEFUL_IMG PMBRCODE ESP_IMG FORCE_FULL
# Return: nothing
# Side effects: Sets these global variables describing the GPT partitions
#   (all units are 512-byte sectors):
#   NUM_ESP_SECTORS
#   NUM_KERN_SECTORS
#   NUM_OEM_SECTORS
#   NUM_RESERVED_SECTORS
#   NUM_ROOTFS_SECTORS
#   NUM_STATEFUL_SECTORS
#   START_ESP
#   START_KERN_A
#   START_KERN_B
#   START_OEM
#   START_RESERVED
#   START_ROOTFS_A
#   START_ROOTFS_B
#   START_STATEFUL
install_gpt() {
  local outdev=$1
  local rootfs_img=$2
  local kernel_img=$3
  local stateful_img=$4
  local pmbrcode=$5
  local esp_img=$6
  local force_full="${7:-}"
  local rootfs_size="${8:-1024}"   # 1G

  # The gpt tool requires a fixed-size target to work on, so we may have to
  # create a file of the appropriate size. Let's figure out what that size is
  # now. The full partition layout will look something like this (indented
  # lines indicate reserved regions that do not have any useful content at the
  # moment).
  #
  #   PMBR (512 bytes)
  #   Primary GPT Header (512 bytes)
  #   Primary GPT Table (16K)
  #     Kernel C (placeholder for future use only)    partition 6
  #     Rootfs C (placeholder for future use only)    partition 7
  #     future use                                    partition 9
  #     future use                                    partition 10
  #     future use                                    partition 11
  #   Kernel A                                        partition 2
  #   Kernel B                                        partition 4
  #   OEM Customization (16M)                         partition 8
  #     reserved space (64M)
  #   EFI System Partition (temporary)                partition 12
  #   Stateful partition (as large as possible)       partition 1
  #   Rootfs B                                        partition 5
  #   Rootfs A                                        partition 3
  #   Secondary GPT Table (16K)
  #   Secondary GPT Header (512 bytes)
  #
  # Please refer to the official ChromeOS documentation for the details and
  # explanation behind the layout and partition numbering scheme. The short
  # version is that 1) we want to avoid ever changing the purpose or number of
  # an existing partition, 2) we want to be able to add new partitions later
  # without breaking current scripts, and 3) we may someday need to increase
  # the size of the rootfs during an upgrade, which means shrinking the size of
  # the stateful partition on a live system.
  #
  # The EFI GPT spec requires that all valid partitions be at least one sector
  # in size, and non-overlapping.

  # Here are the size limits that we're currently requiring
  local max_kern_sectors=32768        # 16M
  local max_rootfs_sectors=$((${rootfs_size} * 2 * 1024))  # 1G by default
  local max_oem_sectors=32768         # 16M
  local max_reserved_sectors=131072   # 64M
  local max_esp_sectors=32768         # 16M
  local min_stateful_sectors=262144   # 128M, expands to fill available space

  local num_pmbr_sectors=1
  local num_gpt_hdr_sectors=1
  local num_gpt_table_sectors=32      # 16K
  local num_footer_sectors=$(($num_gpt_hdr_sectors + $num_gpt_table_sectors))
  local num_header_sectors=$(($num_pmbr_sectors + $num_footer_sectors))

  # In order to align to a 4096-byte boundary, there should be several empty
  # sectors available following the header. We'll pack the single-sector-sized
  # unused partitions in there.
  local start_kern_c=$(($num_header_sectors))
  local num_kern_c_sectors=1
  local kern_c_priority=0
  local start_rootfs_c=$(($start_kern_c + 1))
  local num_rootfs_c_sectors=1
  local start_future_9=$(($start_rootfs_c + 1))
  local num_future_sectors=1
  local start_future_10=$(($start_future_9 + 1))
  local start_future_11=$(($start_future_10 + 1))

  local start_useful=$(roundup $(($start_future_11 + 1)))

  locate_gpt

  # What are we doing?
  if [ -b "$outdev" -o "$force_full" = "true" ]; then
    # Block device, need to be root.
    if [ -b "$outdev" ]; then
      local sudo=sudo
    else
      local sudo=""
    fi

    # Full install, use max sizes and create both A & B images.
    NUM_KERN_SECTORS=$max_kern_sectors
    NUM_ROOTFS_SECTORS=$max_rootfs_sectors
    NUM_OEM_SECTORS=$max_oem_sectors
    NUM_ESP_SECTORS=$max_esp_sectors
    NUM_RESERVED_SECTORS=$max_reserved_sectors

    # Where do things go?
    START_KERN_A=$start_useful
    local num_kern_a_sectors=$NUM_KERN_SECTORS
    local kern_a_priority=15
    START_KERN_B=$(($START_KERN_A + $NUM_KERN_SECTORS))
    local num_kern_b_sectors=$NUM_KERN_SECTORS
    local kern_b_priority=15
    START_OEM=$(($START_KERN_B + $NUM_KERN_SECTORS))
    START_RESERVED=$(($START_OEM + $NUM_OEM_SECTORS))
    START_ESP=$(($START_RESERVED + $NUM_RESERVED_SECTORS))
    START_STATEFUL=$(($START_ESP + $NUM_ESP_SECTORS))

    local total_sectors=$(numsectors $outdev)
    local start_gpt_footer=$(($total_sectors - $num_footer_sectors))
    local end_useful=$(rounddown $start_gpt_footer)

    START_ROOTFS_A=$(($end_useful - $NUM_ROOTFS_SECTORS))
    local num_rootfs_a_sectors=$NUM_ROOTFS_SECTORS
    START_ROOTFS_B=$(($START_ROOTFS_A - $NUM_ROOTFS_SECTORS))
    local num_rootfs_b_sectors=$NUM_ROOTFS_SECTORS

    NUM_STATEFUL_SECTORS=$(($START_ROOTFS_B - $START_STATEFUL))
  else
    # Just a local file.
    local sudo=

    # We're just going to fill partitions 1, 2, 3, 8, and 12. The others will
    # be present but as small as possible. The disk layout isn't crucial here,
    # because we won't be able to upgrade this image in-place as it's only for
    # installation purposes.
    NUM_STATEFUL_SECTORS=$(roundup $(numsectors $stateful_img))
    NUM_KERN_SECTORS=$(roundup $(numsectors $kernel_img))
    local num_kern_a_sectors=$NUM_KERN_SECTORS
    local kern_a_priority=15
    local num_kern_b_sectors=1
    local kern_b_priority=0
    NUM_ROOTFS_SECTORS=$(roundup $(numsectors $rootfs_img))
    local num_rootfs_a_sectors=$NUM_ROOTFS_SECTORS
    local num_rootfs_b_sectors=1
    NUM_OEM_SECTORS=$max_oem_sectors
    NUM_ESP_SECTORS=$(roundup $(numsectors $esp_img))
    NUM_RESERVED_SECTORS=1

    START_KERN_A=$start_useful
    START_ROOTFS_A=$(($START_KERN_A + $NUM_KERN_SECTORS))
    START_STATEFUL=$(($START_ROOTFS_A + $NUM_ROOTFS_SECTORS))
    START_OEM=$(($START_STATEFUL + $NUM_STATEFUL_SECTORS))
    START_ESP=$(($START_OEM + $NUM_OEM_SECTORS))
    START_KERN_B=$(($START_ESP + $NUM_ESP_SECTORS))
    START_ROOTFS_B=$(($START_KERN_B + $num_kern_b_sectors))
    START_RESERVED=$(($START_ROOTFS_B + $num_rootfs_b_sectors))

    # For minimal install, we're not worried about the secondary GPT header
    # being at the end of the device because we're almost always writing to a
    # file. If that's not true, the secondary will just be invalid.
    local start_gpt_footer=$(($START_RESERVED + $NUM_RESERVED_SECTORS))
    local end_useful=$start_gpt_footer

    local total_sectors=$(($start_gpt_footer + $num_footer_sectors))

    # Create the image file if it doesn't exist.
    if [ ! -e ${outdev} ]; then
      $sudo dd if=/dev/zero of=${outdev} bs=512 count=1 \
        seek=$(($total_sectors - 1))
    fi
  fi

  echo "Creating partition tables..."

  # Zap any old partitions (otherwise gpt complains).
  $sudo dd if=/dev/zero of=${outdev} conv=notrunc bs=512 \
    count=$num_header_sectors
  $sudo dd if=/dev/zero of=${outdev} conv=notrunc bs=512 \
    seek=${start_gpt_footer} count=$num_footer_sectors

  # Create the new GPT partitions. The order determines the partition number.
  # Note that the partition label is in the GPT only. The filesystem label is
  # what's used to populate /dev/disk/by-label/, and this is not that.

  $sudo $GPT create ${outdev}

  $sudo $GPT add -b ${START_STATEFUL} -s ${NUM_STATEFUL_SECTORS} \
    -t data -l "STATE" ${outdev}

  $sudo $GPT add -b ${START_KERN_A} -s ${num_kern_a_sectors} \
    -t kernel -l "KERN-A" -S 0 -T 15 -P ${kern_a_priority} ${outdev}

  $sudo $GPT add -b ${START_ROOTFS_A} -s ${num_rootfs_a_sectors} \
    -t rootfs -l "ROOT-A" ${outdev}

  $sudo $GPT add -b ${START_KERN_B} -s ${num_kern_b_sectors} \
    -t kernel -l "KERN-B" -S 0 -T 15 -P ${kern_b_priority} ${outdev}

  $sudo $GPT add -b ${START_ROOTFS_B} -s ${num_rootfs_b_sectors} \
    -t rootfs -l "ROOT-B" ${outdev}

  $sudo $GPT add -b ${start_kern_c} -s ${num_kern_c_sectors} \
    -t kernel -l "KERN-C" -S 0 -T 15 -P ${kern_c_priority} ${outdev}

  $sudo $GPT add -b ${start_rootfs_c} -s ${num_rootfs_c_sectors} \
    -t rootfs -l "ROOT-C" ${outdev}

  $sudo $GPT add -b ${START_OEM} -s ${NUM_OEM_SECTORS} \
    -t data -l "OEM" ${outdev}

  $sudo $GPT add -b ${start_future_9} -s ${num_future_sectors} \
    -t reserved -l "reserved" ${outdev}

  $sudo $GPT add -b ${start_future_10} -s ${num_future_sectors} \
    -t reserved -l "reserved" ${outdev}

  $sudo $GPT add -b ${start_future_11} -s ${num_future_sectors} \
    -t reserved -l "reserved" ${outdev}

  $sudo $GPT add -b ${START_ESP} -s ${NUM_ESP_SECTORS} \
    -t efi -l "EFI-SYSTEM" ${outdev}

  # Create the PMBR and instruct it to boot ROOT-A
  $sudo $GPT boot -p -b ${pmbrcode} -i 3 ${outdev}

  # Display what we've got
  $sudo $GPT show ${outdev}

  sync
}

# Read GPT table to find the starting location of a specific partition.
# Invoke as: subshell
# Args: DEVICE PARTNUM
# Returns: offset (in sectors) of partition PARTNUM
partoffset() {
  sudo $GPT show -b -i $2 $1
}

# Read GPT table to find the size of a specific partition.
# Invoke as: subshell
# Args: DEVICE PARTNUM
# Returns: size (in sectors) of partition PARTNUM
partsize() {
  sudo $GPT show -s -i $2 $1
}

# Extract the whole disk block device from the partition device.
# This works for /dev/sda3 (-> /dev/sda) as well as /dev/mmcblk0p2
# (-> /dev/mmcblk0).
get_block_dev_from_partition_dev() {
  local partition=$1
  if ! (expr match "$partition" ".*[0-9]$" >/dev/null) ; then
    echo "Invalid partition name: $partition" >&2
    exit 1
  fi
  # Remove the last digit.
  local block=$(echo "$partition" | sed -e 's/\(.*\)[0-9]$/\1/')
  # If needed, strip the trailing 'p'.
  if (expr match "$block" ".*[0-9]p$" >/dev/null); then
    echo "${block%p}"
  else
    echo "$block"
  fi
}

# Extract the partition number from the partition device.
# This works for /dev/sda3 (-> 3) as well as /dev/mmcblk0p2 (-> 2).
get_partition_number() {
  local partition=$1
  if ! (expr match "$partition" ".*[0-9]$" >/dev/null) ; then
    echo "Invalid partition name: $partition" >&2
    exit 1
  fi
  # Extract the last digit.
  echo "$partition" | sed -e 's/^.*\([0-9]\)$/\1/'
}

# Construct a partition device name from a whole disk block device and a
# partition number.
# This works for [/dev/sda, 3] (-> /dev/sda3) as well as [/dev/mmcblk0, 2]
# (-> /dev/mmcblk0p2).
make_partition_dev() {
  local block=$1
  local num=$2
  # If the disk block device ends with a number, we add a 'p' before the
  # partition number.
  if (expr match "$block" ".*[0-9]$" >/dev/null) ; then
    echo "${block}p${num}"
  else
    echo "${block}${num}"
  fi
}

# Construct a PMBR for the arm platform, Uboot will load PMBR and "autoscr" it.
# Arguments List:
# $1 : Kernel Partition Offset.
# $2 : Kernel Partition Size ( in Sectors ).
# $3 : DEVICE
# Return file path of the generated PMBR.

make_arm_mbr() {
  # Create the U-Boot script to copy the kernel into memory and boot it.
  local KERNEL_OFFSET=$(printf "0x%08x" ${1})
  local KERNEL_SECS_HEX=$(printf "0x%08x" ${2})
  local DEVICE=${3}
  local EXTRA_BOOTARGS=${4}

  BOOTARGS="root=/dev/mmcblk${DEVICE}p3"
  BOOTARGS="${BOOTARGS} init=/sbin/init"
  BOOTARGS="${BOOTARGS} console=ttySAC2,115200"
  BOOTARGS="${BOOTARGS} mem=1024M"
  BOOTARGS="${BOOTARGS} rootwait"
  BOOTARGS="${BOOTARGS} ${EXTRA_BOOTARGS}"

  MBR_SCRIPT="/var/tmp/mbr_script"
  echo -e "echo\necho ---- ChromeOS Boot ----\necho\n" \
          "setenv bootargs ${BOOTARGS}\n" \
          "mmc read ${DEVICE} C0008000 $KERNEL_OFFSET $KERNEL_SECS_HEX\n" \
          "bootm C0008000" > ${MBR_SCRIPT}
  MKIMAGE="/usr/bin/mkimage"
  if [ -x "$MKIMAGE" ]; then
    MBR_SCRIPT_UIMG="${MBR_SCRIPT}.uimg"
    "$MKIMAGE" -A "arm" -O linux  -T script -a 0 -e 0 -n "COS boot" \
               -d ${MBR_SCRIPT} ${MBR_SCRIPT_UIMG} >&2
  else
    echo "Error: u-boot mkimage not found or not executable." >&2
    exit 1
  fi
  echo ${MBR_SCRIPT_UIMG}
}


# The scripts that source this file typically want to use the root password as
# confirmation, unless the --run_as_root flag is given.
dont_run_as_root() {
  if [ $(id -u) -eq "0" -a "${FLAGS_run_as_root}" -eq "${FLAGS_FALSE}" ]
  then
    echo "Note: You must be the 'chronos' user to run this script. Unless"
    echo "you pass --run_as_root and run as root."
    exit 1
  fi
}

list_usb_disks() {
  local sd
  for sd in /sys/block/sd*; do
    if readlink -f ${sd}/device | grep -q usb &&
      [ "$(cat ${sd}/removable)" = 1 ]; then
      echo ${sd##*/}
    fi
  done
}

get_disk_info() {
  # look for a "given" file somewhere in the path upwards from the device
  local dev_path=/sys/block/${1}/device
  while [ -d "${dev_path}" -a "${dev_path}" != "/sys" ]; do
    if [ -f "${dev_path}/${2}" ]; then
      cat "${dev_path}/${2}"
      return
    fi
    dev_path=$(readlink -f ${dev_path}/..)
  done
  echo '[Unknown]'
}
