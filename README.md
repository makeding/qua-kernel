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

### 推奨導入手順（ベストプラクティス）

1. **Qua rescue U-Boot（v4 以降）を導入する**

   [qua-rescue-uboot](https://github.com/makeding/qua-rescue-uboot/releases) の [setup.md](https://github.com/makeding/qua-rescue-uboot/blob/qua-rescue/setup.md) に従いステージングと有効化を行います（v4 以降は CRC チェックがなく、`usbrecv` による高速転送に対応しています）。

   ```sh
   sudo python3 install-usb-chain.py . stage
   sudo python3 install-usb-chain.py . arm
   ```

2. **RAM メンテナンス環境（`QuaMaintenance`）を起動する**

   再起動時に USB 2.0 ポートへ接続した状態で CDC ACM コンソール（例: macOS `/dev/cu.usbmodemQuaRescue1`）を開き、`usbrecv` で `QuaStation-maintenance-initramfs.cpio.xz`（約 5 秒で転送完了）を `0x28000000` へロードして起動します。

   ```text
   usbrecv 0x28000000 <hex_size>
   fdt addr 0x23000000
   fdt resize
   fdt set /chosen linux,initrd-start <0x28000000>
   fdt set /chosen linux,initrd-end <0x28xxxxxx>
   boot
   ```

   起動後は `adb devices -l` に `QuaMaintenance` が表示されます。

3. **eMMC バックアップと Armbian rootfs インストール**

   ```sh
   adb -s QuaMaintenance shell 'cat /sys/class/block/mmcblk0/device/cid' > cid.txt
   set -o pipefail
   adb -s QuaMaintenance exec-out 'dd if=/dev/mmcblk0 bs=1048576 2>/dev/null' | gzip -1 > factory-emmc.img.gz
   python3 RELEASE_DIR/prepare-target-bundle.py factory-emmc.img.gz "$(cat cid.txt)" RELEASE_DIR TARGET_BUNDLE
   bash image/install-over-usb.sh RELEASE_DIR TARGET_BUNDLE \
     QuaStation-Armbian-trixie-arm64-rootfs.tar.zst authorized_keys wpa.conf \
     QuaStation-maintenance-stage2.tar.gz
   ```

   再起動後は USB ADB（`QuaStation`）、Wi-Fi（`wlan0`）、SSH、および `huastation` が自動で立ち上がります（`wpa.conf` を渡さない場合、`huastation` が本体底面ラベルの初期 SSID / パスワードで AP モードを起動します）。

4. **導入後のカーネル更新・HuaStation 更新**

   `huastation` は Armbian rootfs に同梱済みです。再導入や更新を行う場合は `/usr/local/sbin/install-huastation`（または `sudo bash image/install-huastation.sh`）を実行してください。

   カーネル更新時は USB ADB 経由で `.deb` を送り、`sudo apt install ./linux-image-*_arm64.deb` を実行します（`adb push` + `apt install` が最速で、`postinst` が自動でスロットを書き換えます）。macOS で `adb devices` が `offline` になった場合は `adb -s QuaStation reconnect offline` を実行してください。

## 4. 関連リソース

- [qua-rescue-uboot](https://github.com/makeding/qua-rescue-uboot/)（[setup.md](https://github.com/makeding/qua-rescue-uboot/blob/qua-rescue/setup.md)）: RAM レスキュー U-Boot と USB ブートチェーン
- [huastation](https://git.huggy.moe/qua/huastation): Qua Station 管理パッケージ
- [image/README.md](image/README.md): RAM メンテナンスシステムおよび USB インストーラーの詳細仕様
- [media/qua-transcode/README.md](media/qua-transcode/README.md): ハードウェアトランスコードと FFmpeg パッチのビルド・検証手順
- [firmware/README.md](firmware/README.md) / [modules/README.md](modules/README.md): ファームウェアおよびカーネル外モジュール情報

## 5. ライセンス

本プロジェクトは複数のライセンスを持つコンポーネント（Linux カーネル・GPL-2.0 系ドライバー、LGPL-2.1+ メディアツール、Khronos MIT ヘッダー、Realtek ファームウェア・vendor バイナリ、Armbian / Debian パッケージ等）を組み合わせた混合ライセンス構成です。

詳細は [LICENSE](LICENSE) および各ファイル内の SPDX 識別子・著作権表示を参照してください。
