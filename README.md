
# BonDriver_UDP-TCP

## このリポジトリについて

このリポジトリは、BonDriver_UDP・BonDriver_TCP ver1.20 mod 2 ( [http://www.axfc.net/u/3753613](http://www.axfc.net/u/3753613) ) のミラーです。

ビルド環境を Visual Studio 2019 (VS2019) に更新したこと、この README.md を作成したこと以外はオリジナルのままとなっています。  
以下のドキュメントは [BonDriver_UDP.txt](BonDriver_UDP.txt) を一部改変し、Markdown 形式に書き換えたものです。  
オリジナルの [BonDriver_UDP.txt](BonDriver_UDP.txt) も参照してください。

BonDriver_UDP・BonDriver_TCP の作者の方々に感謝します。

----

## BonDriver (Meru customize edition) for UDP/TCP ver1.20 mod 2 (2016-12-17)

## はじめに

BonDriver_UDP は、UDP で送信されたデータを受信するための BonDriver です。  
Meru-co 氏の作成したものを若干改変しています。  
( mod 2 追記) TVTest 0.7.20 同梱の mod 1 をさらに TCP 対応に改変しました。mod 1 とは別人です。

## ダウンロード

BonDriver_UDP・BonDriver_TCP の両方が同梱されています。  
32bit 版 と 64bit 版があります。お使いの TVTest や EDCB のアーキテクチャに合わせてください。

[BonDriver_UDP-TCP ver1.20 mod 2](https://github.com/tsukumijima/BonDriver_UDP-TCP/releases/download/v1.20-mod2/BonDriver_UDP-TCP.zip)

## 使い方

BonDriver 対応プログラムにて、BonDriver_UDP.dll を選択する。  
受信するポート番号をチャンネルの中から選択する (送信元のプログラムと合わせる)。  

## ライセンス・免責事項

ライセンスはオリジナル版に従います。

Meru-co 氏のオリジナル版のライセンス

    ライセンスは拡張ツール中の人のものに従います。
    受信する TS の使い方によっては著作権・肖像権・人格権等を侵害する可能性があります。
    自己責任の上でモラルある使い方を心がけてください。

  拡張ツール中の人のライセンス

    ・特に著作権は主張しませんが、下記運用に従うようにお願いします。

    (1)「BonDriver.dll」をオリジナルのままアプリケーションに付属して配布する場合。

        アプリケーションのバージョン情報及びドキュメントに著作権表示は必要ありません。
        「拡張ツール中の人」もしくは「http://2sen.dip.jp/friio/」の記載をしても構いませんが任意です。

    (2)ソースコードを流用しアプリケーションに組み込むもしくはライブラリとして再構成して配布する場合。

        著作権表示については(1)に従いますが、必ず最低でも当該処理のソースコードを公開してください。


## 更新履歴

- **1.20 mod 2 (2016-12-17)**
  - BonDriver_TCP もビルドできるようにした (ソースコード不明だったので適当に仕様を想像した)
  - 受信レベルを BonDriver_TCP の仕様に合わせて"常に0"に変更
  - リソースリークと排他漏れについて微修正
- **1.20 mod 1**
  - 初期チャンネル (ポート 1234) を設定しないようにした
  - 無駄なコードを削減した
- 以下、Meru-co 氏のオリジナル版
- **1.20**
  - BonDriver 1.20 のインターフェースを実装
  - 空間=プロトコル チャンネル=ポート番号とし、自由に選べるように実装
  - 受信レベルを受信レート (MBps) として実装 ( 500ms ごとに計算。精度はそんなに高くありません)
- **1.00**
  - 初版
