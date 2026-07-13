# INZONE Buds Battery

INZONE HUB の `ActionLog.log` を読み取り、INZONE Buds のバッテリー残量を通知領域にパーセント表示するWindows用ツールです。

C++ / Win32 APIで作った軽量版です。Pythonは不要です。

## 最新バージョン

### v1.0.4

- v1.0.3で通知領域アイコンを右クリックしてもメニューが開かない問題を修正しました。
- Windows 11の新しい通知領域イベント形式と従来形式の両方に対応しました。
- 左クリックのバッテリー詳細表示も同じイベント形式に対応しました。

### v1.0.3

- 一定時間後やWindows Explorerの再起動後に通知領域アイコンが消える問題を修正しました。
- タスクバーが再作成された場合、通知領域アイコンを自動的に再登録します。
- 定期更新時にアイコンが見つからない場合も自動的に復旧します。

## 必要なもの

- Windows 10 / Windows 11
- INZONE HUB
- INZONE Buds

## 使い方

1. INZONE HUB を起動し、INZONE Buds を接続します。
2. `INZONE Buds Battery.exe` をダブルクリックします。
3. ツールは通知領域に常駐します。起動時に詳細画面は表示されません。
4. Windows の通知領域に、左右イヤホンのうち低い方の残量がアイコン表示されます。
5. アイコンにマウスを乗せると、`L 100% / R 100% / Case 100%` のように詳細が出ます。
6. アイコンを左クリックすると詳細、右クリックするとメニューが開きます。

## メニュー

通知領域アイコンを右クリックすると、次のメニューが開きます。

- `バッテリーを表示`
- `表示を更新`
- `スタートアップ起動`
- `終了`

`表示を更新` は、INZONE HUB のログを読み直して通知領域の表示を更新します。インターネットから最新版を取得する機能ではなく、詳細ポップアップも表示しません。

`スタートアップ起動` を選ぶと、Windows起動時に自動起動する設定をON/OFFできます。設定は現在のユーザーの `HKCU\Software\Microsoft\Windows\CurrentVersion\Run` に保存されます。

## 友達に共有する場合

`.exe` 版を渡してください。

- `INZONE Buds Battery.exe`
- `LICENSE`

配布用の `.exe` は、Visual Studio / MSVC でビルドしたものをおすすめします。

おすすめの配置場所:

```text
%LOCALAPPDATA%\Programs\INZONE Buds Battery\
```

例:

```text
C:\Users\<ユーザー名>\AppData\Local\Programs\INZONE Buds Battery\INZONE Buds Battery.exe
```

`Downloads` や `Desktop` でも起動できますが、長く使う場合はおすすめしません。誤って消したり移動したりすると、スタートアップ起動の設定が古い場所を指して動かなくなるためです。まず上のフォルダへ `.exe` を置いてから起動し、必要なら右クリックメニューの `スタートアップ起動` をONにしてください。

初回起動時に Windows Defender SmartScreen の `Windows によって PC が保護されました` が表示されることがあります。個人作成の未署名 `.exe` ではよく起きます。信頼できる配布元から入手した場合は、`詳細情報` → `実行` で起動できます。

また、一部のセキュリティソフトでは、未署名の個人作成 `.exe` や MinGW の静的リンク版が誤検知されることがあります。特に、スタートアップ起動のON/OFFはWindowsの自動起動設定を変更するため、挙動監視で警告される場合があります。不安な場合は、このリポジトリのソースコードを確認して、Visual Studio で自分でビルドしてください。

## ビルド

MinGW / WinLibs:

```powershell
.\native\build_mingw.bat
```

Visual Studio / Visual Studio Build Tools:

```powershell
.\native\build_msvc.bat
```

ビルド結果:

```text
dist-native\INZONE Buds Battery.exe
```

## 仕組み

HIDを直接解析せず、INZONE HUBが出力している次のログ項目を利用しています。

- `batteryStatusLeft`
- `batteryStatusRight`
- `batteryStatusCase`

標準のログ場所:

```text
%APPDATA%\Sony\INZONE Hub\ActionLog.log
```

INZONE HUB が同じ `batteryStatusLeft` / `batteryStatusRight` / `batteryStatusCase` 形式でログを出す機種なら、INZONE Buds 以外でも使える可能性があります。ただし、現時点で動作確認しているのは INZONE Buds だけです。

## 注意

- INZONE HUB がログを更新しない間は、このツールの表示も更新されません。
- INZONE HUB のログ形式が変わると読み取れなくなる可能性があります。
- 個人情報やイヤホンのシリアル番号は保存しません。INZONE HUBのログからバッテリー関連の行だけを読み取ります。
- 通知領域アイコンの数字は左右イヤホンだけを対象にします。ケース残量は詳細表示とツールチップに表示されます。

## 負荷について

- イヤホン本体とは直接通信しません。PC内のINZONE HUBログを読むだけなので、イヤホン側のバッテリー消費は基本的に増えません。
- 15秒ごとにログを確認します。
- 表示内容が変わった時だけ通知領域アイコンを作り直します。
- 常駐中に履歴を溜め込まないため、長時間起動でメモリが増え続ける設計ではありません。
