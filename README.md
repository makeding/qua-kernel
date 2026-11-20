# Qua Station / Armbian Debian 13 インストール手順

## 手動ビルドと Release への添付

Actions の **quastation-armbian Debian 13 release (manual)** → **Run workflow** で、
既存のカーネル Release タグを `kernel_tag` に指定します。
push・タグ作成・pull request ではこの workflow は起動しません。
公式 Armbian Build Framework の固定コミットから Trixie arm64 rootfs を作成し、
指定 Release のカーネル、ネイティブビルドした media、Qua 設定を組み込みます。
Realtek ライブラリは元リポジトリの固定コミットから取得し、既存の SHA256 一覧で照合します。

同じ Release に rootfs、`QuaStation-install-tools.tar.gz`、`INSTALL.ja.md`、
`SHA256SUMS-armbian` を添付します。既存アセットの上書きは行いません。
ビルドログは Actions、完成品は Actions artifact と Release に保存します。
HuaStation 導入コマンドは rootfs 内にも `sudo install-huastation` として配置します。
カーネルパッケージの `Depends` を読み取り、APT で依存関係を解決・インストールしてから、
カーネルを dpkg に登録・展開した状態で梱包します。実機の初回 APT 処理で
configure が実行され、postinst が実機の起動スロットを更新します。

## ストレージ構成と起動方式

配布対象は arm64 のルートファイルシステムです。ディスク全体のイメージではありません。
純正 Realtek ブートチェーンと個体固有の factory 環境を維持します。
汎用 Armbian イメージ、GRUB、`armbian-install` で起動構成を置き換えないでください。

以下はローカル `qua/porting/armbian/install-device-storage.sh` と
`INSTALL-STATE.md` に記録された実機導入時の構成です。
セクターサイズは 512 バイト、対象 eMMC は 15,269,888 セクターです。
`quastation-layout.mbr` の SHA256 は、その導入記録と一致します。

| 領域 | 開始 LBA | セクター数 | 用途 |
| --- | ---: | ---: | --- |
| ファームウェア領域 | 0 | 196608 | 純正ブートコードと factory データを保持 |
| p1 | 196608 | 32768 | ファイルシステムを作らない起動用 raw 領域 |
| p2 | 229376 | 13443072 | Btrfs `QUA_ROOT`、ルートサブボリューム `@` |
| p3 | — | — | エントリーを削除し、旧領域を p2 に統合 |
| p4 | 13672448 | 1269760 | 既存の拡張パーティションと論理領域を保持 |

インストーラーは工場出荷時のパーティション 1〜3 を再構成します。
カーネルは LBA 196608 から 12288 セクター、DTB は 212736 から 31 セクター、
メンテナンス initrd は 212992 から 15534 セクターを使用します。
個体別レスキュー引き継ぎ領域は LBA 210944〜211455 です。
書き込みには本リポジトリのスロットライターを使用します。
`Image-legacy-uboot.gz` は Qua の旧 U-Boot 向けにヘッダーを調整しています
（`text_offset=0x20000000`）。汎用 ARM64 Image をそのまま書き込まないでください。

## 1. 最初に Qua rescue U-Boot を導入する

[qua-rescue-uboot](https://github.com/makeding/qua-rescue-uboot/) の
[setup.md](https://github.com/makeding/qua-rescue-uboot/blob/qua-rescue/setup.md)
に従います。**パーティション変更や Armbian 導入より先に、対象機の既存 Linux 上で実行します。**

```sh
sudo python3 install-usb-chain.py . stage
sudo python3 install-usb-chain.py . arm
```

v1 インストーラーは `arm` 時に純正 U-Boot 環境の `bootdelay` を `2` に設定し、
両方の factory 環境バンクへ書き込んで読み戻します。
別途 `setenv` / `saveenv` を実行する必要はありません。
実行結果の `original_delay_seconds: 2` は純正側、
`new_delay_seconds: 3` は RAM レスキュー側の操作受付時間です。
電源投入から Linux 起動までの総時間ではありません。

バックアップを保管して再起動します。独立した USB 2.0 Type-A ポートに接続し、
CDC ACM コンソールで `version` と `boot` を確認します。
macOS の例は `/dev/cu.usbmodemQuaRescue1`、115200 baud です。
これは U-Boot のシリアルコンソールであり、**USB ADB ではありません**。
純正 ARM32 U-Boot → RAM レスキュー U-Boot → 純正 ARM64 U-Boot → Linux の順で起動し、
保存済みの純正ブートローダーバイナリは置き換えません。

## 2. Qua 用 Armbian を導入する

### USB 接続の切り替わり

同じ USB ケーブルでも、起動段階によって PC 側の接続方法が変わります。

| 起動段階 | PC 側の表示 | 用途 |
| --- | --- | --- |
| rescue U-Boot | CDC ACM シリアル | 起動操作・コンソール |
| RAM メンテナンス Linux | ADB `QuaMaintenance` | インストールファイル転送 |

RAM メンテナンス起動には、対応するカーネル・DTB・initramfs をロードし、
その initramfs を使う起動設定で Linux へ引き継ぎます。
通常起動の設定のまま `boot` しても、メンテナンス起動への切り替えにはなりません。
Linux が起動したらシリアル接続を閉じ、ADB の `QuaMaintenance` を確認します。

ホストの Android platform-tools を使用します。

```sh
adb devices -l
adb -s QuaMaintenance get-state
```

`QuaMaintenance` が `device` として表示された後、下記のバックアップと
個体別バンドルの準備を済ませ、ホストから rootfs を転送・インストールします。

```sh
bash image/install-over-usb.sh RELEASE_DIR TARGET_BUNDLE \
  QuaStation-Armbian-trixie-arm64-rootfs.tar.zst authorized_keys wpa.conf \
  QuaStation-maintenance-stage2.tar.gz
```

Armbian rootfs 全体を `/run` に置く必要はありません。下記の
`install-over-usb.sh` は stage2・個体別バンドル・認証情報を先に転送し、
圧縮 rootfs は ADB の標準入力から eMMC のマウント先へ直接ストリーム展開します。
USB シリアルへ rootfs を送信したり、ADB を TCP に切り替えたりする方式ではありません。

### バックアップとインストール

1. 対応する Qua RAM メンテナンス環境を起動します。eMMC の各パーティションは
   アンマウント状態とし、USB ADB に `QuaMaintenance` が表示されることを確認します。
2. この個体の eMMC 全体と CID をホストへ保存します。

   ```sh
   adb -s QuaMaintenance shell 'cat /sys/class/block/mmcblk0/device/cid' > cid.txt
   set -o pipefail
   adb -s QuaMaintenance exec-out 'dd if=/dev/mmcblk0 bs=1048576 2>/dev/null' | gzip -1 > factory-emmc.img.gz
   ```

3. `prepare-target-bundle.py` でバックアップから個体別バンドルを作成します。
   個体の factory 環境と対応するレスキュープロファイル・ローダーを組み合わせます。
   バンドルを公開したり、別個体の環境や MAC アドレスを流用したりしないでください。
4. ホストから、バンドル、rootfs、SSH 公開鍵、Wi-Fi 設定、メンテナンス stage2 を渡します。

   ```sh
   bash install-over-usb.sh RELEASE_DIR TARGET_BUNDLE \
     QuaStation-Armbian-trixie-arm64-rootfs.tar.zst authorized_keys wpa.conf \
     QuaStation-maintenance-stage2.tar.gz
   ```

   `install-target.sh` が以下を順に実行します。

   - `quastation-layout.mbr` を適用し、パーティションテーブルを再読み込み。
   - **p2 のみ**を Btrfs でフォーマットし、サブボリューム `@` を作成。
   - `subvol=@,compress=zstd:3` でマウントし、rootfs をストリーム展開。
   - 指定した認証情報と個体固有の Wi-Fi MAC 設定を配置。
   - 起動スロットを書き込み、読み戻して照合。
   - 個体別レスキュー引き継ぎを有効化。

5. 成功表示後に再起動し、`uname -r`、`findmnt /`、
   `systemctl status wpa_supplicant@wlan0 ssh` と USB ADB を確認します。
   ルートは `/dev/mmcblk0p2[/@]`、カーネルとモジュールは同一リリースである必要があります。

初回導入にはカーネルと rootfs のほか、対応する `rescue-package/manifest.json`、
`u-boot-ram-console.bin`、`chain-copy-arm32.bin`、公開用 `profile.json`、
メンテナンス stage2 が必要です。これらは個体別バンドル生成の入力です。
別ボード用 U-Boot で代用しないでください。
ローカルの既存導入には実機記録があります。公開用 USB インストール経路の受け入れ条件は、
工場出荷状態の実機で導入から起動まで通して動作することです。

## 3. HuaStation を導入する

Armbian 起動後、対象機上で実行します。ダウンロードには `curl` と
`ca-certificates`、インストールには管理者権限と APT 接続が必要です。

```sh
sudo bash image/install-huastation.sh
```

スクリプトは以下の固定バージョンを取得し、SHA256 を照合してから
APT で依存パッケージとともにインストールします。

- URL: https://git.huggy.moe/qua/huastation/releases/download/0.1.0/huastation_0.1.0_arm64.deb
- SHA256: `bd10cc1f6a7514e5dfdedb53cb0a6f4a3426dc6a45b8e8271683c3b8e53c7072`

USB ADB の切り替えはパッケージの postinst が担当します。
実機上で実行するため、ビルドホストの chroot でサービスを起動する必要はありません。

## Media と更新

media は `/opt/qua-video/openmax` に配置します。
MPEG-2 / OpenMAX / GPU の直接処理パイプライン、`gpu-bob.so`、`rpc-probe.so`、
パッチ適用済み `ffmpeg-omx`、ラッパー、対応する Realtek ランタイムが対象です。
`h264_metadata` フィルターにはディストリビューションの FFmpeg を使用します。
入力形式とインターレース解除の制約は `media/qua-transcode/README.md` を参照してください。
動作確認には実際の MPEG-2 入力から H.264 / AAC 出力を生成します。

導入後のカーネル更新は Qua 上で `apt install ./linux-image-…_arm64.deb` を使用します。
postinst が既存のスロットライターを呼び出します。再起動後の実行中カーネルで切り替えを確認します。

## LICENSE

本プロジェクトは混合ライセンスです。カーネル、media、vendor ライブラリ、
ファームウェア、配布パッケージにはそれぞれのライセンスが適用されます。
詳細は [LICENSE](LICENSE) と各ファイルの著作権・ライセンス表示を参照してください。


Made with ❤️
