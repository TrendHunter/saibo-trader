# Scripts（LIH 实盘）

## 运行必需

| 路径 | 用途 |
|------|------|
| `deploy_production.py` / `remote_deploy.py` | 部署 |
| `deploy_vps_full.sh` | VPS pull + build |
| `server_start_bot.sh` / `server_*.sh` / `web_*.sh` | 进程启动 |
| `live_lih_reconcile.py` / `prune_live_lih.py` | bridge 定时链上对齐 |

根目录：`start_bot.py` → `dashboard_bridge.py` → `build/trading-core`；core 调用 `fetch_balance.py`、`clob_live.py`、`redeem_positions.py`。

## 可选运维

`_restart_bot_only.py` · `_preflight_live_test.py` · `_watch_test_round.py` · `_emergency_stop_entries.py` · `_pause_live_keep_config.py`

## 历史备份

精简前整仓（shadow 脚本、archive、DH 等）：[`../backup/source-snapshot-2026-06-17.zip`](../backup/source-snapshot-2026-06-17.zip)
