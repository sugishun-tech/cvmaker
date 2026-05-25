# cvmaker

履歴書PDF生成ツールです。

`style.txt` のDSLを読み、`data.yaml` の値を差し込んでA4 PDFを描画します。

## 依存ライブラリ

この版は以下を使います。

- cairo: PDF描画
- pango / pangocairo: 日本語テキスト描画
- gdk-pixbuf-2.0: 写真画像の読み込み
- IPAex系などの日本語フォント

## Debian / Ubuntu 系のインストール例

`fonts-ipaexfont` ではなく、環境によっては分割パッケージ名を指定する必要があります。

```sh
sudo apt update
sudo apt install \
  build-essential \
  pkg-config \
  libcairo2-dev \
  libpango1.0-dev \
  libgdk-pixbuf-2.0-dev \
  fonts-ipaexfont-gothic \
  fonts-ipaexfont-mincho
```

もしフォント名が環境で違う場合は、次で確認できます。

```sh
fc-list | grep -i ipaex
```

Cコード側では標準で以下のPangoファミリ名を使っています。

```c
#define MINCHO_FAMILY "IPAexMincho"
#define GOTHIC_FAMILY "IPAexGothic"
```

別フォントを使う場合は、この2行を書き換えてください。

## ビルド

```sh
make
```

生成される実行ファイル:

```sh
./cvmaker
```

## 実行

```sh
./cvmaker -i data.yaml -s style.txt -o output.pdf
```

省略時のデフォルトは以下です。

```text
input:  data.yaml
style:  style.txt
output: output.pdf
```

## 対応している style.txt 命令

- `string`
- `box`
- `line`
- `lines`
- `multi_lines`
- `photo`
- `education_experience`
- `history`
- `textbox`
- `new_page`

## 注意点

このC版のYAML読み込みは、添付の `data.yaml` で使われている範囲に絞った軽量実装です。完全なYAMLパーサではありません。

対応している主な形式:

```yaml
name: 山田 太郎
education:
  - year: 2010
    month: 4
    value: 入学
```
