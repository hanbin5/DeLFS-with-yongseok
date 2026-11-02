# D 파트 환경 구성 안내: Linux 7.2.8 + 원본 F2FS 테스트 VM

## 목차
0. [개요](#0-개요)
1. [전체 구성](#1-전체-구성)
2. [따라 하기](#2-따라-하기)
3. [(선택) DeLFS 6.6.8 실행 테스트](#3-선택-delfs-668-실행-테스트)
4. [겪은 문제와 해결 방법](#4-겪은-문제와-해결-방법)
5. [Mac(UTM, arm64) 팀원 참고 (미검증)](#5-macutm-arm64-팀원-참고-미검증)
6. [현재 결과](#6-현재-결과)
7. [원본 DeLFS에 원래 있던 문제 후보](#7-원본-delfs에-원래-있던-문제-후보)
8. [[부록] 상세 설명](#8-부록-상세-설명)
    - [8.1 구성 이유](#81-구성-이유)
    - [8.2 준비물과 디렉터리 배치](#82-준비물과-디렉터리-배치)
    - [8.3 VM 준비](#83-vm-준비)
    - [8.4 커널 빌드](#84-커널-빌드)
    - [8.5 nvme-loop](#85-nvme-loop)
    - [8.6 F2FS 도구와 기본 동작 확인](#86-f2fs-도구와-기본-동작-확인)
    - [8.7 축소 벤치마크](#87-축소-벤치마크)

---

## 0. 개요

이 문서는 DeLFS 7.2.8 포팅 팀원이 **순정 Linux 7.2.8 커널과 원본 F2FS 모듈이 동작하는 테스트 VM**을 각자 PC에 만들 수 있도록 정리한 절차다.
x86 VirtualBox 환경에서 실제로 거친 절차와, 그 과정에서 생긴 문제와 해결 방법을 함께 적었다.

- **따라 하는 범위:** **2장(2.1~2.9)만 순서대로 그대로 따라 하면 된다.** **[부록]**(8장)은 2장의 각 단계에서 링크로 가리키는 상세 설명이다. DeLFS 6.6.8 기준선([3장](#3-선택-delfs-668-실행-테스트))은 선택이다.
- **검증한 범위:** x86_64 Windows 호스트(Intel Core i5-12400F)의 VirtualBox
  - 개발 VM: 6 vCPU, RAM 8GB, Ubuntu 20.04.6 LTS
  - 테스트 VM: 4 vCPU, RAM 8GB, Ubuntu 20.04.6 LTS
- **검증하지 않은 범위:** Mac(UTM, arm64). [5장](#5-macutm-arm64-팀원-참고-미검증)에 차이점만 정리했다.
- 이 문서와 실행 스크립트 4개([`scripts/`](scripts/))는 [팀 DeLFS 포크](https://github.com/hanbin5/DeLFS-with-yongseok)의 최상위 `env-guide/`에 함께 들어 있다. 저장소를 clone하면 함께 받으며, 개발 VM의 기본 위치는 `~/DeLFS/env-guide/`다. 개발 VM에서 스크립트 4개를 테스트 VM의 홈(`~/`)으로 복사해 실행한다.

내용 옆의 표시는 확인 수준을 뜻한다. **[실측]**은 실제로 실행해서 확인한 것, **[소스]**는 코드나 공식 문서를 읽고 확인한 것, **[미검증]**은 추정이다. 표시가 없는 절차는 x86 환경에서 실제로 실행한 것이다.

---

## 1. 전체 구성

| 구성 요소 | 내용 |
| --- | --- |
| **개발 VM** | 커널, 모듈, 도구를 빌드한다. 원래 쓰던 VM을 그대로 쓴다. |
| **테스트 VM** | 개발 VM을 완전 복제해서 만든다. 빌드한 커널을 설치해 부팅하고 파일시스템을 시험한다. 커널 패닉이 나도 개발 환경은 안전하다. |
| 연결 | VirtualBox 호스트 전용 네트워크(DHCP)로 연결한다. 결과물은 `scp`로 보낸다. |
| 테스트 장치 | 테스트 VM 안의 **nvme-loop** 장치(20GB 파일 백엔드, 4KB LBA, discard 지원) |
| 커널 | 순정 Linux 7.2.8, `LOCALVERSION=-vanilla`, 커널 이름 `7.2.8-vanilla` |
| F2FS 도구 | 공식 f2fs-tools **1.16.0**. 저장소의 `delfs-mkfs`도 이 버전을 바탕으로 만들어졌다. |

왜 이렇게 구성했는지(vCPU 수, nvme-loop, QEMU 대신 VirtualBox)는 [8.1](#81-구성-이유)에 있다.

---

## 2. 따라 하기

각 단계 제목의 괄호는 명령을 실행할 곳이다. **호스트**는 Windows PowerShell(`C:\Program Files\Oracle\VirtualBox`에서 실행), **개발 VM**과 **테스트 VM**은 각 게스트의 셸이다. 단계 끝의 "설명"은 이유와 문제가 생겼을 때 볼 곳이다.

시작 조건:
- 개발 VM: 지금 쓰는 x86_64 Ubuntu VirtualBox VM. 디스크 여유 10GB 이상.
- 팀 포크를 개발 VM의 `~/DeLFS/`에 clone한다(2.4). 이 문서는 `~/DeLFS/env-guide/README.md`, 실행 스크립트는 `~/DeLFS/env-guide/scripts/`에 놓인다. `env-guide/`를 따로 내려받을 필요는 없다.

아래 clone 명령은 안내서가 들어 있는 `docs/env-guide-team-layout` 브랜치를 지정한다. `main`에 병합된 뒤에는 `--branch docs/env-guide-team-layout`을 생략해도 된다. 기존 clone을 쓰는 경우에는 디렉터리가 있다는 이유만으로 최신 팀 포크라고 가정하지 말고, [8.2.1](#821-delfs-저장소에-들어-있는-것)의 원격 주소와 브랜치 확인 절차를 먼저 따른다.

### 2.1 테스트 VM 만들기 (호스트)
1. 개발 VM을 끈다(`sudo shutdown now`).
2. VirtualBox에서 개발 VM을 우클릭 → **복제** → **완전한 복제**를 고른다. MAC 주소 정책은 **"모든 네트워크 어댑터의 새 MAC 주소 생성"**을 고른다.
3. 테스트 VM의 설정 → 저장소에 VirtualBox NVMe 디스크가 붙어 있으면 **떼어 낸다**.
4. vCPU를 4로 맞추고 시리얼 출력을 파일로 연결한다.
   ```powershell
   .\VBoxManage.exe modifyvm "<테스트 VM 이름>" --cpus 4
   .\VBoxManage.exe modifyvm "<테스트 VM 이름>" --uart1 0x3F8 4 --uartmode1 file "C:\VMs\delfs-test-serial.log"
   ```

설명: [8.3.1](#831-테스트-vm-복제), vCPU 수는 [8.1.1](#811-vcpu-수는-사실상-포맷-파라미터다)

### 2.2 두 VM 연결 (호스트)
1. **도구 → 네트워크 → 호스트 전용 네트워크**에서 어댑터를 하나 만든다. 이미 있으면 그것을 쓴다.
2. 두 VM의 설정 → 네트워크 → **어댑터 2**를 켜고 **호스트 전용 어댑터**의 위 어댑터에 연결한다.
3. DHCP를 켠다. `<IF>`에는 첫 줄 출력의 `Name`을 넣는다(예: `VirtualBox Host-Only Ethernet Adapter #2`).
   ```powershell
   .\VBoxManage.exe list hostonlyifs
   .\VBoxManage.exe dhcpserver modify --interface="<IF>" --server-ip=192.168.56.100 --netmask=255.255.255.0 --lower-ip=192.168.56.101 --upper-ip=192.168.56.254 --enable
   ```
4. 두 VM을 켜고 각각 `ip -br a show enp0s8`을 실행해 `192.168.56.x` 주소가 나오는지 본다.

설명: [8.3.2](#832-호스트-전용-네트워크와-dhcp) (주소가 안 나올 때)

### 2.3 테스트 VM 첫 설정 (테스트 VM)
```bash
sudo hostnamectl set-hostname <새 이름>
sudo sed -i 's/<원래 이름>/<새 이름>/g' /etc/hosts
sudo rm -f /etc/machine-id && sudo systemd-machine-id-setup
sudo sed -i 's/^GRUB_TIMEOUT_STYLE=.*/GRUB_TIMEOUT_STYLE=menu/; s/^GRUB_TIMEOUT=.*/GRUB_TIMEOUT=10/' /etc/default/grub
sudo sed -i 's/^GRUB_CMDLINE_LINUX=.*/GRUB_CMDLINE_LINUX="console=tty0 console=ttyS0,115200"/' /etc/default/grub
sudo update-grub
sudo apt install -y git nvme-cli fio openjdk-11-jre
[ -e "$HOME/DeLFS/.git" ] || git clone --branch docs/env-guide-team-layout https://github.com/hanbin5/DeLFS-with-yongseok.git "$HOME/DeLFS"
sudo reboot
```
재부팅한 뒤 `ip -br a show enp0s8`의 주소를 적어 둔다. 이후 명령의 `<test>`는 `<user>@<이 주소>`다. Tailscale 같은 VPN을 쓰고 있었다면 테스트 VM에서는 로그아웃한다.

설명: [8.3.3](#833-테스트-vm-첫-부팅-설정)

### 2.4 개발 VM 준비 (개발 VM)
```bash
export DELFS=~/DeLFS
export GUIDE="$DELFS/env-guide"
export KSRC=~/linux-7.2.8 BUILD=~/delfs-build
mkdir -p "$BUILD"
sudo apt install -y build-essential flex bison bc libelf-dev libssl-dev dwarves \
    rsync cpio kmod dpkg-dev debhelper fakeroot libncurses-dev \
    autoconf automake libtool pkg-config uuid-dev openjdk-11-jdk maven git curl
[ -e "$DELFS/.git" ] || git clone --branch docs/env-guide-team-layout https://github.com/hanbin5/DeLFS-with-yongseok.git "$DELFS"
ls "$GUIDE/README.md" "$GUIDE"/scripts/*.sh
[ -f ~/.ssh/id_ed25519 ] || ssh-keygen -t ed25519 -N '' -f ~/.ssh/id_ed25519
ssh-copy-id <test>
```
새 터미널을 열면 앞의 `export` 세 줄을 다시 실행한다. `DELFS`를 먼저 설정한 다음 별도 명령으로 `GUIDE`를 설정해야 한다. `ls`에서 README와 스크립트 4개가 보이는지 확인한다. `~/DeLFS`가 이미 있으면 clone을 건너뛰므로, 파일이 없다면 [8.2.1](#821-delfs-저장소에-들어-있는-것)을 따라 원격 주소와 브랜치를 확인한다.

설명: 경로 변수 [8.2.3](#823-경로-변수-개발-vm), 빌드 도구 [8.4.1](#841-빌드-도구와-소스-위치), `maven`이 설치되지 않을 때 [8.2.2](#822-저장소에-없어서-따로-받거나-만드는-것)

### 2.5 7.2.8 커널 빌드 (개발 VM)
빌드하는 30~40분 동안 테스트 VM에서는 아무것도 돌리지 않는다.
```bash
cd $BUILD
curl -LO https://cdn.kernel.org/pub/linux/kernel/v7.x/linux-7.2.8.tar.xz
curl -LO https://cdn.kernel.org/pub/linux/kernel/v7.x/sha256sums.asc
grep ' linux-7.2.8.tar.xz$' sha256sums.asc | sha256sum -c      # linux-7.2.8.tar.xz: OK
tar xf linux-7.2.8.tar.xz -C $(dirname $KSRC)

O=$BUILD/target-7.2.8
mkdir -p $O
cp /boot/config-$(uname -r) $O/.config
$KSRC/scripts/config --file $O/.config --set-str LOCALVERSION -vanilla \
    --set-str SYSTEM_TRUSTED_KEYS "" --set-str SYSTEM_REVOCATION_KEYS "" \
    --disable DEBUG_INFO_DWARF_TOOLCHAIN_DEFAULT --disable DEBUG_INFO_DWARF4 \
    --disable DEBUG_INFO_DWARF5 --disable DEBUG_INFO_BTF --enable DEBUG_INFO_NONE
make -C $KSRC O=$O olddefconfig
make -s -C $KSRC O=$O kernelrelease          # 7.2.8-vanilla
grep -E '^CONFIG_(F2FS_FS|NVME_TARGET|NVME_TARGET_LOOP|DEBUG_INFO_NONE)=' $O/.config
# CONFIG_F2FS_FS=m, CONFIG_NVME_TARGET=m, CONFIG_NVME_TARGET_LOOP=m, CONFIG_DEBUG_INFO_NONE=y 이어야 한다

make -C $KSRC O=$O -j"$(nproc)" bindeb-pkg 2>&1 | tee target-build.log
ls *.deb      # linux-image-7.2.8-vanilla_*.deb, linux-headers-7.2.8-vanilla_*.deb, linux-libc-dev_*.deb
```

설명: config와 grep 값이 다를 때 [8.4.2](#842-config), 빌드 [8.4.4](#844-빌드와-설치)

### 2.6 7.2.8 커널 설치 (테스트 VM)
1. VirtualBox에서 테스트 VM의 **스냅샷을 찍는다**(머신 → 스냅샷 찍기).
2. 개발 VM에서 패키지를 보낸다. `linux-libc-dev`는 보내지도 설치하지도 않는다.
   ```bash
   scp $BUILD/linux-image-7.2.8-vanilla_*.deb $BUILD/linux-headers-7.2.8-vanilla_*.deb <test>:~/
   ```
3. 테스트 VM에서 설치하고 재부팅한다. GRUB 메뉴의 기본 항목이 7.2.8-vanilla가 아니면 **Advanced options for Ubuntu → Ubuntu, with Linux 7.2.8-vanilla**를 고른다.
   ```bash
   sudo dpkg -i ~/linux-image-7.2.8-vanilla_*.deb ~/linux-headers-7.2.8-vanilla_*.deb
   sudo reboot
   uname -r          # 7.2.8-vanilla
   ```

설명: [8.4.4](#844-빌드와-설치), 스냅샷 요령 [8.3.4](#834-스냅샷-요령)

### 2.7 F2FS 도구와 벤치마크 도구 빌드 (개발 VM)
```bash
# f2fs-tools 1.16.0
cd $BUILD
curl -LO https://git.kernel.org/pub/scm/linux/kernel/git/jaegeuk/f2fs-tools.git/snapshot/f2fs-tools-1.16.0.tar.gz
tar xzf f2fs-tools-1.16.0.tar.gz && cd f2fs-tools-1.16.0
./autogen.sh && ./configure --disable-shared && make -j"$(nproc)"
ssh <test> mkdir -p f2fs-tools bench
scp mkfs/mkfs.f2fs fsck/fsck.f2fs <test>:~/f2fs-tools/

# filebench 1.5-alpha3
cd $BUILD
curl -LO https://github.com/filebench/filebench/releases/download/1.5-alpha3/filebench-1.5-alpha3.tar.gz
tar xzf filebench-1.5-alpha3.tar.gz && cd filebench-1.5-alpha3
./configure --prefix=$HOME/bench/filebench && make -j"$(nproc)" && make install
rsync -a $HOME/bench/filebench <test>:bench/

# YCSB + RocksDB
cp -a $DELFS/script/RocksDB $BUILD/ycsb-src && cd $BUILD/ycsb-src
mvn -B -q -pl site.ycsb:rocksdb-binding -am clean package -DskipTests -Dcheckstyle.skip
mkdir -p $BUILD/ycsb-rocksdb
tar xzf rocksdb/target/ycsb-rocksdb-binding-0.18.0-SNAPSHOT.tar.gz -C $BUILD/ycsb-rocksdb --strip-components=1
rsync -a $BUILD/ycsb-rocksdb <test>:bench/

# 팀 포크의 env-guide/scripts/에서 실행 스크립트 4개를 테스트 VM 홈으로 복사
scp "$GUIDE"/scripts/*.sh <test>:~/
ssh <test> 'chmod +x ~/run-fio-small.sh ~/run-macro-small.sh ~/nvme-loop-up.sh'
```
filebench는 개발 VM과 테스트 VM의 사용자 이름이 같아야 복사해서 쓸 수 있다. 복제한 VM이면 같다.

`run-fio-small.sh`와 `run-macro-small.sh`는 **자신과 같은 디렉터리의 `bench-lib.sh`**를 읽으므로 네 파일을 함께 복사한다. `bench-lib.sh`는 직접 실행하지 않아 실행 권한이 없어도 된다. 저장소에서 스크립트를 갱신한 뒤에는 위 `scp`와 `chmod`를 다시 실행해 테스트 VM의 복사본도 갱신한다. 테스트 VM의 `~/DeLFS/script/`도 같은 팀 포크 버전으로 맞춘다([8.2.1](#821-delfs-저장소에-들어-있는-것)).

설명: f2fs-tools [8.6.1](#861-f2fs-tools-1160), 벤치마크 도구 [8.7.1](#871-도구-준비), 테스트 VM의 파일 배치 [8.2.4](#824-테스트-vm의-파일-배치)

### 2.8 nvme-loop와 F2FS 기본 확인 (테스트 VM)
7.2.8-vanilla로 부팅한 상태에서 한다. nvme-loop는 **재부팅할 때마다** 다시 만든다.
```bash
sudo ~/nvme-loop-up.sh
# ctrl=nvme0 kato=600
# dev=/dev/nvme0n1 model=Linux lba=4096 discard_max=2199023255040
```
**`model=Linux`인지 확인한 뒤에만** 다음으로 넘어간다. 아래 mkfs가 `/dev/nvme0n1`을 지운다.
```bash
sudo modprobe f2fs
sudo ~/f2fs-tools/mkfs.f2fs -f /dev/nvme0n1 | tail -3          # format successful
sudo mkdir -p /mnt/test
sudo mount -t f2fs /dev/nvme0n1 /mnt/test && findmnt /mnt/test  # f2fs, rw, discard
echo hello | sudo tee /mnt/test/a.txt; sync
sudo umount /mnt/test
sudo mount -t f2fs /dev/nvme0n1 /mnt/test && cat /mnt/test/a.txt   # hello
sudo umount /mnt/test
sudo ~/f2fs-tools/fsck.f2fs /dev/nvme0n1 | grep -c Fail           # 0
sudo rmmod f2fs
sudo dmesg | grep -E 'WARNING|Oops|BUG:|I/O error'                # 출력 없어야 함
```

설명: nvme-loop [8.5](#85-nvme-loop), 확인 항목 [8.6.2](#862-기본-동작-확인)

### 2.9 축소 벤치마크 (테스트 VM)
돌리는 동안 개발 VM에서는 빌드하지 않는다.
```bash
sudo ~/nvme-loop-up.sh                          # 재부팅했다면
sudo FS=f2fs ~/run-fio-small.sh                 # 약 10분
sudo FS=f2fs ~/run-macro-small.sh all           # filebench + YCSB, 30~60분
```
각 스크립트가 마지막에 결과 표와 `results:` 경로를 출력한다.
- **통과:** fio는 `fio_rc=0`이고 `err=0`, filebench와 YCSB는 `ok=yes`. 그리고 모두 `kerr=0`.
- `lockup` 열은 기록만 한다.
- `kerr`가 0이 아니면 [8.7.3](#873-판정-기준)을 본다.

결과를 [6장](#6-현재-결과)의 표와 비교한다. **여기까지가 D 파트 환경 구성이다.**

설명: [8.7](#87-축소-벤치마크)

---

## 3. (선택) DeLFS 6.6.8 실행 테스트

포팅 전에 **원본 DeLFS가 같은 환경에서 도는지** 확인해 두면, 7.2.8에서 문제가 생겼을 때 원인이 포팅인지 환경인지 구분할 수 있다. 6.6.8 기준선은 x86_64 호스트(Intel Core i5-12400F, Windows, VirtualBox)의 테스트 VM(4 vCPU, RAM 8GB, Ubuntu 20.04.6 LTS)에서 확인했다(6장).

1. **커널:** 저장소의 `DeLFS/linux-6.6.8`을 `O=`로 빌드한다. config는 같은 디렉터리의 `oldconfig` 파일이다. 저장소 README가 말하는 `config-scalecache`는 저장소에 없다. `oldconfig`의 `LOCALVERSION`이 `-DeLFS`(대문자)라서 소문자로 바꿔야 패키징된다.
   ```bash
   K=$DELFS/DeLFS/linux-6.6.8; O6=$BUILD/delfs-6.6.8
   mkdir -p $O6 && cp $K/oldconfig $O6/.config
   $K/scripts/config --file $O6/.config --set-str LOCALVERSION -delfs
   make -C $K O=$O6 olddefconfig
   cd $BUILD && make -C $K O=$O6 -j"$(nproc)" bindeb-pkg      # → linux-image-6.6.8-delfs_*.deb
   ```
2. **모듈:** `make M=`은 산출물을 소스 디렉터리 안에 만든다. 그래서 `DeLFS/module_DeLFS`를 복사해서 빌드한다.
   ```bash
   cp -a $DELFS/DeLFS/module_DeLFS $BUILD/module_DeLFS && cd $BUILD/module_DeLFS
   make -C $O6 M=$PWD -j"$(nproc)" modules                    # → delfs.ko
   ```
3. **gcc 11 미만에서 오류 3곳:** `node.c`의 `refresh_list:`, `retry:`와 `segment.c`의 `next:`처럼 **레이블 바로 뒤에 변수 선언**이 오는 문법은 gcc 11부터 허용된다. 원저자는 gcc 11.4를 썼다. gcc 9.4에서는 **복사본의** 레이블 뒤에 `;`을 붙여 `refresh_list:;`처럼 빈 문장을 넣으면 된다. 생성되는 코드는 같다.
   - **7.2.8 포팅 코드에서도 같은 3곳을 고쳐야 한다.** 7.2.8도 `-std=gnu11`로 컴파일하기 때문이다.
4. **mkfs:** 저장소의 `delfs-mkfs`도 복사해서 빌드한다.
   ```bash
   cp -a $DELFS/delfs-mkfs $BUILD/delfs-mkfs && cd $BUILD/delfs-mkfs
   ./autogen.sh && ./configure --disable-shared && make -j"$(nproc)"   # → mkfs/mkfs.delfs
   scp $BUILD/module_DeLFS/delfs.ko mkfs/mkfs.delfs <test>:~/
   ```
5. **실행:** 테스트 VM을 `6.6.8-delfs`로 부팅한 뒤 `sudo ~/run-fio-small.sh`처럼 실행한다. 기본값이 `FS=delfs`이고, `~/delfs.ko`와 `~/mkfs.delfs`를 쓴다.
6. **discard 경로가 실제로 도는지 확인:** 마운트한 뒤 아래를 확인한다.
   - `findmnt /mnt/test`의 옵션에 `discard`가 있는지 [실측: 있음]
   - `ps -e | grep f3fs_discard`로 DeLFS의 discard 스레드가 떠 있는지 [미검증]
   - 장치가 discard를 지원하지 않으면 DeLFS는 discard를 조용히 끄고 이 스레드도 시작하지 않는다.

참고할 점이 몇 가지 있다.
- **mkfs의 trim 표시:** `mkfs.delfs`는 장치의 discard가 실패해도 메시지만 출력하고, 체크포인트에 `CP_TRIMMED_FLAG`를 무조건 기록한다(`delfs-mkfs/mkfs/f2fs_format_utils.c:179`의 `c.trimmed = 1`). discard가 안 되는 장치에서는 이 표시가 실제와 다르다. nvme-loop에서는 `Discarded 20480 MB`로 실제 discard가 수행된다. [소스]
- **4KB 페이지 필수:** 모듈은 `PAGE_SIZE`가 4096이 아니면 로드를 거부한다(`DeLFS/module_DeLFS/super.c:5175`). x86에서는 항상 4KB다. arm64는 5장을 본다.
- **커널 config 숨은 조건:** 모듈의 `fs/fs-writeback.c` 사본은 `CONFIG_CGROUP_WRITEBACK`일 때만 export되는 커널 함수를 조건 없이 부른다. 이 옵션이 꺼지면 modpost에서 실패한다. `oldconfig`와 일반 배포판 config에서는 켜져 있다. [소스]
- **기준선은 x86 한 명이 한 번만 해도 된다.** 맥 팀원은 arm64용 6.6.8을 빌드할 필요 없이 7.2.8부터 시작하면 된다.

---

## 4. 겪은 문제와 해결 방법

### Hyper-V 위의 VirtualBox: soft lockup과 멈춤
- **증상:**
  - 커널 로그와 콘솔에 `watchdog: BUG: soft lockup - CPU#N stuck for 30~48s!`가 나온다.
  - filebench는 `Run took 60 seconds`라고 기록하지만 실제로는 4분 넘게 걸린다.
- **원인:** VirtualBox 상태 표시줄에 **거북이 아이콘**이 있으면 VirtualBox가 Windows Hyper-V 위에서 돌고 있다는 뜻이다. WSL2, Docker Desktop, 메모리 무결성 같은 기능이 켜져 있으면 이렇게 된다. 여기에 두 VM이 호스트 CPU를 나눠 쓰면, 게스트 vCPU가 수십 초씩 실행되지 못한다.
- **근거:**
  - **아무것도 하지 않던 CPU**(`swapper`)에서도 lockup이 났다.
  - 파일시스템을 하나도 올리지 않은 테스트 VM에서도, 개발 VM이 커널을 빌드하는 동안 `snapd`에서 lockup이 났다.
  - 따라서 파일시스템 버그가 아니라 **환경 때문에 생기는 경고**다.
- **hung task 경고도 같이 날 수 있다.** 멈춤이 길면 I/O를 기다리던 작업이 `INFO: task … blocked for more than 122 seconds`로 한꺼번에 찍힌다. `bench-lib.sh`는 이 경고를 soft lockup과 따로 세지 않고 `kerr`에 넣는다. 그래서 멈춤만으로도 `kerr`가 0이 아닐 수 있다.
  - **실제 사례(7.2.8 F2FS oltp, `kerr=10`):** 10건이 모두 같은 시각에 찍혔다. 콜 스택은 `pwrite64 → f2fs_sync_file → f2fs_write_data_pages → mutex_lock`이었고, mutex를 잡고 있던 쪽도 같은 oltp의 filebench 스레드였다. 같은 시간대에 CPU 3~4개에서 soft lockup이 동시에 45초씩 났고, 그중에는 `tailscaled`, `snapd`, `ksoftirqd`처럼 벤치마크와 관계없는 프로세스도 있었다. VM 전체가 멈춘 패턴이다. oltp는 `IO Summary`까지 정상 종료했다.
  - **판정:** 케이스의 `.dmesg` 파일과 로그에서 아래 세 가지를 확인한다.
    1. 같은 시각에 soft lockup이 **여러 CPU에서, 관계없는 프로세스까지** 함께 찍혔는지
    2. 파일시스템 오류(`F2FS-fs … error`), I/O error, keep-alive 만료가 없는지
    3. 벤치마크가 끝까지 완료됐는지(`IO Summary`, `rc=0`)
  - 셋 다 만족하면 환경 영향으로 판단한다. 그렇더라도 "통과"가 아니라 **"완료, 환경 영향 경고 있음"**으로 구분해 적는다. 확정하려면 Hyper-V를 끄고 그 케이스만 다시 돌린다.
- **대처:**
  - 빌드와 벤치마크를 **동시에 돌리지 않는다.**
  - 벤치마크 판정에서는 lockup을 따로 센다(`lockup` 열).
  - 근본적으로는 벤치마크를 돌리는 동안만 Hyper-V를 끈다. 관리자 PowerShell에서 `bcdedit /set hypervisorlaunchtype off`를 실행하고 재부팅한다. 되돌릴 때는 `bcdedit /set hypervisorlaunchtype auto`를 실행하고 재부팅한다. Hyper-V가 꺼져 있는 동안은 WSL2를 쓸 수 없지만 WSL2의 데이터는 그대로 남는다. BitLocker를 쓴다면 복구 키를 준비한다.
  - Hyper-V를 끈 상태의 대조 실험은 아직 하지 않았다.

### nvme-loop 장치가 갑자기 사라짐 (EIO)
- **증상:**
  - 벤치마크 도중 `rm: cannot remove …: Input/output error`가 나고, 다음 mkfs가 `stat failed errno:2`로 실패한다.
  - 커널 로그에 `nvmet: ctrl 1 keep-alive timer (5 seconds) expired!` → `Removing ctrl` → `no available path - failing I/O`가 찍힌다.
- **원인:** nvme-loop의 keep-alive 기본값이 5초라서, VM이 5초 넘게 멈추면 컨트롤러가 제거되고 `/dev/nvme0n1`이 사라진다.
- **대처:** `nvme connect -t loop -n delfs -k 600`으로 keep-alive를 10분으로 늘린다. `nvme-loop-up.sh`가 이렇게 연결한다. `/sys/class/nvme/nvme0/kato`가 600인지 확인한다.
  - **`-k 0`은 효과가 없다.** 커널(`drivers/nvme/host/fabrics.c`)이 일반 장치 연결에서 0을 기본값 5초로 되돌린다.

### nvme-loop 장치는 시험 중인 커널 안에 있다
VirtualBox 디스크는 게스트 밖에 있지만, nvme-loop 장치(nvmet → 이미지 파일 → ext4)는 **시험 중인 커널 안에서** 돈다. 시험 중인 파일시스템 모듈이 커널 메모리를 망가뜨리면 장치 계층까지 함께 망가질 수 있다. 원인을 알 수 없는 크래시가 나면 이 가능성도 따져 본다. nvmet과 ext4는 오래 검증된 코드라 문제의 원인일 가능성은 낮다. [소스]

### 저장소 스크립트를 그대로 쓸 때 주의
`DeLFS/` 아래의 스크립트는 원저자 서버에 맞춰져 있다. 이 문서에서는 쓰지 않고 [`scripts/`](scripts/)를 썼다. 직접 쓰려면 아래를 먼저 고쳐야 한다. [실측]

| 파일 | 문제 |
| --- | --- |
| `DeLFS/f2fs-mkfs.sh`, `DeLFS/f2fs-test.sh` | **git 병합 충돌 표시**(`<<<<<<< HEAD`, `=======`, `>>>>>>>`)가 남아 있어서 실행되지 않는다 |
| `DeLFS/test.sh`, `DeLFS/f2fs-test.sh` | `sudo chown syslab:syslab /mnt/test`가 있다. `syslab` 사용자가 없으면 실패한다 |
| `DeLFS/test.sh` | 장치를 `/dev/nvme${N}n1p1`(파티션)로 가정한다. 파티션 없이 `nvme0n1` 전체를 쓰면 맞지 않는다 |
| `DeLFS-CrashMonkey/*.sh` | `/home/syslab/workspace_hwan/...` 경로가 하드코딩돼 있다 |
| `DeLFS-CrashMonkey`의 커널 모듈(`cow_brd.c`, `disk_wrapper.c`) | 6.6 API(`blk_alloc_disk(NUMA_NO_NODE)`, `blkdev_get_by_path`)를 쓴다. 7.2.8에서는 각각 `blk_alloc_disk(lim, node)`, `bdev_file_open_by_path()`로 바뀌어서 **빌드되지 않는다.** 크래시 테스트(M3) 전에 포팅해야 한다. [소스] |
| 저장소 README | `cp config-scalecache .config`라고 하지만 그 파일은 없다. 실제 config는 `DeLFS/linux-6.6.8/oldconfig`다 |
| `delfs-mkfs/mkfs/mkfs.delfs`(빌드 전) | libtool 래퍼 스크립트일 뿐이라 빌드해야 한다(3장) |

### 그 밖의 문제
| 문제 | 해결 |
| --- | --- |
| `bindeb-pkg`가 마지막에 `Package-field must be a valid package name`으로 실패 | `LOCALVERSION`을 소문자로 바꾼다 |
| 빌드 세션이 끊겨 `.deb`가 비정상적으로 작게(수백 KB) 남음 | 같은 `make … bindeb-pkg`를 다시 실행한다. 컴파일은 다시 하지 않고 2~5분 안에 패키징만 한다. |
| filebench `cvar-gamma` 를 찾지 못함 | `--prefix`로 설치 경로를 지정하고 `make install`까지 한다 |
| filebench fileserver `could not obtain a file` | 파일 개수는 그대로 두고 파일 크기만 줄인다([8.7.4](#874-축소-기준)) |
| `rsync`로 스크립트를 보낸 뒤 `sudo: command not found` | 원본 파일의 실행 권한이 그대로 복사됐다. 원본에 `chmod +x`를 한다. |
| `/sys/block`에 `nvme0c0n1`과 `nvme0n1`이 같이 보임 | `nvme0c0n1`은 multipath의 숨은 경로 노드다. 실제로 쓰는 장치는 `nvme0n1`이다. |
| VirtualBox GUI에서 켠 DHCP가 동작하지 않음 | `VBoxManage dhcpserver modify … --enable`로 켜고 어댑터를 다시 붙인다([8.3.2](#832-호스트-전용-네트워크와-dhcp)) |
| 스크립트가 `DeLFS repo not found`로 멈춤 | 테스트 VM에 저장소를 clone하거나 `DELFS_REPO=<경로>`를 지정한다 |

---

## 5. Mac(UTM, arm64) 팀원 참고 (미검증)

x86에서 실제로 해 본 절차와, 저장소와 커널 소스를 읽고 확인한 내용을 바탕으로 **맥에서 달라지는 점**을 정리했다. 맥에서 직접 해 보지는 않았다. 맥 팀원이 해 보고 고쳐 주기를 바란다.

### 5.1 VM 도구 (UTM)
| 항목 | x86 팀원 | 맥 팀원 |
| --- | --- | --- |
| VM 도구 | VirtualBox | UTM. **백엔드는 "QEMU"**를 고른다. "Apple Virtualization"은 디스크를 virtio로만 붙일 수 있다. |
| 하드웨어 가속 | VT-x / AMD-V | HVF. **arm64 게스트**여야 한다. 맥에서 x86 게스트는 소프트웨어 에뮬레이션(TCG)이라 쓸 수 없을 만큼 느리다. |
| 게스트 OS | Ubuntu 20.04 (검증) | **Ubuntu 22.04 이상 arm64** 권장([5.2](#52-커널-빌드)의 Python 참고) |
| 되돌리기 | 스냅샷 | UTM에는 스냅샷 메뉴가 사실상 없다. 새 커널을 설치하기 전에 **VM 복제(Clone)**로 되돌릴 지점을 만든다. GRUB에 원래 커널도 남겨 둔다. |
| 개발 VM / 테스트 VM | 따로 둠 | 똑같이 따로 둔다. 테스트 커널이 패닉을 내도 개발 환경은 안전해야 한다. |
| 두 VM 연결 | 호스트 전용 네트워크 | UTM의 공유 네트워크(Shared Network)에서 두 VM이 서로 보이는지 확인한다. [미검증] |
| 테스트 장치 | nvme-loop | **똑같이 nvme-loop.** 하이퍼바이저와 상관없는 게스트 커널 기능이라 장치, 드라이버, 4KB LBA, discard 동작이 x86과 같다. `scripts/nvme-loop-up.sh`도 그대로 쓴다. |
| vCPU | 4 | **4 (팀 공통 값).** UTM 기본값을 그대로 두지 않는다([8.1.1](#811-vcpu-수는-사실상-포맷-파라미터다)). |

### 5.2 커널 빌드
- 테스트 VM이 될 arm64 게스트 안에서 직접 빌드한다. 개발 VM에서 빌드해도 된다. `ARCH`는 자동으로 `arm64`가 되므로 크로스 컴파일러가 필요 없다. 결과 커널은 `vmlinuz`(압축된 `Image`)이고, `bindeb-pkg`는 x86과 같은 방식으로 동작한다.
- config는 **게스트의 `/boot/config-$(uname -r)`**에서 시작한다. 저장소의 `oldconfig`는 x86 전용이다. [8.4.2](#842-config)의 `scripts/config` 명령(인증서 경로 비우기, 디버그 정보와 BTF 끄기)을 그대로 적용한다. Ubuntu arm64 config에도 같은 인증서 경로가 들어 있을 것이다. [미검증]
- **arm64 defconfig는 쓰지 않는다.** 7.2.8 arm64 defconfig에는 F2FS 항목도 NVMe 타깃 항목도 하나도 없다. [실측] defconfig로 만들면 원본 F2FS 모듈이 없거나 x86과 기능이 다른 모듈이 되고, `modprobe nvme-loop`부터 실패한다.
- **필수 config 체크리스트**(만든 뒤 `grep`으로 확인한다):

  | 항목 | 값 | 이유 |
  | --- | --- | --- |
  | `ARM64_4K_PAGES` | y | DeLFS 모듈은 `PAGE_SIZE`가 4096이 아니면 로드를 거부한다(`super.c:5175`). F2FS 블록 크기도 4KB다. 게스트에서 `getconf PAGESIZE`가 4096인지 확인한다. |
  | `F2FS_FS` | m | 원본 F2FS 모듈 |
  | `F2FS_STAT_FS`, `F2FS_FS_XATTR`, `F2FS_FS_POSIX_ACL`, `F2FS_FS_SECURITY`, `F2FS_IOSTAT` | y | x86 config와 같게 맞춘다. DeLFS 모듈도 이 값으로 빌드할 파일을 고른다. |
  | `NVME_CORE`, `NVME_FABRICS`, `NVME_TARGET`, `NVME_TARGET_LOOP` | m | nvme-loop |
  | `CONFIGFS_FS` | y | nvmet 설정 |
  | `CGROUP_WRITEBACK` | y | DeLFS 모듈의 숨은 필수 조건(3장) |
  | `SERIAL_AMBA_PL011`, `SERIAL_AMBA_PL011_CONSOLE` | y | 시리얼 콘솔([5.3](#53-시리얼-콘솔과-패닉-로그)) |
  | `VIRTIO_PCI`, `VIRTIO_BLK`, `VIRTIO_NET`, `DRM_VIRTIO_GPU` | y 또는 m | UTM 부팅 디스크, 네트워크, 화면. 부팅 디스크 드라이버가 빠지면 루트 파일시스템을 찾지 못한다. |
  | `DEBUG_INFO_NONE` | y | 빌드 시간과 크기 |

- **Python:** 7.2.8의 `Documentation/process/changes.rst`에 따르면 arm64 기본 config에는 Python 3.9 이상이 필요하다. Ubuntu 20.04는 3.8이다. [소스]
- **gcc:** 22.04(gcc 11)나 24.04(gcc 13)에서는 3장의 레이블 오류 3곳이 나지 않는다.
- **커널 소스 위치:** 반드시 VM 내부 디스크에 둔다. macOS(APFS)는 기본적으로 대소문자를 구분하지 않아서, 맥과 공유하는 폴더에 커널 소스를 두면 깨진다([8.4.1](#841-빌드-도구와-소스-위치)).

### 5.3 시리얼 콘솔과 패닉 로그
- arm64 `virt` 보드의 기본 UART는 PL011이라 콘솔 이름이 다르다. 부팅 인자는 `console=tty0 console=ttyAMA0`로 준다. 마지막에 쓴 것이 `/dev/console`이 된다. x86의 `ttyS0`(16550A)과는 다르다.
- UTM 내장 터미널은 **창을 닫으면 로그가 사라진다.** 패닉이나 hung task가 나면 `dmesg`도 같이 멈추므로, 그 순간을 남길 수단은 시리얼 로그뿐이다. 시리얼 장치를 **TCP 모드**로 두고 맥에서 파일로 받는 습관을 들인다.
  ```bash
  nc localhost <port> | tee delfs-test-serial.log
  ```

### 5.4 벤치마크
- fio, filebench, nvme-loop 스크립트, f2fs-tools는 아키텍처와 관계없는 C 코드나 셸이다. x86과 같은 절차로 빌드해서 쓴다. 각 도구의 arm64 apt 패키지가 있는지는 확인하지 않았다. [미검증]
- **YCSB RocksDB는 그대로는 실패할 가능성이 크다.** 저장소 `script/RocksDB/pom.xml`의 `rocksdbjni 6.2.2`는 aarch64에서 x86용 네이티브 라이브러리를 불러오는 문제가 보고돼 있다. aarch64에서 정상 동작이 처음 보고된 버전은 6.4.6이다(facebook/rocksdb 이슈 #5559). [소스]
  - 빌드할 때 `-Drocksdb.version=6.4.6`처럼 올려서 시도한다. YCSB rocksdb 바인딩이 바뀐 API와 맞는지는 확인하지 않았다. 이렇게 바꾸면 x86 결과와 RocksDB 버전이 달라진다는 점을 기록해 둔다.
  - 저장소에 함께 들어 있는 `script/mysql+ycsb/ycsb-0.17.0`의 `rocksdbjni-5.11.3.jar`에는 aarch64 라이브러리가 아예 없다. 이것을 쓰면 확실히 실패한다.
- 맥에서는 Hyper-V 문제가 없으므로 soft lockup 경고는 줄어들 것으로 예상한다. [미검증]

### 5.5 결과를 다룰 때
- **빌드 결과물은 아키텍처끼리 공유하지 않는다.** `.config`, 커널, `delfs.ko`, `Module.symvers`는 각자 만든다.
- **성능 수치는 x86과 비교하지 않는다.** macOS는 vCPU를 특정 코어에 고정할 수 없고, P코어와 E코어가 섞여 있다. 지금 단계는 기능 검증만 한다.
- **맥에서만 나는 크래시는 코드 문제로 본다.** ARM은 x86보다 메모리 접근 순서 보장이 약해서, 락 없이 공유 데이터를 다루는 코드의 버그가 ARM에서만 드러날 수 있다. 맥 환경 탓으로 넘기지 않는다. DeLFS에서 먼저 볼 곳은 배리어 지점(`oldinv.c`, `checkpoint.c`, `segment.c`, `super.c`)과 락 없이 읽고 쓰는 `cur_victim_sec`(`gc.c`), `curseg->segno`(`segment.c`)다. [소스]
- 포팅 코드를 머지하기 전에 **x86과 arm64 모두에서 빌드되는지** 확인한다. 컴파일러 경고가 아키텍처마다 다르게 나올 수 있다.
- **DeLFS 커널을 arm64로 빌드할 때(포팅 단계):** 저장소의 `DeLFS/linux-6.6.8/mm/page-writeback.c:1980-1982`는 tracepoint의 static_call 심볼(`__SCT__…`, `__SCK__…`)을 직접 `EXPORT_SYMBOL`한다. arm64 GCC 빌드에는 static_call이 없어서 **커널 링크와 모듈 modpost가 실패할 것으로 보인다.** 포팅할 때는 이 줄들을 표준 매크로 `EXPORT_TRACEPOINT_SYMBOL(balance_dirty_pages);`로 바꾸면 두 아키텍처 모두에서 동작한다. 순정 7.2.8 환경 구성과는 관계없다. [소스, 빌드로는 미확인]

---

## 6. 현재 결과

x86_64 호스트(Intel Core i5-12400F, Windows, VirtualBox)의 테스트 VM(4 vCPU, RAM 8GB, Ubuntu 20.04.6 LTS) 기준이다.

| 환경 | 항목 | 결과 |
| --- | --- | --- |
| **6.6.8-delfs + DeLFS** | 빌드, mkfs, insmod/rmmod, mount/umount, 다시 마운트 후 데이터 보존 | 통과 |
| | fio 12개 | 통과 (`err=0`, 커널 경고 0) |
| | filebench 5개 | 통과. `kerr=0`이고, soft lockup은 Hyper-V 때문에 발생 |
| | YCSB A/B/F/U | 통과 (모든 연산 `Return=OK`) |
| **7.2.8-vanilla + F2FS** | 모듈 로드, mkfs, mount, 다시 마운트 후 데이터 보존, fsck, 모듈 해제 | 통과 (커널 경고 0) |
| | fio 12개 | 통과 (`err=0`, `kerr=0`, lockup 0). 모든 job의 `io=` 총량이 6.6.8과 같고 `short`/`dropped`는 0 |
| | filebench 5개 | 5개 모두 완료(`IO Summary` 있음). fileserver, varmail, videoserver, webserver는 `kerr=0`이다. **oltp는 `kerr=10`**인데, 10건 모두 Hyper-V 멈춤 중에 한꺼번에 찍힌 hung task 경고(`blocked for more than 122 seconds`, `f2fs_write_data_pages`의 mutex 대기)다. F2FS 오류, I/O error, keep-alive 만료는 없다. soft lockup은 varmail 10, webserver 3, oltp 62번 났다(4장) |
| | YCSB A/B/F/U | 통과 (load와 run 모두 연산별 `Operations` = `Return=OK`, `kerr=0`) |

---

## 7. 원본 DeLFS에 원래 있던 문제 후보

포팅과 별개로, **원본 6.6.8 DeLFS 코드에 원래 있던 것으로 보이는 문제**다. 7.2.8에서 벤치마크가 이상하게 동작하면 포팅 버그인지 원래 있던 문제인지 가리는 데 쓴다. 고칠지는 연구실이나 원작자와 정할 일이다. 포팅하면서 몰래 고치지 않고 기록만 해 둔다.

### 7.1 `cur_victim_sec`과 병렬 FG GC [소스, 피해 범위 미확정]
- 원본 F2FS는 foreground GC를 `gc_lock` 쓰기 모드로 하나씩만 돌린다. 그래서 "지금 GC 중인 section"을 전역 변수 `sbi->cur_victim_sec` 하나로 기록해도 충분했다.
- DeLFS는 GC를 병렬로 돌리려고 victim 선택부터 `gc_lock`을 **읽기 모드**로 잡는다(`DeLFS/module_DeLFS/gc.c:2507`). 그래서 여러 CPU의 GC가 동시에 돈다. 그런데 `cur_victim_sec`은 여전히 전역 변수 하나다.
  - CPU A가 section S_A를 GC하면서 값을 S_A로 쓴다(`gc.c:1266`).
  - CPU B가 다른 section을 고르면서 값을 덮어쓰거나, 먼저 끝난 GC가 `NULL_SEGNO`로 지운다(`gc.c:2598`).
  - 그러면 S_A의 보호가 풀려서, 공간이 부족할 때의 SSR 할당이나 다른 GC가 이동 중인 S_A를 다시 고를 수 있다.
- 결과가 GC 작업 낭비로 끝나는지 데이터 정합성까지 깨지는지는 확인하지 않았다. 동시성이 높을수록, 즉 vCPU가 많을수록 잘 드러날 것이다. 장치 사용률이 높은 벤치마크(fio 75-write, GC 계열)에서 이상 동작이 나면 이 문제부터 의심한다.
- 6.6.8 축소 기준선(4 vCPU)에서는 이 문제로 보이는 실패가 없었다.

### 7.2 데이터 경합 전수 점검은 하지 않았다
락 없이 일반 변수로 CPU끼리 값을 주고받는 곳은 키워드 검색으로 다 걸러지지 않아서, 모두 확인하지는 못했다. 포팅이 끝나면 **KCSAN**(커널 데이터 경합 검출기, `CONFIG_KCSAN`, x86_64와 arm64 지원)을 켠 디버그 커널로 축소 벤치마크를 한 번 돌려 보기를 권한다. 실제로 일어난 경합을 위치와 함께 보고해 주므로, 그 위치만 읽어 보면 된다.

### 7.3 참고
- 저장소의 `DeLFS/linux-6.6.8/scripts/mod/modpost.c`는 `MAX_UNRESOLVED_REPORTS`를 10에서 9999999로 올려 두었다. undefined 심볼 경고를 더 많이 보여 줄 뿐이고, 오류를 막지는 않는다. 원저자들이 심볼 export 문제를 많이 겪었다는 흔적으로 보인다.
- 저장소의 `DeLFS/module_DeLFS/hung-full.txt`는 원래 개발 환경에서 hung task가 났던 로그로 보인다. 위 문제와 관련이 있다는 증거는 없다.

---

## 8. [부록] 상세 설명

2장의 각 단계에서 "설명" 링크로 가리키는 내용이다. 따라 하는 데 꼭 읽을 필요는 없다.

### 8.1 구성 이유

#### 8.1.1 vCPU 수는 사실상 포맷 파라미터다
**팀 전체가 같은 vCPU 수를 쓴다.** 이 문서를 검증한 테스트 VM은 4 vCPU다. [소스]
- `mkfs.delfs`는 실행하는 순간의 온라인 CPU 수(`sysconf(_SC_NPROCESSORS_ONLN)`)로 디스크를 CPU별 세그먼트로 나눈다(`delfs-mkfs/mkfs/f2fs_format.c`).
- 모듈은 마운트한 뒤 `num_online_cpus()`로 CPU별 로그, 체크포인트, 세그먼트 담당 CPU 계산(`segno_to_sentry_cpu`)을 정한다.
- **CPU 수는 디스크에 기록되지 않는다.** 그래서 지켜야 할 규칙이 있다.
  - mkfs는 **테스트 VM 안에서** 실행한다. 호스트나 개발 VM에서 포맷하면 그 머신의 CPU 수로 나뉜다.
  - mkfs할 때와 마운트할 때 vCPU 수가 같아야 한다.
  - vCPU 수를 바꾸면 mkfs를 다시 한다.
- 경합 버그는 vCPU 수에 따라 재현 여부가 달라진다. 버그를 보고할 때는 **아키텍처, vCPU 수, 커널 버전**을 같이 적는다.
- 원본 F2FS(`FS=f2fs`)에는 이 제약이 없다.

#### 8.1.2 왜 VirtualBox NVMe 디스크가 아니라 nvme-loop인가
VirtualBox의 NVMe 컨트롤러는 DSM(Dataset Management) 명령을 지원하지 않아서 **discard를 쓸 수 없다.** 설정으로도 바꿀 수 없다.
- VirtualBox 공식 소스 `DevNVMe.cpp`가 ONCS(선택 명령 지원)를 0으로 고정해 둔다. [소스]
- 게스트에서 `nvme id-ctrl`을 보면 `oncs : 0`이고 `discard_max_bytes = 0`이다. [실측]
- VirtualBox SATA(AHCI) 디스크는 `VBoxManage storageattach … --discard on`으로 TRIM을 켤 수 있다고 알려져 있다. [미검증]

DeLFS는 CPU별 discard 처리(`discard_set`, `p_cmd_lock`)를 직접 바꿨다. 그런데 장치가 discard를 지원하지 않으면 DeLFS는 discard를 조용히 끄고, discard 스레드도 시작하지 않는다. [소스] 이 경로를 시험하려면 discard가 되는 장치가 필요하다.

nvme-loop는 커널 안에서 NVMe 타깃(nvmet)과 호스트를 루프백으로 연결한다.
- 커널 공식 기능이다. Kconfig 설명에 "NVMe 호스트와 타깃을 테스트하는 데 유용하다"고 적혀 있다. 커널 블록 계층 테스트 모음(blktests)도 같은 구성을 기본으로 쓴다. [소스]
- 파일을 백엔드로 쓰면 discard는 파일에 구멍을 뚫는 방식(punch hole)으로 처리된다.
- SATA 디스크와 일반 loop 장치는 NVMe가 아니어서 쓰지 않았다.
- DeLFS 모듈에는 NVMe나 PCI 전용 코드가 없다. 그래서 `nvme-pci` 대신 `nvme-loop`를 거쳐도 DeLFS 입장에서는 같은 블록 장치다. [소스]

#### 8.1.3 왜 QEMU VM이 아니라 VirtualBox 테스트 VM인가
팀 분담 문서에는 "QEMU VM 환경"으로 적혀 있다. 하지만 개발 VM 자체가 VirtualBox 게스트이고 중첩 가상화가 꺼져 있다(`vmx`/`svm` 플래그와 `/dev/kvm` 없음). 그래서 그 안에서 QEMU를 띄우면 KVM 없이 소프트웨어 에뮬레이션(TCG)으로 돌아 수십 배 느리다. [실측] 대신 **같은 호스트에 VirtualBox 테스트 VM을 하나 더 두고** 커널을 직접 설치해 부팅한다. 하드웨어 가속을 그대로 받는다.

### 8.2 준비물과 디렉터리 배치

#### 8.2.1 DeLFS 저장소에 들어 있는 것
[팀 포크](https://github.com/hanbin5/DeLFS-with-yongseok)를 아래처럼 받는다. 개발 VM과 테스트 VM 모두 같은 팀 포크와 브랜치를 쓴다. 저장소 이름은 `DeLFS-with-yongseok`이지만 clone 대상 경로를 지정하므로 로컬 디렉터리는 `~/DeLFS`가 된다.

```bash
git clone --branch docs/env-guide-team-layout https://github.com/hanbin5/DeLFS-with-yongseok.git ~/DeLFS
```

기존 `~/DeLFS`가 있다면 먼저 `git -C ~/DeLFS status --short`로 작업 중인 변경을 확인한다. 아래 `origin` 주소가 팀 포크를 가리킬 때, 변경을 정리한 뒤 브랜치를 받아 전환한다. 원본 저장소(`https://github.com/syslab-CAU/DeLFS.git`)를 clone해 둔 경우에는 먼저 팀 포크를 별도 경로에 clone하고 `DELFS`(개발 VM)와 `DELFS_REPO`(테스트 VM)를 그 경로로 지정하거나, 기존 clone의 원격 구성을 팀 포크에 맞춰야 한다.

```bash
git -C ~/DeLFS remote get-url origin
# https://github.com/hanbin5/DeLFS-with-yongseok.git (또는 같은 저장소의 SSH 주소)
git -C ~/DeLFS fetch origin
git -C ~/DeLFS switch docs/env-guide-team-layout
git -C ~/DeLFS pull --ff-only origin docs/env-guide-team-layout
git -C ~/DeLFS rev-parse HEAD
```

두 VM의 `rev-parse HEAD`가 같은지 확인하고 2.7에서 스크립트를 복사한다. 이 브랜치가 `main`에 병합된 뒤에는 위 clone 명령의 `--branch`를 생략하고, 기존 clone의 `switch`와 `pull` 대상도 `main`으로 바꿀 수 있다.

기존 기술 설명과 검증 결과는 커밋 `2abec97a456b484b40efd178c7a3c063a6503075`(2026-11-01 기준 `main`의 HEAD)를 기준으로 한 기록이며, 이번 변경은 안내서의 저장소 포함과 배치·배포·경로 설명을 정리한 것이다. 이 문서에서 쓰는 저장소 안의 경로는 다음과 같다. 경로는 저장소 루트 기준이다.

| 저장소 경로 | 용도 |
| --- | --- |
| [`env-guide/README.md`](README.md) | 이 환경 구성 안내서. 개발 VM에서는 `$DELFS/env-guide/README.md` |
| [`env-guide/scripts/`](scripts/) | 실행 스크립트 4개. 개발 VM에서 테스트 VM 홈으로 복사한다(2.7). |
| `DeLFS/linux-6.6.8/`, `DeLFS/linux-6.6.8/oldconfig` | DeLFS가 수정한 6.6.8 커널과 그 config. [3장](#3-선택-delfs-668-실행-테스트)(선택)에서 쓴다. |
| `DeLFS/module_DeLFS/` | DeLFS 모듈 소스. [3장](#3-선택-delfs-668-실행-테스트)에서 쓴다. |
| `delfs-mkfs/` | DeLFS 포맷 도구. f2fs-tools 1.16.0을 바탕으로 만들었다. [3장](#3-선택-delfs-668-실행-테스트)에서 쓴다. |
| [`script/fio/*.fio`](../script/fio/) | fio job. 2.9에서 쓴다. |
| [`script/filebench/my*.f`](../script/filebench/) | filebench 프로필. 2.9에서 쓴다. |
| [`script/RocksDB/`](../script/RocksDB/) | YCSB 0.18 소스와 `workloads/myworkload{a,b,f,u}`. 2.7과 2.9에서 쓴다. |

```text
~/DeLFS/                         # 개발 VM의 팀 포크 clone
├── DeLFS/                       # 기존 커널·모듈 소스
├── delfs-mkfs/
├── env-guide/
│   ├── README.md
│   └── scripts/
│       ├── bench-lib.sh
│       ├── run-fio-small.sh
│       ├── run-macro-small.sh
│       └── nvme-loop-up.sh
└── script/                      # 기존 벤치마크 프로필·workload·소스
    ├── fio/
    ├── filebench/
    └── RocksDB/
```

README의 상대 링크는 `env-guide/README.md` 위치가 기준이다. 실행 스크립트는 `scripts/…`, 저장소의 기존 벤치마크는 `../script/…`, 저장소 최상위 README는 [`../README.md`](../README.md)로 연결한다.

#### 8.2.2 저장소에 없어서 따로 받거나 만드는 것
| 항목 | 받는 곳 | 단계 |
| --- | --- | --- |
| Linux 7.2.8 소스 | `https://cdn.kernel.org/pub/linux/kernel/v7.x/linux-7.2.8.tar.xz` | 2.5 |
| f2fs-tools 1.16.0 | `https://git.kernel.org/pub/scm/linux/kernel/git/jaegeuk/f2fs-tools.git/snapshot/f2fs-tools-1.16.0.tar.gz` | 2.7 |
| filebench 1.5-alpha3 | `https://github.com/filebench/filebench/releases/download/1.5-alpha3/filebench-1.5-alpha3.tar.gz` | 2.7 |
| Maven (apt로 설치할 수 없을 때) | `https://archive.apache.org/dist/maven/maven-3/3.9.9/binaries/apache-maven-3.9.9-bin.tar.gz`. 풀어서 `bin/mvn`을 쓴다. | 2.7 |
| (선택) linux-stable git 저장소 | `https://git.kernel.org/pub/scm/linux/kernel/git/stable/linux.git` | 코드 분석 |

tarball로 받은 커널 소스에는 git 이력이 없다. 6.6.8과 7.2.8 사이에 upstream이 무엇을 바꿨는지 `git log -L`이나 `git diff v6.6.8 v7.2.8`로 분석하려면 linux-stable을 따로 clone해야 한다. 환경 구성에는 필요 없다.

#### 8.2.3 경로 변수 (개발 VM)
경로는 자유롭게 정해도 된다. 2장의 명령은 아래 변수를 쓴다.

| 변수 | 2장의 값 | 내용 |
| --- | --- | --- |
| `DELFS` | `~/DeLFS` | 팀 DeLFS 포크 clone. 저장소 최상위 경로이며 그 아래 `DeLFS/`와 구분한다. |
| `KSRC` | `~/linux-7.2.8` | 7.2.8 소스. tarball을 `$(dirname $KSRC)`에 풀면 이 이름이 된다. 디렉터리 이름(`linux-7.2.8`)은 바꾸지 않는다. |
| `BUILD` | `~/delfs-build` | 빌드 출력과 내려받은 파일 |
| `GUIDE` | `$DELFS/env-guide` (= `~/DeLFS/env-guide`) | 팀 포크 안의 이 문서와 실행 스크립트 폴더 |

```bash
export DELFS=~/DeLFS
export GUIDE="$DELFS/env-guide"
export KSRC=~/linux-7.2.8 BUILD=~/delfs-build
```

`GUIDE`는 스크립트의 **복사 원본**을 지정할 때만 쓴다. `DELFS`를 바꾸면 `GUIDE`도 다시 설정한다. 테스트 VM의 실행 스크립트는 이 두 변수를 읽지 않고, `DELFS_REPO` 또는 실행 사용자의 홈 아래 `~/DeLFS`(이전 배치인 `~/fs-work/DeLFS`도 검색)에서 벤치마크 원본을 찾는다. 테스트 VM에서 저장소를 다른 곳에 clone했다면 `sudo FS=f2fs DELFS_REPO=/path/to/DeLFS ~/run-fio-small.sh`처럼 `sudo` 뒤에 명시한다.

#### 8.2.4 테스트 VM의 파일 배치
실행 스크립트는 sudo를 실행한 사용자의 홈(`~`)을 기준으로 아래 위치를 쓴다.

| 테스트 VM 경로 | 내용 |
| --- | --- |
| `~/DeLFS` | 팀 포크 clone(개발 VM과 같은 버전). `script/`의 벤치마크 프로필과 workload 파일을 읽는다. 다른 곳에 두었다면 `DELFS_REPO=<경로>`를 지정한다. |
| `~/bench-lib.sh`, `~/run-fio-small.sh`, `~/run-macro-small.sh`, `~/nvme-loop-up.sh` | 개발 VM의 `$GUIDE/scripts/`에서 함께 복사한 파일 4개. 실제 실행은 이 복사본을 쓴다. |
| `~/f2fs-tools/mkfs.f2fs`, `~/f2fs-tools/fsck.f2fs` | 2.7에서 빌드한 결과 |
| `~/bench/filebench/`, `~/bench/ycsb-rocksdb/` | 2.7에서 빌드한 결과 |
| `~/delfs.ko`, `~/mkfs.delfs` | [3장](#3-선택-delfs-668-실행-테스트)에서 빌드한 결과(선택) |

테스트 VM에도 clone에 포함된 `~/DeLFS/env-guide/`가 있지만, 2.8~2.9의 명령은 홈에 복사한 스크립트를 실행한다. 스크립트는 `SUDO_USER`(없으면 `USER`)의 계정 정보에서 홈을 찾으므로, 평소 계정에서 `sudo ~/run-fio-small.sh`처럼 실행한다. `sudo -i`로 root 셸에 들어가 실행하는 배치는 가정하지 않는다.

### 8.3 VM 준비

#### 8.3.1 테스트 VM 복제
- **새 MAC 주소:** 원본과 같은 MAC이면 두 VM이 같은 네트워크에서 충돌한다.
- **vCPU 4:** 팀 공통 값이다([8.1.1](#811-vcpu-수는-사실상-포맷-파라미터다)).
- **시리얼 출력 파일:** 커널 패닉이 나면 화면과 `dmesg`가 멈춘다. 그 순간의 로그는 호스트의 시리얼 로그 파일에만 남는다. 2.3의 `console=ttyS0,115200`과 짝이다.
- **NVMe 디스크 떼기:** VirtualBox NVMe 디스크가 붙어 있으면 그 디스크가 `nvme0n1`이 되어 nvme-loop 장치와 헷갈린다. 이 디스크는 discard도 안 된다([8.1.2](#812-왜-virtualbox-nvme-디스크가-아니라-nvme-loop인가)).

#### 8.3.2 호스트 전용 네트워크와 DHCP
- GUI에서 DHCP를 켜도 적용되지 않는 경우가 있었다. 그래서 2.2에서는 명령으로 켠다.
- IP 대역은 `list hostonlyifs`에 나오는 그 어댑터의 `IPAddress`에 맞춘다. `.\VBoxManage.exe list dhcpservers`에서 해당 네트워크가 `Enabled: Yes`인지 확인한다.
- DHCP 서버 프로세스는 **그 네트워크에 붙은 VM이 켜질 때** 뜬다. 이미 켜져 있던 VM은 주소를 못 받으므로, 어댑터를 한 번 떼었다가 다시 붙인다.
  ```powershell
  .\VBoxManage.exe controlvm "<VM 이름>" nic2 null
  .\VBoxManage.exe controlvm "<VM 이름>" nic2 hostonly "<IF>"
  ```
- 게스트에서 `enp0s8`에 주소가 아예 없으면 netplan에 DHCP 설정을 추가한다. 고정 IP는 쓰지 않았다.
  ```bash
  printf 'network:\n  version: 2\n  ethernets:\n    enp0s8:\n      dhcp4: true\n' | sudo tee /etc/netplan/60-enp0s8.yaml
  sudo netplan apply
  ```

#### 8.3.3 테스트 VM 첫 부팅 설정
- 복제본은 hostname과 machine-id가 원본과 같다. machine-id가 같으면 DHCP가 같은 IP를 줄 수 있으므로 **가장 먼저** 바꾼다.
- GRUB 메뉴를 보이게 해 두면 새 커널이 부팅되지 않을 때 원래 커널을 고를 수 있다. `console=ttyS0,115200`은 커널 메시지를 시리얼(8.3.1의 로그 파일)로도 보낸다.
- `git`, `nvme-cli`, `fio`, `openjdk-11-jre`는 nvme-loop 스크립트와 벤치마크가 쓴다. DeLFS 저장소는 벤치마크 프로필과 workload 파일 때문에 테스트 VM에도 필요하다([8.2.4](#824-테스트-vm의-파일-배치)).
- Tailscale 같은 VPN은 복제본에서 로그아웃해 두 VM이 같은 노드로 충돌하지 않게 한다.

#### 8.3.4 스냅샷 요령
- **새 커널을 설치하기 전, 모듈을 올리기 전**에 스냅샷을 찍는다. 패닉이 나도 바로 되돌릴 수 있다.
- VM이 켜진 상태에서 찍으면 RAM까지 저장된다. 이 스냅샷으로 복원하면 셸과 모듈 상태까지 그 시점으로 돌아간다. 찍기 전에 `sync`를 하고, 가능하면 시험 중인 파일시스템을 umount한다.
- 스냅샷을 찍을 때마다 differencing 디스크가 한 겹씩 쌓이고, 이후의 쓰기는 맨 끝 겹에 기록된다. 몇 겹 정도는 문제가 없지만 I/O가 조금 느려진다. 성능 수치가 중요해지면 필요 없는 스냅샷을 삭제(병합)하거나, 원하는 시점을 **완전한 복제**로 떼어 내서 스냅샷 없는 VM으로 측정한다.
- 복원하려면 VM을 먼저 꺼야 한다. 이때 "현재 상태를 저장하지 않음"을 고르면 마지막 스냅샷 이후의 변경은 사라진다.

### 8.4 커널 빌드

#### 8.4.1 빌드 도구와 소스 위치
- 7.2.8의 최소 요구는 gcc 8.1, binutils 2.30이다. Ubuntu 20.04의 gcc 9.4와 binutils 2.34로 빌드됐다.
- Ubuntu 20.04의 pahole(`dwarves`)은 1.21이라, 7.2.8이 BTF에 요구하는 1.26보다 낮다. 그래서 `CONFIG_DEBUG_INFO_BTF`를 켜지 않는다. 2.5의 config에서는 디버그 정보 자체가 꺼진다(`DEBUG_INFO_NONE`).
- **커널 소스와 빌드 출력은 VM 내부 디스크에 둔다.** 커널에는 `xt_DSCP.h`와 `xt_dscp.h`처럼 대소문자만 다른 파일이 있다. 그래서 대소문자를 구분하지 않는 호스트 공유 폴더(Windows, macOS APFS)에 두면 소스가 깨진다. [소스]
- 디스크 여유: 7.2.8 빌드 출력은 약 4GB다. 6.6.8까지 함께 빌드하면 두 배가 된다.
- `sha256sum -c`로 tarball이 kernel.org가 공개한 것과 같은지 확인한다.

#### 8.4.2 config
- **소스 트리 밖(`O=`)에서 빌드한다.** 소스 트리에 산출물이 섞이면, 나중에 `diff -r`로 6.6.8과 비교하는 분석이 오염된다.
- config는 **테스트 VM에서 지금 돌고 있는 커널의 config**에서 시작해 `olddefconfig`로 맞춘다. 개발 VM과 테스트 VM은 같은 복제본이므로 2.5처럼 개발 VM의 `/boot/config-$(uname -r)`를 쓰면 된다. 이 문서의 7.2.8-vanilla는 [3장](#3-선택-delfs-668-실행-테스트)의 6.6.8-delfs config에서 시작했다.
- **`SYSTEM_TRUSTED_KEYS`, `SYSTEM_REVOCATION_KEYS`를 비운다.** Ubuntu 기본 커널 config에는 `"debian/canonical-certs.pem"`처럼 Ubuntu 빌드 트리에만 있는 인증서 경로가 들어 있다. 그대로 두면 `No rule to make target 'debian/canonical-certs.pem'`으로 빌드가 실패한다. [실측: Ubuntu 20.04 `5.4.0-216-generic` config에 들어 있음]
- **디버그 정보와 BTF를 끈다.** Ubuntu config는 `DEBUG_INFO`와 `DEBUG_INFO_BTF`가 켜져 있다. 켜 두면 빌드 크기와 시간이 크게 늘고, pahole이 낮으면 BTF 생성이 실패한다.
- 위 설정을 Ubuntu 20.04 기본 config에 적용하고 `olddefconfig`를 돌리면, 인증서 경로는 비고 `DEBUG_INFO_NONE=y`가 되며, F2FS와 nvme-loop 옵션은 2.5의 값으로 나온다. [실측] 이 문서의 x86 환경은 6.6.8-delfs config(인증서 경로가 이미 비어 있음)에서 시작해서 이 문제를 직접 겪지는 않았다.
- `grep` 결과가 2.5의 값과 다르면 `$KSRC/scripts/config --file $O/.config --module F2FS_FS`처럼 고친 뒤 `olddefconfig`를 다시 한다.
- F2FS 하위 옵션: 이 문서의 7.2.8-vanilla config에서는 `F2FS_STAT_FS`, `F2FS_FS_XATTR`, `F2FS_FS_POSIX_ACL`, `F2FS_FS_SECURITY`, `F2FS_IOSTAT`가 `y`였다. 저장소의 `DeLFS/linux-6.6.8/oldconfig`와 같다.
- `LOCALVERSION`에는 **소문자만** 쓴다. Debian 패키지 이름에 대문자가 들어가면 `bindeb-pkg`가 마지막 단계에서 실패한다.

#### 8.4.3 VM 드라이버
배포판 커널 config에서 시작하면 VM 하드웨어용 드라이버는 대부분 이미 켜져 있다. 다만 `localmodconfig` 등으로 config를 줄일 때는 아래 항목이 빠지지 않았는지 확인한다. 빠지면 부팅이나 로그 수집이 안 된다.

| 용도 | x86 (VirtualBox) | 빠지면 |
| --- | --- | --- |
| 부팅 디스크 | `SATA_AHCI` (=y 또는 initramfs에 포함) | `VFS: Unable to mount root fs` 패닉 |
| 네트워크 | `E1000` (VirtualBox 기본 NIC) | `scp` 불가 |
| 시리얼 콘솔 | `SERIAL_8250`, `SERIAL_8250_CONSOLE` | 패닉 로그가 시리얼 파일에 남지 않음 |
| 게스트 통합 | `VBOXGUEST`, `DRM_VBOXVIDEO` | 화면 해상도, 마우스 통합 불편 |
| 테스트 장치 | `NVME_CORE`, `NVME_FABRICS`, `NVME_TARGET`, `NVME_TARGET_LOOP`, `CONFIGFS_FS` | nvme-loop를 만들 수 없음 |

VirtualBox Guest Additions(외부 패키지)는 최신 커널에서 빌드되지 않을 수 있다. 위의 커널 내장 드라이버로 충분하다. [미검증]

#### 8.4.4 빌드와 설치
- 6 vCPU에서 30~40분 걸렸다. 배포판 config라 모듈이 약 4,800개다.
- 빌드 중에는 **테스트 VM을 쉬게 둔다.** 호스트 CPU를 함께 쓰기 때문이다([4장](#4-겪은-문제와-해결-방법)).
- `linux-libc-dev`는 **설치하지 않는다.** 시스템의 사용자 공간 헤더를 바꿔 버린다.
- 설치한 뒤 `ls /boot | grep 7.2.8`에 `vmlinuz`, `initrd.img`, `config`, `System.map`이 보여야 한다.
- `/etc/default/grub`의 `GRUB_DEFAULT`가 특정 커널로 고정돼 있으면 새 커널이 기본으로 부팅되지 않는다. 그때는 2.6처럼 메뉴에서 직접 고른다.
- 빌드가 중간에 끊긴 경우는 [4장](#4-겪은-문제와-해결-방법)의 "그 밖의 문제"를 본다.

### 8.5 nvme-loop

`nvme-loop-up.sh`는 nvme-loop 장치를 만들고 연결한 뒤 상태를 출력한다. configfs 설정은 **재부팅하면 사라지므로** 부팅할 때마다 한 번 실행한다. 여러 번 실행해도 안전하다.
- 백엔드 파일은 `/var/lib/delfs-nvme.img`(20GB)다. 없으면 스크립트가 만든다.
- **백엔드는 반드시 파일이어야 한다.** 블록 장치(예: VirtualBox가 붙인 디스크 `/dev/sdb`, `/dev/nvme0n1`)를 백엔드로 주면, 밑 장치가 discard를 못 할 때 nvmet가 **아무것도 하지 않고 성공을 돌려준다**(`drivers/nvme/target/io-cmd-bdev.c`). discard 시험이 모르는 사이에 무의미해진다. 파일 백엔드는 `fallocate(PUNCH_HOLE)`로 실제로 처리한다(`io-cmd-file.c`). [소스]
- 데이터 보장은 실제 SSD와 같다. 쓰기는 기본이 O_DIRECT이고, flush는 `vfs_fsync`, FUA 쓰기는 `IOCB_DSYNC`로 처리한다. 장치는 자신을 "휘발성 쓰기 캐시가 있는 장치"로 알린다. 그래서 flush한 데이터는 남고 flush하지 않은 데이터는 잃을 수 있다. 체크포인트와 복구 시험에도 쓸 수 있다. [소스]
- `nvme list`에서 nvme-loop 장치는 Model이 `Linux`, FW Rev가 커널 버전 앞 8자(예: `7.2.8-va`)로 보인다.
- `model=Linux`인 장치가 nvme-loop 장치다. **mkfs 전에 꼭 이 값을 확인한다.**
- 벤치마크 스크립트는 `/dev/nvme0n1`을 쓴다. 장치 이름이 다르면 `scripts/bench-lib.sh`의 `DEV=`를 고친다.
- `kato=600`이 아니면 [4장의 keep-alive 항목](#nvme-loop-장치가-갑자기-사라짐-eio)을 본다.
- nvme-loop 장치는 시험 중인 커널 안에서 돈다. 크래시를 분석할 때는 [4장](#4-겪은-문제와-해결-방법)을 함께 본다.

### 8.6 F2FS 도구와 기본 동작 확인

#### 8.6.1 f2fs-tools 1.16.0
- Ubuntu 20.04의 apt 패키지는 2018년에 나온 1.11.0이라 쓰지 않는다.
- `--disable-shared`로 빌드하면 `libuuid`와 `libc`만 필요한 단독 실행 파일이 되므로, 테스트 VM에 복사만 해도 동작한다.

#### 8.6.2 기본 동작 확인
2.8의 명령은 아래를 확인한다.

| 확인 항목 | 근거 |
| --- | --- |
| 모듈 로드와 해제 | `modprobe`와 `rmmod`가 오류 없이 끝남 |
| 디스크 형식 | `mkfs` 성공, 커널 로그에 `Mounted with checkpoint version = …` |
| 영속성 | 다시 마운트해도 `hello`가 나오고, checkpoint 버전이 바뀜 |
| 메타데이터 무결성 | `fsck.f2fs`에 `Fail` 0건 |
| 커널 경고 | 커널 로그에 WARNING, Oops, I/O error 0건 |

이것은 **기본 동작 확인**이다. 부하 상황은 2.9의 벤치마크로 확인한다. `rmmod`는 마운트를 모두 해제한 뒤에만 된다. 마운트가 남아 있으면 `Module f2fs is in use`로 거부되고, 데이터는 안전하다.

### 8.7 축소 벤치마크

저장소의 `script/` 아래 벤치마크를 개인 VM(20GB 장치, 4 vCPU, 8GB RAM)에 맞게 줄여서 실행한다. **성능 수치가 아니라 끝까지 오류 없이 도는지**를 본다.

#### 8.7.1 도구 준비
| 도구 | 준비 | 테스트 VM 경로 |
| --- | --- | --- |
| DeLFS 저장소 | 테스트 VM에도 clone한다(프로필과 workload 파일을 읽는다) | `~/DeLFS` |
| fio | `apt install fio` (테스트 VM, 2.3) | 시스템 |
| filebench 1.5-alpha3 | 개발 VM에서 빌드(2.7) | `~/bench/filebench/` |
| YCSB + RocksDB | 개발 VM에서 빌드(2.7) | `~/bench/ycsb-rocksdb/` |
| Java | `openjdk-11-jre` (테스트 VM, 2.3) | 시스템 |

- **filebench `--prefix`:** **테스트 VM에서 실행할 경로**를 그대로 쓴다. filebench는 분포 라이브러리(`cvar-gamma` 등)를 설치 경로에서 찾는다. 그래서 개발 VM과 테스트 VM의 사용자 이름이 같아야 그대로 복사해서 쓸 수 있다. 다르면 테스트 VM에서 직접 빌드한다.
- **YCSB는 저장소의 소스를 복사해서 빌드한다.** 저장소 안에서 빌드하면 `target/` 디렉터리가 생긴다.
- **YCSB 실행기:** `bin/ycsb`는 python2 스크립트다. 스크립트는 `python2`가 있으면 그것을 쓰고, 없으면(Ubuntu 24.04 등) 같은 배포본의 bash 실행기 `bin/ycsb.sh`를 쓴다. 두 방식 모두 load와 run이 동작하는 것을 확인했다.

#### 8.7.2 스크립트
저장소 최상위의 [`script/`](../script/)에는 원본 벤치마크 job·프로필·workload와 YCSB 소스가 있고, [`env-guide/scripts/`](scripts/)에는 개인 VM용 실행 스크립트가 있다. 개발 VM에서는 `$DELFS/script/RocksDB`를 `$BUILD/ycsb-src`로 복사해 YCSB를 빌드하고(2.7), 테스트 VM에서는 홈에 복사한 실행 스크립트가 `DELFS_REPO/script/`의 입력 파일을 읽는다. 이 실행 스크립트들이 `script/`의 기존 실행용 셸 스크립트를 호출하는 것은 아니다.

| 테스트 VM의 실행 스크립트 | 읽는 원본 | 축소 적용 위치 |
| --- | --- | --- |
| `~/run-fio-small.sh` | [`$DELFS_REPO/script/fio/*.fio`](../script/fio/) 중 12개 | `~/fio-small/…/`에 job 사본을 만들고 jobs·size를 줄인다. |
| `~/run-macro-small.sh filebench` | [`$DELFS_REPO/script/filebench/my*.f`](../script/filebench/) 5개 | `~/macro-small/…/`에 프로필 사본을 만들고 파일 크기를 줄인다. |
| `~/run-macro-small.sh ycsb` | [`$DELFS_REPO/script/RocksDB/workloads/myworkload{a,b,f,u}`](../script/RocksDB/workloads/) | 원본 workload를 읽고 실행 인자로 레코드·연산 수를 줄인다. 실행기는 `~/bench/ycsb-rocksdb/`를 쓴다. |

저장소의 원본 입력 파일은 수정하지 않는다. 아래 표는 네 스크립트의 역할이다.

| 스크립트 | 내용 |
| --- | --- |
| `bench-lib.sh` | 공통 부분: 파일시스템 선택(`FS=f2fs`/`FS=delfs`), 저장소 위치 찾기(`~/DeLFS`, 다르면 `DELFS_REPO`), 사전 점검, 커널 로그 판정 |
| `run-fio-small.sh` | `script/fio`의 job 12개. 시작할 때 mkfs를 한 번 하고, job마다 load → mount → fio → umount → unload |
| `run-macro-small.sh` | `script/filebench` 5개와 `script/RocksDB` YCSB 4개. 케이스마다 mkfs부터 새로 시작 |
| `nvme-loop-up.sh` | [8.5](#85-nvme-loop) 참고 |

```bash
# 일부만: sudo FS=f2fs ~/run-macro-small.sh filebench myvarmail
# 저장소 위치가 다르면: sudo FS=f2fs DELFS_REPO=/path/to/DeLFS ~/run-fio-small.sh
```
- 결과는 `~/fio-small/<시각>-<FS>-<커널>/`와 `~/macro-small/<시각>-<종류>-<FS>-<커널>/`에 남는다.
- 스크립트는 시작할 때 `/dev/nvme0n1`의 model이 `Linux`인지, `/mnt/test`가 비어 있는지 확인한다. 조건이 맞지 않으면 아무것도 지우지 않고 멈춘다.
- filebench는 ASLR이 켜져 있으면 실패할 수 있다. 저장소의 `DeLFS/test.sh`처럼 실행하는 동안만 끄고, 끝나면 되돌린다. 스크립트가 처리한다.

#### 8.7.3 판정 기준
- **통과:** fio는 `fio_rc=0`이고 `err=0`, filebench와 YCSB는 `ok=yes`. 그리고 셋 다 `kerr=0`이어야 한다.
- `kerr`는 커널 로그의 WARNING, Oops, BUG, I/O error, keep-alive 오류 수다.
- `lockup`은 soft lockup 경고 수다. Hyper-V 환경 때문에 생기므로 따로 세서 기록만 한다([4장](#4-겪은-문제와-해결-방법)).
- `kerr`가 0이 아니면 케이스 디렉터리의 `.dmesg` 파일을 연다. hung task 경고만 있고 그 원인이 Hyper-V 멈춤이면 [4장](#4-겪은-문제와-해결-방법)의 기준으로 판단한다. 그 밖의 경고(WARNING, Oops, I/O error 등)는 실패로 본다.

#### 8.7.4 축소 기준
| 벤치마크 | 원본 | 축소 | 이유 |
| --- | --- | --- | --- |
| fio 기본 7개 | 128 jobs × 2200M | 4 jobs × 1G | 장치 20GB, vCPU 4 |
| fio 25/50/75-write | 128 × 3727/7455/11182M | 4 × 1280/2560/3840M | 원본 총량이 원본 장치(약 1.82TiB)의 25/50/75%라서, 20GB 장치에서도 **같은 사용률**이 되게 맞췄다 |
| fio table6-90/99 | 128 × 1450/145M | 4 × 500/50M | 같은 비율 |
| fio raw-* 4개 | 원저자의 원시 장치(`/dev/nvme6n1p1` 등) | **제외** | 파일시스템이 아니라 장치 자체를 시험한다. 쓰기 job은 포맷을 덮어쓴다. |
| filebench fileserver | 12,800개 × 평균 10MiB | 평균 **400KiB** | 파일 개수와 스레드 수는 그대로 두었다. 파일 개수를 줄이면 빈자리가 스레드보다 적어져서 `could not obtain a file`로 바로 멈춘다. |
| filebench varmail, webserver | 25,600개 × 평균 10MiB | 평균 200KiB | 같은 이유 |
| filebench videoserver | 64개 × 10g | 128m | |
| filebench oltp | 128개 × 1000m | 40m | |
| YCSB A/B/U | 레코드 1,000만, 연산 128만 | 200만 / 25.6만 | 레코드와 연산을 같은 비율로 줄였다 |
| YCSB F | 레코드 256만(40KB), 연산 128만 | 7.5만 / 3.75만 | 데이터 약 3GB |

- filebench의 실행 시간(60초)과 스레드 수, YCSB의 스레드 수(32)는 원본 그대로다.
- **YCSB 주의:** 저장소의 `script/RocksDB/workloads/myworkloada`, `myworkloadb`, `myworkloadu`에는 `fieldlength`가 `fieldlenght`로 오타가 나 있다. YCSB는 모르는 키를 무시하므로 A, B, U는 실제로 **기본값인 100B 필드(레코드 1KB)**로 돈다. F만 4KB 필드다. 기준선이므로 오타는 고치지 않았다.
