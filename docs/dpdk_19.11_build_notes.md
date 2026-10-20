# DPDK 19.11 编译与运行笔记（CentOS 7 / kernel 3.10 / 无 root 环境）

> 环境：64 核 Xeon Gold 6226R，内核 3.10.0-1160.el7，gcc 11.2.0，
> Python 3.8.2，无 root 权限。目标：编译 DPDK 19.11 LTS，跑通 helloworld
> 和 pcap 虚拟网卡版 testpmd。

## 最终成果

- DPDK 19.11.14 编译成功（legacy make 构建系统）
- `helloworld` 运行成功（EAL 初始化、多核 lcore 启动正常）
- `testpmd` + pcap vdev 运行成功，回放的 200 个 UDP 包全部收到
  （`RX-packets: 200, RX-bytes: 12800`）

## 目录结构

```
~/dpdk/
├── dpdk-stable-19.11.14/     # DPDK 源码及构建（build/ 子目录）
├── deps/                     # 自组装的 libpcap 开发包
│   ├── include/pcap/         # 头文件（来自 libpcap-1.10.1 源码树）
│   └── lib/libpcap.so -> /usr/lib64/libpcap.so.1.9.1
├── gen_pcap.py               # pcap 样本生成器（UDP 包）
└── sample.pcap               # 200 个 64B UDP 包
```

## 构建步骤

```bash
# 1. 下载源码
mkdir -p ~/dpdk && cd ~/dpdk
curl -sLO https://fast.dpdk.org/rel/dpdk-19.11.14.tar.xz
tar xf dpdk-19.11.14.tar.xz

# 2. 自组装 libpcap 开发包（见问题 1）
mkdir -p deps/include deps/lib
cp -r libpcap-1.10.1/pcap deps/include/      # 头文件
cp libpcap-1.10.1/pcap.h deps/include/
ln -sf /usr/lib64/libpcap.so.1.9.1 deps/lib/libpcap.so
ln -sf /usr/lib64/libpcap.so.1.9.1 deps/lib/libpcap.so.1

# 3. 配置（legacy make 系统，19.11 仍支持；meson 本机没有）
cd dpdk-stable-19.11.14
make config T=x86_64-native-linuxapp-gcc O=build

# 4. 修改 build/.config（各项原因见问题清单）
sed -i \
  -e 's/^CONFIG_RTE_LIBRTE_PMD_PCAP=n/CONFIG_RTE_LIBRTE_PMD_PCAP=y/' \
  -e 's/^CONFIG_RTE_EAL_NUMA_AWARE_HUGEPAGES=y/CONFIG_RTE_EAL_NUMA_AWARE_HUGEPAGES=n/' \
  -e 's/^CONFIG_RTE_EAL_IGB_UIO=y/CONFIG_RTE_EAL_IGB_UIO=n/' \
  -e 's/^CONFIG_RTE_KNI_KMOD=y/CONFIG_RTE_KNI_KMOD=n/' \
  -e 's/^CONFIG_RTE_LIBRTE_VHOST_NUMA=y/CONFIG_RTE_LIBRTE_VHOST_NUMA=n/' \
  build/.config

# 5. 编译（-Wno-error 的原因见问题 3）
make -j32 O=build \
  EXTRA_CFLAGS="-I$HOME/dpdk/deps/include -Wno-error" \
  EXTRA_LDFLAGS="-L$HOME/dpdk/deps/lib"

# 6. 编译 helloworld 示例
cd examples/helloworld
make RTE_SDK=$HOME/dpdk/dpdk-stable-19.11.14 RTE_TARGET=build \
  EXTRA_CFLAGS="-Wno-error" EXTRA_LDFLAGS="-L$HOME/dpdk/deps/lib"
```

## 运行

```bash
# helloworld（--no-huge：无大页、无 root 的降级模式）
./build/helloworld --no-huge -l 0-1
# 输出：hello from core 1 / hello from core 0

# 生成回放样本
python3 ~/dpdk/gen_pcap.py ~/dpdk/sample.pcap 200

# testpmd + pcap 虚拟网卡（非交互模式，自动开始收包，每 2 秒打印统计）
timeout 8 ./build/app/testpmd \
  --no-huge -m 2048 -l 0-2 \
  --vdev 'net_pcap0,rx_pcap=/home/dev/dpdk/sample.pcap,tx_pcap=/home/dev/dpdk/out.pcap' \
  -- -a --forward-mode=rxonly --stats-period=2
# NIC statistics: RX-packets: 200  RX-bytes: 12800  ← 全部收到
```

## 遇到的问题与解决方法

### 问题 1：libpcap-devel 未安装，且无 root 权限

- **现象**：pcap PMD 需要 libpcap 头文件和链接库，系统只有运行时
  （`/usr/lib64/libpcap.so.1.9.1`），没有头文件
- **尝试过的弯路**：从源码编译 libpcap-1.10.1 → configure 失败：
  `Neither flex nor lex was found`（过滤表达式语法需要 flex/bison，
  又绕回 root 权限问题）
- **解决**：libpcap 的公共 API 几十年没变，**头文件从源码树拷贝 +
  直接软链系统运行时库**，组装出一个本地"开发包"：
  ```bash
  cp -r libpcap-1.10.1/pcap deps/include/
  ln -sf /usr/lib64/libpcap.so.1.9.1 deps/lib/libpcap.so
  ```

  编译时 `-I deps/include`，链接时 `-L deps/lib -lpcap`，运行时
  由系统 loader 加载 `libpcap.so.1`。用 5 行 C 程序验证链接可行。

### 问题 2：无 root 无法在 eth0 上实时抓包

- **现象**：`--vdev 'net_pcap0,iface=eth0'` 实时抓包需要
  CAP_NET_RAW 权限
- **解决**：改用 pcap PMD 的**文件回放模式**
  `rx_pcap=file.pcap`（离线读取，无需权限），样本文件用 30 行
  Python 脚本生成（`gen_pcap.py`，构造 Ethernet/IPv4/UDP 帧）

### 问题 3：gcc 11 编译 2019 年的代码

- **现象**：DPDK 19.11 发布时最新 gcc 是 9.x，gcc 11 新增了大量
  警告检查，而 DPDK 默认 `-Werror`（警告即错误）
- **解决**：`EXTRA_CFLAGS="-Wno-error"` 追加在命令行末尾，
  覆盖前置的 `-Werror`。实测 gcc 11 + 19.11.14 仅有警告，无真实错误

### 问题 4：`numaif.h: No such file or directory`（iotlb.c）

- **现象**：librte_vhost 编译失败，numactl-devel 未安装
- **分析**：已关闭 `CONFIG_RTE_EAL_NUMA_AWARE_HUGEPAGES`（EAL 的
  numaif.h 引用受它保护），但 vhost 的 numaif.h 引用由另一个开关
  `CONFIG_RTE_LIBRTE_VHOST_NUMA` 控制
- **解决**：`CONFIG_RTE_LIBRTE_VHOST_NUMA=n`。vhost 的 NUMA 感知
  是优化项，关掉不影响功能

### 问题 5：内核模块（igb_uio / KNI）

- **现象**：make 构建默认会编译内核模块，需要 kernel-devel 头文件，
  且 3.10 内核与新版模块代码可能不兼容
- **解决**：本次学习只用用户态虚拟 PMD，不需要任何内核模块：
  `CONFIG_RTE_EAL_IGB_UIO=n`、`CONFIG_RTE_KNI_KMOD=n`

### 问题 6：testpmd 参数错误

- **现象**：`--rxonly` 不是合法参数，testpmd 打印用法后退出
- **解决**：正确写法是 `--forward-mode=rxonly`（或交互模式下
  `set fwd rxonly`）

### 问题 7：`--no-huge` 模式 mbuf 池创建失败

- **现象**：`Creation of mbuf pool for socket 0 failed: Cannot allocate memory`（以及后续的 socket 1 同样失败）
- **分析**：no-huge 模式下 EAL 使用静态内存布局，默认预留量很小；
  testpmd 默认池子 163456 × 2176B ≈ 340MB，**双 NUMA 节点各建一个**，
  共需 ~700MB，超出默认预留
- **解决**：`-m 2048` 显式预留 2GB。报错里的 `mlockall() failed`
  是无 root 时的正常警告，可忽略

### 问题 8：交互模式管道喂命令收不到包

- **现象**：`printf 'start\nshow port stats all\n...' | testpmd -i`
  统计全为 0——管道命令瞬时执行完，转发核心还没开始收包
- **解决**：改用非交互模式 `--auto-start` + `--stats-period=2`，
  让 testpmd 自己跑几秒再自动打印统计
- **注意**：`Forward statistics`（转发引擎计数）与 `NIC statistics`
  （PMD 端口计数）是两组计数器；pcap 文件瞬间读完，确认收包以
  NIC statistics 为准

## 备注

- 本机其实有两块 Intel X710（i40e）物理网卡（`0000:d8:00.0/.1`），
  后续要玩真实网卡需要：升级内核或换台机器（vfio-pci 在 3.10 上太老）、
  配置大页、root 权限绑卡。那是"第 3 步"的事
- 19.11 是最后支持 3.10 内核的 LTS；学习 API 够用，生产勿用（已 EOL）
- 要进阶读源码的话，推荐顺序：`lib/ring`（对照本项目的 ringbuf.hpp）→
  `lib/mbuf` + `lib/mempool` → `drivers/net/pcap`（最简单的 PMD 实现）
