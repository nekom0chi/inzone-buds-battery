# INZONE Buds Battery

INZONE HUB の `ActionLog.log` を読み取り、INZONE Buds のバッテリー残量をパーセントで表示する小さなWindows用ツールです。

## 必要なもの

- Windows 10 / Windows 11
- INZONE HUB
- INZONE Buds

`.exe` 版を使う場合、Python は不要です。

## 使い方

1. INZONE HUB を起動し、INZONE Buds を接続します。
2. `INZONE Buds Battery.exe` をダブルクリックします。
3. 起動確認の小さい画面が出ます。閉じてもツールは通知領域に残ります。
4. Windows の通知領域に、左右イヤホンのうち低い方の残量がアイコン表示されます。
5. アイコンにマウスを乗せると、`L 100% / R 100% / Case 100%` のように詳細が出ます。
6. アイコンを左クリックすると詳細、右クリックするとメニューが開きます。

## メニュー

通知領域アイコンを右クリックすると、次のメニューが開きます。

- `バッテリーを表示`
- `表示を更新`
- `スタートアップ起動`
- `終了`

`表示を更新` は、INZONE HUB のログを読み直して表示を更新します。インターネットから最新版を取得する機能ではありません。

`スタートアップ起動` を選ぶと、Windows起動時に自動起動する設定をON/OFFできます。

## 友達に共有する場合

Pythonを持っていない相手には、`.exe` 版を渡してください。

- `INZONE Buds Battery.exe`
- `README.md`
- `LICENSE`

ソース版で渡す場合は、次のファイルを渡せば動きます。

- `inzone_buds_battery.py`
- `start_inzone_buds_battery.bat`
- `README.md`
- `LICENSE`

ソース版を使う場合のみ、相手のPCにPython 3が必要です。

## コマンド

現在値を一回だけ表示:

```powershell
python .\inzone_buds_battery.py --once
```

コンソールで更新を監視:

```powershell
python .\inzone_buds_battery.py --watch
```

通知領域で常駐:

```powershell
python .\inzone_buds_battery.py --tray
```

`.exe` を作る:

```powershell
.\build_exe.bat
```

## 仕組み

INZONE Buds はこのPCでは `VID_054C&PID_0EC2` のUSB/HIDデバイスとして見えています。現時点の実装ではHIDを直接解析せず、INZONE HUBが出力している次のログ項目を利用しています。

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
- まずは実用優先のログ監視版です。将来的にはUSB/HIDを直接読む方式にすると、INZONE HUB依存を減らせます。
- 個人情報やイヤホンのシリアル番号は保存しません。INZONE HUBのログからバッテリー関連の行だけを読み取ります。
- 通知領域アイコンの数字は左右イヤホンだけを対象にします。ケース残量は詳細表示とツールチップに表示されます。

## 負荷について

- イヤホン本体とは直接通信しません。PC内のINZONE HUBログを読むだけなので、イヤホン側のバッテリー消費は基本的に増えません。
- 15秒ごとにログのサイズと更新時刻だけ確認し、ログが変わった時だけ読み直します。
- 表示内容が変わった時だけ通知領域アイコンを作り直します。
- 常駐中に履歴を溜め込まないため、長時間起動でメモリが増え続ける設計ではありません。
