# Cardputer Adv Lo-Fi Player

[English](README.md) | [简体中文](README.zh-CN.md) | [日本語](README.ja.md)

Cardputer Adv Lo-Fi Player は、M5Stack Cardputer Adv 用のローカル音楽プレーヤーファームウェアです。microSD カードに音楽を入れると、デバイス上で曲を探して再生し、いくつかの Lo-Fi 音色を切り替えられます。

![Cardputer Adv Lo-Fi Player demo](assets/readme/cardputer-adv-lofi-demo.gif)

## 状態

現在のバージョン: `v0.1.0` beta。

release 用の書き込みパッケージは後で追加します。

## 機能

- ローカル音楽のブラウズと再生
- キュー、前/次トラック、再生/一時停止、進捗、音量操作
- Repeat と Shuffle
- いくつかの Lo-Fi 音色プリセット
- 再生状態の保存と復元
- `mp3`、`m4a`、`aac`、`wav` に対応

## 対応ハードウェア

現在は M5Stack Cardputer Adv でのみテストしています。通常版 Cardputer や他の ESP32-S3 ボードでの動作はまだ保証していません。

## microSD レイアウト

FAT32 でフォーマットした microSD カードを使い、音楽を `/Music` の下に置いてください。

推奨レイアウト:

```text
/Music/Artist/Album/Track.m4a
/Music/Artist/Track.mp3
/Music/Track.wav
```

ファームウェアはインデックスと再生状態をここに保存します。

```text
/Music/LOFI/INDEX.TXT
/Music/LOFI/STATE.TXT
```

音楽を再スキャンしたい場合や再生状態をリセットしたい場合は、この 2 つのファイルを削除してください。

## インストール

release 用の書き込みパッケージはまだありません。現時点ではソースからビルドして書き込んでください。

release が公開された後は、release ページの手順を優先してください。

## ソースからビルド

先に ESP-IDF をインストールし、ESP-IDF 環境に入ってください。このプロジェクトは ESP-IDF `v5.4.1` を使用しています。

```bash
idf.py set-target esp32s3
idf.py build
idf.py -p PORT flash monitor
```

`PORT` は実際のシリアルポートに置き換えてください。例: `/dev/ttyUSB0`、`/dev/cu.usbmodemXXXX`、`COM3`。

## 基本操作

- `Enter`: 決定 / 選択項目を開く
- `Space`: 再生 / 一時停止
- 矢印キー: 移動と調整
- `B`: 戻る
- `M`: メニュー、または現在の画面の操作
- `H`: 現在の画面のキー操作ヘルプを開く

画面下部には、そのページで使えるソフトキーのヒントが表示されます。

## プロジェクト構成

```text
main/                 app entry, board bring-up, UI, audio task
src/                  playback core, storage, input, Lo-Fi DSP
assets/               icon and font assets
partitions.csv        flash partition table
sdkconfig.defaults    default ESP-IDF configuration
dependencies.lock     locked dependencies
```

## フィードバック

Issue を作成する場合は、できれば以下を含めてください。

- デバイスのモデル
- ファームウェアの commit またはバージョン
- 音楽ディレクトリ構成
- シリアルログ、または再現手順

## ライセンス

MIT License。詳しくは [LICENSE](LICENSE) を参照してください。

同梱されている第三者フォントやアセットは、それぞれのライセンスに従います。詳細は `assets/` 内の該当ファイルを参照してください。
