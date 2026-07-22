# INZONE Buds Battery

INZONE Budsのバッテリー残量を、Windowsの通知領域へ数字で表示する小さな常駐アプリです。

INZONE HUBが保存している情報を利用するため、イヤホンへ直接通信しません。Pythonなどの追加ソフトも不要です。

## できること

- 通知領域アイコンへ左右イヤホンの残量を表示
- アイコンへカーソルを合わせると、L・R・ケースの残量を表示
- Windows起動時の自動起動をON/OFF
- INZONE HUBの情報が変わった時だけ更新する省負荷動作

専用画面、履歴グラフ、EQ変更機能はありません。必要な機能だけに絞った軽量版です。

## 必要なもの

- Windows 10またはWindows 11
- INZONE HUB
- INZONE Buds

## インストール

1. [GitHub Releases](https://github.com/nekom0chi/inzone-buds-battery/releases/latest)を開きます。
2. `INZONE-Buds-Battery-Setup-v1.3.0.exe`をダウンロードします。
3. ダウンロードしたファイルを開き、画面に沿ってインストールします。
4. INZONE HUBを起動して、INZONE Budsを接続します。

インストールしたくない場合は、ZIP版を展開して`INZONE Buds Battery.exe`を起動しても使えます。

### Windowsの警告が表示された場合

個人作成の未署名アプリなので、初回に「WindowsによってPCが保護されました」と表示されることがあります。GitHubのこのリポジトリから入手したファイルであれば、`詳細情報`から`実行`を選択できます。

## 使い方

起動すると、画面右下の通知領域へ数字のアイコンが表示されます。見つからない場合は、通知領域の`^`を押して隠れているアイコンを確認してください。

- **アイコンの数字**: 左右イヤホンのうち、残量が少ない方
- **カーソルを合わせる**: L・R・ケースの残量を表示
- **左クリック**: INZONE HUBの情報をすぐに読み直す
- **右クリック**: 操作メニューを表示

右クリックメニューには次の項目があります。

- `今すぐ更新`
- `スタートアップ起動: ON / OFF`
- `終了`

## 表示されない時

### アイコンが見つからない

通知領域の`^`を確認してください。それでも見つからない場合は、一度アプリを終了して起動し直します。

### `Unknown`や`Disconnect`になる

INZONE HUBを起動し、INZONE Budsが接続されていることを確認します。その後、通知領域アイコンを左クリックしてください。

### Windows起動時に自動で起動したい

通知領域アイコンを右クリックし、`スタートアップ起動: OFF`を選択してONにします。

## 負荷とプライバシー

- 待機中のCPU使用はほぼ0です。
- メモリ使用量はテスト環境で約10～11 MBでした。
- 60秒ごとにINZONE HUBログの更新有無だけを確認し、変化した時だけ読み込みます。
- バッテリー履歴や個人情報は保存しません。
- イヤホンへ直接通信しないため、このアプリがイヤホン側のバッテリー消費を増やすことは基本的にありません。

## 仕組み

INZONE HUBの次のログを読み取っています。

```text
%APPDATA%\Sony\INZONE Hub\ActionLog.log
```

使用する項目は`batteryStatusLeft`、`batteryStatusRight`、`batteryStatusCase`だけです。

同じ形式のログを出す機種なら、INZONE Buds以外でも動く可能性があります。ただし、現在確認できているのはINZONE Budsだけです。

## 開発者向けビルド

Visual Studio 2022のC++環境で、次を実行します。

```powershell
.\native\build_msvc.bat
```

完成したEXEは`dist-native\INZONE Buds Battery.exe`へ作成されます。

## ライセンス

[MIT License](LICENSE)
