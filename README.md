# quastation-armbian

## 1. プロジェクト概要

KDDI Qua Station（Realtek RTD1295 / ARM64）向けの Linux 7.3.4 メインラインベースカーネル、デバイスツリー、アウトオブツリーモジュール、および Armbian（Debian 13 / Trixie）rootfs・USB ADB 導入ツールを提供するリポジトリです。

純正 Realtek ブートチェーンおよび個体固有の factory 環境を保持したまま、eMMC のルートパーティションに Armbian を導入・運用する構成をとります。汎用 Armbian イメージ、GRUB、`armbian-install` によるブート構成の置き換えは行いません。

主な構成要素:

- `kernel/`・`patches/`・`dts/rtd1295-quastation.dts`: Linux 7.3.4 向けパッチ群、Qua Station 固有ドライバー、ボード DTS
- `modules/`: Mali-T820 GPU 初期化、VE1 ハードウェア動画エンジン、ION メモリアロケーター
- `image/`: RAM メンテナンス initramfs（`QuaMaintenance` USB ADB）、個体別バンドル生成、USB 経由インストーラー、ブートスロットライター
- `media/qua-transcode/`: MPEG-2 → H.264 ハードウェアトランスコードパイプラインおよび FFmpeg 4.4.5 NV12 パッチ

## 2. 適応状況

| 領域 | 状態 | 概要・対象コンポーネント |
| --- | --- | --- |
| CPU / クロック / 割り込み / サーマル | 対応済み | `clk-quastation*`、`irq-quastation`、`rtd129x-quastation-thermal` |
| GPIO / LED | 対応済み | `qua-iso-gpio`、`gpio-leds` |
| eMMC / SD / SATA | 対応済み | `rtkemmc-quastation`、`rtk-sdmmc`、`ahci-quastation` |
| USB ホスト / USB ADB ガジェット | 対応済み | DWC3、`kddi,quastation-ehci`、DWC2 ペリフェラル + `qua-usb-adbd` |
| PCIe / Wi-Fi | 対応済み | `pcie-rtd129x-slot1` / `slot2`、Realtek 無線ファームウェア（`firmware/rtlwifi/`） |
| GPU / 動画エンジン / トランスコード | 対応済み（制約あり） | `qua-gpu`（Mali-T820 / Panfrost）、`video/ve1`、`video/ion`、`media/qua-transcode/` |
| RAM メンテナンス / USB 導入経路 | 検証中 | `QuaMaintenance` initramfs と `install-over-usb.sh` は工場出荷実機での E2E 受け入れ検証を継続中（詳細は [image/README.md](image/README.md)） |

- `media/vendor-openmax/lib/` の Realtek OpenMAX ランタイムは再配布条件が不明なためリポジトリには同梱せず、ビルド時に外部取得またはローカル配置します。
- 動画パイプラインおよび `ffmpeg-omx` の既知制約（1 フィールド補間による 29.97fps 出力等）は [media/qua-transcode/README.md](media/qua-transcode/README.md) を参照してください。

## 3. 推奨インストール方法とパーティション構成

### eMMC パーティション・ブートスロット構成

対象 eMMC（15,269,888 セクター、512 バイト/セクター）は `image/quastation-layout.mbr` に従い構成されます。先頭の純正ブートコードと factory 環境は保持し、ディスク全体の `dd` イメージ書き換えは行いません。

| 領域 | 開始 LBA | セクター数 | 用途 |
| --- | ---: | ---: | --- |
| ファームウェア領域 | 0 | 196608 | 純正ブートコードと factory データを保持 |
| `p1` | 196608 | 32768 | ファイルシステムを作らない起動用 raw 領域 |
| `p2` | 229376 | 13443072 | Btrfs `QUA_ROOT`（ルートサブボリューム `@`、`compress=zstd:3`） |
| `p3` | — | — | エントリーを削除し、旧領域を `p2` に統合 |
| `p4` | 13672448 | 1269760 | 既存の拡張パーティションと論理領域を保持 |

raw 起動スロットには本リポジトリのスロットライター（`image/write-boot-slots.py`）で `Image-legacy-uboot.gz`（`text_offset=0x20000000` 調整済み）を書き込みます。

| スロット | 開始 LBA | セクター数 |
| --- | ---: | ---: |
| カーネル | 196608 | 12288 |
| レスキュー引き継ぎ | 210944 | 512 |
| DTB | 212736 | 31 |
| メンテナンス initrd | 212992 | 15534 |

### 推奨導入手順

1. **Qua rescue U-Boot を導入する（対象機の既存 Linux 上で実行）**

   [qua-rescue-uboot](https://github.com/makeding/qua-rescue-uboot/) の [setup.md](https://github.com/makeding/qua-rescue-uboot/blob/qua-rescue/setup.md) に従いステージングと有効化を行います。

   ```sh
   sudo python3 install-usb-chain.py . stage
   sudo python3 install-usb-chain.py . arm
   ```

   独立した USB 2.0 Type-A ポートへ接続し、CDC ACM シリアルコンソール（例: macOS `/dev/cu.usbmodemQuaRescue1`、115200 baud）から RAM メンテナンス環境を起動します。起動後はシリアルを閉じ、ホストの `adb devices -l` で `QuaMaintenance` が `device` として認識されることを確認します。

2. **eMMC バックアップと個体別バンドルの生成**

   ```sh
   adb -s QuaMaintenance shell 'cat /sys/class/block/mmcblk0/device/cid' > cid.txt
   set -o pipefail
   adb -s QuaMaintenance exec-out 'dd if=/dev/mmcblk0 bs=1048576 2>/dev/null' | gzip -1 > factory-emmc.img.gz
   python3 RELEASE_DIR/prepare-target-bundle.py factory-emmc.img.gz "$(cat cid.txt)" RELEASE_DIR TARGET_BUNDLE
   ```

   生成された `TARGET_BUNDLE` には個体固有の factory 環境・MAC アドレスが含まれるため公開・流用しないでください。

3. **Armbian rootfs の転送・インストール**

   ```sh
   bash image/install-over-usb.sh RELEASE_DIR TARGET_BUNDLE \
     QuaStation-Armbian-trixie-arm64-rootfs.tar.zst authorized_keys wpa.conf \
     QuaStation-maintenance-stage2.tar.gz
   ```

   完了後に再起動し、`uname -r`、`findmnt /`（`/dev/mmcblk0p2[/@]`）、`systemctl status wpa_supplicant@wlan0 ssh`、および USB ADB を確認します。

4. **（任意）HuaStation の導入と導入後のカーネル更新**

   Armbian 起動後、対象機上で以下を実行すると `huastation` パッケージ（SHA256 検証付き）が導入されます。

   ```sh
   sudo bash image/install-huastation.sh
   ```

   導入後のカーネル更新は Qua 上で `sudo apt install ./linux-image-*_arm64.deb` を実行します。パッケージの `postinst` がスロットライターを呼び出し、再起動後に新しいカーネルへ切り替わります。

## 4. 関連リソース

- [qua-rescue-uboot](https://github.com/makeding/qua-rescue-uboot/)（[setup.md](https://github.com/makeding/qua-rescue-uboot/blob/qua-rescue/setup.md)）: RAM レスキュー U-Boot と USB ブートチェーン
- [huastation_0.1.0_arm64.deb](https://git.huggy.moe/qua/huastation/releases/download/0.1.0/huastation_0.1.0_arm64.deb): Qua Station 管理パッケージ
- [image/README.md](image/README.md): RAM メンテナンスシステムおよび USB インストーラーの詳細仕様
- [media/qua-transcode/README.md](media/qua-transcode/README.md): ハードウェアトランスコードと FFmpeg パッチのビルド・検証手順
- [firmware/README.md](firmware/README.md) / [modules/README.md](modules/README.md): ファームウェアおよびカーネル外モジュール情報

## 5. ライセンス

本プロジェクトは複数のライセンスを持つコンポーネント（Linux カーネル・GPL-2.0 系ドライバー、LGPL-2.1+ メディアツール、Khronos MIT ヘッダー、Realtek ファームウェア・vendor バイナリ、Armbian / Debian パッケージ等）を組み合わせた混合ライセンス構成です。

詳細は [LICENSE](LICENSE) および各ファイル内の SPDX 識別子・著作権表示を参照してください。
