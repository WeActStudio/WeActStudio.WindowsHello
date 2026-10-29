[English](README.md) · **简体中文**

# vendor/ —— 仓库自带的 libfprint 源码

`libfprint-1.94.100.tar.gz`（约 12 MB）是**未经修改的上游 libfprint 1.94.100 源码树**，  
打成包放进仓库，目的只有一个：让 clone 下来就能**完全离线**编译安装。

这是第三方代码，不是本项目的代码：libfprint 版权属其各作者，采用 **LGPL-2.1-or-later**  
（完整许可证在包内 `libfprint-1.94.100/COPYING`）。本项目自身代码使用同一许可证，  
这也正是驱动能被编进库里的前提。

## 为什么必须把源码一起带上

libfprint **没有外部插件机制**：驱动只有被编进 `libfprint-2.so.2` 才存在。  
所以 `scripts/install.sh` 需要的是一份 libfprint 源码树，而不是运行时安装包。  
把源码打包进来之后，安装就变成：

```bash
git clone <本仓库>
cd moc5159-libfprint-driver
sudo ./scripts/install.sh          # 不需要联网
```

## 不想占这 12 MB 的话

| 做法                  | 命令                                                                 |
| ------------------- | ------------------------------------------------------------------ |
| 用你自己的 libfprint 源码树 | `sudo ./scripts/install.sh --libfprint-src /path/to/libfprint`     |
| 删掉包，让脚本自己 clone 上游  | `rm vendor/libfprint-1.94.100.tar.gz && sudo ./scripts/install.sh` |
| 以后再补回来              | `./scripts/fetch-libfprint.sh`                                     |

除本包之外，其它途径都是从上游 `v1.94.100` 标签下载。

## 编译时会打一个补丁

上游 1.94.100 的 `tests/meson.build` 里有一处对 dict 的 `foreach`，meson ≥ 1.10 会直接报  
`Foreach expects exactly 2 variables`。本目录里的包**已经修好**，`scripts/install.sh`  
也会对你指定的任何源码树再检查并修补一次，新旧 meson 都能编。

## 为什么锁定 1.94.100

驱动是按 libfprint **1.94 系列的驱动 API** 写的。上游 1.95 及以后调整过  
`FpDevice` / `FpiSsm` 的部分接口，直接编可能报错。锁定版本同时也让"能编译"  
这件事可复现，不受发行版打包差异影响。
